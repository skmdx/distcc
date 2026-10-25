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
        cls.compiler = shutil.which(os.environ.get('DISTCC_TEST_HIP_COMPILER', 'clang++'))
        if not cls.compiler:
            raise unittest.SkipTest('Clang is required')
        cls.daemon_env = {'DISTCCD_PATH': str(Path(cls.compiler).parent) + os.pathsep + os.defpath}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',tcp'))
        # An offload job must bypass the single-frontend pump analyzer.
        self.env.update(DISTCC_VERBOSE='1', INCLUDE_SERVER_PORT=str(self.work / 'absent.sock'))

    def distributed(self, flags, source):
        obj = self.work / 'offload.o'
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
        source = self.work / 'kernel.cpp'
        source.write_text('''#include <hip/hip_runtime.h>
__global__ void increment(int *p) { p[0] += 1; }
int main() { return 0; }
''')
        flags = ['-xhip', '--offload-arch=gfx900', '--offload-arch=gfx1030',
                 '--rocm-device-lib-path=' + libraries, '-O3', '-flto=thin',
                 '-march=native', '-mtune=native', '-MD']
        obj = self.distributed(flags, source)
        self.assertIn('kernel.cpp', obj.with_suffix('.d').read_text())
        exe = self.work / 'hip-linked'
        subprocess.run([self.compiler, '-flto=thin', '--hip-link', str(obj),
                        '-lamdhip64', '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe)], check=True, timeout=30)
        # A second job exercises the shared device-bitcode cache.
        self.distributed(flags, source)


if __name__ == '__main__':
    unittest.main()
