#!/usr/bin/env python3
"""Compare native CPU compilation through real distccd with local output."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

from test_broker import BrokerTestCase


class NativeTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang')
        if compiler:
            cls.daemon_env = {'DISTCCD_PATH': str(Path(compiler).parent) + os.pathsep + os.defpath}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker()
        self.env['DISTCC_VERBOSE'] = '1'
        self.source = self.work / 'native.c'
        self.source.write_text('''
#ifdef __AVX2__
const int avx2 = 1;
#else
const int avx2 = 0;
#endif
void vector_sum(float *restrict a, const float *restrict b) {
    for (int i = 0; i < 128; ++i) a[i] += b[i];
}
int main(void) { float a[128] = {0}, b[128] = {3};
    vector_sum(a, b); return a[0] != 3; }
''')

    def compare(self, compiler, flags, remote=True):
        output = self.work / 'native.o'
        args = [compiler, '-O3', *flags, '-c', str(self.source), '-o', str(output)]
        subprocess.run(args, check=True, capture_output=True, env=self.env)
        expected = output.read_bytes()
        output.unlink()
        result = subprocess.run([str(self.build / 'distcc'), *args], env=self.env,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), expected, result.stderr)
        if remote:
            self.assertIn('expanded native options:', result.stderr)
            self.assertIn(' on 127.0.0.1', result.stderr)
        else:
            self.assertIn('native option expansion unavailable', result.stderr)
            self.assertNotIn(' on 127.0.0.1', result.stderr)
        return result.stderr

    def cases(self, compiler):
        target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
        if target.startswith(('x86_64', 'i686')):
            options = [
                ['-march=native'], ['-mtune=native'],
                ['-march=native', '-mtune=native'],
                ['-mno-avx2', '-march=native'], ['-march=native', '-mno-avx2'],
                ['-march=native', '-march=x86-64'],
                ['-march=x86-64', '-march=native'],
            ]
        elif target.startswith('aarch64'):
            options = [['-mcpu=native'], ['-march=native'], ['-mtune=native']]
        else:
            self.skipTest('native target cases require x86 or AArch64')
        for flags in options:
            with self.subTest(compiler=compiler, flags=flags):
                self.compare(compiler, flags)

    def test_gcc(self):
        self.cases('gcc')

    def test_clang(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is not installed')
        self.cases(compiler)

    def test_unrecognized_probe_keeps_original_command_local(self):
        directory = self.work / 'compiler tools'
        directory.mkdir()
        compiler = directory / 'gcc'
        real = shutil.which('gcc')
        compiler.write_text('#!/bin/sh\nfor arg do\n'
                            '  if [ "$arg" = "-###" ]; then exit 0; fi\ndone\n'
                            'exec ' + real + ' "$@"\n')
        compiler.chmod(0o755)
        self.compare(str(compiler), ['-march=native'], remote=False)


if __name__ == '__main__':
    unittest.main()
