"""Include-server error routing with the production compiler analyzer.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import basics
import compiler_analyzer
import distcc_pump_c_extensions
import include_server


class IncludeServerTest(unittest.TestCase):
  def setUp(self):
    self.addCleanup(os.chdir, os.getcwd())
    self.storage = tempfile.TemporaryDirectory()
    self.addCleanup(self.storage.cleanup)
    self.root = Path(self.storage.name)
    self.keeper = basics.ClientRootKeeper()
    self.addCleanup(self.keeper.CleanOutClientRoots)
    self.analyzer = compiler_analyzer.CompilerAnalyzer(self.keeper)
    self.reports = []

    def report(stream, force=False, never=False):
      stream.seek(0)
      self.reports.append((stream.read(), force, never))
      stream.close()
      raise
    self.analyzer.email_sender = mock.Mock(MaybeSendEmail=report)

  def handle(self, source):
    class StreamHandler:
      def __init__(self):
        self.rfile = self.wfile = mock.Mock(fileno=lambda: 27)
    with (mock.patch.object(include_server.socketserver, 'StreamRequestHandler', StreamHandler),
          mock.patch.object(distcc_pump_c_extensions, 'RCwd', return_value=str(self.root)),
          mock.patch.object(distcc_pump_c_extensions, 'RArgv', return_value=(['gcc', source], 'lzo')),
          mock.patch.object(distcc_pump_c_extensions, 'XArgv')):
      include_server.DistccIncludeHandlerGenerator(self.analyzer)().handle()

  def test_missing_input_reports_compiler_failure(self):
    with self.assertRaises(basics.NotCoveredError):
      self.handle('missing.c')
    message, force, never = self.reports[-1]
    self.assertIn('GCC dependency scan', message)
    self.assertFalse(force)
    self.assertTrue(never)

  def test_unsupported_absolute_include_is_reported(self):
    header = self.root/'header.h'
    header.write_text('int value;\n')
    (self.root/'source.c').write_text('#include "%s"\n' % header)
    with self.assertRaises(basics.NotCoveredError):
      self.handle('source.c')
    message, _, never = self.reports[-1]
    self.assertIn('unsupported GCC dependency scan', message)
    self.assertTrue(never)

  def test_internal_error_is_distinguished_from_unsupported_input(self):
    with mock.patch.object(self.analyzer, 'DoCompilationCommand', side_effect=AssertionError('test failure')):
      with self.assertRaises(AssertionError):
        self.handle('source.c')
    message, _, never = self.reports[-1]
    self.assertIn('Include server internal error', message)
    self.assertFalse(never)


if __name__ == '__main__':
  unittest.main()
