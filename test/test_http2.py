#!/usr/bin/env python3
"""HTTP/2 multiplexing and cancellation through real distccd workers."""
import concurrent.futures
import io
import hashlib
import re
import random
import os
from pathlib import Path
import runpy
import select
import signal
import socket
import struct
import subprocess
import time
import unittest

from test_broker import BrokerTestCase


class Http2Test(BrokerTestCase):
    def cached_compile(self, files, *, abort=False, corrupt=False, reject_manifest=False,
                       ready=None, execute=False):
        helper = runpy.run_path(str(self.build / 'distcc-link'))
        def string(name, value):
            return f'{name}{len(value):08x}'.encode() + value
        args = [b'gcc', b'-c', b'/src/source.c', b'-o', b'/src/result.o']
        header = b'DIST0000000b' + string('CDIR', b'/src') + f'ARGC{len(args):08x}'.encode()
        header += b''.join(string('ARGV', arg) for arg in args)
        header += f'NFIL{len(files):08x}'.encode()
        for name, body in files:
            header += string('NAME', name) + string('HASH', hashlib.sha256(body).digest())
        with self.request(9) as lease:
            status, caps, fds = self.receive(lease)
            self.assertEqual(status, 0)
            self.assertTrue(caps & (1 << 29), 'server cache capability was not negotiated')
            with socket.socket(fileno=fds[0]) as net:
                net.settimeout(10)
                net.sendall(header)
                with net.makefile('rb') as response:
                    if reject_manifest:
                        self.assertEqual(response.read(), b'')
                        return None
                    self.assertEqual(helper['token'](response, 'MISS'), len(files))
                    wanted = helper['receive_exact'](response, len(files))
                    if ready:
                        ready()
                    for (_, body), missing in zip(files, wanted):
                        self.assertIn(missing, (0, 1))
                        if not missing:
                            continue
                        net.sendall(b'FILEffffffff')
                        if abort:
                            net.sendall(struct.pack('!II', len(body), 0) + body[:1])
                            return wanted
                        if corrupt:
                            body += b'changed'
                        for offset in range(0, len(body), 262144):
                            chunk = body[offset:offset + 262144]
                            net.sendall(struct.pack('!II', len(chunk), 0) + chunk)
                        net.sendall(bytes(8))
                    net.shutdown(socket.SHUT_WR)
                    if corrupt:
                        self.assertEqual(response.read(), b'')
                        return wanted
                    self.assertEqual(helper['token'](response, 'DONE'), 11)
                    self.assertEqual(helper['token'](response, 'STAT'), 0)
                    codec = helper['Lz4Chunks']()
                    outputs = []
                    for kind in ('SERR', 'SOUT', 'DOTO', 'DOTD'):
                        output = io.BytesIO()
                        helper['copy_response'](response, kind, output, codec)
                        outputs.append(output.getvalue())
                    self.assertEqual(outputs[:2], [b'', b''])
                    self.assertTrue(outputs[2].startswith(b'\x7fELF'))
                    self.assertEqual(response.read(1), b'')
                    if execute:
                        assert self.cc is not None
                        obj, binary = self.work / 'snapshot.o', self.work / 'snapshot'
                        obj.write_bytes(outputs[2])
                        subprocess.run([self.cc, str(obj), '-o', str(binary)], check=True)
                        subprocess.run([str(binary)], check=True)
                    return wanted

    def test_cached_upload_cancel_corruption_and_concurrency(self):
        self.start_h2()
        files = [(b'/src/source.c', b'int cache_unique(void) { return 937; }\n')]
        self.assertEqual(self.cached_compile(files, abort=True), b'\1')
        self.assertEqual(self.cached_compile(files, corrupt=True), b'\1')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.cached_compile(files), range(4)))
        self.assertIn(b'\1', results)
        self.assertEqual(self.cached_compile(files), b'\0')
        self.job('after-cache-cancel')

    def test_cache_shards_bound_files_and_bytes(self):
        self.start_h2()
        files = [(b'/src/source.c', b'int eviction_unique(void) { return 519; }\n')]
        # Put more than one shard's entry budget in a single batch. No timing
        # assumption about which entry FIFO eviction chooses is necessary.
        index = 0
        while len(files) < 70:
            body = f'/* unused header {index} */\n'.encode()
            if hashlib.sha256(body).digest()[0] % 64 == 0:
                files.append((f'/src/header{index}.h'.encode(), body))
            index += 1
        self.cached_compile(files)
        shard = self.root / f'distcc-cache-{os.geteuid()}' / '00'
        self.assertLessEqual(len([p for p in shard.iterdir() if not p.name.startswith('.')]), 64)
        large = []
        while len(large) < 6:
            body = f'/* large header {index} */\n'.encode() + b' ' * (900 * 1024)
            if hashlib.sha256(body).digest()[0] % 64 == 0:
                large.append((f'/src/large{index}.h'.encode(), body))
            index += 1
        self.cached_compile(files[:1] + large)
        self.assertLessEqual(sum(p.stat().st_size for p in shard.iterdir()), 4 * 1024 * 1024)
        self.assertFalse(list(shard.glob('.tmp-*')))

    def test_cached_manifest_rejects_ambiguous_paths(self):
        self.start_h2()
        body = b'int ambiguous(void) { return 817; }\n'
        for path in (b'/src/../source.c', b'/src//source.c', b'/src/./source.c',
                     b'/src/source.c\0ignored'):
            self.cached_compile([(path, body)], reject_manifest=True)
        self.cached_compile([(b'/src/source.c', body), (b'/src/source.c', body)],
                            reject_manifest=True)
        self.assertEqual(self.cached_compile([(b'/src/source.c', body)]), b'\1')

    def test_active_job_keeps_private_snapshot(self):
        self.start_h2()
        body = b'int main(void) { return 0; }\n'
        files = [(b'/src/source.c', body)]
        self.cached_compile(files, execute=True)
        key = hashlib.sha256(body).hexdigest()
        entry = self.root / f'distcc-cache-{os.geteuid()}' / f'{int(key[:2], 16) % 64:02x}' / key
        def change_cached_inode():
            entry.chmod(0o600)
            entry.write_bytes(b'int main(void) { return 13; }\n')
        # MISS is emitted only after hits have been materialized. The real
        # compiler fixture sleeps before opening inputs, exposing shared inodes.
        self.assertEqual(self.cached_compile(files, ready=change_cached_inode, execute=True), b'\0')
        self.assertEqual(self.cached_compile(files, execute=True), b'\1')

    def start_h2(self, name='broker'):
        return self.start_broker(self.host(0, f',h2,lz4,stats={self.stats_ports[0]}') +
                                 ' --localslots_cpp=4', name=name)

    def accepts(self):
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        return int(match[1])

    def test_parallel_jobs_share_connection(self):
        self.start_h2()
        before = self.accepts()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(self.job, range(8)))
        self.assertEqual(len(results), 8)
        self.assertEqual(self.accepts() - before, 1)
        self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))

    def test_cancel_stream_preserves_other_jobs(self):
        self.start_h2()
        before = self.accepts()
        lease = self.request(6)
        status, capabilities, fds = self.receive(lease)
        self.assertEqual((status, capabilities, len(fds)), (0, 1 << 29, 1))
        with socket.socket(fileno=fds[0]) as net:
            net.sendall(b'DIST00000006ARGC00000002ARGV00000003gcc')
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                other = pool.submit(self.job, 'uncancelled')
                lease.close()
                other.result(timeout=20)
        self.job('after-cancel')
        self.assertEqual(self.accepts() - before, 1)

    def test_large_incompressible_result_and_input(self):
        self.start_h2()
        self.check_large_transfer()

    def check_large_transfer(self):
        source = self.work / 'large.i'
        data = random.Random(19).randbytes(1024 * 1024)
        source.write_text('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n')
        remote, payload = self.work / 'remote.o', self.work / 'payload'
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source), '-o', str(remote)],
                                env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        subprocess.run(['objcopy', '--dump-section', '.rodata=' + str(payload), str(remote)], check=True,
                       capture_output=True, timeout=30)
        self.assertEqual(payload.read_bytes(), data)

    def test_slow_result_consumer_preserves_other_jobs(self):
        self.start_h2()
        data = random.Random(43).randbytes(2 * 1024 * 1024)
        body = ('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n').encode()
        args = [b'gcc', b'-c', b'source.i', b'-o', b'result.o']
        header = b'DIST00000008' + f'ARGC{len(args):08x}'.encode()
        header += b''.join(f'ARGV{len(arg):08x}'.encode() + arg for arg in args)
        with self.request(6) as lease:
            status, _, fds = self.receive(lease)
            self.assertEqual(status, 0)
            with socket.socket(fileno=fds[0]) as net:
                net.settimeout(20)
                net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                net.sendall(header + b'DOTIffffffff')
                for start in range(0, len(body), 262144):
                    chunk = body[start:start + 262144]
                    net.sendall(struct.pack('!II', len(chunk), 0) + chunk)
                net.sendall(bytes(8))
                net.shutdown(socket.SHUT_WR)
                self.assertTrue(select.select([net], [], [], 20)[0])
                # Leave a result larger than every stream/socket buffer unread
                # while another real compilation completes on the same session.
                self.job('while-result-blocked')
                helper = runpy.run_path(str(self.build / 'distcc-link'))
                codec = helper['Lz4Chunks']()
                result = self.work / 'slow.o'
                with net.makefile('rb') as response, result.open('wb') as output:
                    self.assertEqual(helper['token'](response, 'DONE'), 8)
                    self.assertEqual(helper['token'](response, 'STAT'), 0)
                    helper['copy_response'](response, 'SERR', io.BytesIO(), codec)
                    helper['copy_response'](response, 'SOUT', io.BytesIO(), codec)
                    helper['copy_response'](response, 'DOTO', output, codec)
                    self.assertEqual(response.read(1), b'')
        payload = self.work / 'slow.payload'
        subprocess.run(['objcopy', '--dump-section', '.rodata=' + str(payload), str(result)],
                       check=True, capture_output=True, timeout=10)
        self.assertEqual(payload.read_bytes(), data)

    def test_old_lease_cannot_cancel_reconnected_stream(self):
        self.start_h2()
        old_lease = self.request(6)
        status, _, fds = self.receive(old_lease)
        self.assertEqual(status, 0)
        old_net = socket.socket(fileno=fds[0])
        self.addCleanup(old_lease.close)
        self.addCleanup(old_net.close)
        self.restart_server(self.build / 'distccd')
        time.sleep(5.1)  # The broken connection's peer backoff expires.
        with self.request(6) as lease:
            lease.settimeout(10)
            status, _, fds = self.receive(lease)
            self.assertEqual(status, 0)
            with socket.socket(fileno=fds[0]) as net:
                old_lease.close()
                net.settimeout(.2)
                with self.assertRaises(socket.timeout):
                    net.recv(1)
        self.job('reconnected')

    @unittest.skipUnless(os.environ.get('DISTCC_TEST_BASELINE'), 'previous HTTP/2 build is required')
    def test_previous_http2_peers(self):
        previous = Path(os.environ['DISTCC_TEST_BASELINE']).resolve()
        candidate = self.build
        try:
            self.build = previous
            self.start_h2('previous')
            self.check_large_transfer()
        finally:
            self.build = candidate
        try:
            self.restart_server(previous / 'distccd')
            self.start_h2('candidate')
            self.check_large_transfer()
        finally:
            self.restart_server(candidate / 'distccd')

    def test_blocked_worker_does_not_spin_broker(self):
        broker = self.start_h2()
        parent = self.daemons[0].pid
        workers = list(map(int, Path(f'/proc/{parent}/task/{parent}/children').read_text().split()))
        source = self.work / 'blocked.i'
        data = random.Random(31).randbytes(1024 * 1024)
        source.write_text('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n')
        command = [str(self.build/'distcc'), 'gcc', '-c', str(source), '-o', str(self.work/'blocked.o')]
        def cpu():
            fields = Path(f'/proc/{broker.pid}/stat').read_text().rsplit(')', 1)[1].split()
            return sum(map(int, fields[11:13])) / os.sysconf('SC_CLK_TCK')
        proc = None
        errors = b''
        try:
            for pid in workers: os.kill(pid, signal.SIGSTOP)
            proc = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(.3)  # Fill the bounded HTTP/2 window and local socket.
            self.assertIsNone(proc.poll())
            before = cpu()
            time.sleep(.4)
            self.assertLess(cpu() - before, .15, 'broker spun while its stream window was exhausted')
        finally:
            for pid in workers: os.kill(pid, signal.SIGCONT)
            if proc:
                try:
                    _, errors = proc.communicate(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
                    raise
        assert proc is not None
        self.assertEqual(proc.returncode, 0, errors.decode())


if __name__ == '__main__':
    unittest.main()
