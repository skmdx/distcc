#!/usr/bin/env python3
"""Memory admission with actual cgroup accounting and live daemon sockets.

Run in a dedicated 384 MiB cgroup with swap disabled. Larger/unlimited test
runners skip this test rather than allocating host-sized amounts of memory.
"""
from pathlib import Path
import re
import socket
import time
import unittest

from test_adaptive import DistccTestCase


class MemoryAdmissionTest(DistccTestCase):
    daemon_env = {'DISTCC_JOB_MEMORY': '128'}

    def test_budget_and_headroom_recover(self):
        path = next((line[3:] for line in Path('/proc/self/cgroup').read_text().splitlines()
                     if line.startswith('0::/')), None)
        if path is None: self.skipTest('cgroup v2 required')
        group = Path('/sys/fs/cgroup') / path.lstrip('/')
        maximum = (group/'memory.max').read_text().strip()
        if maximum == 'max' or not 320*1024**2 <= int(maximum) <= 512*1024**2:
            self.skipTest('run in a dedicated 384 MiB cgroup')
        maximum = int(maximum)
        self.assertEqual((group/'memory.swap.max').read_text().strip(), '0')
        expected = min(4, (maximum - 32*1024**2) // (128*1024**2))

        def available():
            return int(re.search(r'dcc_available (\d+)', self.get_stats(self.stats_ports[0]))[1])

        def wait_available(value):
            deadline = time.monotonic() + 4
            while available() != value:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.05)

        wait_available(expected)
        leases = []
        for _ in range(expected):
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
            self.addCleanup(net.close)
            net.sendall(b'SLOT00000001')
            self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'GRNT00000001')
            leases.append(net)
        net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
        self.addCleanup(net.close)
        net.sendall(b'SLOT00000001')
        self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'BUSY00000000')
        for lease in leases: lease.close()
        wait_available(expected)
        used = int((group/'memory.current').read_text())
        allocation = maximum - used - 96*1024**2
        self.assertGreater(allocation, 0)
        # Actual resident pages leave insufficient headroom without hitting max.
        memory = bytearray(allocation)
        try:
            wait_available(0)
            net.sendall(b'SLOT00000001')
            self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'BUSY00000000')
        finally:
            del memory
        wait_available(expected)
        self.compile(self.host(0))


if __name__ == '__main__':
    unittest.main()
