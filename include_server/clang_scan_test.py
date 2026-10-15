"""Exercise the actual libclang-cpp extension and its filesystem contract."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import clang_scan


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

  def test_probe_only_header_and_immutable_contents(self):
    header = self.root / 'probe.h'
    header.write_bytes(b'original contents\n')
    self.source.write_text('#if __has_include("probe.h")\nint yes;\n#endif\n')
    result = self.scan()
    self.assertIn(str(header), result['dependencies'])
    self.assertEqual(result['files'][str(header)], b'original contents\n')
    header.write_bytes(b'changed contents\n')
    self.assertEqual(result['files'][str(header)], b'original contents\n')
    self.assertNotEqual(result['lookups'][str(header)][4], os.stat(header).st_mtime_ns)
    self.assertFalse((self.root / 'never.o').exists())

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


if __name__ == '__main__':
  unittest.main()
