#!/usr/bin/env python3
"""Sequential TCP sessions, cancellation and idle-worker availability."""
import array
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import time
import unittest

from test_broker import BrokerTestCase


class PoolTest(BrokerTestCase):
    def test_fragmented_admission_and_cancel(self):
        self.start_broker(self.host(0, f',stats={self.stats_ports[0]}'))
        net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
        self.addCleanup(net.close)
        net.sendall(b'SLO')
        net.settimeout(.05)
        with self.assertRaises(socket.timeout): net.recv(12)
        net.sendall(b'T00000001')
        net.settimeout(2)
        self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'GRNT00000001')
        self.assertIn('dcc_busy 1\n', self.get_stats(self.stats_ports[0]))
        net.close()
        self.job('after-admission-cancel')

    def accepts(self):
        return int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])

    def pooled_lease(self):
        lease = self.request(6)
        status, capabilities, fds = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertTrue(capabilities & (1 << 30))
        self.assertEqual(len(fds), 1)
        net = socket.socket(fileno=fds[0])
        self.addCleanup(net.close)
        return lease, net

    def test_completed_jobs_reuse_one_connection(self):
        self.start_broker(self.host(0, f',stats={self.stats_ports[0]}'))
        before = self.accepts()
        for i in range(5): self.job(i)
        self.assertEqual(self.accepts() - before, 1)

    def test_idle_sessions_do_not_occupy_workers(self):
        self.start_broker()
        for _ in range(12):  # The daemon has only four workers.
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=3)
            self.addCleanup(net.close)
            net.sendall(b'DIST00000005')
            self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'POOL00000001')
        self.job(0)

    def test_abandoned_job_discards_connection(self):
        self.start_broker()
        lease, net = self.pooled_lease()
        before = self.accepts()
        # Abandon a partial request. It must never be handed to another job.
        net.sendall(b'DIST00000001ARGC00000002ARGV00000003gcc')
        net.close()
        lease.close()
        self.job(0)
        self.job(1)
        self.assertEqual(self.accepts() - before, 1)
    def test_shutdown_return_is_discarded(self):
        self.start_broker()
        lease, net = self.pooled_lease()
        before = self.accepts()
        net.shutdown(socket.SHUT_RDWR)
        lease.sendmsg([struct.pack('=II', 2, 7)],
                      [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        net.close()
        lease.close()
        self.job(0)
        self.assertEqual(self.accepts() - before, 1)

    def test_server_restart_reconnects(self):
        self.start_broker()
        self.job(0)
        old = self.daemons[0]
        os.killpg(old.pid, signal.SIGTERM)
        old.wait(timeout=5)
        env = dict(self.base_env, TMPDIR=str(self.root),
                   DISTCCD_PATH=str(self.root / 'bin') + os.pathsep + os.defpath)
        replacement = subprocess.Popen(old.args, env=env, stdout=self.logs[0],
                                       stderr=self.logs[0], start_new_session=True)
        self.daemons[0] = replacement
        deadline = time.monotonic() + 5
        while True:
            try:
                self.get_stats(self.stats_ports[0])
                break
            except OSError:
                self.assertIsNone(replacement.poll())
                if time.monotonic() >= deadline: raise
                time.sleep(.02)
        self.job(1)
        self.job(2)
        self.assertEqual(self.accepts(), 1)

    def test_broker_exits_with_idle_connections(self):
        proc = self.start_broker(idle=1)
        self.job(0)
        self.assertEqual(proc.wait(timeout=3), 0)
        self.assertFalse(Path(self.env['DISTCC_BROKER']).exists())

    def test_worker_replacement_preserves_idle_session(self):
        self.start_broker(self.host(0, f',stats={self.stats_ports[0]}'))
        before = self.accepts()
        self.job(0)
        parent = self.daemons[0].pid
        children = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
        self.assertEqual(len(children), 4)
        for child in children: os.kill(int(child), signal.SIGTERM)
        self.job(1)
        self.assertEqual(self.accepts() - before, 1)

    def test_compiler_error_preserves_framing(self):
        self.start_broker()
        before = self.accepts()
        source = self.work / 'invalid.c'
        source.write_text('int main( {\n')
        result = subprocess.run([str(self.build/'distcc'), 'gcc', '-c', str(source),
                                 '-o', str(self.work/'invalid.o')], env=self.env,
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.job(0)
        self.assertEqual(self.accepts() - before, 1)

    def test_killed_client_does_not_return_session(self):
        self.start_broker()
        source = self.work / 'cancel.c'
        source.write_text('int main(void) { return 0; }\n')
        log = self.work / 'cancel.log'
        env = dict(self.env, DISTCC_VERBOSE='1', DISTCC_LOG=str(log))
        before = self.accepts()
        proc = subprocess.Popen([str(self.build/'distcc'), 'gcc', '-c', str(source),
                                 '-o', str(self.work/'cancel.o')], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while not log.exists() or 'client finished sending request' not in log.read_text():
                self.assertIsNone(proc.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.005)
            proc.kill()
            proc.wait(timeout=5)
        finally:
            if proc.poll() is None: proc.kill()
            proc.wait(timeout=5)
        self.job(0)
        self.job(1)
        self.assertEqual(self.accepts() - before, 2)


class SilentPeerTest(BrokerTestCase):
    daemon_env = {'DISTCC_IO_TIMEOUT': '1'}

    def test_silent_partial_request_releases_worker(self):
        net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=4)
        self.addCleanup(net.close)
        net.sendall(b'SLOT00000001')
        self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'GRNT00000001')
        net.sendall(b'DIST00000001ARGC00000002ARGV00000003gcc')
        self.assertIn('dcc_busy 1\n', self.get_stats(self.stats_ports[0]))
        # Keep both directions open: this must be a timeout, not EOF detection.
        self.assertEqual(net.recv(1), b'')
        deadline = time.monotonic() + 3
        while 'dcc_busy 0\n' not in self.get_stats(self.stats_ports[0]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.start_broker()
        self.job('after-timeout')


if __name__ == '__main__':
    unittest.main()
