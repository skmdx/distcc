#!/usr/bin/env python3
"""Prepared-input TCP jobs through real broker, daemon and compilers."""
import concurrent.futures
import array
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
import tempfile
import pwd
import time
import unittest
import xxhash

from test_broker import BrokerTestCase
from test_adaptive import free_port


class TcpJobsTest(BrokerTestCase):
    def start_tcp(self):
        return self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_sealed_input_maps_and_sends_raw_tail(self):
        self.start_tcp()
        wrappers = self.work / 'client-bin'
        wrappers.mkdir()
        compiler = wrappers / 'gcc'
        # Real preprocessing and compilation, with an incompressible comment
        # added to CPP stdout to exercise raw sealed extents and a short tail.
        compiler.write_text(f'''#!/usr/bin/python3
import random, subprocess, sys
result = subprocess.run([{self.cc!r}, *sys.argv[1:]])
if result.returncode == 0 and '-E' in sys.argv:
    data = random.Random(718).randbytes(1024 * 1024 + 17).replace(b'\\x00', b'x').replace(b'*', b'x')
    sys.stdout.buffer.write(b'\\n/*' + data + b'*/\\n')
sys.exit(result.returncode)
''')
        compiler.chmod(0o755)
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        (wrappers / (target + '-gcc')).symlink_to('gcc')
        env = dict(self.env, PATH=str(wrappers) + os.pathsep + os.environ['PATH'])
        trace = self.work / 'input.trace'
        actual, expected = self.work / 'raw.o', self.work / 'native.o'
        subprocess.run([self.cc, '-c', str(self.source), '-o', str(expected)], check=True)
        subprocess.run(['strace', '-f', '-yy', '-e', 'trace=pread64,mmap,sendfile',
                        '-o', str(trace), str(self.build / 'distcc'), 'gcc', '-c',
                        str(self.source), '-o', str(actual)], env=env, check=True,
                       capture_output=True, timeout=20)
        self.assertEqual(actual.read_bytes(), expected.read_bytes())
        calls = trace.read_text()
        self.assertTrue(re.search(r'mmap\([^\n]*distcc-preprocessed', calls), 'sealed input was not mapped')
        self.assertTrue(re.search(r'sendfile\([^\n]*distcc-preprocessed', calls), 'raw input did not use sendfile')
        self.assertFalse(re.search(r'pread64\([^\n]*distcc-preprocessed', calls), 'sealed input was copied')

    def test_local_error_result_preserves_object_and_reuses_connection(self):
        prefix = str(self.root / 'local-error')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        source, obj = self.work / 'bad.c', self.work / 'bad.o'
        source.write_text('int = ;\n')
        obj.write_bytes(b'previous object')
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source),
                                 '-o', str(obj)], env=self.env, capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'expected identifier', result.stderr)
        self.assertEqual(obj.read_bytes(), b'previous object')
        self.job('after-local-error')

    def test_local_input_descriptor_and_pool_reuse(self):
        prefix = str(self.root / 'local-jobs')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        before = self.get_stats(self.stats_ports[0])
        for i in range(4):
            self.job(f'local-{i}', 'cc' if i % 2 else 'c')
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        with socket.socket(fileno=descriptors[0]) as connection:
            self.assertEqual(connection.family, socket.AF_UNIX)
            self.assertEqual(connection.getpeername(), f'{prefix}-{self.ports[0]}.sock')
        lease.close()
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(re.search(r'dcc_tcp_accept (\d+)', before)[1],
                         re.search(r'dcc_tcp_accept (\d+)', after)[1])
        self.assertEqual(int(re.search(r'dcc_compile_ok (\d+)', after)[1]) -
                         int(re.search(r'dcc_compile_ok (\d+)', before)[1]), 4)
        self.assertIn('dcc_local_input_jobs 4\n', after)
        self.assertGreater(int(re.search(r'dcc_local_input_bytes (\d+)', after)[1]), 0)

    def test_cache_and_cleanup_leave_event_loop_responsive(self):
        hook, library = self.work / 'file-gate.c', self.work / 'file-gate.so'
        arm, marker, release = (self.work / name for name in ('arm', 'entered', 'release'))
        hook.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static pid_t daemon_pid;
__attribute__((constructor)) static void identify_daemon(void) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (n > 0) {
        path[n] = 0;
        const char *name = strrchr(path, '/');
        if (name && !strcmp(name + 1, "distccd")) daemon_pid = getpid();
    }
}
static void gate(const char *kind) {
    if (getpid() != daemon_pid) return;
    const char *mode = getenv("TEST_FILE_KIND"), *arm = getenv("TEST_FILE_ARM");
    if (!mode || strcmp(mode, kind) || !arm || access(arm, F_OK)) return;
    int fd = open(getenv("TEST_FILE_ENTERED"), O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (fd < 0) return;
    dprintf(fd, "%ld %ld", (long)getpid(), (long)syscall(SYS_gettid)); close(fd);
    for (int i = 0; i < 1000 && access(getenv("TEST_FILE_RELEASE"), F_OK); ++i) usleep(10000);
}
ssize_t read(int fd, void *data, size_t size) {
    ssize_t (*real_read)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    char link[64], path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path)-1);
    if (n > 0) { path[n] = 0; if (strstr(path, "/distcc-tcp-")) gate("cache"); }
    return real_read(fd, data, size);
}
int unlinkat(int fd, const char *path, int flags) {
    int (*real_unlinkat)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    if ((flags & AT_REMOVEDIR) && strstr(path, "/distcc-tcp-")) gate("cleanup");
    return real_unlinkat(fd, path, flags);
}
int ioctl(int fd, unsigned long request, ...) {
    va_list args; va_start(args, request);
    unsigned long argument;
    if (request == FICLONE) { argument = va_arg(args, int); gate("copy"); gate("store"); }
    else argument = (unsigned long)va_arg(args, void *);
    va_end(args);
    return syscall(SYS_ioctl, fd, request, argument);
}
''')
        subprocess.run([self.cc, '-shared', '-fPIC', str(hook), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        for kind in ('cache', 'store', 'cleanup', 'copy'):
            with self.subTest(kind=kind):
                for path in (arm, marker, release): path.unlink(missing_ok=True)
                self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library),
                                    DISTCCD_FILE_CACHE='' if kind == 'copy' else str(self.work / 'cache'),
                                    TEST_FILE_KIND=kind, TEST_FILE_ARM=str(arm),
                                    TEST_FILE_ENTERED=str(marker), TEST_FILE_RELEASE=str(release))
                body = f'int asynchronous_{kind};\n'.encode()
                if kind == 'cache':
                    first = self.wire_job(body)
                    self.assertEqual(self.frame(first), (0x54000201, bytes(4)))
                    self.upload(first, body)
                    self.result(first)
                    first.close()
                arm.touch()
                owner = None
                if kind == 'copy':
                    before = int(re.search(r'dcc_tcp_parallel_tasks (\d+)', self.get_stats(self.stats_ports[0]))[1])
                    owner = self.wire_job(body)
                    self.assertEqual(self.frame(owner), (0x54000201, bytes(4)))
                net = self.wire_job(body, cached=kind != 'cleanup')
                if owner:
                    deadline = time.monotonic() + 5
                    while True:
                        stats = self.get_stats(self.stats_ports[0])
                        if ('dcc_preparing 2\n' in stats and
                            int(re.search(r'dcc_tcp_parallel_tasks (\d+)', stats)[1]) >= before + 2): break
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    self.upload(owner, body)
                if kind == 'store':
                    self.assertEqual(self.frame(net), (0x54000201, bytes(4)))
                if kind in ('store', 'cleanup'): self.upload(net, body)
                try:
                    deadline = time.monotonic() + 5
                    while not marker.exists() or not marker.read_text():
                        self.assertLess(time.monotonic(), deadline, 'file operation was not reached')
                        time.sleep(.01)
                    pid, tid = map(int, marker.read_text().split())
                    self.assertEqual(pid, self.daemons[0].pid)
                    self.assertNotEqual(pid, tid, 'file operation ran in the event loop')
                    if owner: owner.close()
                    self.assertIn('dcc_cpus ', self.get_stats(self.stats_ports[0]))
                    if kind == 'cleanup': self.result(net)
                finally:
                    release.touch()
                if kind != 'cleanup': self.result(net)
                net.close()
                deadline = time.monotonic() + 5
                while list(self.root.glob('distcc-tcp-*')):
                    self.assertLess(time.monotonic(), deadline, 'async cleanup leaked a tree')
                    time.sleep(.01)

    def test_local_input_matches_native_and_rejects_writable_fd(self):
        prefix = str(self.root / 'local-validation')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        source = self.work / 'large.c'
        source.write_text('const char payload[] = "' + 'abcdefgh' * 262144 + '";\n')
        expected, actual = self.work / 'native.o', self.work / 'remote.o'
        subprocess.run([self.cc, '-c', str(source), '-o', str(expected)], check=True, capture_output=True)
        subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source), '-o', str(actual)],
                       env=self.env, check=True, capture_output=True, timeout=15)
        self.assertEqual(actual.read_bytes(), expected.read_bytes())
        # Observe the real client/CPP boundary: preprocessing owns the shared
        # memfd, and local transport must not allocate/copy a second snapshot.
        tracer = shutil.which('strace')
        if tracer:
            trace = self.work / 'local-input.trace'
            subprocess.run([tracer, '-f', '-yy', '-e', 'trace=memfd_create,sendfile,recvmsg,sendmsg',
                            '-o', str(trace), str(self.build / 'distcc'), 'gcc',
                            '-c', str(source), '-o', str(actual)],
                           env=self.env, check=True, capture_output=True, timeout=15)
            calls = trace.read_text()
            self.assertIn('memfd_create("distcc-preprocessed"', calls)
            self.assertNotIn('memfd_create("distcc-local-input"', calls)
            self.assertIn('SCM_RIGHTS', calls)
            self.assertIn('sendfile(', calls)  # Result publication, not an input snapshot.
            self.assertNotRegex(calls, r'sendfile\([^\n]*distcc-preprocessed')
            returned = re.search(r'sendmsg\([^\n]*iov_len=32[^\n]*SCM_RIGHTS', calls)
            self.assertIsNotNone(returned, 'pooled connection was not returned')
            self.assertLess(returned.start(), calls.index('sendfile('), 'copy held the pooled connection')
            self.assertEqual(actual.read_bytes(), expected.read_bytes())
        self.assertGreater(int(re.search(r'dcc_local_input_bytes (\d+)',
                                        self.get_stats(self.stats_ports[0]))[1]), 2 * 1024 * 1024)
        # A writable handle must not be inherited into a compiler. Include a
        # valid manifest so rejection tests FD validation, not framing.
        def string(s): return struct.pack('!Q', len(s)) + s
        args = [b'gcc', b'-c', b'/input.i', b'-o', b'/out.o']
        body = b'int input;\n'
        manifest = struct.pack('!4Q', 7, 0, len(args), 1) + string(b'/')
        manifest += b''.join(map(string, args))
        manifest += struct.pack('!5Q', 0, 0, 0, len(body), len(body)) + bytes(8) + string(b'/input.i') + string(b'')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as net, (self.work/'bad.i').open('w+b') as f:
            f.write(body); f.flush()
            net.settimeout(3); net.connect(f'{prefix}-{self.ports[0]}.sock')
            net.sendmsg([struct.pack('!II', 0x54000200, len(manifest)) + manifest],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [f.fileno()]))])
            self.assertEqual(net.recv(1), b'')
        self.job('after-invalid-local-fd')

    def test_memory_estimate_uses_completed_compiler_rss(self):
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='512', DISTCC_MEMORY_LEARNING='1')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        before = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_job_memory_bytes 536870912\n', before)
        for i in range(8):
            self.job(f'memory-{i}')
        after = self.get_stats(self.stats_ports[0])
        estimate = int(re.search(r'dcc_job_memory_bytes (\d+)', after)[1])
        self.assertGreaterEqual(estimate, 256 * 1024 * 1024)
        self.assertLess(estimate, 512 * 1024 * 1024)
        self.assertIn('dcc_job_memory_samples 8\n', after)

    def test_server_calibrates_unseen_size_despite_cached_client_capacity(self):
        def metric(text, name):
            match = re.search(rf'^{name} (\d+)$', text, re.M)
            assert match is not None, (name, text)
            return int(match[1])
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='512', DISTCC_MEMORY_LEARNING='1')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        for i in range(8):
            self.job(f'small-{i}')
        source = self.work / 'larger.i'
        source.write_text('/*' + ' ' * 8192 + '*/\nint main(void) { return 0; }\n')
        bucket = source.stat().st_size.bit_length() - 1
        def capacity(text):
            row = next(line for line in text.splitlines() if line.startswith('dcc_compile_capacity '))
            return list(map(int, row.split(' ', 1)[1].split(',')))[bucket]
        before = self.get_stats(self.stats_ports[0])
        self.assertEqual(capacity(before), -1)
        completed = metric(before, 'dcc_compile_ok')
        # Expose cached client capacity to force a burst onto the real daemon;
        # the broker's own cold-class gate must not hide missing server leases.
        advertised = re.sub(r'dcc_compile_capacity [^\n]+',
                            'dcc_compile_capacity ' + ','.join(['4'] * 64), before)
        peer = self.peer(advertised.encode())
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'), name='cached-capacity')
        def compile_one(index):
            state = self.work / f'independent-{index}'
            state.mkdir()
            env = dict(self.env, DISTCC_DIR=str(state))
            obj = self.work / f'larger-{index}.o'
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c',
                                     str(source), '-o', str(obj)], env=env,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            return obj.read_bytes()
        rows = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(compile_one, i) for i in range(4)]
            while not all(job.done() for job in jobs):
                stats = self.get_stats(self.stats_ports[0])
                rows.append((metric(stats, 'dcc_compile_ok'), metric(stats, 'dcc_busy')))
                time.sleep(.01)
            outputs = [job.result() for job in jobs]
        cold = [busy for done, busy in rows if done == completed]
        self.assertTrue(cold)
        self.assertEqual(max(cold), 1)
        self.assertGreaterEqual(max(busy for done, busy in rows if done > completed), 2)
        native = self.work / 'native.o'
        subprocess.run([self.cc, '-c', str(source), '-o', str(native)], check=True)
        self.assertTrue(all(output == native.read_bytes() for output in outputs))
        after = self.get_stats(self.stats_ports[0])
        self.assertGreater(capacity(after), 0)

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

    def test_worker_crossing_retirement_deadline_stays_available(self):
        # Advance daemon wall time during compilation, after its retirement
        # decision. Avoid a minute-long, timing-dependent deadline test.
        clock = self.work / 'clock.c'
        library = self.work / 'clock.so'
        marker = self.work / 'advance'
        trigger = self.work / 'trigger'
        worker = self.work / 'worker'
        clock.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
time_t time(time_t *out) {
    time_t (*real_time)(time_t *) = dlsym(RTLD_NEXT, "time");
    time_t value = real_time(NULL);
    const char *marker = getenv("TEST_CLOCK_MARKER");
    if (marker && !access(marker, F_OK)) value += 120;
    if (out) *out = value;
    return value;
}
''')
        subprocess.run([self.cc, '-shared', '-fPIC', str(clock), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        wrapper = self.root / 'bin/gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text(f'#!/bin/sh\necho "$PPID" > "{worker}"\n'
                           f'if test -e "{trigger}"; then touch "{marker}"; fi\n'
                           f'exec "{self.cc}" "$@"\n')
        self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library),
                            TEST_CLOCK_MARKER=str(marker))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        for i in range(50):
            self.job(f'warm-retirement-{i}')
        trigger.touch()
        self.job('cross-retirement')
        pid = int(worker.read_text())
        time.sleep(.2)
        self.assertTrue(Path(f'/proc/{pid}').exists(),
                        'worker exited after reporting itself reusable')
        self.job('announced-retirement')
        self.job('replacement-worker')

    def test_failed_reused_connection_does_not_quarantine_worker(self):
        self.start_tcp()
        self.job('warm-pool')
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        # A peer may close after pool_take's liveness check. The client then
        # reports a failed RPC; only that pooled connection is known bad.
        with socket.socket(fileno=descriptors[0]) as net:
            net.shutdown(socket.SHUT_RDWR)
        lease.sendall(struct.pack('=II', 2, 11))
        self.assertEqual(lease.recv(1), b'')
        lease.close()
        self.job('fresh-connection-after-failure')

    def test_failed_fresh_connection_still_quarantines_worker(self):
        self.start_tcp()
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        for fd in descriptors:
            os.close(fd)
        lease.sendall(struct.pack('=II', 2, 11))
        self.assertEqual(lease.recv(1), b'')
        lease.close()
        retry = self.request(6)
        status, _, descriptors = self.receive(retry)
        self.assertEqual(status, 103)
        self.assertEqual(descriptors, [])

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
        # Hold a decoder in a real worker thread so the next frame must be
        # received while the first one still owns its decompression buffer.
        shim = self.work / 'slow-decode.c'
        shim.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
ssize_t write(int fd, const void *data, size_t size) {
    ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    if (size >= 65536 && syscall(SYS_gettid) != getpid()) usleep(100000);
    return real_write(fd, data, size);
}
''')
        library = self.work / 'slow-decode.so'
        subprocess.run([self.cc, '-shared', '-fPIC', str(shim), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        body = b'/*' + b'bounded expansion ' * 65536 + b'*/\nint parallel_input;\n'
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        streams = [self.wire_job(body, cached=False) for _ in range(6)]
        cancelled = self.wire_job(body, cached=False)
        first = codec.encode(body[:262144])
        self.send(cancelled, 0x54010202, struct.pack('!II', 0, 262144) + first[8:])
        self.send(cancelled, 0x54010202, struct.pack('!II', 0, 262144) + first[8:])
        deadline = time.monotonic() + 3
        while not int(re.search(r'dcc_tcp_input_prefetch_bytes (\d+)',
                               self.get_stats(self.stats_ports[0]))[1]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
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

    def wire_job(self, body, cached=True, paths=(b'/src/source.c',), identity=1, receive_buffer=None,
                 verification: bytes | None = b'DCSI\0\0\0\0', net=None):
        def string(value):
            return struct.pack('!Q', len(value)) + value
        args = [b'gcc', b'-c', paths[0], b'-o', b'/src/result.o']
        count = len(paths) + (verification is not None)
        manifest = struct.pack('!Q3Q', identity, 1, len(args), count) + string(b'/src')
        manifest += b''.join(map(string, args))
        for path in paths:
            manifest += (struct.pack('!5Q', 0, 0, cached, len(body), len(body)) +
                         xxhash.xxh3_64(body).digest() + string(path) + string(b''))
        if verification is not None:
            # These wire fixtures contain no system includes. Production
            # analyzer manifests are exercised by test_preprocess.
            manifest += (struct.pack('!5Q', 0, 0, 0, len(verification), len(verification)) +
                         xxhash.xxh3_64(verification).digest() +
                         string(b'/.distcc-system/check') + string(b''))
        if net is None:
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5)
        net.settimeout(5)
        if receive_buffer:
            net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.addCleanup(net.close)
        self.send(net, 0x54000200, manifest)
        if verification is not None:
            self.upload(net, verification, len(paths))
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

    def result(self, net, identity=1, expected_status=0):
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        kind, description = self.frame(net)
        self.assertEqual(kind, 0x54000203)
        self.assertEqual(len(description), 64)
        status, mode, *sizes = struct.unpack('!II4Q', description[:40])
        prepare, queued, execute = struct.unpack('!3Q', description[40:])
        self.assertGreater(execute, 0)
        self.assertLess(prepare + queued + execute, 60 * 1000000000)
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
        self.assertEqual(status, expected_status, outputs[0].decode(errors='replace'))
        if not status:
            self.assertTrue(outputs[2].startswith(b'\x7fELF'))
        return outputs

    def test_missing_or_invalid_system_include_manifest_is_rejected(self):
        body = b'int unchecked_headers;\n'
        for verification in (None, b'bad format', b'DCSI\0\0\0\1',
                             b'DCSI' + struct.pack('!I', 4097),
                             b'DCSI' + struct.pack('!III', 1, 0, 1) + b'x',
                             b'DCSI' + struct.pack('!I', 0) + b'extra'):
            with self.subTest(verification=verification):
                net = self.wire_job(body, cached=False, verification=verification)
                self.upload(net, body)
                # Intact jobs with invalid metadata return a completed protocol
                # error. EOF would incorrectly quarantine a healthy worker.
                outputs = self.result(net, expected_status=109 << 8)
                self.assertEqual(outputs[2], b'')

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
        net = self.wire_job(b'int invalid;', paths=(b'/a', b'/a.b', b'/a/b'), verification=None)
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
            except (FileNotFoundError, ProcessLookupError):
                break
            if state == 'Z':
                break
            self.assertLess(time.monotonic(), deadline, 'compiler outlived its worker')
            time.sleep(.01)
        while list(self.root.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline, 'input cleanup did not finish')
            time.sleep(.01)
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
                self.assertEqual(struct.unpack('!II4Q', description[:40])[4], 4 * 1024 * 1024)
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

    @unittest.skipUnless(shutil.which('objcopy'), 'objcopy is required')
    def test_hinted_broker_leases_release_cpu_during_real_result_transfer(self):
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        noise = self.work / 'noise'
        noise.write_bytes(random.Random(543).randbytes(6 * 1024 * 1024))
        wrapper.write_text(f'''#!/usr/bin/python3
import subprocess, sys
args = sys.argv[1:]
subprocess.run([{self.cc!r}, *args], check=True)
subprocess.run(['objcopy', '--add-section', {'noise=' + str(noise)!r}, args[args.index('-o') + 1]], check=True)
''')
        self.start_tcp()
        held = []
        try:
            for index in range(4):
                lease, net = self.acquire_reserved(10, (990 + index, 4 * 1024 * 1024, 1))
                body = f'int answer_{index}(void) {{ return {index}; }}\n'.encode()
                self.wire_job(body, cached=False, receive_buffer=65536, net=net)
                self.upload(net, body)
                held.append((lease, net, body))
            deadline = time.monotonic() + 10
            while 'dcc_returning 4\n' not in self.get_stats(self.stats_ports[0]):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            wrapper.write_text(original)
            # Include a fresh broker snapshot. All four leases still exist,
            # but the actual worker has completed their GCC compilations.
            time.sleep(1.1)
            self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))
            started = time.monotonic()
            self.job('while-hinted-results-stalled')
            self.assertLess(time.monotonic() - started, 5)
            for index, (lease, net, body) in enumerate(held):
                net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
                outputs = self.result(net)
                actual, expected = self.work / 'returned.o', self.work / 'native.o'
                source = self.work / 'source.c'
                actual.write_bytes(outputs[2])
                subprocess.run(['objcopy', '--remove-section', 'noise', str(actual)], check=True)
                source.write_bytes(body)
                subprocess.run([self.cc, '-c', str(source), '-o', str(expected)], check=True)
                self.assertEqual(actual.read_bytes(), expected.read_bytes())
                lease.close()
        finally:
            for lease, net, _ in held:
                lease.close()
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
        before = int(re.search(r'dcc_tcp_output_overlaps (\d+)',
                               self.get_stats(self.stats_ports[0]))[1])
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
        after = int(re.search(r'dcc_tcp_output_overlaps (\d+)',
                              self.get_stats(self.stats_ports[0]))[1])
        self.assertGreater(after, before)
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
                    response = struct.pack('!IIII7Q', 0x54000203, 64, 0, 0, 0, 0, 4, 0, 0, 0, 0)
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
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1',
                        DISTCC_RECONNECT_TIMEOUT='0')
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


@unittest.skipUnless(os.geteuid() == 0, 'cross-UID integration requires root')
class LocalCrossUidTest(unittest.TestCase):
    def test_private_input_can_be_compiled_by_another_uid(self):
        build = Path.cwd()
        account = pwd.getpwnam('nobody')
        with tempfile.TemporaryDirectory(prefix='distcc-cross-uid-') as directory:
            root = Path(directory)
            os.chown(root, account.pw_uid, account.pw_gid)
            client = root / 'client'
            client.mkdir(mode=0o700)
            source = client / 'private.c'
            source.write_text('const char *name = __FILE__; int private_input;\n')
            source.chmod(0o600)
            port, stats = free_port(), free_port()
            env = {k:v for k,v in os.environ.items() if not k.startswith('DISTCC')}
            env.update(DISTCC_JOB_MEMORY='64', DISTCC_LOCAL_SOCKET=str(root/'jobs'),
                       DISTCC_HOSTS=f'127.0.0.1:{port}/1,tcp,stats={stats}',
                       DISTCC_DIR=str(client), DISTCC_BROKER=str(client/'broker.sock'),
                       DISTCC_FALLBACK='0', DISTCC_SKIP_LOCAL_RETRY='1', TMPDIR=str(client))
            server_env = dict(env, TMPDIR=str(root), DISTCCD_PATH=os.path.dirname(shutil.which('gcc'))+os.pathsep+os.defpath)
            processes = []
            with (root/'server.log').open('w+') as log:
                try:
                    processes.append(subprocess.Popen([str(build/'distccd'), '--daemon', '--no-detach',
                        '--enable-tcp-insecure', '--listen', '127.0.0.1', '--allow', '127.0.0.1',
                        '--port', str(port), '--stats', '--stats-port', str(stats), '--jobs', '1',
                        '--log-stderr'], env=server_env, user=account.pw_uid, group=account.pw_gid,
                        extra_groups=[], start_new_session=True, stdout=log, stderr=log))
                    deadline = time.monotonic()+5
                    while True:
                        try:
                            BrokerTestCase.get_stats(stats)
                            break
                        except OSError:
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.02)
                    processes.append(subprocess.Popen([str(build/'distccd'), '--broker', env['DISTCC_BROKER']],
                        env=env, start_new_session=True, stdout=log, stderr=log))
                    while not Path(env['DISTCC_BROKER']).exists():
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    result = subprocess.run([str(build/'distcc'), 'gcc', '-c', str(source), '-o', str(client/'remote.o')],
                        env=env, capture_output=True, timeout=10)
                    log.flush();log.seek(0)
                    self.assertEqual(result.returncode, 0, result.stderr.decode()+log.read())
                    subprocess.run(['gcc', '-c', str(source), '-o', str(client/'native.o')], check=True, capture_output=True)
                    self.assertEqual((client/'remote.o').read_bytes(), (client/'native.o').read_bytes())
                    self.assertIn('dcc_local_input_jobs 1\n', BrokerTestCase.get_stats(stats))
                    self.assertEqual(source.stat().st_mode & 0o777, 0o600)
                finally:
                    for process in reversed(processes):
                        if process.poll() is None:os.killpg(process.pid, signal.SIGTERM)
                        process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
