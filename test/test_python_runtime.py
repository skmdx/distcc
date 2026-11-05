#!/usr/bin/env python3
"""Compiler helpers must retain their own dependencies in Python builds."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class PythonBuildEnvironmentTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'Clang required')
    def test_helpers_with_system_site_packages_disabled(self):
        root = Path(__file__).resolve().parents[1]
        clang = shutil.which('clang')
        assert clang is not None
        with tempfile.TemporaryDirectory(prefix='distcc-python-runtime-') as directory:
            obj = Path(directory)/'input.o'
            library = Path(directory)/'linked.so'
            subprocess.run([clang, '-x', 'c', '-fPIC', '-c', '-', '-o', str(obj)],
                           input='int answer(void) { return 42; }\n', text=True,
                           check=True, capture_output=True)
            for helper in ('distcc-link', 'distcc-clang', 'distcc-rust',
                           'distcc-thinlto', 'distcc-offload-link'):
                with self.subTest(helper=helper):
                    result = subprocess.run([sys.executable, '-S', '-c', '''
import ctypes, os, runpy, subprocess, sys
root, helper, compiler, obj, output = sys.argv[1:]
sys.path.insert(0, root)
environment = dict(os.environ)
entry = runpy.run_path(root + '/' + helper)
if helper == 'distcc-link':
    command = entry['clang_link_command']([compiler, '-shared', '-fuse-ld=lld', obj, '-o', output])
    subprocess.run(command, check=True)
    assert ctypes.CDLL(output).answer() == 42
elif helper == 'distcc-clang':
    assert callable(entry['load_frontend'](compiler).compile)
else:
    import xxhash
    assert xxhash.xxh3_64(b'input').digest()
assert dict(os.environ) == environment
assert subprocess.check_output([sys.executable, '-c',
    'import os; print(os.environ["GENTOO_CPYTHON_BUILD"])'], text=True).strip() == '1'
''', str(root), helper, clang, str(obj), str(library)],
                        env=dict(os.environ, GENTOO_CPYTHON_BUILD='1'),
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
