#!/usr/bin/env python3
"""HTTP/2 multiplexing and cancellation through real distccd workers."""
import concurrent.futures
import io
import re
import random
import os
from pathlib import Path
import runpy
import select
import signal
import socket
import struct
import subprocess
import time
import unittest

from test_broker import BrokerTestCase


class Http2Test(BrokerTestCase):
    def start_h2(self, name='broker'):
        return self.start_broker(self.host(0, f',h2,lz4,stats={self.stats_ports[0]}') +
                                 ' --localslots_cpp=4', name=name)

    def restart_server(self, binary):
        old = self.daemons[0]
        os.killpg(old.pid, signal.SIGTERM)
        old.wait(timeout=5)
        env = dict(self.base_env, TMPDIR=str(self.root),
                   DISTCCD_PATH=str(self.root / 'bin') + os.pathsep + os.defpath)
        replacement = subprocess.Popen([str(binary), *old.args[1:]], env=env, stdout=self.logs[0],
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

    def accepts(self):
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        return int(match[1])

    def test_parallel_jobs_share_connection(self):
        self.start_h2()
        before = self.accepts()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(self.job, range(8)))
        self.assertEqual(len(results), 8)
        self.assertEqual(self.accepts() - before, 1)
        self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))

    def test_cancel_stream_preserves_other_jobs(self):
        self.start_h2()
        before = self.accepts()
        lease = self.request(6)
        status, capabilities, fds = self.receive(lease)
        self.assertEqual((status, capabilities, len(fds)), (0, 0, 1))
        with socket.socket(fileno=fds[0]) as net:
            net.sendall(b'DIST00000006ARGC00000002ARGV00000003gcc')
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                other = pool.submit(self.job, 'uncancelled')
                lease.close()
                other.result(timeout=20)
        self.job('after-cancel')
        self.assertEqual(self.accepts() - before, 1)

    def test_large_incompressible_result_and_input(self):
        self.start_h2()
        self.check_large_transfer()

    def check_large_transfer(self):
        source = self.work / 'large.i'
        data = random.Random(19).randbytes(1024 * 1024)
        source.write_text('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n')
        remote, payload = self.work / 'remote.o', self.work / 'payload'
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source), '-o', str(remote)],
                                env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        subprocess.run(['objcopy', '--dump-section', '.rodata=' + str(payload), str(remote)], check=True,
                       capture_output=True, timeout=30)
        self.assertEqual(payload.read_bytes(), data)

    def test_slow_result_consumer_preserves_other_jobs(self):
        self.start_h2()
        data = random.Random(43).randbytes(2 * 1024 * 1024)
        body = ('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n').encode()
        args = [b'gcc', b'-c', b'source.i', b'-o', b'result.o']
        header = b'DIST00000008' + f'ARGC{len(args):08x}'.encode()
        header += b''.join(f'ARGV{len(arg):08x}'.encode() + arg for arg in args)
        with self.request(6) as lease:
            status, _, fds = self.receive(lease)
            self.assertEqual(status, 0)
            with socket.socket(fileno=fds[0]) as net:
                net.settimeout(20)
                net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                net.sendall(header + b'DOTIffffffff')
                for start in range(0, len(body), 262144):
                    chunk = body[start:start + 262144]
                    net.sendall(struct.pack('!II', len(chunk), 0) + chunk)
                net.sendall(bytes(8))
                net.shutdown(socket.SHUT_WR)
                self.assertTrue(select.select([net], [], [], 20)[0])
                # Leave a result larger than every stream/socket buffer unread
                # while another real compilation completes on the same session.
                self.job('while-result-blocked')
                helper = runpy.run_path(str(self.build / 'distcc-link'))
                codec = helper['Lz4Chunks']()
                result = self.work / 'slow.o'
                with net.makefile('rb') as response, result.open('wb') as output:
                    self.assertEqual(helper['token'](response, 'DONE'), 8)
                    self.assertEqual(helper['token'](response, 'STAT'), 0)
                    helper['copy_response'](response, 'SERR', io.BytesIO(), codec)
                    helper['copy_response'](response, 'SOUT', io.BytesIO(), codec)
                    helper['copy_response'](response, 'DOTO', output, codec)
                    self.assertEqual(response.read(1), b'')
        payload = self.work / 'slow.payload'
        subprocess.run(['objcopy', '--dump-section', '.rodata=' + str(payload), str(result)],
                       check=True, capture_output=True, timeout=10)
        self.assertEqual(payload.read_bytes(), data)

    def test_old_lease_cannot_cancel_reconnected_stream(self):
        self.start_h2()
        old_lease = self.request(6)
        status, _, fds = self.receive(old_lease)
        self.assertEqual(status, 0)
        old_net = socket.socket(fileno=fds[0])
        self.addCleanup(old_lease.close)
        self.addCleanup(old_net.close)
        self.restart_server(self.build / 'distccd')
        time.sleep(5.1)  # The broken connection's peer backoff expires.
        with self.request(6) as lease:
            lease.settimeout(10)
            status, _, fds = self.receive(lease)
            self.assertEqual(status, 0)
            with socket.socket(fileno=fds[0]) as net:
                old_lease.close()
                net.settimeout(.2)
                with self.assertRaises(socket.timeout):
                    net.recv(1)
        self.job('reconnected')

    @unittest.skipUnless(os.environ.get('DISTCC_TEST_BASELINE'), 'previous HTTP/2 build is required')
    def test_previous_http2_peers(self):
        previous = Path(os.environ['DISTCC_TEST_BASELINE']).resolve()
        candidate = self.build
        try:
            self.build = previous
            self.start_h2('previous')
            self.check_large_transfer()
        finally:
            self.build = candidate
        try:
            self.restart_server(previous / 'distccd')
            self.start_h2('candidate')
            self.check_large_transfer()
        finally:
            self.restart_server(candidate / 'distccd')

    def test_blocked_worker_does_not_spin_broker(self):
        broker = self.start_h2()
        parent = self.daemons[0].pid
        workers = list(map(int, Path(f'/proc/{parent}/task/{parent}/children').read_text().split()))
        source = self.work / 'blocked.i'
        data = random.Random(31).randbytes(1024 * 1024)
        source.write_text('const unsigned char blob[] = {' + ','.join(map(str, data)) + '};\n')
        command = [str(self.build/'distcc'), 'gcc', '-c', str(source), '-o', str(self.work/'blocked.o')]
        def cpu():
            fields = Path(f'/proc/{broker.pid}/stat').read_text().rsplit(')', 1)[1].split()
            return sum(map(int, fields[11:13])) / os.sysconf('SC_CLK_TCK')
        proc = None
        errors = b''
        try:
            for pid in workers: os.kill(pid, signal.SIGSTOP)
            proc = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(.3)  # Fill the bounded HTTP/2 window and local socket.
            self.assertIsNone(proc.poll())
            before = cpu()
            time.sleep(.4)
            self.assertLess(cpu() - before, .15, 'broker spun while its stream window was exhausted')
        finally:
            for pid in workers: os.kill(pid, signal.SIGCONT)
            if proc:
                try:
                    _, errors = proc.communicate(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
                    raise
        assert proc is not None
        self.assertEqual(proc.returncode, 0, errors.decode())


if __name__ == '__main__':
    unittest.main()
