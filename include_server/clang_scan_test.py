"""Exercise the actual libclang-cpp extension and its filesystem contract."""
import os
from pathlib import Path
import shutil
import shlex
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

import basics
import clang_scan
import cache_basics
import compiler_analyzer
import compress_files


class ClangScanTest(unittest.TestCase):
  def setUp(self):
    self.storage = tempfile.TemporaryDirectory()
    self.addCleanup(self.storage.cleanup)
    self.root = Path(self.storage.name)
    self.source = self.root / 'source.c'
    self.compiler = shutil.which('clang')
    if not self.compiler:
      self.skipTest('Clang is required')

  def scan(self):
    assert self.compiler is not None
    return clang_scan.scan([self.compiler, '-c', str(self.source), '-I', str(self.root),
                            '-o', str(self.root / 'never.o')], str(self.root))

  def test_probe_tokens_preserve_nested_macros_and_ignore_strings(self):
    source = (b'#define __has_include(header) 0\n'
              b'const char *s = "__has_include(fake.h)";\n'
              b'#if __has_include(MAKE_HEADER(foo, nested(bar)))\n#endif\n'
              b'#if __has_include_next(<next.h>)\n#endif\n'
              b'#if __has_include/*x*/(/*y*/"comment.h")\n#endif\n')
    self.assertEqual(clang_scan.include_probes(source),
                     [(0, 'MAKE_HEADER(foo, nested(bar))'), (1, '<next.h>'),
                      (0, '     "comment.h"')])

  def test_gcc_rejects_absolute_forwarder_before_clang_preprocessing(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    header = self.root / 'real.h'
    header.write_text('int value;\n')
    (self.root / 'forward.h').write_text('#include "%s"\n' % header)
    self.source.write_text('#include "forward.h"\n')
    cwd = os.getcwd()
    self.addCleanup(os.chdir, cwd)
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    with mock.patch.object(clang_scan, 'scan') as frontend:
      with self.assertRaisesRegex(basics.NotCoveredError, 'Filepath must be relative'):
        analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper)
      self.assertEqual(frontend.call_count, 0)

  def test_compilation_from_filesystem_root(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.source.write_text('int value;\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    files = analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], '/', keeper, 'lz4s')
    self.assertTrue(any('/.distcc-system/' in path for path in files))

  def test_gcc_direct_extern_access_flags_keep_compiler_analysis(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    macros = [gcc, '-dM', '-E', '-x', 'c', '-']
    baseline = subprocess.check_output(macros, input=b'')
    self.source.write_text('extern int value; int f(void) { return value; }\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    for flag in ('-mdirect-extern-access', '-mno-direct-extern-access'):
      with self.subTest(flag=flag):
        probe = subprocess.run([*macros, flag], input=b'', capture_output=True)
        if probe.returncode:
          self.skipTest('GCC target does not support direct extern access options')
        self.assertEqual(probe.stdout, baseline)
        files = analyzer.DoCompilationCommand([gcc, flag, '-c', str(self.source)],
                                             str(self.root), keeper, 'lz4s')
        self.assertTrue(any('/.distcc-system/' in path for path in files))

  def test_missing_default_system_directory_is_verified_and_invalidated(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    missing = self.root / 'default-headers'
    compiler = self.root / 'gcc'
    compiler.write_text('#!/bin/sh\nexec %s -isystem %s "$@"\n' %
                        (shlex.quote(gcc), shlex.quote(str(missing))))
    compiler.chmod(0o755)
    self.source.write_text('#if __has_include(<feature.h>)\nint yes;\n#else\nint no;\n#endif\n')
    cwd = os.getcwd()
    self.addCleanup(os.chdir, cwd)
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    def checks():
      with mock.patch.object(compress_files, 'WriteCompressed',
                             wraps=compress_files.WriteCompressed) as writer:
        analyzer.DoCompilationCommand([str(compiler), '-c', str(self.source)],
                                      str(self.root), keeper, 'lz4s')
      return next(call.args[1].getvalue() for call in writer.call_args_list
                  if '/.distcc-system/' in call.args[0])
    path = os.fsencode(missing)
    negative = struct.pack('!IIQ8s', len(path), 0, 0, bytes(8)) + path
    self.assertIn(negative, checks())
    missing.mkdir()
    (missing / 'feature.h').write_text('/* existence probe */\n')
    updated = checks()
    self.assertNotIn(negative, updated)
    self.assertIn(os.fsencode(missing / 'feature.h'), updated)
    (missing / 'feature.h').write_text('/* different probe */\n')
    self.assertNotEqual(updated, checks())

  def test_probe_only_header_and_immutable_contents(self):
    header = self.root / 'probe.h'
    header.write_bytes(b'original contents\n')
    self.source.write_text('#if __has_include("probe.h")\nint yes;\n#endif\n')
    result = self.scan()
    self.assertIn(str(header), result['dependencies'])
    self.assertEqual(result['files'][str(header)], b'original contents\n')
    header.write_bytes(b'changed contents\n')
    self.assertEqual(result['files'][str(header)], b'original contents\n')
    stamp = result['lookups'][str(header)]
    assert stamp is not None and len(stamp) == 6
    self.assertNotEqual(stamp[4], os.stat(header).st_mtime_ns)
    self.assertFalse((self.root / 'never.o').exists())

  def test_directory_stamps_survive_unrelated_outputs(self):
    self.source.write_text('#if __has_include("generated.h")\nint yes;\n#endif\n')
    result = self.scan()
    self.assertIn(str(self.root), result['lookups'])
    (self.root / 'unrelated.o').write_bytes(b'object')
    for path, expected in result['lookups'].items():
      self.assertEqual(cache_basics.FileStampCache.Stamp(path), expected, path)
    header = self.root / 'generated.h'
    self.assertIsNone(result['lookups'][str(header)])
    header.write_text('/* generated */\n')
    self.assertNotEqual(cache_basics.FileStampCache.Stamp(str(header)),
                        result['lookups'][str(header)])

  def test_conditions_are_extracted_without_comments_or_macro_bodies(self):
    source = (b'#define HAS(x) __has_include(x)\n'
              b'/* #if hidden */\nconst char *s = "#if hidden";\n'
              b'#if HAS("a.h") /* explanation */\nint a;\n'
              b'#elif JOIN(__has_,include)("b.h")\n#endif\n')
    self.assertEqual([value.strip() for value in clang_scan.conditional_expressions(source)],
                     ['HAS("a.h")', 'JOIN(__has_,include)("b.h")'])

  def test_indirect_probe_is_not_silently_ignored(self):
    for source in (b'#define HAS __has_include\n',
                   b'#define HAS(x) __has_include(x)\n',
                   b'#define HAS(x) __has_include_next(x)\n',
                   b'#if CALL(__has_include, "probe.h")\n#endif\n'):
      with self.subTest(source=source):
        with self.assertRaisesRegex(ValueError, 'indirect header existence probe'):
          clang_scan.include_probes(source)
    self.assertEqual(clang_scan.include_probes(
        b'#ifdef __has_include\n#endif\n#if defined(__has_include)\n#endif\n'
        b'#if defined __has_include_next\n#endif\n'), [])

  def test_failed_scan_is_not_reused_after_header_creation(self):
    self.source.write_text('#include "generated.h"\nint n = VALUE;\n')
    with self.assertRaises(RuntimeError):
      self.scan()
    header = self.root / 'generated.h'
    header.write_text('#define VALUE 17\n')
    self.assertIn(str(header), self.scan()['dependencies'])
    header.unlink()
    with self.assertRaises(RuntimeError):
      self.scan()

  def test_module_declarations_require_local_preprocessing(self):
    command = [self.compiler, '-x', 'c++', '-std=c++20', '-c', str(self.source)]
    for text in ('export module example;', 'import example;',
                 'module;\nexport module example;', 'export import example;'):
      with self.subTest(text=text):
        self.source.write_text(text + '\n')
        with self.assertRaises(RuntimeError):
          clang_scan.scan(command, str(self.root))
    self.source.write_text('/* import example; */\nconst char *s = "export module example;";\n')
    self.assertIn(str(self.source), clang_scan.scan(command, str(self.root))['files'])


if __name__ == '__main__':
  unittest.main()
