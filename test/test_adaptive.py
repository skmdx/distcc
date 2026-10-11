#!/usr/bin/env python3
"""Exercise adaptive scheduling through real distcc/distccd/GCC compiles.

Run from the build directory after make, with TMPDIR on temporary storage.
Synthetic HTTP responses cover controlled loads and bad peers; compilation
always goes to a real daemon with local fallback disabled.
"""
import concurrent.futures
import contextlib
import fcntl
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def stats_response(cpus=4, jobs=4, load=0):
    return ("HTTP/1.0 200 OK\n\n<distccstats>\n"
            f"dcc_cpus {cpus}\ndcc_max_kids {jobs}\ndcc_load1 {load}\n"
            "</distccstats>\n").encode()


class StatsPeer:
    def __init__(self, response, delay=0):
        self.response, self.delay = response, delay
        self.requests = 0
        self.stop = threading.Event()
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.listen()
        self.sock.settimeout(0.05)
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(0.5)
                try:
                    conn.recv(1024)
                    self.requests += 1
                    if not self.stop.wait(self.delay):
                        conn.sendall(self.response)
                except OSError:
                    pass

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.sock.close()


class AdaptiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path.cwd()
        cls.cc = shutil.which("gcc")
        if not cls.cc:
            raise unittest.SkipTest("GCC is required")
        cls.storage = tempfile.TemporaryDirectory(prefix="distcc-adaptive-")
        cls.root = Path(cls.storage.name)
        cls.base_env = {k: v for k, v in os.environ.items()
                        if not k.startswith("DISTCC")}
        cls.daemons = []
        cls.ports = []
        cls.stats_ports = []
        cls.logs = []
        cls.addClassCleanup(cls.stop_daemons)
        # Slow the actual compiler slightly to exercise concurrent client locks.
        wrappers = cls.root / "bin"
        wrappers.mkdir()
        wrapper = wrappers / "gcc"
        wrapper.write_text('#!/bin/sh\nsleep 0.3\nexec ' +
                           "'" + cls.cc.replace("'", "'\\''") + "' \"$@\"\n")
        wrapper.chmod(0o755)
        target = subprocess.check_output([cls.cc, "-dumpmachine"], text=True).strip()
        (wrappers / (target + "-gcc")).symlink_to("gcc")
        for i in range(2):
            port, stats = free_port(), free_port()
            log = open(cls.root / f"daemon-{i}.log", "w+")
            cls.logs.append(log)
            env = dict(cls.base_env, TMPDIR=str(cls.root),
                       DISTCCD_PATH=str(wrappers) + os.pathsep + os.defpath)
            proc = subprocess.Popen([
                str(cls.build / "distccd"), "--daemon", "--no-detach",
                "--log-stderr", "--verbose", "--enable-tcp-insecure",
                "--listen", "127.0.0.1", "--allow", "127.0.0.1",
                "--port", str(port), "--stats", "--stats-port", str(stats),
                "--jobs", "4", "--pid-file", str(cls.root / f"daemon-{i}.pid")],
                env=env, stdout=log, stderr=log, start_new_session=True)
            cls.daemons.append(proc)
            cls.ports.append(port)
            cls.stats_ports.append(stats)
            deadline = time.monotonic() + 5
            while True:
                if proc.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(log.read())
                try:
                    cls.get_stats(stats)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.02)

    @classmethod
    def stop_daemons(cls):
        for proc in cls.daemons:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        for log in cls.logs:
            log.close()
        cls.storage.cleanup()

    @staticmethod
    def get_stats(port):
        with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
            sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
            response = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    return response.decode()
                response += chunk

    def setUp(self):
        self.work = self.root / self._testMethodName
        self.work.mkdir()
        self.ddir = self.work / "distcc"
        self.ddir.mkdir()
        self.source = self.work / "hello.c"
        self.source.write_text("int main(void) { return 0; }\n")

    def host(self, index, options="", slots=4):
        return f"127.0.0.1:{self.ports[index]}/{slots}{options}"

    def peer(self, response, delay=0):
        peer = StatsPeer(response, delay)
        self.addCleanup(peer.close)
        return peer

    def compile(self, hosts, name="job"):
        log = self.work / (name + ".log")
        obj = self.work / (name + ".o")
        env = dict(self.base_env, DISTCC_HOSTS=hosts, DISTCC_DIR=str(self.ddir),
                   DISTCC_FALLBACK="0", DISTCC_VERBOSE="1", DISTCC_LOG=str(log),
                   DISTCC_PAUSE_TIME_MSEC="20", TMPDIR=str(self.work))
        command = [str(self.build / "distcc"), "gcc", "-c", str(self.source), "-o", str(obj)]
        if ",cpp" in hosts:
            command.insert(0, str(self.build / "pump"))
        result = subprocess.run(command, env=env,
                                capture_output=True, text=True, timeout=10)
        text = log.read_text()
        self.assertEqual(result.returncode, 0, result.stderr + text)
        # Verify the returned object really links and runs.
        exe = self.work / name
        subprocess.run([self.cc, str(obj), "-o", str(exe)], check=True,
                       capture_output=True, timeout=10)
        subprocess.run([str(exe)], check=True, timeout=5)
        match = re.search(r"adaptive selected 127\.0\.0\.1:(\d+)", text)
        return int(match[1]) if match else None, text

    def hold_slot(self, index, slot):
        locks = self.ddir / "lock"
        locks.mkdir(exist_ok=True)
        stream = open(locks / f"cpu_tcp_127.0.0.1_{self.ports[index]}_{slot}", "w")
        fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.addCleanup(stream.close)
        return stream

    def test_real_stats_and_speed_distribution(self):
        for port in self.stats_ports:
            stats = self.get_stats(port)
            self.assertRegex(stats, r"dcc_cpus [1-9][0-9]*\n")
            self.assertIn("dcc_max_kids 4\n", stats)
            self.assertRegex(stats, r"dcc_load1 [0-9.]+\n")
        hosts = " ".join(self.host(i, f",stats={self.stats_ports[i]},speed={speed}")
                         for i, speed in enumerate((1, 4)))
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda i: self.compile(hosts, f"job{i}"), range(4)))
        counts = [sum(port == p for port, _ in results) for p in self.ports]
        for _, log in results:
            self.assertNotIn("adaptive stats unavailable", log)
        self.assertGreaterEqual(counts[1], 3, counts)
        print("real stats, speed 1:4, four compiles:", counts)

    def test_core_count_and_busy_slots(self):
        small = self.peer(stats_response(cpus=1))
        large = self.peer(stats_response(cpus=4))
        hosts = self.host(0, f",stats={small.port}") + " " + self.host(1, f",stats={large.port}")
        port, log = self.compile(hosts)
        self.assertEqual(port, self.ports[1], log)
        for slot in range(3):
            self.hold_slot(1, slot)
        self.assertEqual(self.compile(hosts, "busy")[0], self.ports[0])

    def test_automatic_slots_and_explicit_ceiling(self):
        peer = self.peer(stats_response(cpus=16, jobs=12))
        for limit, expected in (("", 12), ("/4", 4), ("/20", 12)):
            host = f"127.0.0.1:{self.ports[0]}{limit},stats={peer.port}"
            result = subprocess.run([str(self.build / "distcc"), "-j"],
                                    env=dict(self.base_env, DISTCC_HOSTS=host,
                                             DISTCC_DIR=str(self.ddir)),
                                    capture_output=True, text=True, check=True, timeout=5)
            self.assertEqual(int(result.stdout), expected)

        # The same automatic capacity must reach the selection path, not only -j.
        for slot in range(4):
            self.hold_slot(0, slot)
        host = f"127.0.0.1:{self.ports[0]},stats={peer.port},speed=10"
        self.assertEqual(self.compile(host + " " + self.host(1))[0], self.ports[0])

    def test_load_changes_after_cache_expiry(self):
        loaded = self.peer(stats_response(load=12))
        idle = self.peer(stats_response())
        hosts = self.host(0, f",stats={loaded.port}") + " " + self.host(1, f",stats={idle.port}")
        self.assertEqual(self.compile(hosts)[0], self.ports[1])
        loaded.response, idle.response = stats_response(), stats_response(load=12)
        time.sleep(1.05)
        self.assertEqual(self.compile(hosts, "changed")[0], self.ports[0])

    def test_core_and_daemon_limits(self):
        for field in ("cpus", "jobs"):
            peer = self.peer(stats_response(**{field: 1}))
            held = self.hold_slot(0, 0)
            hosts = self.host(0, f",stats={peer.port},speed=100") + " " + self.host(1)
            self.assertEqual(self.compile(hosts, field)[0], self.ports[1])
            held.close()

    def test_missing_invalid_and_stalled_stats(self):
        for i, response in enumerate((b"garbage", stats_response(load="nan"),
                                      stats_response(cpus=-1),
                                      stats_response().replace(b"dcc_cpus 4\n", b""),
                                      stats_response()[:-20])):
            peer = self.peer(response)
            hosts = self.host(0, f",stats={peer.port},speed=4") + " " + self.host(1)
            port, log = self.compile(hosts, f"invalid{i}")
            self.assertEqual(port, self.ports[0])
            self.assertIn("adaptive stats unavailable", log)
        peer = self.peer(stats_response(), delay=2)
        start = time.monotonic()
        port, log = self.compile(self.host(0, f",stats={peer.port} " ) + self.host(1), "timeout")
        self.assertIn("adaptive stats unavailable", log)
        self.assertLess(time.monotonic() - start, 1.5)

    def test_shared_cache(self):
        peer = self.peer(stats_response())
        hosts = self.host(0, f",stats={peer.port}")
        # Stay within a cache second, including the 0.3s compiler delay.
        time.sleep(1 - time.time() % 1 + 0.02)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(lambda i: self.compile(hosts, f"cached{i}"), range(3)))
        self.assertEqual(peer.requests, 1)

    def test_duplicate_host_keeps_slot_locked(self):
        host = self.host(0, ",speed=2", slots=1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            job = pool.submit(self.compile, host + " " + host)
            deadline = time.monotonic() + 5
            log = self.work / "job.log"
            while not log.exists() or "adaptive selected" not in log.read_text():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.005)
            with open(self.ddir / "lock" / f"cpu_tcp_127.0.0.1_{self.ports[0]}_0", "w") as stream:
                with self.assertRaises(BlockingIOError):
                    fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            job.result()

    def test_waits_for_a_slot(self):
        held = self.hold_slot(0, 0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            job = pool.submit(self.compile, self.host(0, ",speed=1", slots=1))
            try:
                time.sleep(0.15)
                self.assertFalse(job.done())
            finally:
                held.close()
            self.assertEqual(job.result()[0], self.ports[0])

    def test_legacy_and_compression(self):
        self.assertIsNone(self.compile(self.host(0))[0])
        self.assertEqual(self.compile(self.host(0, ",lzo,speed=1"), "lzo")[0], self.ports[0])

    def test_pump(self):
        if not (self.build / "pump").exists() or not list(
                self.build.glob("_include_server/lib*/include_server/*.so")):
            self.skipTest("pump/include server not built")
        self.source.write_text('#include <stdio.h>\nint main(void) { return puts("ok") < 0; }\n')
        host = self.host(0, f",lzo,cpp,stats={self.stats_ports[0]}")
        port, log = self.compile(host)
        self.assertEqual(port, self.ports[0])
        self.assertNotIn("adaptive stats unavailable", log)

    def test_invalid_options(self):
        for option in ("stats=0", "stats=65536", "stats=", "stats=12x",
                       "speed=0", "speed=-1", "speed=nan", "speed=inf",
                       "speed=1001", "speed=2x", "speed="):
            result = subprocess.run([str(self.build / "h_hosts")],
                                    env=dict(self.base_env, DISTCC_HOSTS=self.host(0, "," + option)),
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 106, option)
        result = subprocess.run([str(self.build / "h_hosts")],
                                env=dict(self.base_env, DISTCC_HOSTS="@host,stats=3633"),
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 106)


if __name__ == "__main__":
    unittest.main()
