#!/usr/bin/env python3
"""Check per-LLVM ABI flags through the actual extension setup entry point."""
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


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
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='distcc-llvm-flags-') as directory:
            configs = [Path(directory)/f'llvm-config-{v}' for v in (22, 23)]
            for config in configs:
                config.touch()
            flags = {
                '22': '-std=c++17 -D_GLIBCXX_USE_CXX11_ABI=1',
                '23': '-std=c++17 -stdlib=libc++ -I"/path with spaces/include"',
            }

            def query(command, **kwargs):
                version = Path(command[0]).name.rsplit('-', 1)[1]
                return {'--version': version + '.1.0', '--cxxflags': flags[version],
                        '--libdir': f'/llvm{version}/lib',
                        '--bindir': f'/llvm{version}/bin',
                        '--includedir': f'/llvm{version}/include'}[command[1]]

            environment = {'SRCDIR': str(root), 'CPPFLAGS': '-DLIBDIR=/usr/lib',
                           'LLVM_CONFIG': str(configs[1]), 'SYSROOT': '',
                           'DISTCC_CLANG_CONFIGS': os.pathsep.join(map(str, configs))}
            with mock.patch.dict(os.environ, environment), \
                    mock.patch('subprocess.check_output', side_effect=query), \
                    mock.patch('setuptools.setup') as setup:
                runpy.run_path(str(root/'include_server/setup.py'))
            modules = {e.name: e for e in setup.call_args.kwargs['ext_modules']}
            for name, ext in modules.items():
                if 'clang' not in name:
                    self.assertEqual(ext.extra_compile_args, [])
                    continue
                abi = ('-D_GLIBCXX_USE_CXX11_ABI=1' if '.clang22.' in name
                       else '-stdlib=libc++')
                self.assertIn(abi, ext.extra_compile_args, name)
                self.assertIn(abi, ext.extra_link_args, name)
                if abi == '-stdlib=libc++':
                    self.assertIn('-I/path with spaces/include', ext.extra_compile_args)


if __name__ == '__main__':
    unittest.main()
