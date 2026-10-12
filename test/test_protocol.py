#!/usr/bin/env python3
"""Argument framing and TCP buffering through a real compiler/daemon."""
import os
import re
import shutil
import subprocess
import unittest

from test_adaptive import DistccTestCase


class ProtocolTest(DistccTestCase):
    daemon_env = {'DISTCCD_PATH': os.defpath}

    def setUp(self):
        super().setUp()
        self.env = dict(self.base_env, DISTCC_HOSTS=self.host(0),
                        DISTCC_BROKER='', DISTCC_DIR=str(self.ddir),
                        DISTCC_FALLBACK='0', DISTCC_VERBOSE='1', TMPDIR=str(self.work))
        self.source = self.work / 'source with spaces.c'
        self.source.write_text('int main(void) { return 0; }\n')

    def compile(self, flags=(), prefix=()):
        obj = self.work / 'result.o'
        result = subprocess.run([*prefix, str(self.build / 'distcc'), 'gcc',
                                 *flags, '-c', str(self.source), '-o', str(obj)],
                                env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('compiled on ', result.stderr)
        exe = self.work / 'result'
        subprocess.run(['gcc', str(obj), '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe)], check=True)

    def test_large_argument_frame(self):
        # The wire frame exceeds ordinary socket buffers; all flags must arrive.
        self.compile(['-fno-inline'] * 16000)

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_batched_arguments_and_cork(self):
        trace = self.work / 'transport.trace'
        for enabled in ('0', '1'):
            with self.subTest(cork=enabled):
                self.env['DISTCC_TCP_CORK'] = enabled
                self.compile(['-fno-inline'] * 100, prefix=(
                    'strace', '-yy', '-s', '64', '-e', 'trace=write,setsockopt',
                    '-o', str(trace)))
                lines = trace.read_text().splitlines()
                writes = [line for line in lines if re.search(r'write\(\d+<TCP:', line)]
                frames = [line for line in writes if '"ARGC' in line]
                self.assertEqual(len(frames), 1)
                self.assertIn('ARGV', frames[0])
                corks = [line for line in lines if 'TCP_CORK' in line]
                self.assertEqual(len(corks), 2 if enabled == '1' else 0)
                self.assertTrue(all('= 0' in line for line in corks))


if __name__ == '__main__':
    unittest.main()
