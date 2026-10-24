"""Exercise the actual libclang-cpp extension and its filesystem contract."""
import os
import json
import mmap
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xxhash

import basics
import clang_scan
import gcc_scan
import cache_basics
import compiler_analyzer
import compiler_defaults
import compress_files


class ClangScanTest(unittest.TestCase):
  def test_file_stamps_with_repeated_root_separators(self):
    # Run separately so a regression cannot hang the test runner or grow its
    # ancestor list without bound.
    code = r'''
import os, shutil, tempfile
from pathlib import Path
from cache_basics import FileStampCache
import basics, compiler_analyzer
with tempfile.TemporaryDirectory() as directory:
    header = Path(directory) / 'header.h'
    header.write_text('first')
    for prefix in ('/', '//', '///', '////'):
        cache = FileStampCache()
        spelling = prefix + str(header).lstrip('/')
        assert cache.Observe(spelling) == cache.Stamp(str(header))
        assert cache.Observe(prefix) == cache.Stamp(prefix)
        assert cache.Observe(prefix + directory.lstrip('/') + '/missing.h') is None
        assert len(cache.paths) < 100
    source = Path(directory) / 'source.c'
    source.write_text('#include <header.h>\nint value;\n')
    header.write_text('/* header */\n')
    keeper = basics.ClientRootKeeper()
    try:
        for compiler in ('gcc', 'clang'):
            analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
            files = analyzer.DoCompilationCommand(
                [shutil.which(compiler), '-I///' + directory.lstrip('/'),
                 '-c', str(source)], directory, keeper, 'lz4s')
            assert files
    finally:
        keeper.CleanOutClientRoots()
'''
    subprocess.run([sys.executable, '-c', code], check=True, timeout=5)

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

  def test_guarded_header_aliases_remain_dependencies(self):
    header, alias = self.root/'real.h', self.root/'alias.h'
    for guard in ('#pragma once\nint value;\n',
                  '#ifndef INCLUDED\n#define INCLUDED\nint value;\n#endif\n'):
      for link in ('symlink', 'hardlink'):
        with self.subTest(guard=guard, link=link):
          alias.unlink(missing_ok=True)
          header.write_text(guard)
          if link == 'symlink':
            alias.symlink_to(header.name)
          else:
            os.link(header, alias)
          self.source.write_text('#include "real.h"\n#include "alias.h"\n')
          result = self.scan()
          self.assertTrue({str(header), str(alias)} <= set(result['dependencies']))
          self.assertEqual(result['files'][str(alias)], header.read_bytes())

  def test_absolute_forwarders_and_include_probes(self):
    header, missing = self.root / 'body.h', self.root / 'missing.h'
    header.write_text('int value;\n')
    for operand in ('"%s"' % header, 'HEADER'):
      with self.subTest(operand=operand):
        self.source.write_text(
            '#define HEADER "%s"\n#if __has_include(HEADER)\n#include %s\n#endif\n'
            '#if __has_include("%s")\n#error unexpected header\n#endif\n' %
            (header, operand, missing))
        result = self.scan()
        self.assertIn(str(header), result['dependencies'])
        self.assertEqual(result['files'][str(header)], header.read_bytes())
        self.assertIsNone(result['lookups'][str(missing)])

  def test_compile_absolute_paths_use_only_client_snapshots(self):
    assert self.compiler is not None
    server = self.root / 'worker'
    original = self.root / 'original.c'
    header, host_only = self.root / 'header.h', self.root / 'host-only.h'
    header.write_text('#error worker host header must not be used\n')
    host_only.write_text('#error worker host probe must not be visible\n')
    copied_source = server / str(original).lstrip('/')
    copied_header = server / str(header).lstrip('/')
    copied_source.parent.mkdir(parents=True)
    copied_header.write_text('#define VALUE 42\n')
    copied_source.write_text(
        '#define HEADER "%s"\n#if !__has_include(HEADER)\n#error missing snapshot\n#endif\n'
        '#include HEADER\n#if __has_include("%s")\n#error leaked host lookup\n#endif\n'
        '_Static_assert(VALUE == 42, "snapshot value");\n' % (header, host_only))
    output = self.root / 'result.o'
    status = clang_scan.compile(
        [self.compiler, '--no-default-config', '-nostdinc', '-c', str(copied_source),
         '-o', str(output)], str(original), str(server), str(self.root), True)
    self.assertEqual(status, 0)
    self.assertTrue(output.is_file())

  def test_preprocessed_source_identity_without_client_directory(self):
    assert self.compiler is not None
    original = self.root / 'absent-client' / 'nested' / 'source.c'
    self.assertFalse(original.parent.exists())
    self.source.write_text('const char *identity = __FILE__;\nint main(void) { return 0; }\n')
    output, executable = self.root / 'result.o', self.root / 'result'
    status = clang_scan.compile(
        [self.compiler, '-x', 'c', '-c', str(self.source), '-o', str(output)],
        str(original), str(self.root), str(original.parent), False)
    self.assertEqual(status, 0)
    self.assertIn(os.fsencode(original), output.read_bytes())
    subprocess.run([self.compiler, output, '-o', executable], check=True)
    subprocess.run([executable], check=True)

  def test_large_profile_is_not_a_header_snapshot(self):
    assert self.compiler is not None
    profdata = shutil.which('llvm-profdata')
    if not profdata:
      self.skipTest('llvm-profdata is required')
    training, raw, profile = (self.root / name for name in ('training', 'data.profraw', 'data.profdata'))
    self.source.write_text('int main(void) { return 0; }\n')
    subprocess.run([self.compiler, '-fprofile-instr-generate', self.source, '-o', training], check=True)
    subprocess.run([training], env=dict(os.environ, LLVM_PROFILE_FILE=str(raw)), check=True)
    subprocess.run([profdata, 'merge', raw, '-o', profile], check=True)
    # LLVM's indexed reader accepts trailing storage. Exercise the old request
    # cutoff with real, valid profile data without generating millions of TUs.
    with profile.open('ab') as stream:
      stream.truncate(65 * 1024 * 1024)
    for _ in range(2):
      result = clang_scan.scan([self.compiler, '-fprofile-instr-use=' + str(profile),
                               '-c', str(self.source)], str(self.root))
      self.assertEqual(result['files'][str(self.source)], self.source.read_bytes())
      self.assertNotIn(str(profile), result['files'])
      self.assertIn(str(profile), result['lookups'])

  def test_driver_name_target_and_language_match_native(self):
    assert self.compiler is not None
    driver = self.root / 'aarch64-unknown-linux-gnu-clang++'
    driver.symlink_to(self.compiler)
    for name in ('arm', 'x86', 'c', 'cxx'):
      (self.root / (name + '.h')).write_text('int %s_selected;\n' % name)
    self.source.write_text(
        '#ifdef __aarch64__\n#include "arm.h"\n#else\n#include "x86.h"\n#endif\n'
        '#ifdef __cplusplus\n#include "cxx.h"\n#else\n#include "c.h"\n#endif\n')
    for target, flags in (('arm', []), ('x86', ['--target=x86_64-unknown-linux-gnu'])):
      with self.subTest(target=target):
        command = [str(driver), '--no-default-config', '-nostdinc', *flags]
        # Each operation must infer the target before any previous invocation
        # has initialized LLVM's target registry in the same process.
        scan = json.loads(subprocess.check_output([sys.executable, '-c',
            'import clang_scan,json,sys; print(json.dumps(clang_scan.scan('
            'json.loads(sys.argv[1]), sys.argv[2])["dependencies"]))',
            json.dumps([*command, '-c', str(self.source)]), str(self.root)]))
        headers = {Path(path).name for path in scan if path.endswith('.h')}
        self.assertEqual(headers, {target + '.h', 'cxx.h'})
        native = subprocess.run([*command, '-E', str(self.source)],
                                capture_output=True, check=True)
        output = self.root / 'target.i'
        subprocess.run([sys.executable, '-c',
            'import clang_scan,json,sys,os; sys.exit(clang_scan.compile('
            'json.loads(sys.argv[1]), sys.argv[2], "", os.getcwd(), False))',
            json.dumps([*command, '-E', str(self.source), '-o', str(output)]),
            str(self.source)], check=True)
        self.assertEqual(output.read_bytes(), native.stdout)

  def test_system_search_probe_does_not_initialize_ccache(self):
    ccache, gcc = shutil.which('ccache'), shutil.which('gcc')
    if not ccache or not gcc:
      self.skipTest('ccache and GCC are required')
    cache = self.root / 'cache'
    compiler = self.root / 'wrapped-gcc'
    compiler.write_text('#!/bin/sh\nexport CCACHE_DIR=%s\nexec %s %s "$@"\n' %
                        tuple(map(shlex.quote, (str(cache), ccache, gcc))))
    compiler.chmod(0o755)
    for language in ('c', 'c++'):
      with self.subTest(language=language):
        expected = compiler_defaults._SystemSearchdirsGCC(gcc, '', language, os.path.realpath)
        self.assertEqual(compiler_defaults._SystemSearchdirsGCC(
            str(compiler), '', language, os.path.realpath), expected)
        self.assertFalse(cache.exists(), 'compiler probes must not initialize ccache')

  def test_clang_expands_nested_probe_macros_and_ignores_strings(self):
    (self.root/'present.h').write_text('int present;\n')
    self.source.write_text(
        '#define SELECT(x,...) x\n#define CHECK(x) __has_include(x)\n'
        'const char *s = "__has_include(fake.h)";\n'
        '#if CHECK(SELECT("present.h", \'(\', "a,b", (nested, (args))))\n'
        'int selected;\n#endif\n')
    result = self.scan()
    self.assertIn(str(self.root/'present.h'), result['dependencies'])
    self.assertFalse(any(path.endswith('/fake.h') for path in result['lookups']))

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

  def test_cached_paste_matches_follow_new_definitions_and_cache_reset(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    header = self.root / 'condition.h'
    header.write_text('#define SELECT(x) FEATURE_##x\n#if SELECT(NEW)\nint found;\n#endif\n')
    probe = self.root / 'probe.h'
    probe.write_text('/* existence-only input */\n')
    self.source.write_text('#include "condition.h"\n')
    second = self.root / 'second.c'
    second.write_text('#define FEATURE_NEW __has_include("probe.h")\n'
                      '#include "condition.h"\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    for source, expected, reset in ((self.source, False, False),
                                     (second, True, False),
                                     (self.source, False, True)):
      with self.subTest(source=source.name, reset=reset):
        if reset:
          analyzer.ClearStatCaches()
        files = analyzer.DoCompilationCommand(
            [gcc, '-nostdinc', '-c', str(source)], str(self.root), keeper, 'lz4')
        self.assertEqual(any(path.endswith(str(probe) + '.lz4') for path in files),
                         expected)

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

  def test_gcc_collects_absolute_forwarder_in_native_preprocessing(self):
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
      analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper)
      self.assertIn(str(header), analyzer.realpath_map.string)
      self.assertEqual(frontend.call_count, 0)

  def test_gcc_literal_and_computed_includes_use_native_frontend(self):
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
        with (mock.patch.object(gcc_scan, 'scan', wraps=gcc_scan.scan) as probe,
              mock.patch.object(clang_scan, 'scan', wraps=clang_scan.scan) as frontend):
          files = analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper)
        self.assertTrue(any(path.endswith('/gcc.h.lzo') for path in files))
        self.assertFalse(any(path.endswith('/clang.h.lzo') for path in files))
        self.assertEqual(probe.call_count, 1)
        self.assertEqual(frontend.call_count, 0)

  def test_compiler_search_order_uses_native_header_resolution(self):
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
    with (mock.patch.object(gcc_scan, 'scan', wraps=gcc_scan.scan) as probe,
          mock.patch.object(clang_scan, 'scan', wraps=clang_scan.scan) as frontend):
      files = analyzer.DoCompilationCommand(command, str(self.root), keeper)
    self.assertTrue(any(path.endswith('/second/header.h.lzo') for path in files))
    self.assertEqual(frontend.call_count, 0)
    self.assertEqual(probe.call_count, 1)

  def test_gcc_implicit_precompiled_header_requires_local_preprocessing(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    (self.root / 'header.h').write_text('int header;\n')
    subprocess.run([gcc, '-x', 'c-header', str(self.root/'header.h'),
                    '-o', str(self.root/'header.h.gch')], check=True)
    self.source.write_text('#include "header.h"\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    with self.assertRaisesRegex(basics.NotCoveredError, 'unsupported GCC dependency scan'):
      analyzer.DoCompilationCommand([gcc, '-fpch-preprocess', '-c', str(self.source)],
                                    str(self.root), keeper)

  def test_compiler_scan_reuses_images_and_refreshes_changed_bytes(self):
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

  def test_prior_input_change_restarts_current_scan_with_fresh_caches(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    prior = self.root / 'prior.h'
    prior.write_text('int original;\n')
    self.source.write_text('#include "prior.h"\n')
    current = self.root / 'current.c'
    current.write_text('int current;\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    earlier = analyzer.DoCompilationCommand([gcc, '-c', str(self.source)],
                                            str(self.root), keeper, 'lz4s')
    image = next(path for path in earlier if path.endswith('/prior.h.lz4s'))
    old_image = Path(image).read_bytes()
    native_scan = compiler_analyzer.gcc_scan.scan
    calls = []
    def change_prior(*args):
      result = native_scan(*args)
      calls.append(result)
      if len(calls) == 1:
        prior.write_text('int generated_by_another_job;\n')
      return result
    with mock.patch.object(compiler_analyzer.gcc_scan, 'scan', side_effect=change_prior):
      files = analyzer.DoCompilationCommand([gcc, '-c', str(current)],
                                            str(self.root), keeper, 'lz4s')
    self.assertEqual(len(calls), 2)
    self.assertFalse(any(path.endswith('/prior.h.lz4s') for path in files))
    self.assertEqual(Path(image).read_bytes(), old_image)

  def test_cached_images_refresh_after_hardlink_and_mmap_writes(self):
    self.addCleanup(os.chdir, os.getcwd())
    header = self.root / 'header.h'
    other = self.root / 'other'
    other.mkdir()
    alias = other / 'alias.h'
    header.write_bytes(b'int original;\n')
    os.link(header, alias)
    self.source.write_text('#include "header.h"\n')
    for compiler in ('gcc', 'clang'):
      if not shutil.which(compiler):
        continue
      with self.subTest(compiler=compiler):
        header.write_bytes(b'int original;\n')
        keeper = basics.ClientRootKeeper()
        self.addCleanup(keeper.CleanOutClientRoots)
        analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
        command = [compiler, '-c', str(self.source)]
        original = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
        old_image = next(path for path in original if path.endswith('/header.h.lz4s'))
        old_bytes = Path(old_image).read_bytes()
        for mapped, data in ((False, b'int hardlink;\n'), (True, b'int mmapdata;\n')):
          with alias.open('r+b') as stream:
            if mapped:
              with mmap.mmap(stream.fileno(), 0) as memory:
                memory[:] = data
                memory.flush()
            else:
              stream.write(data)
          files = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
          image = next(path for path in files if path.endswith('/header.h.lz4s'))
          self.assertEqual(Path(image + '.xxh3').read_bytes()[:8], xxhash.xxh3_64_digest(data))
          self.assertEqual(Path(old_image).read_bytes(), old_bytes)

  def test_cached_failed_lookup_is_revalidated_when_a_header_appears(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    self.source.write_text('#if __has_include("generated/header.h")\n'
                           '#include "generated/header.h"\n#endif\n')
    header = self.root / 'generated/header.h'
    native_scan = compiler_analyzer.gcc_scan.scan
    for notifications in (True, False):
      with self.subTest(notifications=notifications):
        if header.exists():
          header.unlink()
          header.parent.rmdir()
        keeper = basics.ClientRootKeeper()
        self.addCleanup(keeper.CleanOutClientRoots)
        analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
        command = [gcc, '-c', str(self.source)]
        analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
        stamps = analyzer.build_stat_cache.file_stamps
        if notifications and stamps.watch_fd < 0:
          self.skipTest('inotify is required for the notification case')
        if not notifications and stamps.watch_fd >= 0:
          os.close(stamps.watch_fd)
          stamps.watch_fd = -1
        calls = []
        def create_header(*args):
          result = native_scan(*args)
          calls.append(result)
          if len(calls) == 1:
            header.parent.mkdir()
            header.write_text('int generated;\n')
          return result
        with mock.patch.object(compiler_analyzer.gcc_scan, 'scan', side_effect=create_header):
          files = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
        self.assertEqual(len(calls), 2)
        image = next(path for path in files if path.endswith('/generated/header.h.lz4s'))
        self.assertEqual(Path(image + '.xxh3').read_bytes()[:8],
                         xxhash.xxh3_64_digest(header.read_bytes()))

  def test_compiler_scan_retries_a_change_during_compression(self):
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
    images = {}
    def changing_write(path, *args):
      write(path, *args)
      if path.endswith('/header.h.lz4s'):
        images[path] = Path(path).read_bytes()
        if len(images) == 1:
          header.write_text('int updated_during_collection;\n')
    with mock.patch.object(compress_files, 'WriteCompressed', side_effect=changing_write):
      files = analyzer.DoCompilationCommand(command, str(self.root), keeper, 'lz4s')
    self.assertEqual(len(images), 2)
    for path, contents in images.items():
      self.assertEqual(Path(path).read_bytes(), contents)
    image = next(path for path in files if path.endswith('/header.h.lz4s'))
    self.assertEqual(Path(image + '.xxh3').read_bytes()[:8], xxhash.xxh3_64_digest(header.read_bytes()))

  def test_compiler_scan_rejects_continuous_changes_during_compression(self):
    gcc = shutil.which('gcc')
    if not gcc:
      self.skipTest('GCC is required')
    self.addCleanup(os.chdir, os.getcwd())
    self.source.write_text('int original;\n')
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    write = compress_files.WriteCompressed
    changes = []
    def changing_write(path, *args):
      write(path, *args)
      if path.endswith('/source.c.lz4s'):
        changes.append(path)
        self.source.write_text('int changed_%d;\n' % len(changes))
    with mock.patch.object(compress_files, 'WriteCompressed', side_effect=changing_write):
      with self.assertRaisesRegex(basics.NotCoveredError, 'Inputs keep changing'):
        analyzer.DoCompilationCommand([gcc, '-c', str(self.source)], str(self.root), keeper, 'lz4s')
    self.assertEqual(len(changes), 2)

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

  def test_clang_release_metadata_is_not_an_analysis_gate(self):
    header = self.root / 'versioned.h'
    header.write_text('#define ANSWER 42\n')
    self.source.write_text('#include "versioned.h"\nint value = ANSWER;\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    with mock.patch.object(clang_scan, 'version', '0.0-compatible-build'):
      files = analyzer.DoCompilationCommand(
          [self.compiler, '-c', str(self.source)], str(self.root), keeper, 'lz4s')
    self.assertTrue(files)
    self.assertTrue(any(path.endswith(str(header) + '.lz4s') for path in files))

  def test_compatible_other_clang_release_scans_compiles_and_links(self):
    for compiler in Path('/usr/lib/llvm').glob('*/bin/clang'):
      version = subprocess.check_output([compiler, '-dumpversion'], text=True).strip()
      if version != clang_scan.version:
        break
    else:
      self.skipTest('an additional Clang release is required')
    self.source.write_text('int main(void) { return 0; }\n')
    self.addCleanup(os.chdir, os.getcwd())
    keeper = basics.ClientRootKeeper()
    self.addCleanup(keeper.CleanOutClientRoots)
    analyzer = compiler_analyzer.CompilerAnalyzer(keeper)
    files = analyzer.DoCompilationCommand([str(compiler), '-c', str(self.source)],
                                         str(self.root), keeper, 'lz4s')
    self.assertTrue(files)
    worker = self.root / 'worker'
    snapshots = clang_scan.scan([str(compiler), '-c', str(self.source)], str(self.root))['files']
    for path, data in snapshots.items():
      copied = worker / path.lstrip('/')
      copied.parent.mkdir(parents=True, exist_ok=True)
      copied.write_bytes(data)
    obj, executable = self.root / 'other.o', self.root / 'other'
    helper = Path(__file__).resolve().parent.parent / 'distcc-clang'
    # The test imports include_server's modules directly; the helper uses the
    # installed package. Do not shadow that package with include_server.py.
    env = dict(os.environ)
    env.pop('PYTHONPATH', None)
    result = subprocess.run([sys.executable, helper, worker, self.source, self.root, 'pump', compiler,
                             '-c', self.source, '-o', obj], env=env, capture_output=True, text=True)
    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    subprocess.run([compiler, obj, '-o', executable], check=True, capture_output=True)
    subprocess.run([executable], check=True)

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
         '-Xclang', '-target-feature', '-Xclang', '-xop',
         '-Xclang', '-target-feature', '-Xclang', '-ospke',
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

  def test_clang_comments_and_raw_strings_do_not_create_includes(self):
    self.source = self.root/'source.cpp'
    (self.root/'present.h').write_text('int present;\n')
    self.source.write_text(
        '/* ?????/2019: fixed */\n// ?????/2015: still present\n'
        '/* #include "comment.h" */\n'
        'const char *s = R"text(\n#include "raw.h"\n)text";\n'
        '#include "present.h"\n')
    result = self.scan()
    self.assertIn(str(self.root/'present.h'), result['dependencies'])
    self.assertFalse(any(path.endswith(('/comment.h', '/raw.h')) for path in result['lookups']))

  def test_clang_condition_aliases_use_preprocessor_expansion(self):
    assert self.compiler is not None
    (self.root/'probe.h').write_text('int present;\n')
    self.source.write_text(
        '#define JOIN(a,b) a##b\n#define CHECK JOIN(__has_,include)\n'
        '#define FORWARD(arg) CHECK(arg)\n'
        '#if defined(CHECK) && FORWARD("probe.h")\nint selected;\n#endif\n')
    subprocess.run([self.compiler, '-E', str(self.source)],
                   stdout=subprocess.DEVNULL, check=True)
    result = self.scan()
    self.assertIn(str(self.root/'probe.h'), result['dependencies'])

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
