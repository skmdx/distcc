"""Metadata reuse executes real drivers; the spy only counts child launches."""
import concurrent.futures
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


class CompilerQueryTest(unittest.TestCase):
    def test_real_query_reuse_invalidation_and_parallel_misses(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is required')
        helper = runpy.run_path(str(Path.cwd() / 'distcc-link'))
        query = helper['compiler_query']
        command = [compiler, '-dumpversion']
        expected = subprocess.check_output(command, text=True)
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, HOME=directory):
            config = Path(directory) / '.config/clang/test.cfg'
            config.parent.mkdir(parents=True)
            with patch.object(subprocess, 'check_output', wraps=subprocess.check_output) as launches:
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    self.assertEqual(list(pool.map(query, [command] * 4)), [expected] * 4)
                self.assertEqual(launches.call_count, 1)
                config.write_text('# new configuration\n')
                self.assertEqual(query(command), expected)
                self.assertEqual(launches.call_count, 2)
                self.assertEqual(query(command), expected)
                self.assertEqual(launches.call_count, 2)
                config.write_text('# changed configuration\n')
                self.assertEqual(query(command), expected)
                self.assertEqual(launches.call_count, 3)

    def test_wrappers_are_not_cached(self):
        helper = runpy.run_path(str(Path.cwd() / 'distcc-link'))
        with tempfile.TemporaryDirectory() as directory:
            wrapper = Path(directory) / 'gcc'
            wrapper.write_text('#!/bin/sh\necho first\n')
            wrapper.chmod(0o700)
            self.assertEqual(helper['compiler_query']([str(wrapper), '-dumpmachine']), 'first\n')
            wrapper.write_text('#!/bin/sh\necho second\n')
            self.assertEqual(helper['compiler_query']([str(wrapper), '-dumpmachine']), 'second\n')
