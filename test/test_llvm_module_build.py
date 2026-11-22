#!/usr/bin/env python3
"""Check per-slot compile and link ABIs on the built helpers."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


class LLVMModuleBuildTest(unittest.TestCase):
    def test_rust_native_links_with_its_compile_abi(self):
        compiler, config = shutil.which('clang++'), shutil.which('llvm-config')
        if not compiler or not config:
            self.skipTest('Clang and LLVM development files required')
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='distcc-rust-abi-') as directory:
            work = Path(directory)
            probe = subprocess.run([compiler, '-stdlib=libc++', '-xc++', '-', '-o', str(work/'probe')],
                                   input='#include <fstream>\nint main() { std::ifstream f; }\n',
                                   text=True, capture_output=True)
            if probe.returncode:
                self.skipTest('libc++ development files required')
            wrapper = work/'llvm-config'
            wrapper.write_text('#!/bin/sh\nif [ "$1" = --cxxflags ]; then\n' +
                               shlex.quote(config) + ' "$@"\nprintf " %s\\n" -stdlib=libc++\n' +
                               'else exec ' + shlex.quote(config) + ' "$@"; fi\n')
            wrapper.chmod(0o755)
            (work/'src').mkdir()
            built = subprocess.run(['make', '-f', str(root/'Makefile.in'), 'distcc-rust-native.so',
                                    'SHELL=/bin/sh', 'srcdir='+str(root),
                                    'CXX='+shlex.quote(compiler)+' -stdlib=libstdc++',
                                    'CXXFLAGS=-O2', 'CPPFLAGS=-I'+shlex.quote(str(root/'src')), 'LDFLAGS=',
                                    'LLVM_CONFIG='+str(wrapper)], cwd=work, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            loaded = subprocess.run(['/bin/true'], capture_output=True,
                                    env=dict(os.environ, LD_PRELOAD=str(work/'distcc-rust-native.so'),
                                             LD_BIND_NOW='1'))
            self.assertEqual((loaded.returncode, loaded.stderr), (0, b''))

    def test_each_llvm_supplies_compile_and_link_abi(self):
        configs = os.environ.get('DISTCC_CLANG_CONFIGS') or os.environ.get('LLVM_CONFIG', 'llvm-config')
        for config in configs.split(os.pathsep):
            with self.subTest(config=config):
                version = subprocess.check_output([config, '--version'], text=True).strip().split('.')[0]
                helper = Path.cwd()/('distcc-clang-' + version)
                self.assertTrue(helper.is_symlink())
                linked = subprocess.check_output(['ldd', str(helper)], text=True)
                self.assertIn('libclang-cpp.so.' + version, linked)
                self.assertNotIn('not found', linked)
                self.assertNotIn('libpython', linked)


if __name__ == '__main__':
    unittest.main()
