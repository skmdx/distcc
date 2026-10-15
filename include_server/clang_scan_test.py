"""Exercise the actual libclang-cpp extension and its filesystem contract."""
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest
from unittest import mock
import xxhash

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

  def test_system_header_aliases_are_transferred(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    sysroot = self.root / 'sysroot'
    system = sysroot / 'usr/include'
    system.mkdir(parents=True)
    (system / 'present.h').write_text('int present;\n')
    (system / 'alias.h').symlink_to('present.h')
    self.source.write_text('#include <alias.h>\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    files = analyzer.DoCompilationCommand(
        [gcc, '--sysroot=' + str(sysroot), '-c', str(self.source)], str(self.root), keeper)
    self.assertTrue(any(path.endswith(str(system / 'present.h') + '.lzo') for path in files))
    self.assertTrue(any(path.endswith(str(system / 'alias.h')) for path in files))
    assert keeper.client_root is not None
    self.assertFalse((Path(keeper.client_root) / str(system).lstrip('/')).is_symlink())

  def test_dependency_probe_keeps_replayable_output_private(self):
    gcc = shutil.which('gcc')
    library = Path(__file__).resolve().parents[1] / 'cpp_lookup.so'
    if not gcc or not library.exists() or not hasattr(os, 'memfd_create'):
      self.skipTest('GCC lookup cache is required')
    self.source.write_text('#if __has_include("optional.h")\n#include "optional.h"\n#endif\n')
    command = [gcc, '-E', '-M', str(self.source)]
    real_run = subprocess.run
    outputs = []
    def checked_run(*args, **kwargs):
      output = kwargs['stdout']
      self.assertTrue(output.seekable())
      self.assertEqual(kwargs['env']['DISTCC_CPP_LOOKUP'], '1')
      self.assertIn(str(library), kwargs['env']['LD_PRELOAD'])
      outputs.append(output)
      return real_run(*args, **kwargs)
    with mock.patch.dict(os.environ, DISTCC_CPP_LOOKUP_LIBRARY=str(library),
                         DISTCC_CPP_LOOKUP='1'):
      for present in (False, True):
        if present:
          (self.root / 'optional.h').write_text('int optional;\n')
        expected = real_run(command, capture_output=True, check=True)
        with mock.patch.object(subprocess, 'run', side_effect=checked_run):
          actual = compiler_analyzer.run(command)
        self.assertEqual(actual.stdout, expected.stdout)
        self.assertEqual(actual.stderr, expected.stderr)
        self.assertEqual(b'optional.h' in actual.stdout, present)
    self.assertTrue(all(output.closed for output in outputs))

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

  def test_gcc_literal_graph_and_computed_verification_use_real_frontends(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    (self.root / 'gcc.h').write_text('int gcc_only;\n')
    (self.root / 'clang.h').write_text('int clang_only;\n')
    for computed in (False, True):
      with self.subTest(computed=computed):
        operand = 'HEADER' if computed else '"gcc.h"'
        self.source.write_text(
            '#define HEADER "gcc.h"\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#\\\ninc\\\nlude ' + operand + '\n'
            '#else\n%:include "clang.h"\n#endif\n')
        keeper = basics.ClientRootKeeper()
        self.addCleanup(keeper.CleanOutClientRoots)
        analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
        with (mock.patch.object(compiler_analyzer, 'run', wraps=compiler_analyzer.run) as probe,
              mock.patch.object(clang_scan, 'scan', wraps=clang_scan.scan) as frontend):
          files = analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper)
        self.assertTrue(any(path.endswith('/gcc.h.lzo') for path in files))
        self.assertTrue(any(path.endswith('/clang.h.lzo') for path in files))
        self.assertEqual(any('-M' in call.args[0] for call in probe.call_args_list), computed)
        self.assertEqual(frontend.call_count, int(computed))

  def test_compiler_search_order_mismatch_keeps_native_verification(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    first, second = self.root / 'first', self.root / 'second'
    for directory in (first, second):
      directory.mkdir()
      (directory / 'header.h').write_text('int %s;\n' % directory.name)
    self.source.write_text('#include <header.h>\n')
    command = [gcc, '-I', str(first), '-I', str(second), '-isystem', str(first),
               '-c', str(self.source)]
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    with (mock.patch.object(compiler_analyzer, 'run', wraps=compiler_analyzer.run) as probe,
          mock.patch.object(clang_scan, 'scan', wraps=clang_scan.scan) as frontend):
      files = analyzer.DoCompilationCommand(command, str(self.root), keeper)
    self.assertTrue(any(path.endswith('/second/header.h.lzo') for path in files))
    self.assertEqual(frontend.call_count, 1)
    self.assertTrue(any('-M' in call.args[0] for call in probe.call_args_list))

  def test_gcc_implicit_precompiled_header_requires_local_preprocessing(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    (self.root / 'header.h').write_text('int header;\n')
    (self.root / 'header.h.gch').write_bytes(b'cached compiler state')
    self.source.write_text('#include "header.h"\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    with self.assertRaisesRegex(basics.NotCoveredError, 'Precompiled header'):
      analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper)

  def test_literal_graph_reuses_images_and_refreshes_changed_bytes(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    header = self.root / 'header.h'
    header.write_text('int original;\n')
    self.source.write_text('#include "header.h"\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    command = [gcc, '-c', str(self.source)]
    analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    with mock.patch('builtins.open', wraps=open) as opened:
      analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    self.assertFalse(any(call.args[0] == str(header) for call in opened.call_args_list))
    header.write_text('int updated;\n')
    files = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    image = next(path for path in files if path.endswith('/header.h.lz4s'))
    self.assertEqual(Path(image + '.xxh3').read_bytes()[:8], xxhash.xxh3_64_digest(header.read_bytes()))

  def test_literal_graph_does_not_publish_a_change_during_compression(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    header = self.root / 'header.h'
    header.write_text('int original;\n')
    self.source.write_text('#include "header.h"\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    command = [gcc, '-c', str(self.source)]
    write = compress_files.WriteCompressed
    def changing_write(path, *args):
      write(path, *args)
      if path.endswith('/header.h.lz4s'):
        header.write_text('int updated_during_collection;\n')
    with mock.patch.object(compress_files, 'WriteCompressed', side_effect=changing_write):
      with self.assertRaisesRegex(basics.NotCoveredError, 'changed while collecting'):
        analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    files = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    image = next(path for path in files if path.endswith('/header.h.lz4s'))
    self.assertEqual(Path(image + '.xxh3').read_bytes()[:8], xxhash.xxh3_64_digest(header.read_bytes()))

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

  def test_clang_expanded_cpu_flags_reach_header_analysis(self):
    self.source.write_text('#ifndef __AVX2__\n#error missing CPU feature\n#endif\nint value;\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    files = analyzer.DoCompilationCommand(
        [self.compiler, '-Xclang', '-target-cpu', '-Xclang', 'haswell',
         '-Xclang', '-tune-cpu', '-Xclang', 'haswell',
         '-Xclang', '-target-feature', '-Xclang', '+avx2',
         '-c', str(self.source)], str(self.root), keeper, 'lz4s')
    self.assertTrue(any('/.distcc-system/' in path for path in files))

  def test_clang_other_frontend_overrides_still_require_local_preprocessing(self):
    self.source.write_text('int value;\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    for flags in (['-Xclang', '-load', '-Xclang', 'plugin.so'],
                  ['-Xclang=-load'], ['-Xclang', '-target-feature']):
      with self.subTest(flags=flags):
        analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
        with self.assertRaises(basics.NotCoveredError):
          analyzer.DoCompilationCommand([self.compiler, *flags, '-c', str(self.source)],
                                        str(self.root), keeper, 'lz4s')

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
        files = analyzer.DoCompilationCommand([str(compiler), '-c', str(self.source)],
                                             str(self.root), keeper, 'lz4s')
      manifest = next(call.args[1].getvalue() for call in writer.call_args_list
                      if '/.distcc-system/' in call.args[0])
      return manifest, files
    path = os.fsencode(missing)
    self.assertNotIn(path, checks()[0])
    missing.mkdir()
    (missing / 'feature.h').write_text('/* existence probe */\n')
    updated, files = checks()
    self.assertIn(path, updated)
    self.assertTrue(any(p.endswith(str(missing / 'feature.h') + '.lz4s') for p in files))
    (missing / 'feature.h').write_text('/* different probe */\n')
    self.assertNotEqual(files, checks()[1])

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
    self.assertEqual([value for name, value in clang_scan.directives(source, True)
                      if name in ('if', 'elif')],
                     ['HAS("a.h")', 'JOIN(__has_,include)("b.h")'])

  def test_raw_string_mode_preserves_active_directives(self):
    source = (b'#define START R"(\n#if __has_include("present.h")\n'
              b'int present;\n#endif\n#define FINISH )"\n')
    for raw_strings, expected in ((True, []), (False, ['__has_include("present.h")'])):
      with self.subTest(raw_strings=raw_strings):
        self.assertEqual([value for name, value in clang_scan.directives(source, raw_strings)
                          if name == 'if'], expected)

  def test_harmless_comment_trigraphs_do_not_disable_analysis(self):
    source = b'// double check ???)\n/* ??= ??( ??! ??- */\n#include "present.h"\n'
    self.assertEqual(clang_scan.directives(source, True), [('include', '"present.h"')])
    for source in (b'??=include "real.h"\n', b'// ??/\n#include "hidden.h"\n',
                   b'/* *??/\n/\n#include "real.h"\n'):
      with self.subTest(source=source):
        with self.assertRaisesRegex(ValueError, 'trigraphs require'):
          clang_scan.directives(source, True)

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
