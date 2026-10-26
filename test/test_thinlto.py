#!/usr/bin/env python3
"""Real LLD planning, indexed imports, distributed backends and final links."""
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
import unittest

from test_broker import BrokerTestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distcc_job import fingerprint, write_metadata


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
        try:
            return subprocess.run(list(map(str, command)), cwd=self.work, env=self.env,
                                  text=True, capture_output=True, timeout=60, **kwargs)
        except subprocess.TimeoutExpired as error:
            broker = self.work / 'broker.log'
            details = broker.read_text()[-8192:] if broker.exists() else ''
            self.fail(f'command timed out: {command}\n{error.stderr!r}\n{details}')

    def link(self, compiler, archive=False, hip=False):
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
        if hip:
            args.extend(['--hip-link', '--offload-arch=gfx900'])
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

    def test_ranked_submission_survives_slow_process_start(self):
        """Use real workers while delaying the largest job before broker IPC."""
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_broker(' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}')
                                  for i in range(2)), name='ranked')
        planner = self.work / 'planner'
        planner.mkdir()
        for name in ('distcc-thinlto', 'distcc_lto.py', 'distcc_job.py'):
            shutil.copy2(self.build / name, planner / name)
        proxy = planner / 'distcc'
        proxy.write_text('#!/usr/bin/env python3\nimport os, sys, time\n'
                         'if len(sys.argv) > 2 and sys.argv[2].endswith("/0/job"):\n'
                         '    time.sleep(0.2)\n'
                         'os.execv(' + repr(str(self.build / 'distcc')) + ', sys.argv)\n')
        proxy.chmod(0o755)
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        jobs, sizes, objects = [], [], []
        for index, size in enumerate((8192, 2048, 512)):
            source = self.work / f'part{index}.c'
            source.write_text(f'int value{index}(void) {{ return {index + 1}; }}\n'
                              f'const char pad{index}[] = "' + 'a' * size + '";\n')
            bitcode = source.with_suffix('.bc')
            result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
            self.assertEqual(result.returncode, 0, result.stderr)
            output = source.with_suffix('.o')
            objects.append(output)
            sizes.append(bitcode.stat().st_size)
            jobs.append(dict(args=['-O2', '-c', str(bitcode), '-o', str(output)],
                             inputs=[str(bitcode)], outputs=[str(output)]))
        description = self.work / 'jobs.json'
        description.write_text(json.dumps(dict(common=dict(args=[compiler], inputs=[]), jobs=jobs)))
        command = [planner / 'distcc-thinlto', description]
        result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        assigned = list(map(int, re.findall(r'assigning host=.* source=(\d+)',
                                           (self.work / 'ranked.log').read_text())))
        self.assertEqual(assigned, sorted(sizes, reverse=True))
        main = self.work / 'main.c'
        main.write_text('int value0(void), value1(void), value2(void);\n'
                        'int main(void) { return value0()+value1()+value2()!=6; }\n')
        result = self.run_command([compiler, main, *objects, '-o', 'check'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([self.work / 'check']).returncode, 0)
        # A child failing before enqueueing must release the startup barrier
        # and leave previously published objects intact.
        previous = [path.read_bytes() for path in objects]
        self.env.update(DISTCC_BROKER=str(self.work / 'absent.sock'), DISTCC_RECONNECT_TIMEOUT='0')
        self.assertNotEqual(self.run_command(command).returncode, 0)
        self.assertEqual([path.read_bytes() for path in objects], previous)

    def test_closed_submission_notification_cancels_without_reconnect(self):
        inputs = self.work / 'inputs'
        inputs.mkdir()
        source = inputs / '0'
        source.write_bytes(b'')
        metadata, output = self.work / 'job', self.work / 'result.tar'
        write_metadata(4, str(self.work), 'distcc-thinlto',
                       [(str(self.work / 'empty.bc'), *fingerprint(source))], metadata)
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([str(self.build / 'distcc'), '--file-job', str(metadata),
                str(inputs), str(output), str(write_fd)], env=self.env, capture_output=True,
                pass_fds=(write_fd,), timeout=3)
        finally:
            os.close(write_fd)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_hip_wrapper_distributes_host_thinlto(self):
        self.link(shutil.which('clang'), hip=True)

    def test_hip_device_links_and_host_backends(self):
        """Resolve a cross-TU GPU call on two targets, then link the host."""
        compiler = shutil.which('clang++')
        devices = []
        if shutil.which('amdgpu-arch'):
            detected = self.run_command(['amdgpu-arch'])
            if detected.returncode == 0:
                devices = detected.stdout.split()
        targets = list(dict.fromkeys(devices)) or ['gfx900', 'gfx1030']
        flags = ['-O2', '-flto=thin', '-fgpu-rdc',
                 *('--offload-arch=' + target for target in targets)]
        sources = (
            '__attribute__((device)) int value(int n) { return n + 17; }',
            '#include <hip/hip_runtime.h>\n'
            '__attribute__((device)) int value(int);\n'
            '__global__ void kernel(int *p) { *p = value(*p); }\n'
            '#include <cstdio>\n'
            'int main(int argc, char **) { if (argc == 1) return 0; '
            'int count = 0; if (hipGetDeviceCount(&count) != hipSuccess || !count) return 10; '
            'for (int i = 0; i < count; ++i) { int *p, n = 5; '
            'if (hipSetDevice(i) != hipSuccess || hipMalloc(&p, sizeof(n)) != hipSuccess) return 11; '
            'if (hipMemcpy(p, &n, sizeof(n), hipMemcpyHostToDevice) != hipSuccess) return 12; '
            'hipLaunchKernelGGL(kernel, dim3(1), dim3(1), 0, 0, p); '
            'if (hipMemcpy(&n, p, sizeof(n), hipMemcpyDeviceToHost) != hipSuccess || n != 22) return 13; '
            'if (hipFree(p) != hipSuccess) return 14; } '
            'std::printf("%d GPU devices checked\\n", count); return 0; }',
        )
        objects = []
        for index, source in enumerate(sources):
            path = self.work / f'device {index}.hip'
            path.write_text(source)
            obj = path.with_suffix('.o')
            result = self.run_command([compiler, *flags, '-c', path, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        args = [compiler, *flags, '--hip-link', '-fuse-ld=lld', *objects, '-lamdhip64']
        for prefix, name in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            output = self.work / name
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-offload-link: compiled on'), len(targets),
                                 result.stderr)
                self.assertGreater(result.stderr.count('distcc-thinlto: compiled on'), 0,
                                   result.stderr)
            self.assertEqual(self.run_command([output]).returncode, 0)
            if devices:
                runtime = self.run_command([output, '--gpu'])
                self.assertEqual(runtime.returncode, 0, runtime.stdout + runtime.stderr)
                print(name + ': ' + runtime.stdout.strip(), flush=True)
            image = self.work / (name + '.hipfb')
            dump = self.run_command(['llvm-objcopy', '--dump-section',
                                     '.hip_fatbin=' + str(image), output, '/dev/null'])
            self.assertEqual(dump.returncode, 0, dump.stderr)
            bundles = self.run_command(['clang-offload-bundler', '-list', '-type=o',
                                        '-input=' + str(image)])
            self.assertEqual(bundles.returncode, 0, bundles.stderr)
            for target in targets:
                self.assertIn(target, bundles.stdout)

    def test_target_prefixed_compiler(self):
        selected = shutil.which('clang')
        assert selected is not None
        clang = Path(selected)
        target = subprocess.check_output([clang, '-dumpmachine'], text=True).strip()
        compiler = clang.parent / (target + '-clang++')
        if not compiler.is_file():
            self.skipTest('target-prefixed Clang is required')
        self.link(compiler)

    def test_static_constructor_order(self):
        objects = []
        for index, name in enumerate(('z_first', 'a_second', 'm_third')):
            source = self.work / (name + '.cpp')
            source.write_text('#include <cstdio>\nnamespace { struct Init { Init() { std::puts("' +
                              str(index) + '"); } }; Init init; }\n')
            obj = source.with_suffix('.o')
            result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        main = self.work / 'main.cpp'
        main.write_text('int main() {}\n')
        result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', main, '-o', 'main.o'])
        self.assertEqual(result.returncode, 0, result.stderr)
        args = ['clang++', '-O2', '-flto=thin', '-fuse-ld=lld', *objects, 'main.o']
        for prefix, output in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 4, result.stderr)
            actual = self.run_command([self.work / output])
            self.assertEqual(actual.stdout, '0\n1\n2\n')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
    def test_cmake_link_launcher(self):
        for language, suffix, driver in (('C', 'c', 'clang'), ('HIP', 'hip', 'clang++')):
            with self.subTest(language=language):
                source = self.work / language
                source.mkdir()
                (source / ('part.' + suffix)).write_text('int value(void) { return 42; }')
                (source / ('main.' + suffix)).write_text('int value(void); int main(void) { return value()!=42; }')
                (source / 'CMakeLists.txt').write_text(
                    f'cmake_minimum_required(VERSION 3.21)\nproject(distributed_lto {language})\n'
                    f'add_executable(check main.{suffix} part.{suffix})\n'
                    'target_compile_options(check PRIVATE -flto=thin)\n'
                    'target_link_options(check PRIVATE -flto=thin -fuse-ld=lld)\n')
                self.env[f'CMAKE_{language}_LINKER_LAUNCHER'] = 'distcc'
                self.env[f'CMAKE_{language}_COMPILER_LAUNCHER'] = 'distcc'
                compiler = shutil.which(driver)
                assert compiler is not None
                build = source / 'build'
                result = self.run_command(['cmake', '-S', source, '-B', build, '-G', 'Ninja',
                                           f'-DCMAKE_{language}_COMPILER=' + compiler,
                                           *(['-DCMAKE_HIP_ARCHITECTURES=gfx900'] if language == 'HIP' else [])])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = self.run_command(['cmake', '--build', build])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((result.stdout + result.stderr).count('distcc-thinlto: compiled on'), 2)
                self.assertEqual(self.run_command([build / 'check']).returncode, 0)

    def test_absolute_clang_preserves_client_target(self):
        selected = shutil.which('clang')
        assert selected is not None
        compiler = Path(selected).resolve()
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
