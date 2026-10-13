#!/usr/bin/env python3
"""HTTP/2 multiplexing and cancellation through real distccd workers."""
import concurrent.futures
import array
from contextlib import ExitStack
import fcntl
import io
import json
import xxhash
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
import sys
import time
import threading
import unittest

from test_broker import BrokerTestCase


class Http2Test(BrokerTestCase):
    def test_concurrent_jobs_keep_reserved_result_files(self):
        # Force the initial temporary-name candidate to collide across workers.
        # The actual exclusive creation must keep each job's files separate.
        clock = self.work / 'clock.c'
        library = self.work / 'clock.so'
        clock.write_text('''#include <sys/time.h>
#include <unistd.h>
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    tv->tv_sec = COLLISION_SECONDS;
    tv->tv_usec = (getpid() & 65535) ^ 12345;
    return 0;
}
'''.replace('COLLISION_SECONDS', str(int(time.time()))))
        subprocess.run([self.cc, '-shared', '-fPIC', str(clock), '-o', str(library)],
                       check=True, capture_output=True, timeout=15)
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        gate = self.work / 'gate'
        gate.mkdir()
        wrapper.write_text(f'''#!{sys.executable}
import json, os, subprocess, sys, time
from pathlib import Path
gate = Path({str(gate)!r})
args = sys.argv[1:]
source = Path(args[args.index('-c') + 1]).read_text()
role = 'a' if 'ROLE_A' in source else 'b'
deps = args[args.index('-MF') + 1]
ready = gate / (role + '.ready')
temporary = gate / (role + '.tmp')
temporary.write_text(json.dumps(deps))
temporary.rename(ready)
def wait(name):
    deadline = time.monotonic() + 10
    while not (gate / name).exists():
        if time.monotonic() > deadline: sys.exit(99)
        time.sleep(.005)
wait('a.ready')
wait('b.ready')
if role == 'a': wait('b.compiled')
result = subprocess.run([{self.cc!r}, *args]).returncode
if role == 'b':
    (gate / 'b.compiled').touch()
    wait('release')
sys.exit(result)
''')
        try:
            self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library))
            self.start_h2()
            def job(role):
                body = f'/* ROLE_{role} */\nint main(void) {{ return 0; }}\n'.encode()
                return self.cached_compile([(b'/src/source.c', body)], execute=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                b = pool.submit(job, 'B')
                try:
                    deadline = time.monotonic() + 5
                    while not (gate / 'b.ready').exists() and time.monotonic() < deadline:
                        if b.done(): b.result()
                        time.sleep(.005)
                    self.assertTrue((gate / 'b.ready').exists(), 'second job did not reach compiler')
                    a = pool.submit(job, 'A')
                    self.assertEqual(a.result(timeout=15), b'\1')
                    deps_a = Path(json.loads((gate / 'a.ready').read_text()))
                    deadline = time.monotonic() + 5
                    while deps_a.exists() and time.monotonic() < deadline:
                        time.sleep(.005)
                    self.assertFalse(deps_a.exists(), 'first job did not finish cleanup')
                finally:
                    (gate / 'release').touch()
                self.assertEqual(b.result(timeout=15), b'\1')
                self.assertNotEqual((gate / 'a.ready').read_text(), (gate / 'b.ready').read_text())
        except Exception:
            self.logs[0].flush()
            self.logs[0].seek(0)
            print(self.logs[0].read()[-10000:], file=sys.stderr)
            raise
        finally:
            wrapper.write_text(original)
            self.restart_server(self.build / 'distccd')

    def cached_compile(self, files, *, abort=False, corrupt=False, reject_manifest=False,
                       ready=None, execute=False, inline=(), after_upload=None, capture=None,
                       declared_size=None):
        def string(value):
            return struct.pack('!Q', len(value)) + value
        args = [b'gcc', b'-c', b'/src/source.c', b'-o', b'/src/result.o']
        header = struct.pack('!3Q', 1, len(args), len(files)) + string(b'/src')
        header += b''.join(string(arg) for arg in args)
        for index, (name, body) in enumerate(files):
            size = len(body) if declared_size is None else declared_size
            header += struct.pack('!5Q', 0, 0, int(index not in inline), size, size)
            header += xxhash.xxh3_64(body).digest() + string(name) + string(b'')
        wanted = bytearray(len(files))
        with ExitStack() as stack, self.request(9) as lease:
            lease.settimeout(15)
            def send(operation, index, data):
                fd = os.memfd_create('test-job', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
                try:
                    with os.fdopen(os.dup(fd), 'wb') as stream:
                        stream.write(data)
                    fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK |
                                fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL)
                    lease.sendmsg([struct.pack('=II', operation, index)],
                                  [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                finally:
                    os.close(fd)
            status, caps, fds = self.receive(lease)
            self.assertEqual(status, 0)
            self.assertTrue(caps & (1 << 28))
            self.assertEqual(fds, [])
            send(2, 12, header)
            outputs = [b''] * 4
            while True:
                packet, ancillary, flags, _ = lease.recvmsg(8, socket.CMSG_SPACE(16), socket.MSG_CMSG_CLOEXEC)
                received = []
                for level, kind, data in ancillary:
                    if (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS):
                        received.extend(array.array('i', data))
                for fd in received:
                    stack.callback(os.close, fd)
                self.assertFalse(flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC))
                if reject_manifest and not packet:
                    return None
                self.assertEqual(len(packet), 8)
                operation, index = struct.unpack('=II', packet)
                if operation == 65536:
                    self.assertFalse(received)
                    wanted[index] = int(index not in inline)
                    if ready:
                        ready()
                        ready = None
                    if abort:
                        return bytes(wanted)
                    body = files[index][1]
                    if corrupt:
                        body = bytes([body[0] ^ 1]) + body[1:]
                    send(65537, index, body)
                    if after_upload:
                        after_upload(lease)
                        after_upload = None
                elif operation == 65538:
                    self.assertEqual(len(received), 1)
                    outputs[index] = os.pread(received[0], os.fstat(received[0]).st_size, 0)
                else:
                    self.assertEqual(operation, 65539)
                    if corrupt or reject_manifest:
                        self.assertNotEqual(index, 0)
                        return None if reject_manifest else bytes(wanted)
                    self.assertEqual(index, 0)
                    self.assertEqual(len(received), 1)
                    status, mode, compressed, *lengths = struct.unpack('!11Q', os.pread(received[0], 88, 0))
                    sizes, wire_sizes = lengths[:4], lengths[4:]
                    self.assertEqual(status, 0)
                    self.assertEqual(wire_sizes, list(map(len, outputs)))
                    link = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'distcc-link'))
                    for artifact in range(4):
                        if compressed & (1 << artifact):
                            source, target = io.BytesIO(outputs[artifact]), io.BytesIO()
                            link['copy_lz4'](source.read, target, link['Lz4Chunks'](), sizes[artifact])
                            self.assertEqual(source.tell(), wire_sizes[artifact])
                            outputs[artifact] = target.getvalue()
                    self.assertEqual(sizes, list(map(len, outputs)))
                    break
            self.assertEqual(outputs[:2], [b'', b''])
            self.assertTrue(outputs[2].startswith(b'\x7fELF'))
            if capture is not None:
                capture.extend(outputs)
            if execute:
                obj, binary = self.work / ('snapshot-' + str(threading.get_ident()) + '.o'), self.work / ('snapshot-' + str(threading.get_ident()))
                obj.write_bytes(outputs[2])
                subprocess.run([self.cc, str(obj), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
            return bytes(wanted)

    def test_cached_upload_cancel_corruption_and_concurrency(self):
        self.start_h2()
        files = [(b'/src/source.c', b'int cache_unique(void) { return 937; }\n')]
        self.assertEqual(self.cached_compile(files, abort=True), b'\1')
        self.assertEqual(self.cached_compile(files, corrupt=True), b'\1')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.cached_compile(files), range(4)))
        self.assertCountEqual(results, [b'\1', b'\0', b'\0', b'\0'])
        self.assertEqual(self.cached_compile(files), b'\0')
        self.job('after-cache-cancel')

    def test_duplicate_contents_and_reversed_manifests(self):
        self.start_h2()
        files = [(b'/src/source.c', b'#include "a.h"\n#include "b.h"\nint n = VALUE;\n'),
                 (b'/src/a.h', b'#define VALUE 631\n'),
                 (b'/src/b.h', b'#define VALUE 631\n')]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda i: self.cached_compile(files if i % 2 else files[::-1]), range(4)))
        transferred = 0
        for result in results:
            assert result is not None
            transferred += result.count(b'\1')
        self.assertEqual(transferred, 2)

    def test_abandoned_uploader_releases_waiting_job(self):
        self.start_h2()
        files = [(b'/src/source.c', b'int abandoned_uploader(void) { return 634; }\n')]
        entered, release = threading.Event(), threading.Event()
        def hold_upload():
            entered.set()
            self.assertTrue(release.wait(5))
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            owner = pool.submit(self.cached_compile, files, ready=hold_upload, abort=True)
            try:
                self.assertTrue(entered.wait(5))
                waiter = pool.submit(self.cached_compile, files)
                self.assertFalse(waiter.done())
            finally:
                release.set()
            self.assertEqual(owner.result(timeout=10), b'\1')
            self.assertEqual(waiter.result(timeout=10), b'\1')
        self.assertEqual(self.cached_compile(files), b'\0')

    def test_inline_uncached_input_with_cached_headers(self):
        self.start_h2()
        files = [(b'/src/source.c', b'#include "large.h"\n#include "small.h"\nint n = VALUE;\n'),
                 (b'/src/large.h', b'/*' + b' ' * (5 * 1024 * 1024) + b'*/\n'),
                 (b'/src/small.h', b'#define VALUE 633\n')]
        self.assertEqual(self.cached_compile(files, inline=(1,)), b'\1\0\1')
        self.assertEqual(self.cached_compile(files, inline=(1,)), b'\0\0\0')

    def test_cache_shards_bound_files_and_bytes(self):
        self.start_h2()
        files = [(b'/src/source.c', b'int eviction_unique(void) { return 519; }\n')]
        # Put more than one shard's entry budget in a single batch. No timing
        # assumption about which entry FIFO eviction chooses is necessary.
        index = 0
        while len(files) < 70:
            body = f'/* unused header {index} */\n'.encode()
            if xxhash.xxh3_64(body).digest()[0] % 64 == 0:
                files.append((f'/src/header{index}.h'.encode(), body))
            index += 1
        self.cached_compile(files)
        shard = self.root / f'distcc-cache-{os.geteuid()}' / '00'
        self.assertLessEqual(len([p for p in shard.iterdir() if not p.name.startswith('.')]), 64)
        large = []
        while len(large) < 6:
            body = f'/* large header {index} */\n'.encode() + b' ' * (900 * 1024)
            if xxhash.xxh3_64(body).digest()[0] % 64 == 0:
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

    def test_expanded_input_budget_rejects_before_upload(self):
        self.start_h2()
        self.cached_compile([(b'/src/source.c', b'')], inline=(0,),
                            declared_size=512 * 1024 * 1024, reject_manifest=True)
        self.job('after-resource-rejection')

    def test_active_job_keeps_private_snapshot(self):
        self.start_h2()
        body = b'int main(void) { return 0; }\n'
        files = [(b'/src/source.c', body), (b'/src/barrier.h', b'/* barrier */')]
        self.cached_compile(files, execute=True, inline=(1,))
        key = xxhash.xxh3_64(body).hexdigest()
        entry = self.root / f'distcc-cache-{os.geteuid()}' / f'{int(key[:2], 16) % 64:02x}' / key
        def change_cached_inode():
            entry.chmod(0o600)
            entry.write_bytes(b'int main(void) { return 13; }\n')
        # MISS is emitted only after hits have been materialized. The real
        # compiler fixture sleeps before opening inputs, exposing shared inodes.
        self.assertEqual(self.cached_compile(files, ready=change_cached_inode, execute=True, inline=(1,)), b'\0\0')
        self.assertEqual(self.cached_compile(files, execute=True, inline=(1,)), b'\1\0')

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

    def test_completed_jobs_immediately_reuse_workers(self):
        self.start_h2()
        before = self.accepts()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(self.job, (f'handoff-{i}' for i in range(64))))
        self.assertEqual(len(results), 64)
        self.assertEqual(self.accepts() - before, 1)

    def test_retiring_workers_deliver_queued_results(self):
        clock = self.work / 'retirement.c'
        library = self.work / 'retirement.so'
        gate = self.work / 'retire'
        clock.write_text('''#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <dlfcn.h>
static pid_t parent;
__attribute__((constructor)) static void remember_parent(void) { parent = getpid(); }
void exit(int status) {
    void (*real_exit)(int) = dlsym(RTLD_NEXT, "exit");
    const char *gate = getenv("DISTCC_TEST_CLOCK_GATE");
    /* Leave retiring workers visible after FINISHED while clients submit
     * their next jobs. This must wait for replacement, never return 503. */
    if (getpid() != parent && gate && access(gate, F_OK) == 0) usleep(1000000);
    real_exit(status);
    __builtin_unreachable();
}
time_t time(time_t *out) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const char *gate = getenv("DISTCC_TEST_CLOCK_GATE");
    time_t value = now.tv_sec + (gate && access(gate, F_OK) == 0 ? 61 : 0);
    if (out) *out = value;
    return value;
}
int poll(struct pollfd *fds, nfds_t count, int timeout) {
    int (*real_poll)(struct pollfd *, nfds_t, int) = dlsym(RTLD_NEXT, "poll");
    int result = real_poll(fds, count, timeout);
    const char *gate = getenv("DISTCC_TEST_CLOCK_GATE");
    if (result > 0 && gate && access(gate, F_OK) == 0) usleep(20000);
    return result;
}
''')
        subprocess.run([self.cc, '-shared', '-fPIC', str(clock), '-o', str(library)],
                       check=True, capture_output=True, timeout=15)
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        wrapper.write_text('#!/bin/sh\nexec ' + self.cc + ' "$@"\n')
        try:
            self.restart_server(Path(os.environ.get('DISTCC_RETIRE_TEST_SERVER', self.build / 'distccd')),
                                LD_PRELOAD=str(library), DISTCC_TEST_CLOCK_GATE=str(gate))
            self.start_h2()
            def compile_job(index):
                return self.cached_compile([(b'/src/source.c', f'int retired_{index};'.encode())], inline=(0,))
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(compile_job, range(512)))
                parent = self.daemons[0].pid
                children = Path(f'/proc/{parent}/task/{parent}/children')
                before = set(children.read_text().split())
                gate.touch()
                list(pool.map(compile_job, range(512, 576)))
                self.assertNotEqual(set(children.read_text().split()), before,
                                    'test did not reach worker retirement')
        finally:
            wrapper.write_text(original)
            self.restart_server(self.build / 'distccd')

    def test_stalled_uploads_leave_execution_slots_available(self):
        self.start_h2()
        entered = [threading.Event() for _ in range(4)]
        release = threading.Event()
        def stalled(index):
            def ready():
                entered[index].set()
                self.assertTrue(release.wait(10))
            return self.cached_compile([(b'/src/source.c', f'int stalled_{index};'.encode())],
                                       ready=ready, abort=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(stalled, index) for index in range(4)]
            try:
                for event in entered:
                    self.assertTrue(event.wait(5))
                stats = self.get_stats(self.stats_ports[0])
                self.assertIn('dcc_busy 0\n', stats)
                self.assertIn('dcc_queued 4\n', stats)
                self.job('ready-behind-stalled-inputs')
            finally:
                release.set()
            for future in futures:
                future.result(timeout=10)
        self.job('after-stalled-cancel')

    def test_pipeline_respects_execution_limit(self):
        self.start_h2()
        peak = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(self.job, f'pipeline-{i}') for i in range(16)]
            while any(not future.done() for future in futures):
                stats = self.get_stats(self.stats_ports[0])
                match = re.search(r'dcc_busy (\d+)', stats)
                assert match is not None
                peak = max(peak, int(match[1]))
                self.assertLessEqual(peak, 4)
                time.sleep(.02)
            for future in futures: future.result()
        self.assertGreaterEqual(peak, 2)

    def test_cancel_job_waiting_for_execution(self):
        self.start_h2()
        self.job('negotiate-pipeline')
        reservations = []
        proc = None
        try:
            for _ in range(4):
                net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5)
                reservations.append(net)
                net.sendall(b'SLOT00000001')
                self.assertEqual(net.recv(12), b'GRNT00000001')
            source = self.work / 'waiting.i'
            source.write_text('int waiting;\n')
            proc = subprocess.Popen([str(self.build / 'distcc'), 'gcc', '-c', str(source),
                                     '-o', str(self.work / 'waiting.o')], env=self.env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 5
            while 'dcc_queued 1\n' not in self.get_stats(self.stats_ports[0]):
                self.assertIsNone(proc.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            self.assertIn('dcc_busy 4\n', self.get_stats(self.stats_ports[0]))
            proc.terminate()
            proc.wait(timeout=5)
            while 'dcc_queued 0\n' not in self.get_stats(self.stats_ports[0]):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            for net in reservations: net.close()
        self.job('after-waiter-cancel')

    def test_cancel_stream_preserves_other_jobs(self):
        self.start_h2()
        before = self.accepts()
        self.cached_compile([(b'/src/source.c', b'int cancelled;')],
                            ready=lambda: self.job('uncancelled'), abort=True)
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

    def test_unread_result_descriptors_preserve_other_jobs(self):
        self.start_h2()
        data = random.Random(43).randbytes(2 * 1024 * 1024)
        body = ('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n').encode()
        outputs = []
        def wait(lease):
            self.assertTrue(select.select([lease], [], [], 20)[0])
            self.job('while-result-unread')
        self.cached_compile([(b'/src/source.c', body)], inline=(0,),
                            after_upload=wait, capture=outputs)
        result = self.work / 'slow.o'
        result.write_bytes(outputs[2])
        payload = self.work / 'slow.payload'
        subprocess.run(['objcopy', '--dump-section', '.rodata=' + str(payload), str(result)],
                       check=True, capture_output=True, timeout=10)
        self.assertEqual(payload.read_bytes(), data)

    def test_old_lease_cannot_cancel_reconnected_stream(self):
        self.start_h2()
        old_lease = self.request(6)
        status, caps, fds = self.receive(old_lease)
        self.assertEqual(status, 0)
        self.assertEqual(fds, [])
        self.assertTrue(caps & (1 << 28))
        self.restart_server(self.build / 'distccd')
        time.sleep(5.1)  # The broken connection's peer backoff expires.
        self.job('new-connection')
        old_lease.close()
        self.job('reconnected')

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
