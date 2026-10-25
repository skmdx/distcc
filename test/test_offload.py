#!/usr/bin/env python3
"""AMDGPU jobs through real preprocessing, broker, TCP worker and linker."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

from test_broker import BrokerTestCase


class OffloadTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('DISTCC_TEST_HIP_COMPILER', 'clang++'))
        if not compiler:
            raise unittest.SkipTest('Clang is required')
        cls.compiler = compiler
        cls.daemon_env = {'DISTCCD_PATH': str(Path(cls.compiler).parent) + os.pathsep + os.defpath}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',tcp'))
        # An offload job must bypass the single-frontend pump analyzer.
        self.env.update(DISTCC_VERBOSE='1', INCLUDE_SERVER_PORT=str(self.work / 'absent.sock'))
        self.env['PATH'] = str(self.build) + os.pathsep + self.env.get('PATH', os.defpath)

    def distributed(self, flags, source):
        obj = source.with_suffix('.o')
        args = [self.compiler, *flags, '-c', str(source), '-o', str(obj)]
        result = subprocess.run([str(self.build / 'distcc'), *args], env=self.env,
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(' on 127.0.0.1', result.stderr)
        self.assertIn('prepared AMDGPU offload:', result.stderr)
        self.assertNotIn('include server gave up', result.stderr)
        self.assertGreater(obj.stat().st_size, 0)
        return obj

    def test_opencl(self):
        source = self.work / 'kernel.cl'
        source.write_text('__kernel void increment(__global int *p) { p[0] += 1; }\n')
        flags = ['--target=amdgcn-amd-amdhsa', '-mcpu=gfx900', '-nogpulib', '-O2', '-x', 'cl']
        obj = self.distributed(flags, source)
        output = subprocess.check_output(['readelf', '-h', obj], text=True)
        self.assertIn('AMD GPU', output)

    def test_device_c(self):
        source = self.work / 'device.c'
        source.write_text('int device_add(int a, int b) { return a + b; }\n')
        obj = self.distributed(['--target=amdgcn-amd-amdhsa', '-mcpu=gfx900',
                                '-nogpulib', '-O2', '-x', 'c'], source)
        self.assertIn('AMD GPU', subprocess.check_output(['readelf', '-h', obj], text=True))

    def test_absolute_cmake_compiler_preserves_lto_version(self):
        source = self.work / 'configure.cpp'
        source.write_text('int main() { return 0; }\n')
        obj = self.work / 'configure.o'
        env = dict(self.env)
        env.pop('INCLUDE_SERVER_PORT')
        result = subprocess.run([str(self.build / 'distcc'), self.compiler, '-flto=thin',
                                 '-c', str(source), '-o', str(obj)], env=env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        exe = self.work / 'configure'
        subprocess.run([self.compiler, '-flto=thin', str(obj), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

    def test_hip_device_only(self):
        source = self.work / 'device.hip'
        source.write_text('__attribute__((global)) void increment(int *p) { p[0] += 1; }\n')
        obj = self.distributed(['--offload-device-only', '--offload-arch=gfx900',
                                '-nogpuinc', '-nogpulib', '-O2'], source)
        bundler = str(Path(self.compiler).parent / 'clang-offload-bundler')
        targets = subprocess.check_output([bundler, '-list', '-type=o', '-input=' + str(obj)], text=True)
        target = next(t for t in targets.splitlines() if 'gfx900' in t)
        device = self.work / 'gpu.o'
        subprocess.run([bundler, '-unbundle', '-type=o', '-targets=' + target,
                        '-input=' + str(obj), '-output=' + str(device)], check=True)
        self.assertIn('AMD GPU', subprocess.check_output(['readelf', '-h', device], text=True))

    def test_hip_bundle_native_thinlto(self):
        libraries = os.environ.get('ROCM_DEVICE_LIB_PATH')
        if not libraries:
            self.skipTest('Set ROCM_DEVICE_LIB_PATH to test the installed HIP toolchain')
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}', slots=1) + ' ' +
                          self.host(1, f',tcp,stats={self.stats_ports[1]}', slots=1),
                          name='gpu-targets')
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        def completed_jobs():
            return [int(next(line.split()[1] for line in self.get_stats(port).splitlines()
                             if line.startswith('dcc_compile_ok '))) for port in self.stats_ports]
        before = completed_jobs()
        positions = [os.fstat(log.fileno()).st_size for log in self.logs]
        source = self.work / 'kernel.cpp'
        source.write_text('''#include <hip/hip_runtime.h>
__global__ void increment(int *p) { p[0] += 1; }
int main() { return 0; }
''')
        flags = ['-xhip', '--offload-arch=gfx900', '--offload-arch=gfx1030',
                 '--rocm-device-lib-path=' + libraries, '-O3', '-flto=thin',
                 '--offload-compress', '-march=native', '-mtune=native', '-MD']
        obj = self.distributed(flags, source)
        self.assertIn('kernel.cpp', obj.with_suffix('.d').read_text())
        exe = self.work / 'hip-linked'
        subprocess.run([self.compiler, '-flto=thin', '--hip-link', str(obj),
                        '-lamdhip64', '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe)], check=True, timeout=30)
        self.assertEqual(obj.read_bytes()[:4], b'BC\xc0\xde')
        fatbinary = self.work / 'embedded.hipfb'
        subprocess.run(['llvm-objcopy', '--dump-section', '.hip_fatbin=' + str(fatbinary),
                        str(exe), '-'], stdout=subprocess.DEVNULL, check=True)
        bundler = str(Path(self.compiler).parent / 'clang-offload-bundler')
        targets = subprocess.check_output([bundler, '-list', '-type=o',
                                           '-input=' + str(fatbinary)], text=True)
        self.assertEqual({t.rsplit('--', 1)[-1] for t in targets.splitlines()
                          if not t.startswith('host-')}, {'gfx900', 'gfx1030'})
        after = completed_jobs()
        self.assertEqual(sum(a - b for a, b in zip(after, before)), 3)
        self.assertTrue(all(a > b for a, b in zip(after, before)), (before, after))
        for log, position in zip(self.logs, positions):
            with open(log.name) as worker:
                worker.seek(position)
                self.assertIn('--offload-device-only', worker.read())
        feedback = [line for line in (self.work / 'gpu-targets.log').read_text().splitlines()
                    if 'job cost:' in line]
        self.assertEqual(sum('kind=9 ' in line for line in feedback), 2, feedback)
        self.assertEqual(sum('kind=2 ' in line for line in feedback), 1)
        # A second job exercises the shared device-bitcode cache.
        self.distributed(flags, source)

    def test_multi_gpu_device_only(self):
        source = self.work / 'devices.hip'
        source.write_text('__attribute__((global)) void increment(int *p) { p[0] += 1; }\n')
        obj = self.distributed(['--offload-device-only', '--offload-arch=gfx900',
                                '--offload-arch=gfx1030', '-nogpuinc', '-nogpulib', '-O2'], source)
        bundler = str(Path(self.compiler).parent / 'clang-offload-bundler')
        targets = subprocess.check_output([bundler, '-list', '-type=o', '-input=' + str(obj)], text=True)
        self.assertIn('gfx900', targets)
        self.assertIn('gfx1030', targets)

    def test_gpu_failure_does_not_publish_partial_output(self):
        source = self.work / 'failure.hip'
        source.write_text('''__attribute__((global)) void kernel(int *p) {
#ifdef __gfx900__
    missing_symbol();
#else
    p[0] += 1;
#endif
}
''')
        obj = source.with_suffix('.o')
        obj.write_bytes(b'previous output')
        result = subprocess.run([str(self.build / 'distcc'), self.compiler,
                                 '--offload-arch=gfx900', '--offload-arch=gfx1030',
                                 '-nogpuinc', '-nogpulib', '-c', str(source), '-o', str(obj)],
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing_symbol', result.stderr)
        self.assertEqual(obj.read_bytes(), b'previous output')
        self.assertFalse(list(self.work.glob('distcc-offload-*')))

    def test_hip_compilation_unit_identity(self):
        libraries = os.environ.get('ROCM_DEVICE_LIB_PATH')
        if not libraries:
            self.skipTest('Set ROCM_DEVICE_LIB_PATH to test the installed HIP toolchain')
        flags = ['-xhip', '--offload-arch=gfx900', '--offload-arch=gfx1030',
                 '--rocm-device-lib-path=' + libraries, '-O2']
        objects = []
        for name in ('first', 'second'):
            source = self.work / (name + '.hip')
            source.write_text('''#include <hip/hip_runtime.h>
static __device__ int state;
__global__ void ''' + name + '''(int *p) { p[0] += ++state; }
''')
            objects.append(self.distributed(flags, source))
        main = self.work / 'main.cpp'
        main.write_text('int main() { return 0; }\n')
        exe = self.work / 'hip-multiple-units'
        result = subprocess.run([self.compiler, '--hip-link', *map(str, objects),
                                 str(main), '-lamdhip64', '-o', str(exe)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
