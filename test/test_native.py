#!/usr/bin/env python3
"""Compare native CPU compilation through real distccd with local output."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import concurrent.futures

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

    def compare(self, compiler, flags, remote=True, prefix=()):
        output = self.work / 'native.o'
        args = [compiler, '-O3', *flags, '-c', str(self.source), '-o', str(output)]
        subprocess.run(args, check=True, capture_output=True, env=self.env)
        expected = output.read_bytes()
        output.unlink()
        result = subprocess.run([*prefix, str(self.build / 'distcc'), *args], env=self.env,
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
                self.assertIn('native expansion cache hit', self.compare(compiler, flags))

    def test_gcc(self):
        self.cases('gcc')

    def test_clang(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is not installed')
        self.cases(compiler)

    def test_cache_reuse_and_flag_order(self):
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            with self.subTest(compiler=compiler):
                first = self.compare(compiler, ['-march=native'])
                self.assertNotIn('native expansion cache hit', first)
                self.assertIn('native expansion cache hit', self.compare(compiler, ['-march=native']))
                self.assertNotIn('native expansion cache hit', self.compare(compiler, ['-march=native', '-O2']))
                self.assertIn('native expansion cache hit', self.compare(compiler, ['-march=native', '-O2']))

    def test_cache_invalidates_configuration_and_environment(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is not installed')
        self.env['HOME'] = str(self.work)
        directory = self.work / '.config/clang'
        directory.mkdir(parents=True)
        self.compare(compiler, ['-march=native'])
        self.assertIn('native expansion cache hit', self.compare(compiler, ['-march=native']))
        config = directory / 'probe.cfg'
        config.write_text('# configuration generation one\n')
        self.assertNotIn('native expansion cache hit', self.compare(compiler, ['-march=native']))
        self.assertIn('native expansion cache hit', self.compare(compiler, ['-march=native']))
        config.write_text('# configuration generation two\n')
        self.assertNotIn('native expansion cache hit', self.compare(compiler, ['-march=native']))
        self.env['LANG'] = 'C'
        self.assertNotIn('native expansion cache hit', self.compare(compiler, ['-march=native']))

    def test_local_calls_do_not_probe(self):
        for flags in (['-E'], []):
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-march=native',
                                     *flags, str(self.source), '-o', str(self.work / 'local')],
                                    env=self.env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('native option expansion', result.stderr)
            self.assertNotIn('expanded native options', result.stderr)

    def test_portage_sandbox_cache(self):
        if not shutil.which('sandbox'):
            self.skipTest('Gentoo sandbox is not installed')
        self.env.update(SANDBOX_WRITE=str(self.root), SANDBOX_READ='/', SANDBOX_ON='1')
        self.compare('gcc', ['-march=native'], prefix=('sandbox',))
        self.assertIn('native expansion cache hit',
                      self.compare('gcc', ['-march=native'], prefix=('sandbox',)))
        if any((Path(p) / 'libjemalloc.so').exists() for p in ('/usr/lib64', '/usr/lib')):
            self.env['LD_PRELOAD'] = 'libjemalloc.so'
            self.compare('gcc', ['-march=native'], prefix=('sandbox',))
            self.assertIn('native expansion cache hit',
                          self.compare('gcc', ['-march=native'], prefix=('sandbox',)))

    def test_cache_shared_across_source_and_output_paths(self):
        self.compare('gcc', ['-march=native'])
        self.source = self.work / 'other.c'
        self.source.write_text('int other(void) { return 7; }\n')
        args = ['gcc', '-O3', '-march=native', '-c', str(self.source)]
        def compile_one(index):
            return subprocess.run([str(self.build / 'distcc'), *args, '-o', str(self.work / f'{index}.o')],
                                  env=self.env, capture_output=True, text=True, timeout=20)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for result in pool.map(compile_one, range(4)):
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('native expansion cache hit', result.stderr)

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
