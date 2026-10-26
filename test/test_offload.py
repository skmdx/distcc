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
        cls.daemon_env = {'DISTCCD_PATH': os.pathsep.join((str(Path.cwd()),
                          str(Path(cls.compiler).parent), os.defpath))}
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
        self.hip_device_only([])

    def test_hip_new_driver_device_only(self):
        self.hip_device_only(['--offload-new-driver'])

    def hip_device_only(self, driver_flags):
        source = self.work / 'device.hip'
        source.write_text('__attribute__((global)) void increment(int *p) { p[0] += 1; }\n')
        obj = self.distributed([*driver_flags, '--offload-device-only', '--offload-arch=gfx900',
                                '-nogpuinc', '-nogpulib', '-O2'], source)
        bundler = str(Path(self.compiler).parent / 'clang-offload-bundler')
        targets = subprocess.check_output([bundler, '-list', '-type=o', '-input=' + str(obj)], text=True)
        target = next(t for t in targets.splitlines() if 'gfx900' in t)
        device = self.work / 'gpu.o'
        subprocess.run([bundler, '-unbundle', '-type=o', '-targets=' + target,
                        '-input=' + str(obj), '-output=' + str(device)], check=True)
        self.assertIn('AMD GPU', subprocess.check_output(['readelf', '-h', device], text=True))

    def test_hip_bundle_native_thinlto(self):
        self.hip_bundle_native_thinlto([])

    def test_hip_legacy_bundle_native_thinlto(self):
        self.hip_bundle_native_thinlto(['--no-offload-new-driver'])

    def hip_bundle_native_thinlto(self, driver_flags):
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
        devices = []
        if shutil.which('amdgpu-arch'):
            detected = subprocess.run(['amdgpu-arch'], capture_output=True, text=True)
            if detected.returncode == 0:
                devices = detected.stdout.split()
        targets = list(dict.fromkeys(devices)) or ['gfx900', 'gfx1030']
        source = self.work / 'kernel.cpp'
        source.write_text('''#include <hip/hip_runtime.h>
__global__ void increment(int *p) { p[0] += 1; }
int main(int argc, char **) {
    if (argc > 1) {
        int count;
        if (hipGetDeviceCount(&count) != hipSuccess || !count) return 1;
        for (int i = 0; i < count; ++i) {
            int *p, n = 7;
            if (hipSetDevice(i) != hipSuccess || hipMalloc(&p, sizeof(n)) != hipSuccess) return 2;
            if (hipMemcpy(p, &n, sizeof(n), hipMemcpyHostToDevice) != hipSuccess) return 3;
            hipLaunchKernelGGL(increment, dim3(1), dim3(1), 0, 0, p);
            if (hipMemcpy(&n, p, sizeof(n), hipMemcpyDeviceToHost) != hipSuccess || n != 8) return 4;
            if (hipFree(p) != hipSuccess) return 5;
        }
    }
    return 0;
}
''')
        flags = ['-xhip', *driver_flags, *('--offload-arch=' + target for target in targets),
                 '--rocm-device-lib-path=' + libraries, '-O3', '-flto=thin',
                 '--offload-compress', '-march=native', '-mtune=native', '-MD']
        obj = self.distributed(flags, source)
        self.assertIn('kernel.cpp', obj.with_suffix('.d').read_text())
        self.assertTrue(obj.with_suffix('.d').read_text().startswith(str(obj) + ':'))
        exe = self.work / 'hip-linked'
        subprocess.run([self.compiler, '-flto=thin', '--hip-link', str(obj),
                        '-lamdhip64', '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe)], check=True, timeout=30)
        if devices:
            subprocess.run([str(exe), '--gpu'], check=True, timeout=30)
        self.assertEqual(obj.read_bytes()[:4], b'BC\xc0\xde')
        fatbinary = self.work / 'embedded.hipfb'
        subprocess.run(['llvm-objcopy', '--dump-section', '.hip_fatbin=' + str(fatbinary),
                        str(exe), '-'], stdout=subprocess.DEVNULL, check=True)
        bundler = str(Path(self.compiler).parent / 'clang-offload-bundler')
        bundles = subprocess.check_output([bundler, '-list', '-type=o',
                                           '-input=' + str(fatbinary)], text=True)
        self.assertEqual({t.rsplit('--', 1)[-1] for t in bundles.splitlines()
                          if not t.startswith('host-')}, set(targets))
        after = completed_jobs()
        self.assertEqual(sum(a - b for a, b in zip(after, before)), 2 * len(targets) + 1)
        self.assertTrue(all(a > b for a, b in zip(after, before)), (before, after))
        feedback = [line for line in (self.work / 'gpu-targets.log').read_text().splitlines()
                    if 'job cost:' in line]
        self.assertEqual(sum('kind=10 ' in line for line in feedback), 2 * len(targets) + 1, feedback)
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
