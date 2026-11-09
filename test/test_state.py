#!/usr/bin/env python3
"""State updates, ownership and monitoring with real processes and files."""
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile
import unittest


class StateTest(unittest.TestCase):
    def setUp(self):
        self.build = Path.cwd()
        self.storage = tempfile.TemporaryDirectory(prefix='distcc-state-')
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.env = dict(os.environ, DISTCC_DIR=str(self.root))

    def line(self, process):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(5), 'state helper did not respond')
        result = process.stdout.readline().strip()
        self.assertTrue(result, process.poll())
        return result

    def start(self, prefix=()):
        process = subprocess.Popen([*prefix, str(self.build/'h_state')], env=self.env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, umask=0o027)
        def finish():
            if process.poll() is None: process.kill()
            process.communicate(timeout=5)
        self.addCleanup(finish)
        pid = int(self.line(process))
        return process, pid

    def command(self, process, command):
        process.stdin.write(command + '\n')
        process.stdin.flush()
        return self.line(process)

    def files(self):
        return sorted((self.root/'state').glob('binstate_*'))

    def monitor(self):
        result = subprocess.run([self.build/'distccmon-text'], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stderr, result.stderr)
        return result.stdout

    def test_updates_reuse_file_and_preserve_read_permissions(self):
        process, pid = self.start()
        path, = self.files()
        info = path.stat()
        self.assertEqual(info.st_mode & 0o777, 0o640)
        descriptors = list(Path(f'/proc/{pid}/fd').iterdir())
        self.assertEqual(sum(fd.stat().st_ino == info.st_ino and
                             fd.stat().st_dev == info.st_dev for fd in descriptors), 1)
        self.assertIn('owner.c', self.monitor())
        for _ in range(20):
            self.assertEqual(self.command(process, 'p'), 'ok')
            self.assertIn('Receive', self.monitor())
            self.assertEqual(path.stat().st_ino, info.st_ino)
            self.assertEqual(path.stat().st_size, info.st_size)
        process.communicate('q\n', timeout=5)
        self.assertEqual(process.returncode, 0)
        self.assertFalse(self.files())

    def test_old_live_record_is_not_removed(self):
        self.start()
        path, = self.files()
        os.utime(path, (1, 1))
        self.assertIn('owner.c', self.monitor())
        self.assertTrue(path.exists())

    def test_fork_cleanup_and_child_updates_preserve_parent(self):
        process, pid = self.start()
        path, = self.files()
        before = path.read_bytes()
        for command in ('f', 'c'):
            self.assertEqual(self.command(process, command), 'ok')
            self.assertEqual(self.files(), [path])
            self.assertEqual(path.read_bytes(), before)
            self.assertIn('owner.c', self.monitor())

    def test_killed_owner_is_reaped_even_with_inherited_descriptor(self):
        process, pid = self.start()
        child = int(self.command(process, 'h'))
        self.addCleanup(os.kill, child, signal.SIGKILL)
        process.kill()
        process.wait(timeout=5)
        self.assertFalse(self.monitor().strip())
        self.assertFalse(self.files())

    def test_exec_releases_ownership(self):
        process, pid = self.start()
        assert process.stdin is not None
        process.stdin.write('e')
        process.stdin.flush()
        self.assertEqual(self.command(process, 'executed'), 'executed')
        self.assertIsNone(process.poll())
        self.assertFalse(self.monitor().strip())
        self.assertFalse(self.files())

    def test_two_pid_namespaces_keep_independent_records(self):
        prefix = ['unshare', '--user', '--map-root-user', '--pid', '--fork', '--kill-child']
        probe = subprocess.run([*prefix, 'true'], capture_output=True, text=True)
        if probe.returncode: self.skipTest(probe.stderr.strip())
        first, first_pid = self.start(prefix)
        second, second_pid = self.start(prefix)
        self.assertEqual(first_pid, second_pid)
        self.assertEqual(len(self.files()), 2)
        self.assertEqual(self.monitor().count('owner.c'), 2)
        first.communicate('q\n', timeout=5)
        self.assertEqual(first.returncode, 0)
        self.assertEqual(self.monitor().count('owner.c'), 1)
        second.communicate('q\n', timeout=5)
        self.assertFalse(self.files())


if __name__ == '__main__':
    unittest.main()
