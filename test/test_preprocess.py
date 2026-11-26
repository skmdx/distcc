#!/usr/bin/env python3
"""Remote preprocessing with mutable inputs, real compilers and pooled TCP."""
import os
import concurrent.futures
import contextlib
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


class PreparedCompilerTest(BrokerTestCase):
    worker_address = '127.0.0.2'

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}') + ' --localslots_cpp=1')
        self.env.update(DISTCC_VERBOSE='1', DISTCC_LOG=str(self.work/'client.log'),
                        DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_CPP_LOOKUP_LIBRARY=str(self.build/'cpp_lookup.so'))
        self.source = self.work/'source.c'
        self.source.write_text('int value;\n')

    def run_cc(self, compiler, args, remote=True):
        prefix = [str(self.build / 'distcc')] if remote else []
        return subprocess.run(prefix + [compiler] + list(map(str, args)),
                              cwd=self.work, env=self.env, capture_output=True,
                              timeout=15)

    def code_object(self, name):
        output = self.work / (str(name) + '.stripped')
        subprocess.run(['objcopy', '--strip-debug', str(name), str(output)],
                       cwd=self.work, check=True, capture_output=True, timeout=5)
        return output.read_bytes()

    def test_frontend_ir_without_broker(self):
        self.env.pop('DISTCC_BROKER')
        self.env['DISTCC_HOSTS'] = self.host(0, ',lz4')
        self.test_compile_reuses_include_expansion_without_dependency_scan()

    def test_frontend_ir_links_with_lto_and_instrumentation(self):
        source = self.work / 'program.cc'
        source.write_text('static int value;\n'
                          'struct Init { Init() { value = 42; } } init;\n'
                          'extern "C" int answer() { return value; }\n'
                          'extern "C" int alias() __attribute__((alias("answer")));\n'
                          'int main() { return alias() != 42; }\n')
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

    def test_response_files_are_expanded_before_native_options(self):
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
                    self.assertEqual(remote.returncode, 0, remote.stderr.decode())
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

    def test_clang_profile_generation_matches_native(self):
        self.check_clang_profile_generation(self.source)

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

    def test_local_lookup_cache_then_remote_compile(self):
        search = []
        directories = [self.root / ('lookup-' + str(i)) for i in range(300)]
        for directory in directories:
            directory.mkdir()
            search.append('-I' + str(directory))
        (directories[-1] / 'answer.h').write_text('#define ANSWER 42\n')
        self.source.write_text('#include "answer.h"\nint answer(void) { return ANSWER; }\n')
        result = self.run_cc('gcc', [*search, '-c', self.source, '-o', 'lookup.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertTrue((self.work / 'lookup.o').exists())
        self.assertNotIn('getline failed', (self.work / 'client.log').read_text())
        self.assertRegex(self.get_stats(self.stats_ports[0]), r'dcc_compile_ok [1-9]')

    @staticmethod
    def completed(stats):
        match = re.search(r'dcc_compile_ok (\d+)', stats)
        assert match is not None
        return int(match[1])

    def test_clang_unused_link_inputs_preserve_compile_semantics(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is required')
        archive = self.work / 'unused.a'
        subprocess.run(['ar', 'rcs', archive], check=True)
        before = self.get_stats(self.stats_ports[0])
        objects = []
        for remote in (False, True):
            result = self.run_cc(compiler, ['-c', self.source, archive, '-o', 'result.a'], remote)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            objects.append((self.work / 'result.a').read_bytes())
        self.assertEqual(*objects)
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(self.completed(after) - self.completed(before), 1)
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
            '    while len(Path(' + repr(str(records)) + ').read_text().splitlines()) < 6:\n' +
            '        if time.monotonic() > deadline: raise RuntimeError("second profile reader did not arrive")\n' +
            '        time.sleep(.01)\n' +
            'os.execv(' + repr(str(self.build / 'distcc-clang')) + ', [' +
            repr(str(self.build / 'distcc-clang')) + ', *sys.argv[1:]])\n')
        worker.chmod(0o755)
        self.restart_server(self.build / 'distccd', DISTCC_CLANG_DRIVER=str(worker))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        before = self.get_stats(self.stats_ports[0])
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
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(self.completed(after) - self.completed(before), 4)
        inodes = [tuple(map(int, row.split())) for row in records.read_text().splitlines()]
        self.assertEqual(len(inodes), 4)
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
        large = [tuple(map(int, row.split())) for row in records.read_text().splitlines()[4:]]
        self.assertEqual(len(large), 2)
        self.assertEqual(large[0][0], large[1][0], 'concurrent large profiles were copied')
        self.assertTrue(all(links >= 2 and mode == 0o400 for _, links, mode in large))

    def test_clang_relative_profile_generation_matches_native(self):
        self.check_clang_profile_generation(Path(self.source.name))



    def test_metadata_query_bypasses_preprocessing_slot(self):
        lease = self.request(12, cpp_limit=1)
        try:
            status, _, _ = self.receive(lease)
            self.assertEqual(status, 0)
            direct = self.run_cc('gcc', ['-dumpmachine'], False)
            wrapped = self.run_cc('gcc', ['-dumpmachine'])
            self.assertEqual((wrapped.returncode, wrapped.stdout), (0, direct.stdout))
        finally:
            lease.close()

    def test_local_preprocessing_modes(self):
        self.env['DISTCC_FRONTEND_REMOTE'] = '1'
        self.source.write_text('#include <stddef.h>\nsize_t size;\n')
        for compiler, flags in (('gcc', ['-E', '-H']), ('gcc', ['-E', '-MD']),
                                ('gcc', ['-M']), ('gcc', ['-MM']), ('clang', ['-E']),
                                ('gcc', ['-c', '-MMD', '-MF', 'literal.d', '-MT', '-E'])):
            with self.subTest(compiler=compiler, flags=flags):
                args = [*flags, self.source]
                native = self.run_cc(compiler, args, remote=False)
                result = self.run_cc(compiler, args)
                self.assertEqual(native.returncode, 0, native.stderr)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (0, native.stdout, native.stderr))
        trace = (self.work/'client.log').read_text()
        self.assertNotIn('pooled TCP job completed', trace)
        self.assertNotIn('GCC plan memo', trace)

    def test_preprocessing_appends_environment_dependencies_locally(self):
        self.env['DISTCC_FRONTEND_REMOTE'] = '1'
        (self.work/'value.h').write_text('#define VALUE 1\n')
        self.source.write_text('#include "value.h"\nint n = VALUE;\n')
        deps = self.work/'environment.d'
        for variable in ('DEPENDENCIES_OUTPUT', 'SUNPRO_DEPENDENCIES'):
            with self.subTest(variable=variable):
                self.env[variable] = str(deps)
                deps.write_bytes(b'previous.o: previous.c\n')
                native = self.run_cc('gcc', ['-E', self.source], remote=False)
                expected = deps.read_bytes()
                deps.write_bytes(b'previous.o: previous.c\n')
                result = self.run_cc('gcc', ['-E', self.source])
                self.assertEqual(native.returncode, 0, native.stderr)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (0, native.stdout, native.stderr))
                self.assertEqual(deps.read_bytes(), expected)
                del self.env[variable]

    def test_gcc_frontend_packs_large_include_search(self):
        first = self.work / 'first search'
        second = self.work / 'second search'
        for path, value in ((first, 19), (second, 42)):
            path.mkdir()
            (path/'value.h').write_text(f'#define VALUE {value}\n')
        self.source.write_text('#include <value.h>\n_Static_assert(VALUE == 19, "search order");\n')
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_FRONTEND_ROOTS=str(self.work),
                        DISTCC_GCC_PLAN_LIBRARY=str(self.build/'gcc_plan.so'))
        if module := os.environ.get('DISTCC_GCC_SCAN_LIBRARY'):
            self.env['DISTCC_GCC_SCAN_LIBRARY'] = module
            self.restart_server(self.build/'distccd', DISTCC_GCC_SCAN_LIBRARY=module)
        flags = ['-I' + str(first)] * (100000 // len(str(first)))
        result = self.run_cc('gcc', [*flags, '-I' + str(second), '-c', self.source, '-o', 'large.o'])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn('frontend filesystem:', (self.work/'client.log').read_text())

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
                                DISTCC_CLANG_DRIVER=str(self.build / 'distcc-clang'))
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


if __name__ == '__main__':
    unittest.main()
