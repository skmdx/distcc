#!/usr/bin/env python3
"""Remote preprocessing with mutable inputs, real compilers and pooled TCP."""
import os
import concurrent.futures
import fcntl
import xxhash
import random
from pathlib import Path
import re
import select
import shlex
import shutil
import socket
import signal
import struct
import subprocess
import sys
import textwrap
import threading
import time
import unittest

from test_broker import BrokerTestCase


class PumpTestCase(BrokerTestCase):
    compression = 'lzo'
    transport = ''
    cpp_slots = 1

    @classmethod
    def setUpClass(cls):
        cls.daemon_env = {}
        module = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if module:
            cls.daemon_env['DISTCC_GCC_SCAN_LIBRARY'] = module
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',' + self.compression + self.transport) +
                          f' --localslots_cpp={self.cpp_slots}', idle=60)
        self.env.update(TMPDIR=str(self.root), DISTCC_CLIENT_TMP=str(self.work),
                        INCLUDE_SERVER_ARGS=f'--workers={self.cpp_slots}',
                        DISTCC_CPP_LOOKUP_LIBRARY=str(self.build / 'cpp_lookup.so'),
                        DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_TESTING_INCLUDE_SERVER='1',
                        DISTCC_VERBOSE='1', DISTCC_LOG=str(self.work / 'client.log'))
        # The isolated broker fixture drops production DISTCC settings. This
        # path selects a build artifact and is required before pump starts.
        module = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if module:
            self.assertTrue(Path(module).is_file(), 'Build the GCC dependency module first')
            self.env['DISTCC_GCC_SCAN_LIBRARY'] = module
        result = subprocess.run([str(self.build / 'pump'), '--startup'],
                                cwd=self.work, env=self.env, capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        for line in result.stdout.splitlines():
            words = shlex.split(line)
            if len(words) == 2 and words[0] == 'export':
                key, value = words[1].split('=', 1)
                self.env[key] = value
        self.assertIn('INCLUDE_SERVER_PORT', self.env)
        self.addCleanup(self.shutdown_pump)

    def shutdown_pump(self):
        subprocess.run([str(self.build / 'pump'), '--shutdown'], env=self.env,
                       cwd=self.work, capture_output=True, timeout=10, check=True)

    def run_cc(self, compiler, args, remote=True):
        prefix = [str(self.build / 'distcc')] if remote else []
        return subprocess.run(prefix + [compiler] + list(map(str, args)),
                              cwd=self.work, env=self.env, capture_output=True,
                              timeout=15)

    def analyzer_pids(self):
        pid = int(self.env['INCLUDE_SERVER_PID'])
        return set(map(int, Path(f'/proc/{pid}/task/{pid}/children').read_text().split()))


class AnalyzerPoolTest(PumpTestCase):
    cpp_slots = 2
    compression = 'lz4'
    transport = ',tcp'

    def test_broker_and_worker_classify_the_same_pump_input_tree(self):
        source = self.work / 'tiny.c'
        source.write_text('#include "large.h"\nint value = ANSWER;\n')
        (self.work / 'large.h').write_text('/*' + ' ' * (2 * 1024 * 1024) + '*/\n#define ANSWER 42\n')
        saved = self.env.copy()
        self.base_env['DISTCC_VERBOSE'] = '1'
        self.addCleanup(self.base_env.pop, 'DISTCC_VERBOSE', None)
        self.start_broker(self.host(0, f',lz4,tcp,stats={self.stats_ports[0]}'), name='tree-capacity')
        saved.update(DISTCC_BROKER=self.env['DISTCC_BROKER'], DISTCC_HOSTS=self.env['DISTCC_HOSTS'])
        self.env = saved
        remote = self.run_cc('gcc', ['-c', source, '-o', 'tree.o'])
        self.assertEqual(remote.returncode, 0, remote.stderr.decode())
        output = (self.work / 'tree.o').read_bytes()
        local = self.run_cc('gcc', ['-c', source, '-o', 'tree.o'], remote=False)
        self.assertEqual(local.returncode, 0, local.stderr.decode())
        self.assertEqual(output, (self.work / 'tree.o').read_bytes())
        feedback = (self.work / 'tree-capacity.log').read_text()
        match = re.search(r'job cost:.*kind=4 bytes=(\d+) source=(\d+)', feedback)
        self.assertIsNotNone(match, feedback)
        size, raw = map(int, match.groups())
        self.assertGreater(size, 2 * 1024 * 1024)
        self.assertEqual(raw, source.stat().st_size)
        stats = self.get_stats(self.stats_ports[0])
        vector = re.search(r'dcc_pump_capacity ([^\n]+)', stats).group(1)
        self.assertGreater(int(vector.split(',')[size.bit_length() - 1]), 0)

    def test_module_scan_and_compile_are_local_without_analysis_fallback(self):
        source = self.work / 'module.cc'
        source.write_text('export module example;\nexport int answer() { return 42; }\n')
        commands = [(['-std=c++20', '-fmodules-ts', '-E', source, '-o', 'module.i',
                      '-MD', '-MF', 'module.d',
                      '-fdeps-file=module.ddi', '-fdeps-format=p1689r5',
                      '-fdeps-target=module.o'], ['module.i', 'module.ddi']),
                    (['-std=c++20', '-fmodules-ts', '-c', source, '-o', 'module.o'],
                     ['module.o', 'gcm.cache/example.gcm'])]
        log = self.work / 'client.log'
        log.write_text('')
        for args, outputs in commands:
            local = self.run_cc('g++', args, remote=False)
            self.assertEqual(local.returncode, 0, local.stderr.decode())
            expected = {name: (self.work / name).read_bytes() for name in outputs}
            for name in outputs:
                (self.work / name).unlink()
            wrapped = self.run_cc('g++', args)
            self.assertEqual(wrapped.returncode, 0, wrapped.stderr.decode())
            self.assertEqual(expected, {name: (self.work / name).read_bytes() for name in outputs})
        self.assertNotIn('send CDIR', log.read_text())
        self.assertNotIn('gave up analyzing', log.read_text())

    def test_shutdown_finishes_before_supervisor_is_reaped(self):
        # A service supervisor may reap adopted daemons only after its shutdown
        # command returns. Exercise that lifecycle with a real include server.
        script = textwrap.dedent('''\
            import ctypes, os, shlex, signal, subprocess, sys
            assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('INCLUDE_SERVER_')}
            env['INCLUDE_SERVER_ARGS'] = '--workers=1'
            startup = subprocess.run([sys.argv[1], '--startup'], env=env,
                                     capture_output=True, text=True, timeout=15, check=True)
            for line in startup.stdout.splitlines():
                words = shlex.split(line)
                if len(words) == 2 and words[0] == 'export':
                    key, value = words[1].split('=', 1)
                    env[key] = value
            pid = int(env['INCLUDE_SERVER_PID'])
            try:
                subprocess.run([sys.argv[1], '--shutdown'], env=env,
                               capture_output=True, timeout=5, check=True)
                # waitid with WNOWAIT proves the dead supervisor is still ours
                # to reap; shutdown did not depend on this parent doing so.
                status = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
                assert status is not None and status.si_pid == pid
                assert not os.path.exists(env['INCLUDE_SERVER_PORT'])
            finally:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)
            ''')
        result = subprocess.run([sys.executable, '-c', script, str(self.build / 'pump')],
                                cwd=self.work, env=self.env, capture_output=True,
                                text=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_cpu_capacity_obeys_affinity(self):
        allowed = os.sched_getaffinity(0)
        result = subprocess.check_output(
            [str(self.build / 'distcc'), '--show-analyzer-slots'], env=self.env,
            preexec_fn=lambda: os.sched_setaffinity(0, {min(allowed)}), text=True)
        self.assertEqual(int(result), 1)

    def test_measured_memory_controls_analysis_admission(self):
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'measured.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        path = Path(self.env['INCLUDE_SERVER_PORT'] + '.memory')
        peak, = struct.unpack('=Q', path.read_bytes())
        self.assertGreater(peak, 0)
        def publish(value):
            with path.open('r+b', buffering=0) as stream:
                fcntl.lockf(stream, fcntl.LOCK_EX)
                stream.write(struct.pack('=Q', value))
        # A measured demand larger than the test cgroup must close admission;
        # no allocation or memory-limit violation is needed to test the gate.
        publish(1 << 40)
        log = self.work / 'client.log'
        log.write_text('')
        client = subprocess.Popen([str(self.build / 'distcc'), 'gcc', '-E', str(self.source)],
                                  cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                client.wait(timeout=.2)
            self.assertNotIn('send CDIR', log.read_text())
            publish(peak)
            stdout, stderr = client.communicate(timeout=15)
            self.assertEqual(client.returncode, 0, stderr.decode())
            self.assertTrue(stdout)
            self.assertIn('send CDIR', log.read_text())
            self.assertGreaterEqual(struct.unpack('=Q', path.read_bytes())[0], peak)
        finally:
            if client.poll() is None:
                client.kill()
                client.communicate()

    def test_parallel_admission_and_context_isolation(self):
        warm = self.run_cc('gcc', ['-E', self.source])
        self.assertEqual(warm.returncode, 0, warm.stderr.decode())
        (self.work / 'client.log').write_text('')
        workers = self.analyzer_pids()
        self.assertEqual(len(workers), self.cpp_slots)
        for pid in workers:
            os.kill(pid, signal.SIGSTOP)
        clients = []
        try:
            for i in range(2):
                directory = self.work / f'context-{i}'
                directory.mkdir()
                (directory / 'value.h').write_text(f'#define VALUE {i + 17}\n')
                (directory / 'input.c').write_text('#include "value.h"\nint value = VALUE + OFFSET;\n')
                clients.append(subprocess.Popen(
                    [str(self.build / 'distcc'), 'gcc', '-E', '-P', '-DOFFSET=3', 'input.c'],
                    cwd=directory, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
            log = self.work / 'client.log'
            deadline = time.monotonic() + 5
            while not log.exists() or log.read_text().count('send CDIR') < 2:
                self.assertTrue(all(client.poll() is None for client in clients))
                self.assertLess(time.monotonic(), deadline, 'parallel requests not admitted')
                time.sleep(.01)
            lease = self.request(12)
            self.assertFalse(select.select([lease], [], [], .1)[0])
            lease.close()
        finally:
            for pid in workers:
                os.kill(pid, signal.SIGCONT)
            for i, client in enumerate(clients):
                try:
                    stdout, stderr = client.communicate(timeout=15)
                    self.assertEqual(client.returncode, 0, stderr.decode())
                    self.assertIn(f'int value = {i + 17} + 3;'.encode(), stdout)
                finally:
                    if client.poll() is None:
                        client.kill()
                        client.communicate()
        # Exercise both native backends with different cwd/header namespaces,
        # generated headers, definitions and output actions across warm workers.
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            def compare(index):
                compiler = 'gcc' if index % 2 else 'clang'
                source = self.work / f'context-{index % 2}' / 'input.c'
                args = ['-E', '-P', f'-DOFFSET={index}', source]
                remote = self.run_cc(compiler, args)
                local = self.run_cc(compiler, args, False)
                self.assertEqual(remote.returncode, 0, remote.stderr.decode())
                self.assertEqual((remote.stdout, remote.stderr), (local.stdout, local.stderr))
            for generation in range(2):
                for i in range(2):
                    (self.work / f'context-{i}' / 'value.h').write_text(
                        f'#define VALUE {generation * 11 + i}\n')
                list(pool.map(compare, range(8)))
        self.assertEqual(self.analyzer_pids(), workers)
        self.assertNotIn('preprocessing locally', log.read_text())

    def test_recovery_preserves_published_inputs_and_shutdown_reaps_workers(self):
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'first.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        supervisor = int(self.env['INCLUDE_SERVER_PID'])
        roots = list(self.work.glob(f'*.include_server-{supervisor}-*'))
        snapshots = {p: p.read_bytes() for root in roots for p in root.rglob('*') if p.is_file()}
        self.assertTrue(snapshots, 'published inputs not reached')
        workers = self.analyzer_pids()
        victim = next(iter(workers))
        os.kill(victim, signal.SIGKILL)
        deadline = time.monotonic() + 5
        while True:
            replacements = self.analyzer_pids()
            if len(replacements) == 2 and victim not in replacements:
                break
            self.assertLess(time.monotonic(), deadline, 'analyzer was not replaced')
            time.sleep(.01)
        self.assertTrue(all(p.read_bytes() == content for p, content in snapshots.items()))
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'recovered.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual((self.work / 'first.o').read_bytes(), (self.work / 'recovered.o').read_bytes())
        for pid in replacements:
            os.kill(pid, signal.SIGSTOP)
        self.shutdown_pump()
        self.assertFalse(any(Path(f'/proc/{pid}').exists() for pid in replacements))
        self.assertFalse(list(self.work.glob(f'*.include_server-{supervisor}-*')))
        self.assertFalse(Path(self.env['INCLUDE_SERVER_PORT']).exists())


class PreprocessTest(PumpTestCase):
    def test_unused_include_directories_exist_on_worker(self):
        # Build generators pass directories for whole dependency sets, even
        # when this translation unit does not read any of their headers.
        empty = self.work / 'unused headers'
        empty.mkdir()
        (empty / 'child').mkdir()
        (self.work / 'include-link').symlink_to(empty, target_is_directory=True)
        options = ['-I', str(empty), '-Iinclude-link/child/..',
                   '-iquote', str(empty / 'child'), '-isystem', 'include-link',
                   '-idirafter', str(empty / 'child')]
        for compiler in ('gcc', 'g++', 'clang', 'clang++'):
            if not shutil.which(compiler):
                continue
            with self.subTest(compiler=compiler):
                args = ['-x', 'c++' if '++' in compiler else 'c',
                        '-Werror=missing-include-dirs', *options,
                        '-c', self.source, '-o', 'directories.o']
                local = self.run_cc(compiler, args, False)
                self.assertEqual(local.returncode, 0, local.stderr.decode())
                expected = (self.work / 'directories.o').read_bytes()
                (self.work / 'directories.o').unlink()
                remote = self.run_cc(compiler, args)
                self.assertEqual(remote.returncode, 0, remote.stderr.decode())
                self.assertEqual(remote.stderr, b'')
                self.assertEqual((self.work / 'directories.o').read_bytes(), expected)
                self.assertNotIn('preprocessing locally',
                                 (self.work / 'client.log').read_text())

    def test_unused_include_directory_changes_are_not_cached(self):
        directory = self.work / 'generated-headers'
        # Reuse one analyzer and command through absence, creation and removal.
        # A real missing path must still produce the compiler's diagnostic.
        for exists in (False, True, False, True):
            if exists:
                directory.mkdir()
            elif directory.exists():
                directory.rmdir()
            args = ['-Wmissing-include-dirs', '-I', str(directory),
                    '-c', self.source, '-o', 'generated.o']
            local = self.run_cc('gcc', args, False)
            remote = self.run_cc('gcc', args)
            self.assertEqual(remote.returncode, local.returncode, remote.stderr.decode())
            self.assertEqual(b'-Wmissing-include-dirs' in remote.stderr, not exists)
            self.assertEqual(b'-Wmissing-include-dirs' in local.stderr, not exists)
            self.assertNotIn('preprocessing locally',
                             (self.work / 'client.log').read_text())

    def test_analysis_holds_cpp_capacity_without_reserving_remote_worker(self):
        workers = self.analyzer_pids()
        self.assertEqual(len(workers), 1)
        for pid in workers:
            os.kill(pid, signal.SIGSTOP)
        client = subprocess.Popen([str(self.build / 'distcc'), 'gcc', '-c',
                                   str(self.source), '-o', 'waiting.o'],
                                  cwd=self.work, env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        leases = []
        try:
            deadline = time.monotonic() + 5
            log = self.work / 'client.log'
            while not log.exists() or 'send CDIR' not in log.read_text():
                self.assertIsNone(client.poll())
                self.assertLess(time.monotonic(), deadline, 'analysis not admitted')
                time.sleep(.01)
            # Input analysis owns one CPP lease and no remote worker lease.
            other = self.request(12)
            self.assertFalse(select.select([other], [], [], .1)[0])
            other.close()
            for _ in range(4):
                lease = self.request(9)
                leases.append(lease)
                status, _, descriptors = self.receive(lease)
                for fd in descriptors:
                    os.close(fd)
                self.assertEqual(status, 0)
        finally:
            for lease in leases:
                lease.close()
            for pid in workers:
                os.kill(pid, signal.SIGCONT)
            try:
                stdout, stderr = client.communicate(timeout=15)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.communicate()
        self.assertEqual(client.returncode, 0, stderr.decode())
        self.assertTrue((self.work / 'waiting.o').exists())
        self.assertNotIn('preprocessing locally', log.read_text())
        lease = self.request(12)
        self.assertEqual(self.receive(lease), (0, 1, []))
        lease.close()

    def test_queued_analysis_preserves_macro_diagnostics(self):
        self.source.write_text('#define PRESENT(p) ((p) != 0)\n'
                               'int present(void) { int values[1]; return PRESENT(values); }\n')
        args = ['-Werror=address', '-c', self.source, '-o', 'macro.o', '-MMD', '-MF', 'macro.d']
        local = self.run_cc('gcc', args, remote=False)
        self.assertEqual(local.returncode, 0, local.stderr.decode())
        expected = (self.work / 'macro.o').read_bytes()
        expected_deps = (self.work / 'macro.d').read_bytes()
        capacity = int(subprocess.check_output([str(self.build / 'distcc'), '--show-analyzer-slots'],
                                               env=self.env, text=True))
        leases = []
        for index in range(capacity):
            lease = self.request(13)
            leases.append(lease)
            self.assertEqual(self.receive(lease), (0, index + 1, []))
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(self.run_cc, 'gcc', ['-Werror=address', '-c', self.source,
                                                    '-o', f'macro-{i}.o'])
                    for i in range(2)]
            try:
                self.assertFalse(concurrent.futures.wait(jobs, timeout=.1).done)
            finally:
                for lease in leases:
                    lease.close()
            for i, job in enumerate(jobs):
                result = job.result(timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual((self.work / f'macro-{i}.o').read_bytes(), expected)
                self.assertEqual(result.stderr, local.stderr)
        result = self.run_cc('gcc', args)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual((self.work / 'macro.d').read_bytes(), expected_deps)
        self.assertNotIn('preprocessing locally', (self.work / 'client.log').read_text())


    def test_failed_scanner_start_reports_error_and_cleans_up(self):
        env = dict(self.env, INCLUDE_SERVER_ARGS='--unknown-test-option')
        for key in ('INCLUDE_SERVER_PID', 'INCLUDE_SERVER_DIR', 'INCLUDE_SERVER_PORT'):
            env.pop(key, None)
        before = set(self.root.glob('distcc-pump*'))
        failed = subprocess.run([str(self.build / 'pump'), '--startup'],
                                cwd=self.work, env=env, capture_output=True,
                                text=True, timeout=15)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('Usage:', failed.stdout)
        self.assertEqual(set(self.root.glob('distcc-pump*')), before)

    def test_local_lookup_cache_then_remote_compile(self):
        search = []
        directories = [self.root / ('lookup-' + str(i)) for i in range(300)]
        for directory in directories:
            directory.mkdir()
            search.append('-I' + str(directory))
        (directories[-1] / 'answer.h').write_text('#define ANSWER 42\n')
        self.source.write_text('#include "answer.h"\nint answer(void) { return ANSWER; }\n')
        scanner = self.env.pop('INCLUDE_SERVER_PORT')
        try:
            result = self.run_cc('gcc', [*search, '-c', self.source, '-o', 'lookup.o'])
        finally:
            self.env['INCLUDE_SERVER_PORT'] = scanner
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertTrue((self.work / 'lookup.o').exists())
        self.assertNotIn('getline failed', (self.work / 'client.log').read_text())
        self.assertRegex(self.get_stats(self.stats_ports[0]), r'dcc_compile_ok [1-9]')

    def test_compiler_probe_bypasses_local_cpp_admission(self):
        masquerade = self.work / 'masquerade'
        masquerade.mkdir()
        (masquerade / 'gcc').symlink_to(self.build / 'distcc')
        module = next((self.build / '_include_server').glob('lib*/include_server'))
        env = dict(self.env, PATH=str(masquerade) + os.pathsep + self.env['PATH'],
                   PYTHONPATH=str(module))
        env.pop('INCLUDE_SERVER_PORT')
        lease = self.request(12)
        self.assertEqual(self.receive(lease), (0, 1, []))
        script = ('from compiler_analyzer import run; import sys; '
                  'result = run(sys.argv[1:]); sys.stdout.buffer.write(result.stdout)')
        probe = subprocess.Popen([sys.executable, '-c', script, 'gcc', '-E', '-P', self.source],
                                 cwd=self.work, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
        try:
            stdout, stderr = probe.communicate(timeout=3)
            self.assertEqual(probe.returncode, 0, stderr.decode())
            expected = self.run_cc('gcc', ['-E', '-P', self.source], False)
            self.assertEqual(stdout, expected.stdout)
        finally:
            if probe.poll() is None:
                os.killpg(probe.pid, signal.SIGTERM)
                probe.wait(timeout=5)
            lease.close()

    def test_remote_cpp_difference_still_disables_pump(self):
        self.source.write_text('#ifdef REMOTE_COMPILER_FAULT\n#error wrong worker setup\n#endif\nint value;\n')
        wrapper = self.root / 'bin/gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(self.cc) +
                           ' -DREMOTE_COMPILER_FAULT=1 "$@"\n')
        self.env.pop('DISTCC_SKIP_LOCAL_RETRY')
        self.env['DISTCC_FALLBACK'] = '1'
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'recovered.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        counter = Path(self.env['INCLUDE_SERVER_PORT']).parent / 'discrepancy_counter'
        self.assertTrue(counter.exists())
        self.assertIn('send CDIR', (self.work / 'client.log').read_text())

    def client_system_headers(self, compiler='gcc'):
        """Change the worker's filesystem after the immutable scan was sent."""
        sysroot = self.work / 'sysroot'
        includes = sysroot / 'usr/include'
        includes.mkdir(parents=True)
        header = includes / 'value.h'
        header.write_text('#define VALUE 731\n')
        (includes / 'alias.h').symlink_to('value.h')
        self.source.write_text('#include <alias.h>\n'
                               '#if __has_include(<worker-only.h>)\n#error wrong filesystem\n#endif\n'
                               'int value = VALUE;\n')
        args = ['--sysroot=' + str(sysroot), '-c', self.source,
                '-o', 'system.o', '-MD', '-MF', 'system.d']
        local = self.run_cc(compiler, args, remote=False)
        self.assertEqual(local.returncode, 0, local.stderr.decode())
        expected = (self.work / 'system.o').read_bytes()
        dependencies = (self.work / 'system.d').read_bytes()
        (self.work / 'system.o').unlink()
        (self.work / 'system.d').unlink()
        native = shutil.which(compiler)
        assert native is not None
        wrappers = self.work / 'worker-bin'
        wrappers.mkdir()
        target = subprocess.check_output([native, '-dumpmachine'], text=True).strip()
        name = compiler if compiler.startswith('clang') else target + '-gcc'
        wrapper = wrappers / name
        wrapper.write_text('#!/bin/sh\n' +
            "printf '%s\\n' '#error worker header must not be used' > " + shlex.quote(str(header)) + '\n' +
            ': > ' + shlex.quote(str(includes / 'worker-only.h')) + '\n' +
            'exec ' + shlex.quote(native) + ' "$@"\n')
        wrapper.chmod(0o755)
        settings = {'DISTCCD_PATH': str(wrappers) + os.pathsep + os.defpath}
        if 'DISTCC_LOCAL_SOCKET' in self.env:
            settings['DISTCC_LOCAL_SOCKET'] = self.env['DISTCC_LOCAL_SOCKET']
        self.restart_server(self.build / 'distccd', **settings)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        remote = self.run_cc(compiler, args)
        self.assertEqual(remote.returncode, 0, remote.stderr.decode())
        self.assertIn('#error worker header', header.read_text())
        self.assertEqual((self.work / 'system.o').read_bytes(), expected)
        self.assertEqual((self.work / 'system.d').read_bytes(), dependencies)
        self.assertNotIn('preprocessing locally', (self.work / 'client.log').read_text())

    def test_gcc_uses_client_system_header_snapshots(self):
        self.client_system_headers()

    def test_clang_uses_client_system_header_snapshots(self):
        self.client_system_headers('clang')

    def test_system_header_snapshots_without_broker(self):
        if self.transport:
            self.skipTest('Prepared TCP requires the broker')
        self.env.pop('DISTCC_BROKER')
        self.env['DISTCC_HOSTS'] = self.host(0, ',' + self.compression + ',cpp')
        self.client_system_headers()

    def test_system_include_search_order(self):
        first, last = self.work / 'first', self.work / 'last'
        first.mkdir()
        last.mkdir()
        (first / 'value.h').write_text('#define FIRST 1\n#include_next <value.h>\n')
        (last / 'value.h').write_text('#define LAST 2\n')
        self.source.write_text('#include <value.h>\nint value = FIRST + LAST;\n')
        for compiler in ('gcc', 'clang'):
            self.compare(compiler, ['-nostdinc', '-isystem', str(first),
                                    '-idirafter', str(last), '-P'])

    def test_invalid_system_include_manifest_is_rejected(self):
        self.compare(args=['-P'])
        manifest, = [p for p in self.work.rglob('.distcc-system/*')
                     if not p.name.endswith('.xxh3')]
        module = next((self.build / '_include_server').glob('lib*/include_server'))
        sys.path.insert(0, str(module))
        try:
            import compress_files
        finally:
            sys.path.pop(0)
        import io
        payload = b'DCSI' + struct.pack('!II', 1, 4096)
        compress_files.WriteCompressed(str(manifest), io.BytesIO(payload), manifest.suffix[1:])
        started = time.monotonic()
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'invalid.o'])
        self.assertNotEqual(result.returncode, 0)
        self.assertLess(time.monotonic() - started, 3)
        self.assertIn(b'invalid system include manifest', result.stderr)
        self.assertFalse((self.work / 'invalid.o').exists())
        lease = self.request(9)
        status, _, descriptors = self.receive(lease)
        for fd in descriptors:
            os.close(fd)
        self.assertEqual(status, 0)
        lease.close()

    def test_scan_includes_omits_header_validation_metadata(self):
        self.source.write_text('#include <stddef.h>\nint value;\n')
        env = dict(self.env, DISTCC_HOSTS=self.host(0, ',cpp,' + self.compression))
        env.pop('DISTCC_BROKER')
        result = subprocess.run([str(self.build / 'distcc'), '--scan-includes',
                                 'gcc', '-c', str(self.source)], env=env,
                                cwd=self.work, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(os.fsencode(self.source), result.stdout)
        self.assertNotIn(b'.distcc-system', result.stdout)

    def compare(self, compiler='gcc', args=()):
        args = ['-E', *args, self.source]
        local = self.run_cc(compiler, args, False)
        local_deps = ((self.work / 'dep.d').read_text().split()
                      if '-MF' in args else None)
        remote = self.run_cc(compiler, args)
        self.assertEqual(remote.returncode, local.returncode,
                         remote.stderr.decode() + (self.work / 'client.log').read_text())
        self.assertEqual(remote.stdout, local.stdout, remote.stderr.decode())
        if local.returncode:
            self.assertEqual(remote.stderr, local.stderr)
        self.assertNotIn(b'preprocessing locally', remote.stderr)
        if local_deps is not None:
            self.assertEqual((self.work / 'dep.d').read_text().split(), local_deps)
        return remote

    def test_stdout_files_macros_and_dependencies(self):
        header = self.work / 'value.h'
        header.write_text('#define VALUE 17\n')
        self.source.write_text('#include <stddef.h>\n#include "value.h"\nint n = VALUE;\nconst char *p = __FILE__;\n')
        for compiler in ('gcc', shutil.which('clang'), 'clang' if shutil.which('clang') else None):
            if not compiler:
                continue
            with self.subTest(compiler=compiler):
                self.compare(compiler)
                self.compare(compiler, ['-P', '-dM'])
                self.compare(compiler, [f'-ffile-prefix-map={self.work}=/mapped'])
                self.compare(compiler, ['-MMD', '-MF', 'dep.d', '-MT', 'target'])
                self.assertIn('target:', (self.work / 'dep.d').read_text())
                self.compare(compiler, ['-MMD', '-MF', 'dep.d'])
                args = ['-E', self.source, '-o', 'result.i']
                local = self.run_cc(compiler, args, False)
                expected = (self.work / 'result.i').read_bytes()
                remote = self.run_cc(compiler, args)
                self.assertEqual(remote.returncode, local.returncode, remote.stderr.decode())
                self.assertEqual(remote.stdout, b'')
                self.assertEqual((self.work / 'result.i').read_bytes(), expected)
        log = (self.work / 'client.log').read_text()
        self.assertIn('pooled TCP job completed' if self.transport == ',tcp' else
                      'client finished sending request to server', log)

    def test_gcc_absolute_includes_probes_and_dependencies(self):
        header = self.work / 'absolute.h'
        header.write_text('int absolute_value;\nconst char *header_file = __FILE__;\n')
        self.source.write_text('#define HEADER "%s"\n#if __has_include(HEADER)\n'
                               '#include HEADER\n#else\n#error missing\n#endif\n' % header)
        self.compare(args=['-MD', '-MF', 'dep.d'])
        self.assertNotIn('preprocessing locally', (self.work/'client.log').read_text())

    def test_gcc_cxx_system_header_attributes(self):
        self.source.write_text('#include <clocale>\n')
        self.compare('g++')

    def test_parent_components_in_include_search_paths(self):
        directory = self.work/'headers'
        directory.mkdir()
        (directory/'child').mkdir()
        (directory/'selected.h').write_text('int selected;\n')
        self.source.write_text('#include <selected.h>\n#if __has_include(<absent.h>)\n#error unexpected\n#endif\n')
        self.compare(args=['-I'+str(directory/'child'/'..'), '-MD', '-MF', 'dep.d'])

    def test_utf8_bom_header_and_macro_diagnostic(self):
        header = self.work / 'bom.h'
        header.write_bytes(b'\xef\xbb\xbf#define NONNULL(x) ((x) != 0)\n'
                           b'static int values[2];\n')
        self.source.write_text('#include "bom.h"\n'
                               'int f(void) { return NONNULL(values); }\n')
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            args = ['-I' + str(self.work), '-O2', '-Werror=address',
                    '-c', self.source, '-o', 'bom.o']
            local = self.run_cc(compiler, args, False)
            self.assertEqual(local.returncode, 0, local.stderr.decode())
            remote = self.run_cc(compiler, args)
            self.assertEqual(remote.returncode, 0,
                             compiler + ': ' + remote.stderr.decode() +
                             (self.work / 'client.log').read_text())

    def test_parallel_outputs_do_not_invalidate_header_analysis(self):
        (self.work / 'value.h').write_text('#define VALUE 17\n')
        self.source.write_text('#include "value.h"\n' + 'int n;\n' * 10000)
        stop = threading.Event()
        writes = []
        def build_outputs():
            output = self.work / 'unrelated.o'
            while not stop.is_set():
                output.write_bytes(b'object')
                output.unlink()
                writes.append(1)
                stop.wait(.002)
        writer = threading.Thread(target=build_outputs)
        writer.start()
        try:
            self.compare(args=['-I', str(self.work), '-P'])
        finally:
            stop.set()
            writer.join()
        self.assertGreater(len(writes), 1)

    def test_compiled_file_macros_preserve_relative_spelling(self):
        (self.work / 'value.h').write_text('const char *header = __FILE__;\n')
        self.source.write_text('#include <stdio.h>\n#include "value.h"\n'
                               'int main(void) { printf("%s\\n%s\\n%s\\n", '
                               '__FILE__, __BASE_FILE__, header); }\n')
        for compiler in ('gcc', 'clang'):
            if not shutil.which(compiler):
                continue
            for source in ('./' + self.source.name, str(self.source)):
                with self.subTest(compiler=compiler, source=source):
                    output = []
                    for remote in (False, True):
                        result = self.run_cc(compiler, ['-c', source, '-o', 'macros.o'], remote)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        self.assertNotIn(b'preprocessing locally', result.stderr)
                        subprocess.run([compiler, 'macros.o', '-o', 'macros'], cwd=self.work,
                                       check=True, capture_output=True, timeout=10)
                        output.append(subprocess.check_output([str(self.work / 'macros')]))
                    self.assertEqual(*output)

    def test_changed_generated_deleted_headers_and_symlinks(self):
        for directory in ('first', 'second'):
            (self.work / directory).mkdir()
        later = self.work / 'second' / 'value.h'
        earlier = self.work / 'first' / 'value.h'
        later.write_text('#define VALUE 11\n')
        self.source.write_text('#include <value.h>\nint value = VALUE;\n')
        args = ['-I', 'first', '-I', 'second', '-P']
        self.assertIn(b'11', self.compare(args=args).stdout)
        later.write_text('#define VALUE 12\n')
        self.assertIn(b'12', self.compare(args=args).stdout)
        earlier.write_text('#define VALUE 13\n')
        self.assertIn(b'13', self.compare(args=args).stdout)
        earlier.unlink()
        self.assertIn(b'12', self.compare(args=args).stdout)
        (self.work / 'alias').symlink_to('second')
        self.compare(args=['-I', 'alias'])
        earlier.write_text('#define VALUE 14\n')
        (self.work / 'alias').unlink()
        (self.work / 'alias').symlink_to('first')
        self.assertIn(b'14', self.compare(args=['-I', 'alias', '-P']).stdout)
        self.source.write_text('int changed_source = 15;\n')
        self.assertIn(b'15', self.compare().stdout)

    def test_compiler_scanner_computed_has_include_and_generation(self):
        self.source.write_text('#define HEADER(name) <name.h>\n'
                               '#if __has_include(HEADER(generated))\n'
                               '#include HEADER(generated)\n'
                               '#else\n#define VALUE 7\n#endif\nint n = VALUE;\n')
        header = self.work / 'generated.h'
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            with self.subTest(compiler=compiler):
                self.assertIn(b'7', self.compare(compiler, ['-I.', '-P']).stdout)
                header.write_text('#define VALUE 19\n')
                self.assertIn(b'19', self.compare(compiler, ['-I.', '-P']).stdout)
                header.unlink()
                self.assertIn(b'7', self.compare(compiler, ['-I.', '-P']).stdout)

    def test_compiler_scanner_uses_gcc_macros(self):
        (self.work / 'gcc.h').write_text('#define VALUE 41\n')
        (self.work / 'clang.h').write_text('#define VALUE 43\n')
        self.source.write_text('#ifdef __clang__\n#define HEADER "clang.h"\n'
                               '#else\n#define HEADER "gcc.h"\n#endif\n'
                               '#include HEADER\nint n = VALUE;\n')
        for compiler, value in (('gcc', b'41'), ('g++', b'41'),
                                (shutil.which('clang'), b'43')):
            if compiler:
                with self.subTest(compiler=compiler):
                    args = ['-P'] if compiler != 'g++' else ['-x', 'c++', '-P']
                    self.assertIn(value, self.compare(compiler, args).stdout)

    def test_gcc_only_branch_existence_probe_is_transferred(self):
        # GCC -M does not list existence-only probes, and Clang selects the
        # other branch for this GCC builtin. Use the actual GCC frontend.
        (self.work / 'probe.h').write_text('/* must exist remotely */\n')
        self.source.write_text(
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if __has_include("probe.h")\n#define VALUE 19\n'
            '#else\n#define VALUE 7\n#endif\n'
            '#else\n#define VALUE 42\n#endif\nint value = VALUE;\n')
        dependencies = self.run_cc('gcc', ['-M', self.source], False)
        self.assertNotIn(b'probe.h', dependencies.stdout)
        self.compare('gcc', ['-P'])

    def test_ordinary_macro_expansion_selects_later_include(self):
        (self.work / 'selected.h').write_text('int selected = 19;\n')
        self.source.write_text('int before = __COUNTER__;\n'
                               '#if __COUNTER__ == 1\n#include "selected.h"\n#endif\n')
        for compiler in ('gcc', 'g++', 'clang'):
            with self.subTest(compiler=compiler):
                result = self.compare(compiler, ['-P'])
                self.assertIn(b'int selected = 19;', result.stdout)

    def test_gcc_only_literal_directives_follow_compiler_lexing(self):
        (self.work / 'gcc.h').write_text('#define VALUE 19\n')
        (self.work / 'other.h').write_text('#define VALUE 42\n')
        self.source.write_text(
            '/*\n#include NOT_A_HEADER\n*/\n'
            'const char *text = R"raw(\n#include ALSO_NOT_A_HEADER\n)raw";\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#\\\r\ninc\\\r\nlude "gcc.h"\n'
            '#else\n%:include "other.h"\n#endif\nint value = VALUE;\n')
        self.assertIn(b'19', self.compare('g++', ['-x', 'c++', '-std=c++17', '-P']).stdout)

    def test_gcc_duplicate_system_search_directory_preserves_precedence(self):
        for name, value in (('first', 19), ('second', 42)):
            directory = self.work / name
            directory.mkdir()
            (directory / 'header.h').write_text('#define VALUE %d\n' % value)
        self.source.write_text('#include <header.h>\nint value = VALUE;\n')
        self.assertIn(b'42', self.compare('gcc', ['-I', 'first', '-I', 'second',
                                               '-isystem', 'first', '-P']).stdout)

    def test_large_include_arguments_preserve_paths_and_precedence(self):
        first = self.work / 'search with spaces,quotes\'"and\\slashes'
        second = self.work / 'second'
        for directory, value in ((first, 19), (second, 42)):
            directory.mkdir()
            (directory / 'selection.h').write_text(
                '#define VALUE %d\nconst char *header = __FILE__;\n' % value)
        self.source.write_text('#include <selection.h>\nint value = VALUE;\n')
        flags = []
        size = 0
        while size < 96 * 1024:
            extra = ['-I', str(first)] if len(flags) % 2 else ['-I' + str(first)]
            flags += extra
            size += sum(len(arg) + 1 for arg in extra)
        flags += ['-I' + str(second), '-MD', '-MF', 'dep.d', '-MT', 'target.o']
        for extra, value in (([], b'19'), (['-isystem', str(first)], b'42')):
            with self.subTest(extra=extra):
                result = self.compare('gcc', [*flags, *extra])
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn(b'int value = ' + value, result.stdout)
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_c_macro_string_does_not_hide_a_header_probe(self):
        (self.work / 'present.h').write_text('/* existence only */\n')
        self.source.write_text(
            '#define START R"(\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if __has_include("present.h")\nint included = 1;\n'
            '#else\nint included = 0;\n#endif\n#endif\n'
            '#define FINISH )"\n')
        # Reuse one analyzer and unchanged input across modes in both orders.
        # GNU C accepts raw strings as an extension, but ISO C does not.
        for standard, expected in (('gnu11', b''), ('c11', b'int included = 1;\n'),
                                   ('gnu11', b''), ('c11', b'int included = 1;\n')):
            with self.subTest(standard=standard):
                result = self.compare('gcc', ['-std=' + standard, '-P'])
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stdout, expected)
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_gcc_native_and_optimizer_flags_preserve_header_selection(self):
        (self.work / 'abm.h').write_text('#define VALUE 19\n')
        (self.work / 'plain.h').write_text('#define VALUE 7\n')
        self.source.write_text('#ifdef __ABM__\n#include "abm.h"\n'
                               '#else\n#include "plain.h"\n#endif\nint n = VALUE;\n')
        for flags in (['-mabm', '-fno-tree-pre'],
                      ['-mno-abm', '-ftree-pre'], ['-march=native', '-mtune=native']):
            with self.subTest(flags=flags):
                self.compare('gcc', ['-P', *flags])

    def test_gcc_cxx_system_header_conditions(self):
        self.source.write_text('#include <string>\nstd::string value;\n')
        self.compare('g++', ['-P', '-std=c++17'])

    def test_directory_candidate_does_not_shadow_system_header(self):
        (self.work / 'stdint.h').mkdir()
        self.source.write_text('#include <stdint.h>\nuint64_t value;\n')
        self.compare(args=['-I' + str(self.work), '-P'])

    def test_comment_trigraphs_preserve_remote_macro_diagnostics(self):
        (self.work / 'macro.h').write_text('// double check ???)\n#define PRESENT(p) ((p) != 0)\n')
        self.source.write_text('#include "macro.h"\nint present(void) { int values[1]; return PRESENT(values); }\n')
        for standard in ('c++14', 'c++17'):
            with self.subTest(standard=standard):
                args = ['-std=' + standard, '-Werror=address', '-c', self.source, '-o', 'macro.o']
                local = self.run_cc('g++', args, False)
                self.assertEqual(local.returncode, 0, local.stderr.decode())
                expected = (self.work / 'macro.o').read_bytes()
                (self.work / 'macro.o').unlink()
                result = self.run_cc('g++', args)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual((self.work / 'macro.o').read_bytes(), expected)
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_cxx_driver_keeps_system_headers_when_suffix_changes(self):
        sources = [self.work / name for name in ('first.cpp', 'second.c')]
        for source in sources:
            source.write_text('#include <cstdio>\nint value = EOF;\n')
        for source in (sources[0], sources[1], sources[0]):
            with self.subTest(source=source.name):
                self.source = source
                result = self.compare('g++', ['-std=c++14', '-P'])
                self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_explicit_cpp_language_with_c_driver(self):
        self.source.write_text('#include <cstdio>\nint value = EOF;\n')
        for flags in (['-xc++'], ['-x', 'c++']):
            with self.subTest(flags=flags):
                result = self.compare('gcc', [*flags, '-std=c++14', '-P'])
                self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_gcc_only_indirect_probe_is_transferred(self):
        (self.work / 'probe.h').write_text('/* existence only */\n')
        for definition in ('#define HAS __has_include',
                           '#define HAS(x) __has_include(x)',
                           '#define JOIN(a,b) a ## b\n#define HAS JOIN(__has_,include)'):
            with self.subTest(definition=definition):
                self.source.write_text(
                    definition + '\n#if __has_builtin(__builtin_speculation_safe_value)\n'
                    '#if HAS("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
                    '#else\nint other;\n#endif\n')
                self.assertIn(b'present', self.compare('gcc', ['-P']).stdout)

    def test_gcc_defined_guard_keeps_active_probe_and_invalidation(self):
        header = self.work / 'probe.h'
        header.write_text('/* existence only */\n')
        self.source.write_text(
            '#include <stdint.h>\n'
            '#define HAS(x) __has_include(x)\n'
            '#if defined(INT64_C) && __has_builtin(__builtin_speculation_safe_value)\n'
            '#if defined(HAS) && HAS("probe.h")\nint present;\n'
            '#else\nint absent;\n#endif\n#else\nint other;\n#endif\n')
        self.assertIn(b'present', self.compare('gcc', ['-P']).stdout)
        header.unlink()
        self.assertIn(b'absent', self.compare('gcc', ['-P']).stdout)

    def test_gcc_command_line_probe_alias_is_transferred(self):
        (self.work / 'probe.h').write_text('/* existence only */\n')
        self.source.write_text(
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if HAS("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
            '#else\nint other;\n#endif\n')
        for definition in ('HAS=__has_include', 'HAS(x)=__has_include(x)'):
            with self.subTest(definition=definition):
                self.assertIn(b'present', self.compare('gcc', ['-P', '-D' + definition]).stdout)

    def test_gcc_pasted_alias_rechecks_macro_namespace_and_header(self):
        header = self.work / 'probe.h'
        header.write_text('/* existence only */\n')
        self.source.write_text(
            '#define JOIN(a,b) a ## b\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if JOIN(H,A)("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
            '#else\nint other;\n#endif\n')
        self.assertIn(b'absent', self.compare('gcc', ['-P', '-DHA(x)=0']).stdout)
        args = ['-P', '-DHA=__has_include']
        self.assertIn(b'present', self.compare('gcc', args).stdout)
        header.unlink()
        self.assertIn(b'absent', self.compare('gcc', args).stdout)
        header.write_text('/* recreated */\n')
        self.assertIn(b'present', self.compare('gcc', args).stdout)

    def test_gcc_computed_next_probe_is_transferred(self):
        first, second = self.work / 'first', self.work / 'second'
        first.mkdir()
        second.mkdir()
        (second / 'optional.h').write_text('/* existence only */\n')
        (first / 'probe.h').write_text(
            '#define NAME(x) <x.h>\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if __has_include_next(NAME(optional))\nint present;\n'
            '#else\nint absent;\n#endif\n#else\nint other;\n#endif\n')
        self.source.write_text('#include "probe.h"\n')
        self.assertIn(b'present', self.compare('gcc', ['-P', '-Ifirst', '-Isecond']).stdout)

    def test_compile_execute_and_pool_reuse(self):
        self.source.write_text('#include "value.h"\nint main(void) { return VALUE - 19; }\n')
        (self.work / 'value.h').write_text('#define VALUE 19\n')
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        before = int(match[1])
        for _ in range(3):
            result = self.run_cc('gcc', ['-c', self.source, '-o', 'hello.o'])
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        after = int(match[1])
        self.assertEqual(after - before, 1)
        subprocess.run([self.cc, 'hello.o', '-o', 'hello'], cwd=self.work, check=True)
        subprocess.run([str(self.work / 'hello')], check=True)

    def test_error_preserves_partial_stdout(self):
        self.source.write_text('int before;\n#error deliberate_error\nint after;\n')
        self.compare(args=['-P'])
        args = ['-E', '-P', self.source, '-o', 'failed.i']
        local = self.run_cc('gcc', args, False)
        self.assertFalse((self.work / 'failed.i').exists())
        (self.work / 'failed.i').write_text('old output')
        remote = self.run_cc('gcc', args)
        self.assertEqual(remote.returncode, local.returncode)
        self.assertFalse((self.work / 'failed.i').exists())

    def test_unsupported_inputs_preprocess_locally(self):
        self.env.pop('DISTCC_TESTING_INCLUDE_SERVER')
        header = self.work / 'absolute.h'
        header.write_text('int n;\n')
        self.source.write_text('#include "%s"\n' % (self.work/'..'/self.work.name/header.name))
        # Include-analysis fallback still permits remote compilation, while -E
        # itself runs locally only when fallback is allowed.
        self.env['DISTCC_FALLBACK'] = '1'
        local = self.run_cc('gcc', ['-E', self.source], False)
        remote = self.run_cc('gcc', ['-E', self.source])
        self.assertEqual(remote.returncode, 0, remote.stderr.decode())
        self.assertEqual(remote.stdout, local.stdout)
        self.assertIn('preprocessing locally', (self.work / 'client.log').read_text())

    def test_header_existence_without_including_it(self):
        self.source.write_text('#if __has_include("optional.h")\nint present;\n#else\nint absent;\n#endif\n')
        self.assertIn(b'absent', self.compare(args=['-P']).stdout)
        (self.work / 'optional.h').write_text('/* presence is enough */\n')
        self.assertIn(b'present', self.compare(args=['-P']).stdout)

    def test_failed_include_analysis_is_not_cached_as_complete(self):
        (self.work / 'unsupported.h').write_text(
            '#include "%s"\n' % (self.work / '..' / self.work.name / 'value.h'))
        (self.work / 'value.h').write_text('#define VALUE 17\n')
        self.source.write_text('#include "unsupported.h"\nint n = VALUE;\n')
        for attempt in range(3):
            with self.subTest(attempt=attempt):
                (self.work / 'client.log').write_text('')
                result = self.run_cc('gcc', ['-E', self.source])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'failed to get includes',
                              (self.work / 'client.log').read_bytes())
        (self.work / 'unsupported.h').write_text('#include "value.h"\n')
        self.assertIn(b'17', self.compare(args=['-P']).stdout)

    def test_has_include_compatibility_definition(self):
        (self.work / 'optional.h').write_text('#define VALUE 17\n')
        self.source.write_text(
            '#ifndef __has_include\n#define __has_include(header) 0\n#endif\n'
            '#if __has_include("optional.h")\n#include "optional.h"\n'
            '#else\n#define VALUE 0\n#endif\nint n = VALUE;\n')
        for compiler in ('gcc', shutil.which('clang')):
            if compiler:
                with self.subTest(compiler=compiler):
                    self.compare(compiler, ['-P'])

    def test_truncated_response_does_not_publish_stdout_twice(self):
        # Forward a real compilation, then drop the end of its result after
        # SOUT. The local retry must be the sole writer of user-visible output.
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(10)
        self.addCleanup(listener.close)
        failures = []

        def proxy():
            try:
                with listener.accept()[0] as client, socket.create_connection(
                        ('127.0.0.1', self.ports[0]), timeout=10) as server:
                    response = bytearray()
                    while True:
                        readable, _, _ = select.select([client, server], [], [], 10)
                        if not readable:
                            raise TimeoutError('compiler proxy stalled')
                        for sock in readable:
                            chunk = sock.recv(65536)
                            if not chunk:
                                if sock is server:
                                    client.sendall(response[:-12])
                                    return
                                raise ConnectionError('client abandoned compile')
                            if sock is server:
                                response.extend(chunk)
                            else:
                                server.sendall(chunk)
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=proxy)
        worker.start()
        self.env.pop('DISTCC_BROKER')
        self.env.update(DISTCC_HOSTS=f'127.0.0.1:{listener.getsockname()[1]},cpp,{self.compression}',
                        DISTCC_FALLBACK='1', DISTCC_MAX_RETRIES='1')
        self.compare(args=['-P'])
        worker.join(timeout=12)
        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])
        self.assertIn('running locally instead', (self.work / 'client.log').read_text())


class Lz4PreprocessTest(PreprocessTest):
    compression = 'lz4'

    def test_mixed_codec_cache(self):
        self.env.pop('DISTCC_BROKER')
        (self.work / 'empty.h').write_bytes(b'')
        self.source.write_text('#include "empty.h"\nint n;\n')
        for codec in ('lz4', 'lzo', 'lz4'):
            self.env['DISTCC_HOSTS'] = self.host(0, ',cpp,' + codec)
            self.compare(args=['-P'])
        for codec in ('lz4', 'lzo'):
            self.assertTrue(list(self.work.rglob('empty.h.' + codec)))


class TcpPreprocessTest(PreprocessTest):
    compression = 'lz4s'
    transport = ',tcp'

    def use_local_socket(self):
        prefix = str(self.root / 'header-jobs')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        pump_env = dict(self.env)
        self.start_broker(self.host(0, ',lz4s,tcp'), name='header-local')
        broker = self.env['DISTCC_BROKER']
        self.env.update(pump_env, DISTCC_BROKER=broker, DISTCC_LOCAL_SOCKET=prefix)

    def test_local_socket_validates_matching_system_headers(self):
        self.use_local_socket()
        self.source.write_text('#include <stddef.h>\nint value = 7;\n')
        self.compare(args=['-P'])
        self.assertNotIn('worker system headers differ', (self.work / 'client.log').read_text())
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_tcp_accept 0\n', stats)
        self.assertIn('dcc_compile_ok 1\n', stats)

    def test_local_socket_uses_client_system_headers(self):
        self.use_local_socket()
        self.client_system_headers()

    def cache_counts(self):
        log = (self.work / 'client.log').read_text()
        counts = re.findall(r'input cache: (\d+) of (\d+) entries transferred', log)
        self.assertTrue(counts, 'cached RPC was not reached: ' + log)
        return tuple(map(int, counts[-1]))

    def test_cache_reuses_and_repairs_input_snapshots(self):
        header = ('/*' + random.Random(31).randbytes(16384).hex() + '*/\n#define VALUE 731\n').encode()
        (self.work / 'value.h').write_bytes(header)
        (self.work / 'empty.h').write_bytes(b'')
        self.source.write_text('#include "value.h"\n#include "empty.h"\nint n = VALUE;\n')
        self.compare(args=['-P'])
        self.assertGreater(self.cache_counts()[0], 0)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

        key = xxhash.xxh3_64(header).hexdigest()
        entry = self.root / f'distcc-cache-{os.geteuid()}' / f'{int(key[:2], 16) % 64:02x}' / key
        self.assertEqual(entry.read_bytes(), header)
        entry.chmod(0o600)
        entry.write_bytes(b'corrupt cached input')
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 1)
        self.assertEqual(entry.read_bytes(), header)
        entry.unlink()
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 1)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

        (self.work / 'value.h').write_bytes(header.replace(b'VALUE 731', b'VALUE 732'))
        self.assertIn(b'732', self.compare(args=['-P']).stdout)
        self.assertEqual(self.cache_counts()[0], 1)

    def test_cached_symlink_retarget(self):
        padding = '/*' + random.Random(39).randbytes(16384).hex() + '*/\n'
        for name, value in (('first', 71), ('second', 72)):
            directory = self.work / name
            directory.mkdir()
            (directory / 'nested').mkdir()
            (directory / 'value.h').write_text(padding + f'#define VALUE {value}\n')
        alias = self.work / 'alias'
        alias.symlink_to('first/nested')
        # Resolving '..' before the symlink would choose the wrong header.
        (self.work / 'value.h').write_text('#error incorrect path normalization\n')
        self.source.write_text('#include "alias/../value.h"\nint n = VALUE;\n')
        self.assertIn(b'71', self.compare(args=['-P']).stdout)
        self.cache_counts()
        alias.unlink()
        alias.symlink_to('second/nested')
        self.assertIn(b'72', self.compare(args=['-P']).stdout)
        self.cache_counts()
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

    def test_uncacheable_input_skips_lookup(self):
        self.source.write_text('/*' + random.Random(61).randbytes(3 * 1024 * 1024).hex() + '*/\nint n;\n')
        for _ in range(2):
            self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())

    def test_mixed_uncacheable_and_cached_inputs(self):
        (self.work / 'large.h').write_bytes(b'/*' + b' ' * (5 * 1024 * 1024) + b'*/\n')
        header = '/*' + random.Random(62).randbytes(16384).hex() + '*/\n#define VALUE 733\n'
        (self.work / 'value.h').write_text(header)
        self.source.write_text('#include "large.h"\n#include "value.h"\nint n = VALUE;\n')
        self.compare(args=['-P'])
        self.assertGreater(self.cache_counts()[0], 0)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

    def test_missing_sidecar_does_not_reuse_cache(self):
        self.source.write_text('/*' + random.Random(32).randbytes(16384).hex() + '*/\nint n;\n')
        self.compare(args=['-P'])
        self.cache_counts()
        for digest in self.work.rglob('*.xxh3'):
            digest.unlink()
        (self.work / 'client.log').write_text('')
        self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())

    def test_disabled_server_cache(self):
        self.source.write_text('/*' + random.Random(33).randbytes(16384).hex() + '*/\nint n;\n')
        try:
            self.restart_server(self.build / 'distccd', DISTCCD_FILE_CACHE='')
            self.compare(args=['-P'])
            first = self.cache_counts()[0]
            self.compare(args=['-P'])
            self.assertGreater(first, 0)
            self.assertEqual(self.cache_counts()[0], first)
        finally:
            self.restart_server(self.build / 'distccd')

    def test_small_inputs_skip_manifest_round_trip(self):
        for _ in range(2):
            self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())


if __name__ == '__main__':
    unittest.main()
