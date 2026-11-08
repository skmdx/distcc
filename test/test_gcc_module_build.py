#!/usr/bin/env python3
"""Exercise the GCC module builder's flag contract with a real C++ toolchain."""
import ctypes
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(shutil.which('gcc') and shutil.which('g++') and shutil.which('readelf'),
                     'GCC C++ toolchain and readelf required')
class ModuleBuildTest(unittest.TestCase):
    def build(self, root, flags, source):
        # The small sources isolate the build contract; installed frontend
        # integration tests separately exercise the real GCC module ABI.
        for directory in ('include_server', 'src', 'gcc-source/gcc', 'gcc-source/libcpp/include', 'headers'):
            (root/directory).mkdir(parents=True, exist_ok=True)
        script = root/'include_server/build_gcc_scan.py'
        shutil.copyfile(Path(__file__).resolve().parents[1]/'include_server/build_gcc_scan.py', script)
        version = subprocess.check_output(['gcc', '-dumpfullversion'], text=True).strip()
        (root/'gcc-source/gcc/BASE-VER').write_text(version)
        (root/'gcc-source/gcc/DATESTAMP').write_text('20260101')
        for header in ('gcc-source/libcpp/include/cpplib.h', 'headers/cpplib.h'):
            (root/header).write_text('/* matching test headers */\n')
        (root/'src/gcc_scan.cc').write_text(source)
        (root/'src/gcc_scan_inputs.cc').write_text('int hidden_helper() { return 42; }\n')
        result = subprocess.run([sys.executable, str(script), '--gcc', 'gcc', '--cxx', 'g++',
                '--source', str(root/'gcc-source'), '--plugin-headers', str(root/'headers'),
                '--output', str(root/'output')], env=dict(os.environ, **flags),
                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return Path(result.stdout.strip())

    def test_caller_compile_and_link_flags_with_required_module_abi(self):
        with tempfile.TemporaryDirectory(prefix='distcc-module-flags-') as directory:
            module = self.build(Path(directory), {
                'CXXFLAGS': shlex.join(['-O3', '-flto', '-fno-PIC', '-frtti', '-fexceptions',
                                        '-DMODULE_LABEL="two words"']),
                'LDFLAGS': '-flto -Wl,-soname,distcc-module-flags.so',
                'CPPFLAGS': '-include /nonexistent/distcc-config.h',
            }, '''
#if !defined(__OPTIMIZE__) || !defined(__PIC__) || defined(__GXX_RTTI) || defined(__EXCEPTIONS)
#error caller optimization and required module ABI flags must both apply
#endif
extern "C" __attribute__((visibility("default"))) const char *label() { return MODULE_LABEL; }
''')
            library = ctypes.CDLL(str(module))
            library.label.restype = ctypes.c_char_p
            self.assertEqual(library.label(), b'two words')
            dynamic = subprocess.check_output(['readelf', '-d', str(module)], text=True)
            self.assertIn('[distcc-module-flags.so]', dynamic)

    def test_explicit_empty_flags_do_not_force_optimization(self):
        with tempfile.TemporaryDirectory(prefix='distcc-module-empty-flags-') as directory:
            self.build(Path(directory), {'CXXFLAGS': '', 'LDFLAGS': ''}, '''
#ifdef __OPTIMIZE__
#error an explicitly empty CXXFLAGS must not be replaced with a default
#endif
extern "C" __attribute__((visibility("default"))) int value() { return 42; }
''')


if __name__ == '__main__':
    unittest.main()
