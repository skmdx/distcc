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
from test_adaptive import stats_response


class PumpTestCase(BrokerTestCase):
    compression = 'lzo'
    transport = ''
    cpp_slots = 1

    @classmethod
    def setUpClass(cls):
        cls.daemon_env = {'DISTCC_CLANG_DRIVER': str(Path.cwd() / 'distcc-clang')}
        cls.daemon_env['PYTHONPATH'] = str(next(Path.cwd().glob('_include_server/lib.*')))
        module = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if module:
            cls.daemon_env['DISTCC_GCC_SCAN_LIBRARY'] = module
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',' + self.compression + self.transport) +
                          f' --localslots_cpp={self.cpp_slots}', idle=60)
        self.env.update(TMPDIR=str(self.root), DISTCC_CLIENT_TMP=str(self.work),
                        INCLUDE_SERVER_ARGS=f'--workers={self.cpp_slots}' +
                            (' --statistics' if getattr(self, 'analyzer_statistics', False) else ''),
                        DISTCC_CPP_LOOKUP_LIBRARY=str(self.build / 'cpp_lookup.so'),
                        DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_TESTING_INCLUDE_SERVER='1',
                        DISTCC_VERBOSE='1', DISTCC_LOG=str(self.work / 'client.log'))
        if getattr(self, 'analyzer_statistics', False):
            self.env['PYTHONUNBUFFERED'] = '1'
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

    def code_object(self, name):
        output = self.work / (str(name) + '.stripped')
        subprocess.run(['objcopy', '--strip-debug', str(name), str(output)],
                       cwd=self.work, check=True, capture_output=True, timeout=5)
        return output.read_bytes()


class PreparedClangTest(PumpTestCase):
    compression = 'lz4s'
    transport = ',tcp'

    def test_preprocessing_bootstraps_while_a_compiler_lease_is_held(self):
        held = self.request(12)
        self.assertEqual(self.receive(held), (0, 1, []))
        (self.work / 'shared.h').write_text('#pragma once\nenum { value = __COUNTER__ };\n')
        source = self.work / 'program.cc'
        source.write_text('#include "shared.h"\nstatic_assert(__COUNTER__ == 1);\n'
                          'int main() { return value; }\n')
        args = ['-E', '-P', source, '-o', 'program.ii', '-MD', '-MF', 'program.d']
        native = self.run_cc('clang++', args, remote=False)
        self.assertEqual(native.returncode, 0, native.stderr.decode())
        expected = (self.work / 'program.ii').read_bytes()
        expected_deps = (self.work / 'program.d').read_text()
        (self.work / 'program.ii').unlink()
        before = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
        assert before is not None
        result = self.run_cc('clang++', args)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(f'dcc_compile_ok {int(before[1]) + 1}\n',
                      self.get_stats(self.stats_ports[0]))
        self.assertEqual((self.work / 'program.ii').read_bytes(), expected)
        self.assertEqual((self.work / 'program.d').read_text(), expected_deps)
        subprocess.run(['clang++', 'program.ii', '-o', 'program'], cwd=self.work,
                       check=True, capture_output=True, timeout=10)
        subprocess.run([str(self.work / 'program')], check=True, timeout=5)
        held.close()

    def test_frontend_ir_without_broker(self):
        self.env.pop('DISTCC_BROKER')
        self.env['DISTCC_HOSTS'] = self.host(0, ',lz4,cpp')
        self.test_compile_reuses_include_expansion_without_dependency_scan()

    def test_frontend_ir_links_with_lto_and_instrumentation(self):
        source = self.work / 'program.cc'
        source.write_text('static int value;\n'
                          'struct Init { Init() { value = 42; } } init;\n'
                          'extern "C" int answer() { return value; }\n'
                          'extern "C" int alias() __attribute__((alias("answer")));\n'
                          'int main() { return alias() != 42; }\n')
        self.env['INCLUDE_SERVER_PORT'] = str(self.work / 'absent-analyzer.sock')
        for options in (['-O0'], ['-O3'], ['-O3', '-flto=thin'],
                        ['-O3', '-flto'], ['-O3', '-fsanitize=undefined']):
            with self.subTest(options=options):
                result = self.run_cc('clang++', [*options, '-c', source, '-o', 'program.o'])
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                subprocess.run(['clang++', *options, 'program.o', '-o', 'program'],
                               cwd=self.work, check=True, capture_output=True, timeout=15)
                subprocess.run([str(self.work / 'program')], check=True, timeout=5)

    def test_compile_reuses_include_expansion_without_dependency_scan(self):
        header = self.work / 'shared.h'
        source = self.work / 'prepared.cc'
        source.write_text('#include "shared.h"\n'
                          'static_assert(__COUNTER__ == 1);\n'
                          '#if __COUNTER__ != 2\n#error replayed counter\n#endif\n'
                          'static_assert(__COUNTER__ == 3);\n'
                          'const char *identity = __BASE_FILE__;\n'
                          'extern "C" int aliased() { return VALUE; }\n'
                          'extern "C" int public_alias() __attribute__((alias("aliased")));\n'
                          'int result() { return VALUE + first_counter; }\n')
        socket = self.env['INCLUDE_SERVER_PORT']
        # A successful request must not rely on a hidden analysis/fallback.
        self.env['INCLUDE_SERVER_PORT'] = str(self.work / 'absent-analyzer.sock')
        try:
            before = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
            assert before is not None
            for value in (17, 29):
                header.write_text('#pragma once\nenum { first_counter = __COUNTER__ };\n'
                                  f'#define VALUE {value}\n')
                args = ['-g', '-O3', '-c', source, '-o', 'prepared.o', '-MD', '-MF', 'prepared.d']
                local = self.run_cc('clang++', args, remote=False)
                self.assertEqual(local.returncode, 0, local.stderr.decode())
                output, deps = self.work / 'prepared.o', self.work / 'prepared.d'
                expected, expected_deps = output.read_bytes(), deps.read_bytes()
                output.unlink()
                deps.unlink()
                remote = self.run_cc('clang++', args)
                self.assertEqual(remote.returncode, 0, remote.stderr.decode())
                self.assertEqual(output.read_bytes(), expected)
                debug = subprocess.run(['llvm-dwarfdump', '--verify', str(output)],
                                       capture_output=True, text=True, timeout=5)
                self.assertEqual(debug.returncode, 0, debug.stdout + debug.stderr)
                debug = subprocess.check_output(['llvm-dwarfdump', '--debug-info',
                                                 '--debug-line', str(output)], text=True)
                self.assertIn(f'DW_AT_name\t("{source}")', debug)
                self.assertIn('DW_AT_name\t("result")', debug)
                self.assertIn('is_stmt prologue_end', debug)
                self.assertEqual(shlex.split(deps.read_text().replace('\\\n', '')),
                                 shlex.split(expected_deps.decode().replace('\\\n', '')))
            self.assertIn(f'dcc_compile_ok {int(before[1]) + 2}\n',
                          self.get_stats(self.stats_ports[0]))
        finally:
            self.env['INCLUDE_SERVER_PORT'] = socket


    def test_cached_headers_compile_remotely_and_invalidate(self):
        header = self.work / 'shared.h'
        header.write_text('#pragma once\n#include <vector>\n'
                          '#if __has_include("generated.h")\n#include "generated.h"\n'
                          '#else\n#define VALUE 3\n#endif\n'
                          'enum { first_counter = __COUNTER__ };\n')
        before = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
        assert before is not None
        for index in range(6):
            if index == 3:
                (self.work / 'generated.h').write_text('#define VALUE 17\n')
            source = self.work / f'cached{index}.cc'
            source.write_text('#include "shared.h"\n'
                              'static_assert(__COUNTER__ == 1);\n'
                              f'static_assert(VALUE == {3 if index < 3 else 17});\n'
                              'int result() { std::vector<int> v(VALUE); return v.size(); }\n')
            args = ['-g', '-c', source, '-o', 'cached.o', '-MD', '-MF', 'cached.d']
            local = self.run_cc('clang++', args, remote=False)
            self.assertEqual(local.returncode, 0, local.stderr.decode())
            output, deps = self.work / 'cached.o', self.work / 'cached.d'
            expected, expected_deps = self.code_object('cached.o'), deps.read_bytes()
            output.unlink()
            deps.unlink()
            remote = self.run_cc('clang++', args)
            self.assertEqual(remote.returncode, 0, remote.stderr.decode())
            self.assertEqual(self.code_object('cached.o'), expected)
            subprocess.run(['llvm-dwarfdump', '--verify', 'cached.o'],
                           cwd=self.work, check=True, capture_output=True, timeout=5)
            self.assertEqual(shlex.split(deps.read_text().replace('\\\n', '')),
                             shlex.split(expected_deps.decode().replace('\\\n', '')))
        self.assertIn(f'dcc_compile_ok {int(before[1]) + 6}\n',
                      self.get_stats(self.stats_ports[0]))
        self.assertNotIn('include server gave up analyzing',
                         (self.work / 'client.log').read_text())


class PreparedLocalClangTest(BrokerTestCase):
    def test_ready_ir_runs_only_the_local_backend(self):
        source = self.work / 'prepared.cc'
        source.write_text('#include <vector>\n'
                          'enum { first = __COUNTER__ };\n'
                          'static_assert(__COUNTER__ == 1);\n'
                          'extern "C" int answer() { std::vector<int> a(3, 14); return a[0] * 3; }\n'
                          'extern "C" int alias() __attribute__((alias("answer")));\n'
                          'int main() { return alias() != 42; }\n')
        for index, (save, options) in enumerate(((False, []), (True, []),
                                               (False, ['-fprofile-generate']),
                                               (False, ['-flto=thin']))):
            with self.subTest(save_temps=save, options=options):
                ready = threading.Event()
                peer = self.peer(lambda: stats_response(cpus=1, cores=1, jobs=1, capacity=1,
                    busy=0 if ready.is_set() else 1, running=0 if ready.is_set() else 1,
                    queued=0, preparing=0, returning=0))
                name = f'prepared-{index}'
                self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
                self.start_broker(self.host(0, f',tcp,stats={peer.port}') +
                                  ' --localslots_cpp=1', name=name)
                log = self.work / f'client-{index}.log'
                self.env.update(DISTCC_LOG=str(log), DISTCC_SAVE_TEMPS=str(int(save)),
                                DISTCC_CLANG_DRIVER=str(self.build / 'distcc-clang'),
                                PYTHONPATH=str(next(self.build.glob('_include_server/lib.*'))))
                args = ['clang++', '-O3', '-g', *options, '-c', str(source),
                        '-o', 'prepared.o', '-MD', '-MF', 'prepared.d']
                subprocess.run(args, cwd=self.work, check=True, capture_output=True, timeout=15)
                expected = (self.work / 'prepared.o').read_bytes()
                dependencies = (self.work / 'prepared.d').read_bytes()
                (self.work / 'prepared.o').unlink()
                (self.work / 'prepared.d').unlink()
                process = subprocess.Popen([str(self.build / 'distcc'), *args],
                    cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    deadline = time.monotonic() + 10
                    while not ready.is_set():
                        self.assertIsNone(process.poll(), log.read_text() if log.exists() else '')
                        self.assertLess(time.monotonic(), deadline, 'frontend was not reached')
                        children = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text()
                        for child in children.split():
                            try:
                                arguments = Path(f'/proc/{child}/cmdline').read_bytes().split(b'\0')
                            except FileNotFoundError:
                                continue
                            if b'-emit-llvm' in arguments:
                                ready.set()
                        if not ready.is_set():
                            time.sleep(.005)
                    stdout, stderr = process.communicate(timeout=20)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                self.assertEqual(process.returncode, 0, stderr.decode())
                self.assertIn('prepared=1', (self.work / f'{name}.log').read_text())
                self.assertIn('-x ir', log.read_text())
                if '-flto=thin' not in options:
                    self.assertEqual((self.work / 'prepared.o').read_bytes(), expected)
                self.assertEqual((self.work / 'prepared.d').read_bytes(), dependencies)
                subprocess.run(['clang++', *options, 'prepared.o', '-o', 'prepared'],
                               cwd=self.work, check=True, capture_output=True, timeout=15)
                subprocess.run([str(self.work / 'prepared')], cwd=self.work, check=True, timeout=5)


class AnalyzerPoolTest(PumpTestCase):
    # Exercise RPC even when live statistics enable direct local admission.
    worker_address = '127.0.0.2'
    cpp_slots = 2
    compression = 'lz4'
    transport = ',tcp'

    def test_response_files_are_expanded_before_pump_and_native_options(self):
        source = self.work / 'response source.cc'
        header = self.work / 'response header.h'
        source.write_text('#include "response header.h"\nint answer() { return VALUE; }\n')
        header.write_text('#define VALUE 42\n')
        empty = self.work / 'empty.modmap'
        empty.write_text('')
        nested = self.work / 'nested options.rsp'
        outer = self.work / 'outer.rsp'
        for compiler in ('g++', 'clang++'):
            for mode in ('empty', 'nested'):
                with self.subTest(compiler=compiler, mode=mode):
                    args = ['-march=native', '-mtune=native', '-c', source,
                            '-o', 'response.o', '-MD', '-MF', 'response.d']
                    if mode == 'empty':
                        args.append('@' + empty.name)
                    else:
                        # Both compilers resolve nested response paths from cwd.
                        outer.write_text(shlex.quote('@' + nested.name))
                        nested.write_text(shlex.join(list(map(str, args))))
                        args = ['@' + outer.name]
                    before = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
                    assert before is not None
                    local = self.run_cc(compiler, args, remote=False)
                    self.assertEqual(local.returncode, 0, local.stderr.decode())
                    output, deps = self.work / 'response.o', self.work / 'response.d'
                    expected = output.read_bytes()
                    output.unlink()
                    deps.unlink()
                    remote = self.run_cc(compiler, args)
                    self.assertEqual(remote.returncode, 0, remote.stderr.decode() +
                                     (Path(self.env['INCLUDE_SERVER_DIR']) / 'stderr').read_text())
                    self.assertEqual(output.read_bytes(), expected)
                    self.assertIn('response\\ header.h', deps.read_text())
                    self.assertIn(f'dcc_compile_ok {int(before[1]) + 1}\n',
                                  self.get_stats(self.stats_ports[0]))

    def test_response_file_changes_are_not_cached_as_arguments(self):
        response = self.work / 'changing.rsp'
        for value in (17, 29):
            self.source.write_text(f'_Static_assert(VALUE == {value}, "stale response");\n')
            response.write_text(f'-DVALUE={value} -c {shlex.quote(str(self.source))} -o changed.o')
            result = self.run_cc('clang', ['@' + response.name])
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_invalid_response_files_stay_local_with_compiler_diagnostics(self):
        response = self.work / 'cycle.rsp'
        response.write_text('@cycle.rsp')
        for name in ('missing.rsp', response.name):
            args = ['@' + name, '-c', self.source, '-o', 'bad.o']
            local = self.run_cc('clang', args, remote=False)
            result = self.run_cc('clang', args)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.returncode, local.returncode)
            self.assertEqual(result.stderr, local.stderr)
            self.assertFalse((self.work / 'bad.o').exists())
        self.assertNotIn('send CDIR', (self.work / 'client.log').read_text())

    def test_portage_hook_preserves_analyzer_storage_selection(self):
        hook = self.build / 'packaging/gentoo/pump.bashrc'
        self.shutdown_pump()
        for directory in (None, str(self.work)):
            with self.subTest(directory=directory):
                temporary = self.root / ('long-package-work-path-' * 6) if directory is None else self.root
                temporary.mkdir(exist_ok=True)
                env = dict(self.env, T=str(temporary), DISTCC_PUMP='1', FEATURES='distcc',
                           PATH=str(self.build) + ':' + self.env['PATH'])
                for key in ('INCLUDE_SERVER_PID', 'INCLUDE_SERVER_DIR', 'INCLUDE_SERVER_PORT',
                            'DISTCC_CLIENT_TMP', 'DISTCC_PUMP_ACTIVE', 'DISTCC_PUMP_CLIENT_TMP',
                            'DISTCC_PUMP_CLIENT_FD'):
                    env.pop(key, None)
                if directory is not None:
                    env['DISTCC_CLIENT_TMP'] = directory
                counter = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
                assert counter is not None
                result = subprocess.run(['bash', '-c', '''
has() { local word=$1; shift; [[ " $* " == *" $word "* ]]; }
register_die_hook() { :; }
die() { echo "$*" >&2; exit 1; }
source "$1"
trap distcc_pump_stop EXIT
distcc_pump_start
printf '%s\n' "$DISTCC_CLIENT_TMP"
shopt -s nullglob
roots=( "$DISTCC_CLIENT_TMP"/*.include_server-"$INCLUDE_SERVER_PID"-* )
(( ${#roots[@]} )) || exit 1
for compiler in gcc clang; do
    "$2" "$compiler" -c "$3" -o portage-storage.o || {
        cat "$INCLUDE_SERVER_DIR/stderr" >&2
        exit 1
    }
done
''', 'portage-pump-test', str(hook), str(self.build / 'distcc'), str(self.source)],
                    cwd=self.work, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                storage = Path(result.stdout.strip())
                if directory is None:
                    self.assertEqual(storage.parent, Path('/dev/shm'))
                    self.assertTrue(storage.name.startswith('distcc-pump.'))
                    self.assertFalse(storage.exists(), 'The owned tmpfs root must be removed')
                else:
                    self.assertEqual(storage, Path(directory))
                    self.assertTrue(self.source.exists(), 'User storage must survive shutdown')
                self.assertIn(f'dcc_compile_ok {int(counter[1]) + 2}\n',
                              self.get_stats(self.stats_ports[0]))

    def portage_namespace_command(self):
        probe = subprocess.run(['unshare', '-Urpf', '--mount-proc', 'true'],
                               capture_output=True, timeout=5)
        if probe.returncode:
            self.skipTest('Unprivileged PID namespaces are unavailable')
        self.shutdown_pump()
        env = dict(self.env, T=str(self.root), DISTCC_PUMP='1', FEATURES='distcc',
                   PATH=str(self.build) + ':' + self.env['PATH'])
        for key in ('INCLUDE_SERVER_PID', 'INCLUDE_SERVER_DIR', 'INCLUDE_SERVER_PORT',
                    'DISTCC_CLIENT_TMP', 'DISTCC_PUMP_ACTIVE', 'DISTCC_PUMP_CLIENT_TMP',
                    'DISTCC_PUMP_CLIENT_FD'):
            env.pop(key, None)
        command = ['unshare', '-Urpf', '--kill-child', '--mount-proc', 'bash', '-c', '''
has() { local word=$1; shift; [[ " $* " == *" $word "* ]]; }
register_die_hook() { :; }
die() { echo "$*" >&2; exit 1; }
pump() {
    if [[ $1 == --startup ]]; then
        echo 1000 > /proc/sys/kernel/ns_last_pid || exit 1
    fi
    command pump "$@"
}
source "$1"
trap distcc_pump_stop EXIT
distcc_pump_start
"$2" gcc -c "$3" -o "$DISTCC_CLIENT_TMP/result.o" || exit 1
printf '%s\t%s\n' "$DISTCC_CLIENT_TMP" "$INCLUDE_SERVER_PID"
read -r proceed || exit 1
"$2" gcc -c "$3" -o "$DISTCC_CLIENT_TMP/result.o"
''', 'portage-namespace-test', str(self.build / 'packaging/gentoo/pump.bashrc'),
                   str(self.build / 'distcc'), str(self.source)]
        return command, env

    def start_portage_namespace(self, command, env):
        process = subprocess.Popen(command, cwd=self.work, env=env, text=True,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        def cleanup():
            process.communicate('\n' if process.poll() is None else None, timeout=15)
        self.addCleanup(cleanup)
        assert process.stdout is not None
        self.assertTrue(select.select([process.stdout], [], [], 10)[0])
        values = process.stdout.readline().strip().split('\t')
        self.assertEqual(len(values), 2, 'The namespace must start and compile successfully')
        return process, Path(values[0]), values[1]

    def test_portage_tmpfs_cleanup_isolates_pid_namespaces(self):
        command, env = self.portage_namespace_command()
        first = self.start_portage_namespace(command, env)
        second = self.start_portage_namespace(command, env)
        self.assertEqual(first[2], second[2], 'Exercise the same PID in separate namespaces')
        self.assertNotEqual(first[1], second[1])
        for index, (process, root, _) in enumerate((first, second)):
            _, stderr = process.communicate('\n', timeout=15)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertFalse(root.exists())
            if index == 0:
                self.assertTrue(second[1].exists(), 'Peer shutdown must preserve live storage')

    def test_portage_tmpfs_recovers_after_namespace_kill(self):
        command, env = self.portage_namespace_command()
        process, root, _ = self.start_portage_namespace(command, env)
        process.kill()
        process.communicate(timeout=5)
        self.assertTrue(root.exists(), 'SIGKILL must exercise abandoned storage')
        # Wait for the kernel to finish killing namespace descendants, not just
        # the outer unshare process, before testing collection of a dead lease.
        with (root / 'lease').open('r+') as lease:
            deadline = time.monotonic() + 5
            while True:
                try:
                    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
        replacement, new_root, _ = self.start_portage_namespace(command, env)
        self.assertFalse(root.exists(), 'The next startup must collect abandoned storage')
        _, stderr = replacement.communicate('\n', timeout=15)
        self.assertEqual(replacement.returncode, 0, stderr)
        self.assertFalse(new_root.exists())

    def test_startup_preserves_multiline_host_configuration(self):
        self.shutdown_pump()
        for key in ('INCLUDE_SERVER_PID', 'INCLUDE_SERVER_DIR', 'INCLUDE_SERVER_PORT'):
            self.env.pop(key, None)
        hosts = "# worker's configuration\n" + self.env['DISTCC_HOSTS'] + '\n'
        self.env['DISTCC_HOSTS'] = hosts
        self.env['PATH'] += ":/nonexistent/quoted' path"
        startup = subprocess.run([str(self.build / 'pump'), '--startup'],
                                 cwd=self.work, env=self.env, capture_output=True,
                                 text=True, timeout=15, check=True)
        restored = subprocess.run(['/bin/sh', '-c', startup.stdout + '\nexec env -0'],
                                  env=self.env, capture_output=True, timeout=5, check=True)
        for entry in restored.stdout.decode().split('\0'):
            if entry:
                key, value = entry.split('=', 1)
                self.env[key] = value
        self.assertEqual(self.env['DISTCC_HOSTS'], hosts)
        self.assertTrue(self.env['PATH'].endswith(":/nonexistent/quoted' path"))
        counter = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
        assert counter is not None
        before = int(counter[1])
        result = self.run_cc('gcc', ['-c', self.source, '-o', 'environment.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(f'dcc_compile_ok {before + 1}\n', self.get_stats(self.stats_ports[0]))

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
        match = re.search(r'job cost:.*kind=4 bytes=(\d+)', feedback)
        assert match is not None, feedback
        size = int(match[1])
        self.assertGreater(size, 2 * 1024 * 1024)
        stats = self.get_stats(self.stats_ports[0])
        vector = re.search(r'dcc_pump_capacity ([^\n]+)', stats)
        assert vector is not None, stats
        self.assertGreater(int(vector[1].split(',')[size.bit_length() - 1]), 0)

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
            response = self.work / 'module.rsp'
            response.write_text(shlex.join(list(map(str, args))))
            wrapped = self.run_cc('g++', ['@' + response.name])
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
    def test_other_clang_release_uses_selected_builtin_headers(self):
        default = shutil.which('clang')
        if not default:
            self.skipTest('Clang required')
        compiler = next((p for p in sorted(Path('/usr/lib/llvm').glob('*/bin/clang'))
                         if p.parents[1].name in ('22', '23') and
                         p.resolve() != Path(default).resolve()), None)
        if compiler is None:
            self.skipTest('an additional Clang release is required')
        version = subprocess.check_output([compiler, '-dumpversion'], text=True).strip()
        resource = Path(subprocess.check_output([compiler, '-print-resource-dir'],
                                                text=True).strip()).resolve()
        wrapper = self.root / 'bin' / ('clang-' + version.split('.')[0])
        wrapper.symlink_to(compiler)
        self.addCleanup(wrapper.unlink)
        self.source.write_text('#include <stddef.h>\n#include <stdint.h>\n'
                               'int main(void) { size_t n = sizeof(uint64_t); return n != 8; }\n')
        result = self.run_cc(str(compiler), ['-O2', '-flto=thin', '-MD', '-MF', 'builtins.d',
                                             '-c', self.source, '-o', 'builtins.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        dependencies = {Path(p).resolve() for p in shlex.split(
            (self.work / 'builtins.d').read_text().replace('\\\n', ''))[1:]}
        self.assertIn(resource / 'include' / 'stddef.h', dependencies)
        subprocess.run([compiler, '-flto=thin', '-fuse-ld=lld', self.work / 'builtins.o',
                        '-o', self.work / 'builtins'],
                       check=True, capture_output=True, timeout=15)
        subprocess.run([self.work / 'builtins'], check=True, timeout=5)
        log = (self.work / 'client.log').read_text()
        self.assertNotIn('gave up analyzing', log)
        self.assertNotIn('retrying locally', log)

    def test_shader_preprocessing_and_frontend_macro_overrides(self):
        self.source.write_text('#ifdef WR_VERTEX_SHADER\n'
                               'layout(location=0) in vec4 position;\n'
                               'void main() { gl_Position = position; }\n#endif\n')
        for compiler in ('gcc', 'clang'):
            for options in (['-undef'], ['-U__clang__', '-U__GNUC__'],
                            ['-D__clang__=1'] if compiler == 'gcc' else ['-U__clang__']):
                with self.subTest(compiler=compiler, options=options):
                    self.compare(compiler, ['-xc', '-P', '-DWR_VERTEX_SHADER=1', *options])

    def test_clang_analyzer_checks_disabled_compile_remotely(self):
        (self.work / 'header.h').write_text('#define ANSWER 42\n')
        self.source.write_text('#include "header.h"\nint main(void) { return ANSWER != 42; }\n')
        for flags in (['-Xclang', '-analyzer-disable-all-checks'],
                      ['-Xclang=-analyzer-disable-all-checks']):
            with self.subTest(flags=flags):
                objects = []
                for remote in (False, True):
                    result = self.run_cc('clang', [*flags, '-c', self.source, '-o', 'check.o'], remote)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    objects.append((self.work / 'check.o').read_bytes())
                self.assertEqual(*objects)
                subprocess.run(['clang', self.work / 'check.o', '-o', self.work / 'check'],
                               check=True, capture_output=True, timeout=15)
                subprocess.run([self.work / 'check'], check=True, timeout=5)
        log = (self.work / 'client.log').read_text()
        self.assertNotIn('gave up analyzing', log)
        self.assertNotIn('retrying locally', log)

    def test_c_module_typedef_compiles_remotely(self):
        self.source.write_text('typedef struct entry *module;\nint main(void) {\n'
                               ' module m = 0;\n return m != 0;\n}\n')
        result = self.run_cc('clang', ['-std=gnu17', '-c', self.source, '-o', 'module.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        subprocess.run(['clang', self.work / 'module.o', '-o', self.work / 'module'],
                       check=True, capture_output=True, timeout=15)
        subprocess.run([self.work / 'module'], check=True, timeout=5)
        log = (self.work / 'client.log').read_text()
        self.assertNotIn('gave up analyzing', log)
        self.assertNotIn('retrying locally', log)

    def test_guarded_header_aliases_compile_remotely(self):
        header, alias = self.work / 'real.h', self.work / 'alias.h'
        declarations = 'struct Once { int member; };\nint value;\nconst char *path = __FILE__;\n'
        for guard in ('#pragma once\n' + declarations,
                      '#ifndef INCLUDED\n#define INCLUDED\n' + declarations + '#endif\n'):
            for link in ('symlink', 'hardlink'):
                alias.unlink(missing_ok=True)
                header.write_text(guard)
                if link == 'symlink':
                    alias.symlink_to(header.name)
                else:
                    os.link(header, alias)
                self.source.write_text('#include "real.h"\n#include "alias.h"\n'
                                       'int main(void) { return value; }\n')
                for compiler in ('gcc', 'clang'):
                    with self.subTest(guard=guard, link=link, compiler=compiler):
                        objects = []
                        for remote in (False, True):
                            result = self.run_cc(compiler, ['-O1', '-c', self.source, '-o', 'alias.o'], remote)
                            self.assertEqual(result.returncode, 0, result.stderr.decode())
                            objects.append((self.work / 'alias.o').read_bytes())
                        self.assertEqual(objects[0], objects[1])
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

    def test_clang_profile_generation_matches_native(self):
        self.check_clang_profile_generation(self.source)

    def test_clang_relative_profile_generation_matches_native(self):
        self.check_clang_profile_generation(Path(self.source.name))

    def test_clang_raw_profile_generation_matches_native(self):
        self.addCleanup(self.env.__setitem__, 'INCLUDE_SERVER_PORT',
                        self.env.pop('INCLUDE_SERVER_PORT'))
        self.check_clang_profile_generation(self.source)

    def test_clang_raw_relative_profile_generation_matches_native(self):
        self.addCleanup(self.env.__setitem__, 'INCLUDE_SERVER_PORT',
                        self.env.pop('INCLUDE_SERVER_PORT'))
        self.check_clang_profile_generation(Path(self.source.name))

    def check_clang_profile_generation(self, source):
        compiler, profdata = shutil.which('clang'), shutil.which('llvm-profdata')
        if not compiler or not profdata:
            self.skipTest('Clang and llvm-profdata are required')
        (self.work / 'profile.h').write_text('static int classify(int n) { return n > 2 ? 7 : 3; }\n')
        self.source.write_text('#include "profile.h"\n'
                               'const char *source_name = __FILE__, *base_name = __BASE_FILE__;\n'
                               'int main(int n, char **v) { return classify(n) != 3; }\n')
        for option in ('-fprofile-generate', '-fprofile-generate=relative profiles',
                       '-fprofile-instr-generate',
                       '-fprofile-instr-generate=' + str(self.work / 'named.profraw')):
            with self.subTest(option=option):
                match = re.search(r'dcc_compile_ok (\d+)', self.get_stats(self.stats_ports[0]))
                assert match is not None
                before = int(match.group(1))
                objects, counts = [], []
                for remote in (False, True):
                    result = self.run_cc(compiler, ['-O1', '-g', '-fno-inline', '-march=native', option,
                                                   '-c', source, '-o', 'profile.o'], remote)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    # AST deserialization may reorder debug type records.
                    # Code/data and live PGO names/counts must still agree.
                    objects.append(self.code_object('profile.o'))
                    subprocess.run([compiler, option, 'profile.o', '-o', 'profile'],
                                   cwd=self.work, check=True, capture_output=True, timeout=15)
                    raw = self.work / ('remote.profraw' if remote else 'native.profraw')
                    subprocess.run([str(self.work / 'profile')], cwd=self.work,
                                   env=dict(self.env, LLVM_PROFILE_FILE=str(raw)),
                                   check=True, capture_output=True, timeout=5)
                    counts.append(subprocess.check_output(
                        [profdata, 'show', '--all-functions', '--counts', str(raw)], timeout=5))
                self.assertEqual(counts[0], counts[1])
                self.assertEqual(objects[0], objects[1])
                self.assertIn(f'dcc_compile_ok {before + 1}\n',
                              self.get_stats(self.stats_ports[0]))
        self.assertNotIn('gave up analyzing', (self.work / 'client.log').read_text())

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
        worker = str(self.build / 'distcc-clang') if compiler.startswith('clang') else native
        wrapper.write_text('#!/bin/sh\n' +
            'if [ "$1" = -dumpversion ]; then exec ' + shlex.quote(native) + ' "$@"; fi\n' +
            "printf '%s\\n' '#error worker header must not be used' > " + shlex.quote(str(header)) + '\n' +
            ': > ' + shlex.quote(str(includes / 'worker-only.h')) + '\n' +
            'case "$1" in -*) exec ' + shlex.quote(native) + ' "$@";; esac\n' +
            'exec ' + shlex.quote(worker) + ' "$@"\n')
        wrapper.chmod(0o755)
        settings = {'DISTCCD_PATH': str(wrappers) + os.pathsep + os.defpath}
        if compiler.startswith('clang'):
            # The Clang worker uses libTooling directly. Intercept that actual
            # execution boundary; a compiler-binary wrapper is never invoked.
            settings['DISTCC_CLANG_DRIVER'] = str(wrapper)
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

    def test_absolute_includes_probes_and_dependencies(self):
        header = self.work / 'absolute.h'
        missing = self.work / 'missing.h'
        header.write_text('int absolute_value;\nconst char *header_file = __FILE__;\n')
        self.source.write_text('#define HEADER "%s"\n#if __has_include(HEADER)\n'
                               '#include HEADER\n#else\n#error missing\n#endif\n'
                               '#if __has_include("%s")\n#error unexpected\n#endif\n'
                               'const char *source_file = __FILE__;\n' %
                               (header, missing))
        for compiler in ('gcc', 'clang'):
            with self.subTest(compiler=compiler):
                self.compare(compiler, args=['-MD', '-MF', 'dep.d'])
                for mapping in (f'-ffile-prefix-map={self.work}=/mapped',
                                '-ffile-prefix-map=/=/mapped/',
                                f'-fmacro-prefix-map={self.work}=/mapped'):
                    with self.subTest(mapping=mapping):
                        self.compare(compiler, args=[mapping, '-MD', '-MF', 'dep.d'])
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
    def test_launcher_with_absolute_gcc_masquerade_uses_remote_pump(self):
        masquerade = self.work / 'masquerade'
        masquerade.mkdir()
        compiler = masquerade / 'g++'
        compiler.symlink_to(self.build / 'distcc')
        self.env['PATH'] = str(masquerade) + os.pathsep + self.env['PATH']
        header = self.work / 'local.h'
        header.write_text('#define ANSWER 42\n')
        source = self.work / 'source.cc'
        source.write_text('#include "local.h"\nint main() { return ANSWER != 42; }\n')
        result = self.run_cc(str(compiler), ['-c', source, '-o', 'source.o'])
        log = (self.work / 'client.log').read_text()
        self.assertEqual(result.returncode, 0, result.stderr.decode() + log[-4000:])
        self.assertIn('pooled TCP job completed', log)
        self.assertNotIn('gave up analyzing', log)
        self.assertNotIn('preprocessing locally', log)
        subprocess.run([self.cc, self.work / 'source.o', '-o', self.work / 'exe'], check=True)
        subprocess.run([self.work / 'exe'], check=True)

    def test_clang_unused_link_inputs_preserve_compile_semantics(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is required')
        archive = self.work / 'unused.a'
        subprocess.run(['ar', 'rcs', archive], check=True)
        before = self.get_stats(self.stats_ports[0])
        for pump in (False, True):
            with self.subTest(pump=pump):
                port = self.env.pop('INCLUDE_SERVER_PORT') if not pump else None
                try:
                    objects = []
                    for remote in (False, True):
                        result = self.run_cc(compiler, ['-c', self.source, archive, '-o', 'result.a'], remote)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        objects.append((self.work / 'result.a').read_bytes())
                    self.assertEqual(*objects)
                finally:
                    if port: self.env['INCLUDE_SERVER_PORT'] = port
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(int(re.search(r'dcc_compile_ok (\d+)', after)[1]) -
                         int(re.search(r'dcc_compile_ok (\d+)', before)[1]), 2)
        # Explicit language makes an archive another frontend input. The
        # driver's original error must not turn into a successful compilation.
        for remote in (False, True):
            result = self.run_cc(compiler, ['-x', 'c', '-c', self.source, archive, '-o', 'bad.o'], remote)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'multiple output files', result.stderr)

    def test_clang_profile_use_transfers_auxiliary_inputs(self):
        compiler, profdata = shutil.which('clang'), shutil.which('llvm-profdata')
        if not compiler or not profdata:
            self.skipTest('Clang and llvm-profdata are required')
        self.source.write_text('static int value(int n) { return n > 2 ? 7 : 3; }\n'
                               'int main(int n, char **v) { return value(n) != 3; }\n')
        subprocess.run([compiler, '-O2', '-fprofile-generate', str(self.source), '-o', 'training'],
                       cwd=self.work, check=True, capture_output=True)
        subprocess.run([str(self.work / 'training')], cwd=self.work, check=True,
                       env=dict(self.env, LLVM_PROFILE_FILE=str(self.work / 'training.profraw')))
        subprocess.run([profdata, 'merge', 'training.profraw', '-o', 'training.profdata'],
                       cwd=self.work, check=True, capture_output=True)
        (self.work / 'default.profdata').hardlink_to(self.work / 'training.profdata')
        records, worker = self.work / 'profile-inodes', self.work / 'clang-worker'
        worker.write_text('#!/usr/bin/python3\nimport os, sys, time\nfrom pathlib import Path\n' +
            'profile = next(Path(sys.argv[1]).rglob("*.profdata"))\n' +
            'st = profile.stat()\n' +
            'with open(' + repr(str(records)) + ', "a") as out:\n' +
            '    out.write(f"{st.st_ino} {st.st_nlink} {st.st_mode & 0o777}\\n")\n' +
            'if st.st_size > 64 * 1024 * 1024:\n' +
            '    deadline = time.monotonic() + 10\n' +
            '    while len(Path(' + repr(str(records)) + ').read_text().splitlines()) < 10:\n' +
            '        if time.monotonic() > deadline: raise RuntimeError("second profile reader did not arrive")\n' +
            '        time.sleep(.01)\n' +
            'os.execv(' + repr(str(self.build / 'distcc-clang')) + ', [' +
            repr(str(self.build / 'distcc-clang')) + ', *sys.argv[1:]])\n')
        worker.chmod(0o755)
        self.restart_server(self.build / 'distccd', DISTCC_CLANG_DRIVER=str(worker))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        before = self.get_stats(self.stats_ports[0])
        for pump in (False, True):
            port = self.env.pop('INCLUDE_SERVER_PORT') if not pump else None
            try:
                for profile in ('training.profdata', str(self.work / 'training.profdata'), '.', None):
                    objects = []
                    for remote in (False, True):
                        option = '-fprofile-use' if profile is None else '-fprofile-use=' + profile
                        result = self.run_cc(compiler, ['-O2', option,
                                                       '-c', self.source, '-o', 'optimized.o'], remote)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        self.assertNotIn(b'profile data may be out of date', result.stderr)
                        objects.append((self.work / 'optimized.o').read_bytes())
                    self.assertEqual(*objects)
                    subprocess.run([compiler, 'optimized.o', '-o', 'optimized'], cwd=self.work, check=True)
                    subprocess.run([str(self.work / 'optimized')], check=True)
            finally:
                if port: self.env['INCLUDE_SERVER_PORT'] = port
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(int(re.search(r'dcc_compile_ok (\d+)', after)[1]) -
                         int(re.search(r'dcc_compile_ok (\d+)', before)[1]), 8)
        self.assertNotIn('include server gave up analyzing', (self.work / 'client.log').read_text())
        inodes = [tuple(map(int, row.split())) for row in records.read_text().splitlines()]
        self.assertEqual(len(inodes), 8)
        self.assertEqual(len({row[0] for row in inodes}), 1, 'profile was copied between jobs')
        self.assertTrue(all(links >= 2 and mode == 0o400 for _, links, mode in inodes))
        # Large idle inputs are reclaimed; concurrent consumers must still pin
        # one shared inode, including profiles larger than the old scan limit.
        with (self.work / 'training.profdata').open('ab') as profile:
            profile.truncate(65 * 1024 * 1024)
        def compile_large(index):
            return self.run_cc(compiler, ['-O2', '-fprofile-use=training.profdata',
                                         '-c', self.source, '-o', f'large{index}.o'])
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(compile_large, range(2)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertNotIn('include server gave up analyzing', (self.work / 'client.log').read_text())
        large = [tuple(map(int, row.split())) for row in records.read_text().splitlines()[8:]]
        self.assertEqual(len(large), 2)
        self.assertEqual(large[0][0], large[1][0], 'concurrent large profiles were copied')
        self.assertTrue(all(links >= 2 and mode == 0o400 for _, links, mode in large))

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
