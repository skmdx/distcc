#!/usr/bin/env python3
"""Broker IPC, lifetime and scheduling with real distccd/GCC workers."""
import array
import concurrent.futures
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import time
import unittest

from test_adaptive import DistccTestCase, free_port, stats_response


class BrokerTest(DistccTestCase):
    def start_broker(self, hosts=None, idle=10):
        path = self.root / "broker.sock"
        env = dict(self.base_env, DISTCC_HOSTS=hosts or self.host(0),
                   DISTCC_DIR=str(self.ddir), DISTCC_BROKER_IDLE=str(idle))
        log = open(self.work / "broker.log", "w+")
        self.addCleanup(log.close)
        proc = subprocess.Popen([str(self.build / "distccd"), "--broker", str(path)],
                                env=env, stdout=log, stderr=log)
        def stop():
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=5)
        self.addCleanup(stop)
        deadline = time.monotonic() + 5
        while not path.exists():
            if proc.poll() is not None or time.monotonic() > deadline:
                log.seek(0)
                self.fail(log.read())
            time.sleep(.01)
        self.env = dict(env, DISTCC_BROKER=str(path), DISTCC_FALLBACK="0",
                        TMPDIR=str(self.work))
        return proc

    def request(self, command=1):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sock.settimeout(5)
        sock.connect(self.env["DISTCC_BROKER"])
        sock.sendall(struct.pack("=II", 1, command))
        self.addCleanup(sock.close)
        return sock

    def receive(self, sock):
        reply, ancillary, flags, _ = sock.recvmsg(1032, socket.CMSG_SPACE(4))
        self.assertEqual(flags, 0)
        self.assertEqual(len(reply), 1032)
        status, slots = struct.unpack("=ii", reply[:8])
        descriptors = []
        for level, kind, data in ancillary:
            self.assertEqual((level, kind), (socket.SOL_SOCKET, socket.SCM_RIGHTS))
            descriptors.extend(array.array("i", data))
        return status, slots, descriptors

    def job(self, index, language="c", prefix=()):
        source = self.work / f"source-{index}.{language}"
        source.write_text("int main() { return 0; }\n")
        obj = self.work / f"job-{index}.o"
        compiler = "g++" if language == "cc" else "gcc"
        result = subprocess.run([*prefix, str(self.build / "distcc"), compiler, "-c",
                                 str(source), "-o", str(obj)], env=self.env,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        exe = self.work / f"exe-{index}"
        subprocess.run([compiler, str(obj), "-o", str(exe)], check=True,
                       capture_output=True, timeout=10)
        subprocess.run([str(exe)], check=True, timeout=5)
        return obj.read_bytes()

    def test_parallel_real_compiles_and_no_children(self):
        proc = self.start_broker(self.host(0) + " " + self.host(1))
        with concurrent.futures.ThreadPoolExecutor(max_workers=28) as pool:
            objects = list(pool.map(self.job, range(28)))
        self.assertEqual(len(objects), 28)
        self.job(29, "cc")
        children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children")
        self.assertEqual(children.read_text().strip(), "")
        self.assertLess(len(list(Path(f"/proc/{proc.pid}/fd").iterdir())), 12)

    def test_lease_release_and_idle(self):
        proc = self.start_broker(self.host(0, slots=1), idle=1)
        first = self.request()
        status, _, fds = self.receive(first)
        self.assertEqual(status, 0)
        for fd in fds:
            os.close(fd)
        second = self.request()
        second.settimeout(1.3)
        with self.assertRaises(socket.timeout):
            second.recv(1032)
        self.assertIsNone(proc.poll(), "active jobs must prevent idle exit")
        first.close()
        status, _, fds = self.receive(second)
        self.assertEqual(status, 0)
        for fd in fds:
            os.close(fd)
        second.close()
        self.assertEqual(proc.wait(timeout=3), 0)
        self.assertFalse(Path(self.env["DISTCC_BROKER"]).exists())

    def test_stats_refresh_does_not_block_warm_requests(self):
        slow = self.peer(stats_response(), delay=.8)
        fast = self.peer(stats_response(cpus=2, jobs=2))
        self.start_broker(self.host(0, f",stats={slow.port}") + " " +
                          self.host(1, f",stats={fast.port}"))
        subprocess.run([str(self.build / "distcc"), "-j"], env=self.env,
                       check=True, capture_output=True, timeout=5)
        start = time.monotonic()
        result = subprocess.run([str(self.build / "distcc"), "-j"], env=self.env,
                                check=True, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout.strip(), "6")
        self.assertLess(time.monotonic() - start, .2)

    def test_dead_host_is_skipped(self):
        self.start_broker(f"127.0.0.1:{free_port()}/8 " + self.host(1))
        self.job(0)

    def test_singleton(self):
        proc = self.start_broker()
        second = subprocess.run([str(self.build / "distccd"), "--broker",
                                 self.env["DISTCC_BROKER"]], env=self.env,
                                capture_output=True, timeout=5)
        self.assertNotEqual(second.returncode, 0)
        self.assertIsNone(proc.poll())
        self.job(0)

    def test_socket_activation_with_isolated_network(self):
        if not shutil.which("systemd-socket-activate") or not shutil.which("unshare"):
            self.skipTest("systemd-socket-activate and unshare required")
        isolation = ["unshare", "--user", "--map-root-user", "--net"]
        check = subprocess.run([*isolation, "true"], capture_output=True)
        if check.returncode:
            self.skipTest("unprivileged network namespaces unavailable")
        path = self.root / "activation.sock"
        self.env = dict(self.base_env, DISTCC_HOSTS=self.host(0),
                        DISTCC_DIR=str(self.ddir), DISTCC_BROKER=str(path),
                        DISTCC_BROKER_IDLE="1", DISTCC_FALLBACK="0",
                        TMPDIR=str(self.work))
        command = ["systemd-socket-activate", "--seqpacket", "-l", str(path)]
        for name in ("DISTCC_HOSTS", "DISTCC_DIR", "DISTCC_BROKER_IDLE"):
            command.extend(["-E", name])
        command.extend([str(self.build / "distccd"), "--broker", str(path)])
        with subprocess.Popen(command, env=self.env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE) as proc:
            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    self.assertIsNone(proc.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                self.job(0, prefix=isolation)
                self.assertEqual(proc.wait(timeout=5), 0)
                self.assertTrue(path.exists(), "inherited socket must not be unlinked")
            finally:
                if proc.poll() is None:
                    proc.terminate()


if __name__ == "__main__":
    unittest.main()
