#!/usr/bin/env python3
"""Real GCC preprocessing, including an input changing while GCC is blocked."""
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest


class LookupTest(unittest.TestCase):
    def setUp(self):
        self.build = Path(__file__).resolve().parents[1]
        self.temp = tempfile.TemporaryDirectory(prefix='cpp-lookup-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.work = self.root / 'build'
        self.headers = self.root / 'headers'
        self.work.mkdir()
        self.headers.mkdir()
        self.search = []
        for i in range(300):
            directory = self.root / ('empty-' + str(i))
            directory.mkdir()
            self.search.append('-I' + str(directory))
        self.source = self.root / 'input.c'
        self.env = dict(os.environ, LC_ALL='C',
                        LD_PRELOAD=str(self.build / 'cpp_lookup.so'), DISTCC_CPP_LOOKUP='1')

    def preprocess(self, cache, *options):
        env = dict(self.env, DISTCC_CPP_LOOKUP=str(int(cache)))
        with tempfile.TemporaryFile(dir=self.work) as output:
            result = subprocess.run(['gcc', '-E', *self.search, '-I' + str(self.headers), *options,
                                     str(self.source)], cwd=self.work, env=env,
                                    stdout=output, stderr=subprocess.PIPE, timeout=15)
            output.seek(0)
            return result.returncode, output.read(), result.stderr

    def test_search_and_macro_identity(self):
        later = self.root / 'later'
        later.mkdir()
        (self.headers / 'common.h').write_text('#include_next "common.h"\n')
        (later / 'common.h').write_text('int present;\n')
        (self.headers / 'alias.h').symlink_to(later / 'common.h')
        self.source.write_text('''#include "common.h"
#include "alias.h"
#if __has_include("missing/absent.h")
#error unexpected header
#endif
const char *file = __FILE__, *base = __BASE_FILE__, *stamp = __TIMESTAMP__;
''')
        options = ('-I' + str(later), '-MD', '-MF', 'deps.d')
        expected = self.preprocess(False, *options)
        deps = (self.work / 'deps.d').read_bytes()
        self.assertEqual(expected[0], 0, expected[2])
        self.assertEqual(self.preprocess(True, *options), expected)
        self.assertEqual((self.work / 'deps.d').read_bytes(), deps)

    def test_changed_directory_replays_preprocessing(self):
        self.source.write_text('''#if __has_include("appeared.h")
#include "appeared.h"
#else
int selected = 0;
#endif
#include "gate.h"
''')
        gate = self.headers / 'gate.h'
        os.mkfifo(gate)
        errors = []
        def change_inputs():
            try:
                # The open completes only after GCC evaluated __has_include.
                with gate.open('w') as writer:
                    gate.unlink()
                    gate.write_text('\n')
                    (self.headers / 'appeared.h').write_text('int selected = 1;\n')
                    writer.write('\n')
            except Exception as error:
                errors.append(error)
        thread = threading.Thread(target=change_inputs, daemon=True)
        thread.start()
        actual = self.preprocess(True, '-MD', '-MF', 'deps.d')
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(actual[0], 0, actual[2])
        self.assertIn(b'int selected = 1;', actual[1])
        self.assertNotIn(b'int selected = 0;', actual[1])
        deps = (self.work / 'deps.d').read_bytes()
        self.assertIn(b'appeared.h', deps)
        self.assertEqual(actual, self.preprocess(False, '-MD', '-MF', 'deps.d'))
        self.assertEqual((self.work / 'deps.d').read_bytes(), deps)

    def test_missing_header_diagnostic(self):
        self.source.write_text('#include "does-not-exist.h"\n')
        expected = self.preprocess(False)
        self.assertNotEqual(expected[0], 0)
        self.assertEqual(self.preprocess(True), expected)

    def test_pipe_output_uses_normal_lookup(self):
        self.source.write_text('int selected;\n')
        command = ['gcc', '-E', str(self.source)]
        expected = subprocess.run(command, env=dict(self.env, DISTCC_CPP_LOOKUP='0'),
                                  capture_output=True, timeout=15)
        actual = subprocess.run(command, env=self.env, capture_output=True, timeout=15)
        self.assertEqual((actual.returncode, actual.stdout, actual.stderr),
                         (expected.returncode, expected.stdout, expected.stderr))


if __name__ == '__main__':
    unittest.main()
