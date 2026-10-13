#!/usr/bin/env python3
"""Prepared-input TCP jobs through real broker, daemon and compilers."""
import concurrent.futures
import os
from pathlib import Path
import re
import random
import runpy
import signal
import shutil
import socket
import struct
import subprocess
import threading
import time
import unittest
import xxhash

from test_broker import BrokerTestCase


class TcpJobsTest(BrokerTestCase):
    def start_tcp(self):
        return self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))

    def test_compile_and_pool_reuse(self):
        self.start_tcp()
        before = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        for i in range(5):
            self.job(i, 'cc' if i % 2 else 'c')
        stats = self.get_stats(self.stats_ports[0])
        self.assertEqual(int(re.search(r'dcc_tcp_accept (\d+)', stats)[1]) - before, 1)
        self.assertIn('dcc_busy 0\n', stats)

    def test_parallel_compile(self):
        self.start_tcp()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(self.job, range(24)))

    def test_preparing_and_ready_are_distinct(self):
        reservations = []
        for _ in range(4):
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
            self.addCleanup(net.close)
            net.sendall(b'SLOT00000001')
            self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'GRNT00000001')
            reservations.append(net)
        body = b'int phase_accounting;\n'
        net = self.wire_job(body)
        self.assertEqual(self.frame(net), (0x54000201, bytes(4)))
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_preparing 1\n', stats)
        self.assertIn('dcc_queued 0\n', stats)
        self.upload(net, body)
        deadline = time.monotonic() + 3
        while True:
            stats = self.get_stats(self.stats_ports[0])
            if 'dcc_queued 1\n' in stats:
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        self.assertIn('dcc_preparing 0\n', stats)
        self.assertIn('dcc_busy 4\n', stats)
        for lease in reservations:
            lease.close()
        self.result(net)

    def test_multiple_brokers_share_workers(self):
        hosts = ' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2))
        environments = []
        for i in range(2):
            self.start_broker(hosts, name=f'shared-{i}')
            environments.append(dict(self.env, DISTCC_VERBOSE='1'))
        before = [int(re.search(r'dcc_compile_ok (\d+)', self.get_stats(port))[1])
                  for port in self.stats_ports]

        def compile_job(i):
            obj, exe = self.work / f'shared-{i}.o', self.work / f'shared-{i}'
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                     '-o', str(obj)], env=environments[i % 2],
                                    capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn(b'pooled TCP job completed', result.stderr)
            subprocess.run([self.cc, str(obj), '-o', str(exe)], check=True,
                           capture_output=True, timeout=5)
            subprocess.run([str(exe)], check=True, timeout=5)

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(compile_job, range(32)))
        for i, port in enumerate(self.stats_ports):
            stats = self.get_stats(port)
            self.assertGreater(int(re.search(r'dcc_compile_ok (\d+)', stats)[1]), before[i])

    def test_publication_failure_keeps_completed_connection_reusable(self):
        self.start_tcp()
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_VERBOSE='1')
        destination = self.work / 'directory.o'
        destination.mkdir()
        before = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(destination)], env=self.env, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'pooled TCP job completed', result.stderr)
        self.assertTrue(destination.is_dir())
        self.assertFalse(list(self.work.glob('directory.o.distcc-*')))
        self.job('after-publication-failure')
        after = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        self.assertEqual(after - before, 1)

    def test_parallel_compressed_inputs_and_disconnect(self):
        body = b'/*' + b'bounded expansion ' * 65536 + b'*/\nint parallel_input;\n'
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        streams = [self.wire_job(body, cached=False) for _ in range(6)]
        cancelled = self.wire_job(body, cached=False)
        first = codec.encode(body[:262144])
        self.send(cancelled, 0x54010202, struct.pack('!II', 0, 262144) + first[8:])
        cancelled.close()
        for offset in range(0, len(body), 262144):
            chunk = body[offset:offset + 262144]
            packed = codec.encode(chunk)
            plain, size = struct.unpack('!II', packed[:8])
            self.assertGreater(size, 0)
            kind = 0x54010202 | (0x20000 if offset + plain == len(body) else 0)
            for net in streams:
                self.send(net, kind, struct.pack('!II', 0, plain) + packed[8:])
        for net in streams:
            self.result(net)
        stats = self.get_stats(self.stats_ports[0])
        self.assertGreaterEqual(int(re.search(r'dcc_tcp_parallel_tasks (\d+)', stats)[1]), 24)
        self.assertIn(int(re.search(r'dcc_tcp_parallel_peak (\d+)', stats)[1]), (1, 2))

    def test_partial_manifest_does_not_reserve_worker(self):
        self.start_tcp()
        sockets = []
        try:
            for _ in range(8):
                net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
                sockets.append(net)
                net.sendall(struct.pack('!II', 0x54000200, 100) + b'partial')
            self.job('while-receiving')
        finally:
            for net in sockets:
                net.close()
        self.job('after-cancel')

    def wire_job(self, body, cached=True, paths=(b'/src/source.c',), identity=1, receive_buffer=None):
        def string(value):
            return struct.pack('!Q', len(value)) + value
        args = [b'gcc', b'-c', paths[0], b'-o', b'/src/result.o']
        manifest = struct.pack('!Q3Q', identity, 1, len(args), len(paths)) + string(b'/src')
        manifest += b''.join(map(string, args))
        for path in paths:
            manifest += (struct.pack('!5Q', 0, 0, cached, len(body), len(body)) +
                         xxhash.xxh3_64(body).digest() + string(path) + string(b''))
        net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5)
        if receive_buffer:
            net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.addCleanup(net.close)
        self.send(net, 0x54000200, manifest)
        return net

    @staticmethod
    def send(net, kind, body):
        net.sendall(struct.pack('!II', kind, len(body)) + body)

    def frame(self, net):
        def read(size):
            result = bytearray()
            while len(result) < size:
                part = net.recv(size - len(result))
                self.assertTrue(part, 'unexpected connection EOF')
                result.extend(part)
            return bytes(result)
        kind, size = struct.unpack('!II', read(8))
        self.assertLessEqual(size, 256 * 1024 + 8)
        return kind, read(size)

    def upload(self, net, body, index=0):
        self.send(net, 0x54000202 | 0x20000, struct.pack('!II', index, len(body)) + body)

    def result(self, net, identity=1):
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        kind, description = self.frame(net)
        self.assertEqual(kind, 0x54000203)
        status, mode, *sizes = struct.unpack('!II4Q', description)
        outputs = [bytearray() for _ in sizes]
        while True:
            kind, body = self.frame(net)
            if kind == 0x54000205:
                self.assertEqual(body, struct.pack('!Q', identity))
                break
            self.assertEqual(kind & ~0x30000, 0x54000204)
            index, plain = struct.unpack('!II', body[:8])
            data = codec.decode(body[8:], plain) if kind & 0x10000 else body[8:]
            outputs[index].extend(data)
        self.assertEqual(list(map(len, outputs)), sizes)
        self.assertEqual(status, 0, outputs[0].decode(errors='replace'))
        self.assertTrue(outputs[2].startswith(b'\x7fELF'))
        return outputs

    def test_single_upload_and_owner_disconnect(self):
        body = b'int cache_owner_recovery(void) { return 901; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000201, bytes(4)))
        waiter = self.wire_job(body)
        waiter.settimeout(.15)
        with self.assertRaises(socket.timeout):
            waiter.recv(8)
        owner.close()
        waiter.settimeout(5)
        self.assertEqual(self.frame(waiter), (0x54000201, bytes(4)))
        self.upload(waiter, body)
        self.result(waiter)
        hit = self.wire_job(body)
        self.result(hit)  # No NEED on the warm cache path.

    def test_parallel_cache_waiters_share_upload(self):
        body = b'int single_upload_for_all(void) { return 902; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000201, bytes(4)))
        waiters = [self.wire_job(body) for _ in range(7)]
        self.upload(owner, body)
        self.result(owner)
        for waiter in waiters:
            self.result(waiter)

    def test_path_ancestor_rejected_even_with_intervening_name(self):
        # Checking only adjacent sorted names misses /a as parent of /a/b.
        net = self.wire_job(b'int invalid;', paths=(b'/a', b'/a.b', b'/a/b'))
        self.assertEqual(net.recv(8), b'')

    def test_bad_digest_discards_connection_and_releases_waiter(self):
        body = b'int verified_upload;\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000201, bytes(4)))
        waiter = self.wire_job(body)
        self.upload(owner, b'x' * len(body))
        self.assertEqual(owner.recv(8), b'')
        self.assertEqual(self.frame(waiter), (0x54000201, bytes(4)))
        self.upload(waiter, body)
        self.result(waiter)

    def test_invalid_chunks_close_connection_without_dispatch(self):
        body = b'int bounded_input;\n'
        cases = [
            (0x54020202, struct.pack('!II', 0, 262145)),
            (0x54020202, struct.pack('!II', 1, len(body)) + body),
            (0x54030202, struct.pack('!II', 0, len(body)) + b'\xff'),
            (0x54020202, struct.pack('!II', 0, len(body)) + body[:-1]),
        ]
        for kind, data in cases:
            with self.subTest(kind=kind, payload=len(data)):
                net = self.wire_job(body, cached=False)
                self.send(net, kind, data)
                self.assertEqual(net.recv(8), b'')
                self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))
        self.start_tcp()
        self.job('after-invalid-chunks')

    def test_cancel_running_job_reclaims_tree_and_worker(self):
        body = b'int cancel_running_job;\n'
        net = self.wire_job(body, cached=False)
        self.upload(net, body)
        deadline = time.monotonic() + 3
        while 'dcc_busy 1\n' not in self.get_stats(self.stats_ports[0]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        net.close()
        deadline = time.monotonic() + 5
        while list(self.root.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.start_tcp()
        self.job('after-running-cancel')

    def test_worker_death_kills_compiler_and_reclaims_inputs(self):
        marker = self.work / 'compiler-pids'
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text(f'#!/bin/sh\necho "$PPID $$" > "{marker}"\n'
                           'echo private-temporary > "$TMPDIR/compiler-owned"\nexec sleep 30\n')
        body = b'int killed_compiler;\n'
        net = self.wire_job(body, cached=False)
        self.upload(net, body)
        deadline = time.monotonic() + 5
        while not marker.exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        worker, compiler = map(int, marker.read_text().split())
        os.kill(worker, signal.SIGKILL)
        self.assertEqual(net.recv(8), b'')
        while True:
            try:
                with open(f'/proc/{compiler}/stat') as status:
                    state = status.read().rsplit(')', 1)[1].split()[0]
            except FileNotFoundError:
                break
            if state == 'Z':
                break
            self.assertLess(time.monotonic(), deadline, 'compiler outlived its worker')
            time.sleep(.01)
        self.assertFalse(list(self.root.glob('distcc-tcp-*')))
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.assertFalse((self.root / 'compiler-owned').exists())
        wrapper.write_text(original)
        self.start_tcp()
        self.job('after-worker-death')

    def test_stalled_results_do_not_hold_compiler_workers(self):
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text('''#!/usr/bin/python3
import random, sys
from pathlib import Path
args = sys.argv[1:]
Path(args[args.index('-o') + 1]).write_bytes(random.Random(543).randbytes(4 * 1024 * 1024))
''')
        stalled = []
        try:
            for i in range(4):
                body = f'int stalled_{i};\n'.encode()
                net = self.wire_job(body, cached=False, receive_buffer=1024)
                stalled.append(net)
                self.upload(net, body)
                kind, description = self.frame(net)
                self.assertEqual(kind, 0x54000203)
                self.assertEqual(struct.unpack('!II4Q', description)[4], 4 * 1024 * 1024)
            wrapper.write_text(original)
            stats = self.get_stats(self.stats_ports[0])
            self.assertIn('dcc_busy 0\n', stats)
            self.assertIn('dcc_returning 4\n', stats)
            # Replacement must fork only after joining transfer threads.
            parent = self.daemons[0].pid
            workers = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
            self.assertTrue(workers)
            os.kill(int(workers[0]), signal.SIGKILL)
            self.start_tcp()
            self.job('while-results-stalled')
        finally:
            for net in stalled:
                net.close()

    def test_daemon_shutdown_reclaims_preparing_and_running_jobs(self):
        waiting = self.wire_job(b'int waiting_for_upload;\n', cached=False)
        body = b'int running_at_shutdown;\n'
        running = self.wire_job(body, cached=False)
        self.upload(running, body)
        deadline = time.monotonic() + 3
        while 'dcc_busy 1\n' not in self.get_stats(self.stats_ports[0]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        self.restart_server(self.build / 'distccd')
        self.assertEqual(waiting.recv(8), b'')
        self.assertEqual(running.recv(8), b'')
        self.assertFalse(list(self.root.glob('distcc-tcp-*')))
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.start_tcp()
        self.job('after-daemon-restart')

    def test_large_raw_result_keeps_the_last_partial_chunk(self):
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        size = 4 * 1024 * 1024 + 17
        wrapper.write_text(f'''#!/usr/bin/python3
import random, subprocess, sys
args = sys.argv[1:]
subprocess.run([{self.cc!r}, *args], check=True)
with open(args[args.index('-o') + 1], 'ab') as output:
    output.write(random.Random(764).randbytes({size}))
''')
        body = b'int main(void) { return 0; }\n'
        net = self.wire_job(body, cached=False, receive_buffer=65536)
        self.upload(net, body)
        outputs = self.result(net)
        self.assertEqual(outputs[2][-size:], random.Random(764).randbytes(size))
        obj = self.work / 'large.o'
        obj.write_bytes(outputs[2])
        executable = self.work / 'large'
        subprocess.run([self.cc, str(obj), '-o', str(executable)], check=True, capture_output=True)
        subprocess.run([str(executable)], check=True)

    def test_missing_complete_preserves_previous_object(self):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        errors = []
        reached = threading.Event()

        def server():
            try:
                with listener.accept()[0] as net:
                    net.settimeout(5)
                    kind, manifest = self.frame(net)
                    self.assertEqual(kind, 0x54000200)
                    while True:
                        kind, body = self.frame(net)
                        self.assertEqual(kind & ~0x30000, 0x54000202)
                        if kind & 0x20000:
                            break
                    # Deliberately fragment the response header.
                    response = struct.pack('!IIII4Q', 0x54000203, 40, 0, 0, 0, 0, 4, 0)
                    net.sendall(response[:3])
                    time.sleep(.01)
                    net.sendall(response[3:])
                    self.send(net, 0x54020204, struct.pack('!II', 2, 4) + b'FAKE')
                    reached.set()  # Close after a complete artifact, before COMPLETE.
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=server)
        thread.start()
        self.start_broker(f'127.0.0.1:{listener.getsockname()[1]}/1,tcp')
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1')
        output = self.work / 'previous.o'
        output.write_bytes(b'original artifact')
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(output)], env=self.env, capture_output=True, timeout=10)
        thread.join(timeout=6)
        self.assertFalse(thread.is_alive())
        if errors:
            raise errors[0]
        self.assertTrue(reached.is_set())
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b'original artifact')
        self.assertFalse(list(self.work.glob('previous.o.distcc-*')))

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_execution_ipc_is_one_request_and_one_result(self):
        trace = self.work / 'execution.trace'
        wrapper = self.work / 'traced-daemon'
        # Keep the tracked process as distccd; the tracer must not replace
        # the daemon's PID/process-group lifecycle in restart_server.
        wrapper.write_text(f'#!/bin/sh\nexec strace -D -f -qq -e trace=sendmsg -o "{trace}" '
                           f'"{self.build / "distccd"}" "$@"\n')
        wrapper.chmod(0o755)
        try:
            self.restart_server(wrapper)
            self.start_tcp()
            self.job('ipc')
            messages = [line for line in trace.read_text().splitlines()
                        if 'sendmsg(' in line and 'SCM_RIGHTS' in line]
            self.assertEqual(len(messages), 2, '\n'.join(messages))
        finally:
            self.restart_server(self.build / 'distccd')


if __name__ == '__main__':
    unittest.main()
