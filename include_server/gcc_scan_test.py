"""Check native GCC preprocessing and dependency probes, without syntax emulation."""
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest import mock

import gcc_scan


class GccScanTest(unittest.TestCase):
  def setUp(self):
    self.compiler = shutil.which('gcc')
    self.library = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
    if not self.compiler or not self.library:
      self.skipTest('A GCC dependency module built for this compiler is required')
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

  def test_absolute_probe_is_rejected(self):
    header = self.root/'present.h'
    header.write_text('int present;\n')
    self.source.write_text('#if __has_include("%s")\n#endif\n' % header)
    with self.assertRaisesRegex(RuntimeError, 'unsupported'):
      self.scan()

  def test_source_error_retains_inputs_for_remote_diagnostics(self):
    header = self.root/'after.h'
    header.write_text('int after;\n')
    self.source.write_text('int before;\n#error intended\n#include "after.h"\n')
    result = self.scan()
    self.assertIn(str(header), result['dependencies'])
    self.assertIn(str(self.source), result['dependencies'])

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
      search.append('-I' + str(directory))
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
      result = gcc_scan.scan([self.compiler, *search, '-I' + str(headers), str(self.source)],
                             str(work), self.library)
    thread.join(timeout=5)
    self.assertFalse(thread.is_alive())
    self.assertFalse(errors, errors)
    self.assertIn(str(headers/'appeared.h'), result['dependencies'])
    self.assertIn(str(gate), result['dependencies'])


if __name__ == '__main__':
  unittest.main()
