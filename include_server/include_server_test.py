"""Include-server error routing with the production compiler analyzer.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
from pathlib import Path
import tempfile
import subprocess
import errno
import select
import sys
import unittest
from unittest import mock

import basics
import compiler_analyzer
import distcc_pump_c_extensions
import include_server


class IncludeServerTest(unittest.TestCase):
  @unittest.skipUnless(hasattr(os, 'pidfd_open'), 'Linux child lifetime check')
  def test_worker_exits_even_when_storage_and_error_log_are_full(self):
    pool = include_server.AnalyzerPool(str(self.root / 'pool'))
    self.addCleanup(pool.close)
    def fail():
      full = os.open('/dev/full', os.O_WRONLY)
      os.dup2(full, sys.stderr.fileno())
      os.close(full)
      raise OSError(errno.ENOSPC, 'snapshot storage full')
    with mock.patch.object(pool.server, 'serve_forever', side_effect=fail):
      pool.start_worker()
    pid = next(iter(pool.workers))
    fd = os.pidfd_open(pid)
    try:
      self.assertTrue(select.select([fd], [], [], 5)[0], 'failed worker did not exit')
    finally:
      os.close(fd)
    _, status = os.waitpid(pid, 0)
    pool.workers.remove(pid)
    self.assertTrue(os.WIFEXITED(status))
    self.assertEqual(os.WEXITSTATUS(status), 1)
    pool.keeper.CollectClientRoots()

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
    self.sender = mock.Mock(MaybeSendEmail=report)

  def handle(self, source):
    class StreamHandler:
      def __init__(self, request, client_address, server):
        self.rfile = self.wfile = mock.Mock(fileno=lambda: 27)
        self.request = request
    with (mock.patch.object(include_server.socketserver, 'StreamRequestHandler', StreamHandler),
          mock.patch.object(include_server, '_EmailSender', return_value=self.sender),
          mock.patch.object(distcc_pump_c_extensions, 'RCwd', return_value=str(self.root)),
          mock.patch.object(distcc_pump_c_extensions, 'RArgv', return_value=(['gcc', source], 'lzo')),
          mock.patch.object(distcc_pump_c_extensions, 'XArgv') as reply):
      handler = include_server.DistccIncludeHandlerGenerator(self.analyzer)
      handler(mock.Mock(), ('localhost', 0), mock.Mock()).handle()
      return reply.call_args.args[1]

  def test_pch_requests_original_compilation_without_warning(self):
    header = self.root/'header.h'
    header.write_text('#define VALUE 23\n')
    subprocess.run(['gcc', '-x', 'c-header', str(header)], check=True,
                   capture_output=True)
    header.unlink()
    (self.root/'source.c').write_text('#include "header.h"\nint value = VALUE;\n')
    self.assertEqual(self.handle('source.c'), ['LOCAL'])
    self.assertEqual(self.reports, [])
    # The disposition belongs to this compilation, not the following request.
    (self.root/'ordinary.c').write_text('int ordinary;\n')
    files = self.handle('ordinary.c')
    self.assertTrue(files)
    self.assertTrue(all(path.startswith('/') for path in files))

  def test_missing_input_reports_compiler_failure(self):
    with self.assertRaises(basics.NotCoveredError):
      self.handle('missing.c')
    message, force, never = self.reports[-1]
    self.assertIn('GCC dependency scan', message)
    self.assertFalse(force)
    self.assertTrue(never)

  def test_unsupported_parent_traversal_is_reported(self):
    header = self.root/'header.h'
    header.write_text('int value;\n')
    (self.root/'source.c').write_text(
        '#include "%s"\n' % (self.root/'..'/self.root.name/header.name))
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
