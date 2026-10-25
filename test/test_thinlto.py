#!/usr/bin/env python3
"""Real LLD planning, indexed imports, distributed backends and final links."""
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import unittest

from test_broker import BrokerTestCase


@unittest.skipUnless(shutil.which('clang') and shutil.which('bwrap'), 'Clang and bubblewrap required')
class ThinLTOTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon_env = {'DISTCCD_PATH': str(Path.cwd()) + os.pathsep + os.environ['PATH']}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        hosts = ' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2))
        self.start_broker(hosts)
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        self.env.pop('DISTCC_LINK_SYSROOT', None)

    def run_command(self, command, **kwargs):
        return subprocess.run(list(map(str, command)), cwd=self.work, env=self.env,
                              text=True, capture_output=True, timeout=60, **kwargs)

    def link(self, compiler, archive=False):
        objects = []
        for i in range(8):
            source = self.work / f'part {i}.c'
            source.write_text(f'int part{i}(int n) {{ int v = 0; '
                              f'for (int j=0;j<n;j++) v += (j * {i+1}) % 101; return v; }}')
            obj = source.with_suffix('.o')
            result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        source = self.work / 'main.c'
        source.write_text('\n'.join(f'int part{i}(int);' for i in range(8)) +
                          '\n#include <stdio.h>\nint main(int argc, char **argv) { '
                          'printf("%d\\n", ' + '+'.join(f'part{i}(argc*23)' for i in range(8)) +
                          '); return 0; }')
        main = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', main])
        self.assertEqual(result.returncode, 0, result.stderr)
        if archive:
            library = self.work / 'library.a'
            result = self.run_command(['llvm-ar', 'rcs', library, *objects])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects = [library]
        args = [compiler, '-flto=thin', '-O2', '-fuse-ld=lld', main, *objects]
        reference = self.work / 'reference'
        result = self.run_command([*args, '-o', reference])
        self.assertEqual(result.returncode, 0, result.stderr)
        before = [self.get_stats(port) for port in self.stats_ports]
        output = self.work / 'distributed'
        result = self.run_command([self.build / 'distcc', *args, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 9, result.stderr)
        for port, stats in zip(self.stats_ports, before):
            def completed(text):
                return int(next(line.split()[1] for line in text.splitlines()
                                if line.startswith('dcc_compile_ok ')))
            self.assertGreater(completed(self.get_stats(port)), completed(stats))
        for arguments in ([], ['argument'], ['one', 'two']):
            expected = self.run_command([reference, *arguments])
            actual = self.run_command([output, *arguments])
            self.assertEqual((actual.returncode, actual.stdout), (expected.returncode, expected.stdout))

    def test_objects_and_imports(self):
        self.link(shutil.which('clang'))

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
    def test_cmake_link_launcher(self):
        (self.work / 'part.c').write_text('int value(void) { return 42; }')
        (self.work / 'main.c').write_text('int value(void); int main(void) { return value()!=42; }')
        (self.work / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(distributed_lto C)\n'
            'add_executable(check main.c part.c)\n'
            'set_property(TARGET check PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)\n')
        self.env.update(CMAKE_C_LINKER_LAUNCHER='distcc', CMAKE_C_COMPILER_LAUNCHER='distcc')
        result = self.run_command(['cmake', '-S', '.', '-B', 'build', '-G', 'Ninja',
                                   '-DCMAKE_C_COMPILER=' + shutil.which('clang'),
                                   '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_command(['cmake', '--build', 'build'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((result.stdout + result.stderr).count('distcc-thinlto: compiled on'), 2)
        self.assertEqual(self.run_command([self.work / 'build/check']).returncode, 0)

    def test_absolute_clang_preserves_client_target(self):
        compiler = Path(shutil.which('clang')).resolve()
        target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
        other = 'x86_64-linux-gnu' if target.startswith('aarch64') else 'aarch64-linux-gnu'
        wrappers = self.work / 'bin'
        wrappers.mkdir()
        wrapper = wrappers / compiler.name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler)) +
                           ' --target=' + other + ' "$@"\n')
        wrapper.chmod(0o755)
        self.restart_server(self.build / 'distccd', DISTCCD_PATH=str(wrappers) + os.pathsep + os.defpath)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        saved = dict(self.env)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='target')
        saved['DISTCC_BROKER'] = self.env['DISTCC_BROKER']
        self.env = saved
        source = self.work / 'target.c'
        source.write_text('int main(void) { return 0; }')
        obj = self.work / 'target.o'
        result = self.run_command([self.build / 'distcc', compiler, '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        output = self.work / 'target'
        result = self.run_command([compiler, obj, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_archive_and_selected_compiler(self):
        compiler = '/usr/lib/llvm/22/bin/clang'
        if not Path(compiler).exists():
            self.skipTest('second Clang toolchain not installed')
        self.link(compiler, archive=True)

    def test_failure_propagates_without_local_retry(self):
        source = self.work / 'invalid.o'
        source.write_bytes(b'invalid bitcode')
        index = self.work / 'invalid.thinlto.bc'
        index.write_bytes(b'invalid index')
        output = self.work / 'result.o'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c'], 'inputs': []},
            'jobs': [{'args': ['-x', 'ir', str(source), '-fthinlto-index=' + str(index), '-o', str(output)],
                      'inputs': [str(source), str(index)], 'outputs': [str(output)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertNotIn('retrying backend locally', result.stderr)

    def test_lld_cache_avoids_backend_reexecution(self):
        source = self.work / 'main.c'
        source.write_text('int main(void) { return 0; }')
        obj = source.with_suffix('.o')
        result = self.run_command(['clang', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        command = [self.build / 'distcc', 'clang', '-flto=thin', '-fuse-ld=lld', obj,
                   '-Wl,--thinlto-cache-dir=' + str(self.work / 'cache'), '-o', self.work / 'linked']
        for count in (1, 0):
            result = self.run_command(command)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), count, result.stderr)
            self.assertEqual(self.run_command([self.work / 'linked']).returncode, 0)

    def test_multiple_declared_outputs(self):
        source = self.work / 'input.bc'
        result = self.run_command(['clang', '-x', 'c', '-flto=thin', '-c', '-o', source, '-'],
                                  input='int value(void) { return 42; }')
        self.assertEqual(result.returncode, 0, result.stderr)
        output, trace = self.work / 'output.o', self.work / 'trace.json'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c', '-x', 'ir'], 'inputs': []},
            'jobs': [{'args': [str(source), '-o', str(output), '-ftime-trace=' + str(trace)],
                      'inputs': [str(source)], 'outputs': [str(output), str(trace)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-thinlto: compiled on', result.stderr)
        self.assertEqual(output.read_bytes()[:4], b'\x7fELF')
        self.assertIn('traceEvents', json.loads(trace.read_text()))


if __name__ == '__main__':
    unittest.main()
