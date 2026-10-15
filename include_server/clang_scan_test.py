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
