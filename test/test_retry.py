#!/usr/bin/env python3
"""Real-worker argument rejection, retry bounds and preprocessing lifetime."""
import contextlib
import os
import socket
import socketserver
import subprocess
import threading
import time
import unittest

from test_adaptive import stats_response
from test_broker import BrokerTestCase


class RejectingProxy(socketserver.ThreadingTCPServer):
    """Change -c to -M in transit so the real daemon rejects the argv."""
    daemon_threads = True

    def __init__(self, port):
        self.worker_port, self.rejections = port, 0
        super().__init__(('127.0.0.1', 0), RejectionConnection)
        self.thread = threading.Thread(target=self.serve_forever)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.thread.join()
        self.server_close()


class RejectionConnection(socketserver.BaseRequestHandler):
    def handle(self):
        with socket.create_connection(('127.0.0.1', self.server.worker_port), timeout=3) as worker:
            worker.settimeout(5)
            self.request.settimeout(5)

            def copy(source, target, reject=False):
                pending = b''
                needle = b'ARGV00000002-c'
                try:
                    while data := source.recv(65536):
                        pending += data
                        if reject and needle in pending:
                            pending = pending.replace(needle, b'ARGV00000002-M', 1)
                            self.server.rejections += 1
                            reject = False
                        # Retain only a possible fragmented token, so SLOT and
                        # POOL handshakes are forwarded without buffering.
                        tail = 0
                        if reject:
                            for n in range(1, len(needle)):
                                if pending.endswith(needle[:n]): tail = n
                        if len(pending) > tail:
                            target.sendall(pending[:len(pending)-tail])
                            pending = pending[len(pending)-tail:]
                except OSError:
                    pass
                finally:
                    with contextlib.suppress(OSError):
                        target.shutdown(socket.SHUT_RDWR)

            reverse = threading.Thread(target=copy, args=(worker, self.request))
            reverse.start()
            copy(self.request, worker, True)
            reverse.join(timeout=6)


class RetryTest(BrokerTestCase):
    def rejecting_host(self):
        proxy = RejectingProxy(self.ports[0])
        self.addCleanup(proxy.close)
        peer = self.peer(stats_response(admission=1, available=4))
        return proxy, f'127.0.0.1:{proxy.server_address[1]}/4,stats={peer.port},speed=100'

    def test_argument_rejection_fallback_disabled_and_enabled(self):
        for fallback in ('0', '1'):
            with self.subTest(fallback=fallback):
                proxy, host = self.rejecting_host()
                self.start_broker(host, name='broker-' + fallback)
                obj = self.work / ('fallback-' + fallback + '.o')
                result = subprocess.run([str(self.build/'distcc'), 'gcc', '-c',
                                         str(self.source), '-o', str(obj)],
                                        env=dict(self.env, DISTCC_FALLBACK=fallback),
                                        capture_output=True, text=True, timeout=8)
                self.assertEqual(proxy.rejections, 1, result.stderr)
                if fallback == '0':
                    self.assertNotEqual(result.returncode, 0, result.stderr)
                    self.assertFalse(obj.exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    exe = self.work/'fallback-exe'
                    subprocess.run(['gcc', str(obj), '-o', str(exe)], check=True)
                    subprocess.run([str(exe)], check=True)

    def client_compiler(self, block=False):
        directory = self.work/'client-bin'
        directory.mkdir()
        wrapper = directory/'gcc'
        marker = self.work/'cpp-count'
        gate = self.work/'cpp-release'
        # Only the client sees this wrapper; the worker uses the fixture GCC.
        wrapper.write_text('#!/bin/sh\nfor arg do\nif [ "$arg" = -E ]; then\n'
                           'echo cpp >> "' + str(marker) + '"\n' +
                           ('while [ ! -e "' + str(gate) + '" ]; do sleep .02; done\n' if block else '') +
                           'fi\ndone\nexec "' + self.cc + '" "$@"\n')
        wrapper.chmod(0o755)
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        (directory/(target + '-gcc')).symlink_to('gcc')
        self.env['PATH'] = str(directory) + os.pathsep + self.env['PATH']
        return marker, gate

    def test_disabled_backoff_still_bounds_retries(self):
        self.base_env = dict(self.base_env, DISTCC_BACKOFF_PERIOD='0')
        proxy, host = self.rejecting_host()
        self.start_broker(host)
        result = subprocess.run([str(self.build/'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(self.work/'rejected.o')], env=self.env,
                                capture_output=True, timeout=8)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(proxy.rejections, 3)

    def test_retry_reuses_preprocessed_input(self):
        proxy, host = self.rejecting_host()
        self.start_broker(host + ' ' + self.host(1, f',stats={self.stats_ports[1]}'))
        marker, _ = self.client_compiler()
        self.job('recovered')
        self.assertEqual(proxy.rejections, 1)
        self.assertEqual(marker.read_text().splitlines(), ['cpp'])

    def test_preprocessing_does_not_reserve_worker(self):
        self.start_broker(self.host(0, f',stats={self.stats_ports[0]}'))
        marker, gate = self.client_compiler(block=True)
        with subprocess.Popen([str(self.build/'distcc'), 'gcc', '-c', str(self.source),
                               '-o', str(self.work/'blocked.o')], env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
            try:
                deadline = time.monotonic() + 5
                while not marker.exists():
                    self.assertIsNone(proc.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.02)
                self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))
                gate.touch()
                _, stderr = proc.communicate(timeout=8)
                self.assertEqual(proc.returncode, 0, stderr)
            finally:
                gate.touch()
                if proc.poll() is None: proc.kill()


if __name__ == '__main__':
    unittest.main()
