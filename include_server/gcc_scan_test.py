"""Check native GCC preprocessing and dependency probes, without syntax emulation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

import gcc_scan


class GccScanTest(unittest.TestCase):
  def setUp(self):
    compiler = shutil.which('gcc')
    library = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
    if not compiler or not library:
      self.skipTest('A GCC dependency module built for this compiler is required')
    self.compiler = compiler
    self.library = library
    self.storage = tempfile.TemporaryDirectory()
    self.addCleanup(self.storage.cleanup)
    self.root = Path(self.storage.name)
    self.source = self.root/'source.c'

  def scan(self, *options):
    return gcc_scan.scan([self.compiler, '-I', str(self.root), *options, str(self.source)],
                         str(self.root), self.library)

  def test_native_feature_branch_and_existence_only_inputs(self):
    (self.root/'gcc.h').write_text('int gcc_only;\n')
    (self.root/'clang.h').write_text('int clang_only;\n')
    (self.root/'present.h').write_text('/* exists without inclusion */\n')
    self.source.write_text(
        '#define CAT_(a,b) a##b\n#define CAT(a,b) CAT_(a,b)\n'
        '#define HEADER(x) #x\n'
        '#if __has_builtin(__builtin_speculation_safe_value)\n'
        '#include HEADER(gcc.h)\n'
        '#if !CAT(__has_,include)(HEADER(present.h))\n#error missing probe\n#endif\n'
        '#else\n#include "clang.h"\n#endif\n'
        '#if __has_include("missing.h")\n#error unexpected input\n#endif\n')
    result = self.scan()
    self.assertIn(str(self.root/'gcc.h'), result['dependencies'])
    self.assertIn(str(self.root/'present.h'), result['dependencies'])
    self.assertNotIn(str(self.root/'clang.h'), result['dependencies'])
    self.assertIsNone(result['lookups'][str(self.root/'missing.h')])

  def test_compiler_reports_directory_attributes(self):
    ordinary, system = self.root/'ordinary', self.root/'system'
    ordinary.mkdir()
    system.mkdir()
    self.source.write_text('int n;\n')
    result = self.scan('-I', str(ordinary), '-isystem', str(system))
    self.assertEqual(result['directories'][str(ordinary)], 0)
    self.assertEqual(result['directories'][str(system)], 2)
    result = self.scan('-x', 'c++')
    self.assertTrue(any(kind == 1 for kind in result['directories'].values()))

  def test_trigraphs_variadics_and_include_next_use_compiler_semantics(self):
    first, second = self.root/'first', self.root/'second'
    first.mkdir()
    second.mkdir()
    (first/'next.h').write_text('#include_next "next.h"\n')
    (second/'next.h').write_text('int selected;\n')
    self.source.write_text(
        '/* ?????/2019 */\n'
        '#define CHOOSE(x,...) x\n'
        '??=include CHOOSE("next.h", \'(\', "a,b", (nested, (args)))\n')
    result = self.scan('-trigraphs', '-I', str(first), '-I', str(second))
    self.assertIn(str(first/'next.h'), result['dependencies'])
    self.assertIn(str(second/'next.h'), result['dependencies'])

  def test_absolute_probe_and_include_use_native_lookup(self):
    header = self.root/'present.h'
    header.write_text('int present;\n')
    self.source.write_text('#if __has_include("%s")\n#include "%s"\n#endif\n' % (header, header))
    self.assertIn(str(header), self.scan()['dependencies'])

  def test_absolute_parent_traversal_is_rejected(self):
    self.source.write_text('#if __has_include("/../missing-distcc-header.h")\n#endif\n')
    with self.assertRaisesRegex(RuntimeError, 'unsupported'):
      self.scan()

  def test_parent_components_in_search_paths_are_supported(self):
    directory = self.root/'headers'
    directory.mkdir()
    (directory/'child').mkdir()
    header = directory/'selected.h'
    header.write_text('int selected;\n')
    self.source.write_text('#include <selected.h>\n#if __has_include(<absent.h>)\n#error unexpected\n#endif\n')
    result = self.scan('-I' + str(directory/'child'/'..'))
    self.assertIn(str(directory/'child'/'..'/'selected.h'), result['dependencies'])

  def test_worker_absolute_lookup_is_confined_to_input_tree(self):
    header, absent = self.root/'present.h', self.root/'worker-only.h'
    header.write_text('int wrong;\n')
    absent.write_text('int worker_only;\n')
    virtual = self.root/'worker'
    copied = virtual/str(header).lstrip('/')
    copied.parent.mkdir(parents=True)
    copied.write_text('int transferred;\n')
    self.source.write_text('#if !__has_include("%s")\n#error missing\n#endif\n'
                           '#include "%s"\n#if __has_include("%s")\n'
                           '#error worker file leaked\n#endif\n' % (header, header, absent))
    invocation = [self.compiler, '-E', '-nostdinc', '-fplugin=' + self.library,
                  '-fplugin-arg-' + Path(self.library).stem + '-input-root=' + str(virtual),
                  str(self.source)]
    result = subprocess.run(invocation, capture_output=True, text=True)
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assertIn('int transferred;', result.stdout)
    self.assertNotIn('int wrong;', result.stdout)
    # Forced includes, whose paths are already rewritten by distccd, must be
    # opened once under the root, not prefixed a second time.
    result = subprocess.run([*invocation, '-include', str(copied)],
                            capture_output=True, text=True)
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assertIn('int transferred;', result.stdout)

  def test_source_error_retains_inputs_for_remote_diagnostics(self):
    header = self.root/'after.h'
    header.write_text('int after;\n')
    self.source.write_text('int before;\n#error intended\n#include "after.h"\n')
    result = self.scan()
    self.assertIn(str(header), result['dependencies'])
    self.assertIn(str(self.source), result['dependencies'])

  def test_ordinary_macro_expansion_can_affect_later_includes(self):
    header = self.root/'selected.h'
    header.write_text('int selected;\n')
    self.source.write_text('int before = __COUNTER__;\n'
                           '#if __COUNTER__ == 1\n#include "selected.h"\n#endif\n')
    native = subprocess.run([self.compiler, '-E', str(self.source)],
                            check=True, capture_output=True, text=True)
    self.assertIn('int selected;', native.stdout)
    self.assertIn(str(header), self.scan()['dependencies'])

  def test_token_collection_omits_text_but_keeps_pragma_and_forced_include_effects(self):
    forced, selected = self.root/'forced.h', self.root/'selected.h'
    forced.write_text('int forced = __COUNTER__;\n')
    selected.write_text('int selected;\n')
    self.source.write_text(
        '#define VALUE 1\n#pragma push_macro("VALUE")\n'
        '#undef VALUE\n#define VALUE 2\n#pragma pop_macro("VALUE")\n'
        'int value = __COUNTER__;\n'
        '#if __COUNTER__ == 2 && VALUE == 1\n#include "selected.h"\n#endif\n'
        '_Pragma("GCC diagnostic error \\\"-Wundef\\\"")\n'
        '#if UNDEFINED\n#endif\n')
    for language in ('c', 'c++'):
      with self.subTest(language=language):
        command = [self.compiler, '-x', language, '-E', '-P',
                   '-ftrack-macro-expansion=0', '-include', str(forced), str(self.source)]
        native = subprocess.run(command, capture_output=True)
        self.assertIn(b'int selected;', native.stdout)
        self.assertIn(b'UNDEFINED', native.stderr)
        with os.fdopen(os.memfd_create('gcc-test', os.MFD_CLOEXEC), 'w+b') as output:
          fd = output.fileno()
          scanned = subprocess.run([*command, '-fplugin=' + self.library,
              '-fplugin-arg-' + Path(self.library).stem + '-output-fd=' + str(fd)],
              pass_fds=(fd,), capture_output=True)
          # Early pragma handlers may echo their arguments; ordinary tokens
          # must bypass the text formatter completely.
          self.assertNotIn(b'int ', scanned.stdout)
          self.assertEqual(scanned.returncode, native.returncode)
          self.assertEqual(scanned.stderr, native.stderr)
          output.seek(0)
          manifest = output.read()
        self.assertTrue(manifest.startswith(b'DCGS2\0'))
        self.assertTrue(manifest.endswith(b'\0\0'))
        self.assertIn(os.fsencode(selected) + b'\0', manifest)
        self.assertIn(str(selected), self.scan('-x', language, '-include', str(forced))['dependencies'])

  def test_explicit_directives_only_keeps_native_scanner(self):
    header = self.root/'selected.h'
    header.write_text('int selected;\n')
    self.source.write_text('#define HEADER "selected.h"\n'
                           'int before = __COUNTER__;\n#include HEADER\n')
    native = subprocess.run([self.compiler, '-E', '-fdirectives-only', str(self.source)],
                            check=True, capture_output=True, text=True)
    self.assertIn('int selected;', native.stdout)
    self.assertIn(str(header), self.scan('-fdirectives-only')['dependencies'])

  def test_large_manifest_preserves_all_negative_probes(self):
    names = [('missing-' + str(i) + '-' + 'x' * 64 + '.h') for i in range(1000)]
    self.source.write_text(''.join('#if __has_include("' + name + '")\n#endif\n'
                                   for name in names))
    result = self.scan()
    self.assertTrue(all(str(self.root/name) in result['lookups'] for name in names))
    self.assertTrue(all(result['lookups'][str(self.root/name)] is None for name in names))

  def test_bom_and_input_charset_keep_raw_input_size(self):
    for contents, options in ((b'\xef\xbb\xbfint n;\n', ()),
                              (b'/* \xe9 */ int n;\n', ('-finput-charset=ISO-8859-1',))):
      with self.subTest(options=options):
        self.source.write_bytes(contents)
        result = self.scan(*options)
        self.assertEqual(result['lookups'][str(self.source)][3], len(contents))

  def test_directory_candidates_are_observed_but_not_transferred(self):
    directory = self.root/'stdint.h'
    directory.mkdir()
    self.source.write_text('#include <stdint.h>\nint n;\n')
    result = self.scan()
    self.assertNotIn(str(directory), result['dependencies'])
    self.assertIsNone(result['lookups'][str(directory)])

  def test_lookup_cache_replaces_the_manifest_after_namespace_change(self):
    self.check_lookup_cache_replaces_the_manifest_after_namespace_change(False)

  def test_relative_lookup_cache_replaces_manifest_after_namespace_change(self):
    self.check_lookup_cache_replaces_the_manifest_after_namespace_change(True)

  def check_lookup_cache_replaces_the_manifest_after_namespace_change(self, relative):
    lookup = (os.environ.get('DISTCC_CPP_LOOKUP_LIBRARY') or
              str(Path(__file__).resolve().parents[1] / 'cpp_lookup.so'))
    if not os.path.isfile(lookup):
      self.skipTest('GCC lookup cache is required')
    work, headers = self.root/'build', self.root/'headers'
    work.mkdir()
    headers.mkdir()
    search = []
    for i in range(300):
      directory = self.root/('empty-' + str(i))
      directory.mkdir()
      search.append('-I' + (os.path.relpath(directory, work) if relative else str(directory)))
    gate = headers/'gate.h'
    os.mkfifo(gate)
    self.source.write_text('#if __has_include("appeared.h")\n'
                           '#include "appeared.h"\n#endif\n#include "gate.h"\n')
    errors = []
    def change_inputs():
      try:
        with gate.open('w') as writer:
          gate.unlink()
          gate.write_text('\n')
          (headers/'appeared.h').write_text('int appeared;\n')
          writer.write('\n')
      except Exception as error:
        errors.append(error)
    thread = threading.Thread(target=change_inputs, daemon=True)
    thread.start()
    with mock.patch.dict(os.environ, DISTCC_CPP_LOOKUP_LIBRARY=lookup, DISTCC_CPP_LOOKUP='1'):
      header_path = os.path.relpath(headers, work) if relative else str(headers)
      result = gcc_scan.scan([self.compiler, *search, '-I' + header_path, str(self.source)],
                             str(work), self.library)
    thread.join(timeout=5)
    self.assertFalse(thread.is_alive())
    self.assertFalse(errors, errors)
    dependencies = {Path(path).resolve() for path in result['dependencies']}
    self.assertIn(headers/'appeared.h', dependencies)
    self.assertIn(gate, dependencies)


if __name__ == '__main__':
  unittest.main()
