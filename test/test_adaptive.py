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


def stats_response(cpus: float | str = 4, jobs: float | str = 4,
                   load: float | str = 0, cores=None,
                   core_scale: float | str = 1, smt_gain: float | str = .25,
                   cpu_scale: float | str = -1, cpu_age: float | str = -1,
                   cpu_probe: float | str = 0, **metrics):
    return ("HTTP/1.0 200 OK\n\n<distccstats>\n"
            f"dcc_cpus {cpus}\ndcc_max_kids {jobs}\ndcc_load1 {load}\n"
            f"dcc_cores {cpus if cores is None else cores}\ndcc_smt_gain {smt_gain}\n"
            f"dcc_core_scale {core_scale}\n"
            f"dcc_cpu_scale {cpu_scale}\ndcc_cpu_age {cpu_age}\ndcc_cpu_probe {cpu_probe}\n"
            + ''.join(f'dcc_{name} {value}\n' for name, value in metrics.items())
            + "</distccstats>\n").encode()


class StatsPeer:
    def __init__(self, response, delay: float = 0):
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


class DistccTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path.cwd()
        compiler = shutil.which("gcc")
        if not compiler:
            raise unittest.SkipTest("GCC is required")
        cls.cc = compiler
        cls.storage = tempfile.TemporaryDirectory(prefix="distcc-adaptive-")
        cls.root = Path(cls.storage.name)
        cls.base_env = {k: v for k, v in os.environ.items()
                        if not k.startswith("DISTCC")}
        # These fixtures compile tiny C units; do not reserve the production
        # C++ budget for every test worker sharing the runner's cgroup.
        cls.base_env['DISTCC_JOB_MEMORY'] = '64'
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
        # Use the client's selected slotted compiler for bare Clang names too.
        for name in ('clang', 'clang++'):
            compiler_path = shutil.which(name)
            if compiler_path:
                (wrappers / name).symlink_to(compiler_path)
        target = subprocess.check_output([cls.cc, "-dumpmachine"], text=True).strip()
        (wrappers / (target + "-gcc")).symlink_to("gcc")
        for i in range(2):
            port, stats = free_port(), free_port()
            log = open(cls.root / f"daemon-{i}.log", "w+")
            cls.logs.append(log)
            env = dict(cls.base_env, TMPDIR=str(cls.root),
                       DISTCCD_PATH=str(wrappers) + os.pathsep + os.defpath)
            env.update(getattr(cls, "daemon_env", {}))
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

    def peer(self, response, delay: float = 0):
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

class AdaptiveTest(DistccTestCase):
    def test_topology_tracks_allowed_cpu_set(self):
        if not hasattr(os, 'sched_getaffinity'):
            self.skipTest('Linux CPU affinity is required')
        pid = self.daemons[0].pid
        original = os.sched_getaffinity(pid)
        groups = {}
        for cpu in original:
            key = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text()
            groups.setdefault(key, []).append(cpu)
        try:
            for selected in (original, set(next(iter(groups.values()))), {min(original)}, original):
                os.sched_setaffinity(pid, selected)
                stats = self.get_stats(self.stats_ports[0])
                cores = len({Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text()
                             for cpu in selected})
                self.assertIn(f'dcc_cpus {len(selected)}\n', stats)
                self.assertIn(f'dcc_cores {cores}\n', stats)
        finally:
            os.sched_setaffinity(pid, original)

    def test_smt_load_changes_host_selection(self):
        first = self.peer(stats_response(cpus=8, cores=4, jobs=8, busy=4, queued=0))
        second = self.peer(stats_response(cpus=8, cores=8, jobs=8, busy=0, queued=0))
        hosts = self.host(0, f',stats={first.port},speed=1') + ' ' + self.host(1, f',stats={second.port},speed=.9')
        port, _ = self.compile(hosts, 'smt-contention')
        self.assertEqual(port, self.ports[1])

    def test_measured_cpu_contention_changes_host_selection(self):
        for name, metrics, other_speed, selected in (
                ('physical-scaling', dict(busy=3, core_scale=.5), .8, 1),
                ('smt-loss', dict(busy=7, smt_gain=-.25), .4, 1),
                ('fast-smt', dict(busy=4), .5, 0),
                ('external-work', dict(busy=0, running=4), .9, 1)):
            with self.subTest(name=name):
                first = self.peer(stats_response(cpus=8, cores=4, jobs=8, queued=0, **metrics))
                second = self.peer(stats_response(cpus=8, cores=8, jobs=8, busy=0, queued=0))
                hosts = (self.host(0, f',stats={first.port},speed=1') + ' ' +
                         self.host(1, f',stats={second.port},speed={other_speed}'))
                port, _ = self.compile(hosts, name)
                self.assertEqual(port, self.ports[selected])

    def test_real_stats_and_speed_distribution(self):
        for port in self.stats_ports:
            stats = self.get_stats(port)
            self.assertRegex(stats, r"dcc_cpus [1-9][0-9]*\n")
            self.assertIn("dcc_max_kids 4\n", stats)
            self.assertRegex(stats, r"dcc_load1 [0-9.]+\n")
            self.assertRegex(stats, r"dcc_free_mem -?[0-9]+ MB\n")
            for diagnostic in ('dcc_num_procstate_D', 'dcc_max_RSS', 'dcc_max_RSS_name'):
                self.assertNotIn(diagnostic + ' ', stats)
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

    def test_jobs_use_effective_capacity_not_idle_slots(self):
        for capacity, expected in ((2, 2), (0, 0), (-1, 8)):
            with self.subTest(capacity=capacity):
                peer = self.peer(stats_response(cpus=8, jobs=8, capacity=capacity,
                                                busy=2, available=0))
                result = subprocess.run([str(self.build / 'distcc'), '-j'],
                    env=dict(self.base_env, DISTCC_HOSTS=f'127.0.0.1:{self.ports[0]},stats={peer.port}',
                             DISTCC_DIR=str(self.ddir)),
                    capture_output=True, text=True, check=True, timeout=5)
                self.assertEqual(int(result.stdout), expected)

    def test_load_changes_after_cache_expiry(self):
        loaded = self.peer(stats_response(load=12))
        idle = self.peer(stats_response())
        hosts = self.host(0, f",stats={loaded.port}") + " " + self.host(1, f",stats={idle.port}")
        self.assertEqual(self.compile(hosts)[0], self.ports[1])
        loaded.response, idle.response = stats_response(), stats_response(load=12)
        time.sleep(1.05)
        self.assertEqual(self.compile(hosts, "changed")[0], self.ports[0])

    def test_live_occupancy_and_memory_pressure(self):
        current = self.peer(stats_response(load=100, running=0, busy=0, queued=0))
        other = self.peer(stats_response())
        hosts = self.host(0, f',stats={current.port}') + ' ' + self.host(1, f',stats={other.port}')
        # Live runnable work replaces the old average; it is not added to it.
        self.assertEqual(self.compile(hosts, 'idle')[0], self.ports[0])
        for name, values in [('queued', dict(busy=4, queued=4)),
                             ('memory', dict(memory_pressure=50))]:
            current.response = stats_response(running=0, **values)
            time.sleep(1.05)
            self.assertEqual(self.compile(hosts, name)[0], self.ports[1])

    def test_invalid_live_metrics_are_not_idle(self):
        peer = self.peer(stats_response(busy='nan'))
        hosts = self.host(0, f',stats={peer.port},speed=4') + ' ' + self.host(1)
        _, log = self.compile(hosts)
        self.assertIn('adaptive stats unavailable', log)

    def test_local_leases_and_server_queue_are_not_added_twice(self):
        peer = self.peer(stats_response(running=0, busy=1, queued=2))
        locks = [self.hold_slot(0, i) for i in range(3)]
        hosts = self.host(0, f',stats={peer.port}') + ' ' + self.host(1, ',speed=0.8')
        self.assertEqual(self.compile(hosts)[0], self.ports[0])
        for lock in locks:
            lock.close()

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
                                      stats_response(cores=5),
                                      stats_response(smt_gain='nan'),
                                      stats_response(smt_gain=1.1),
                                      stats_response(core_scale='nan'),
                                      stats_response().replace(b'dcc_cores 4\n', b''),
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
        peer = self.peer(stats_response(cpus=8, jobs=8, cores=4,
                                        core_scale=.8, smt_gain=.25, busy=6))
        other = self.peer(stats_response())
        hosts = (self.host(0, f",stats={peer.port}") + " " +
                 self.host(1, f",stats={other.port},speed=.5"))
        # Stay within a cache second, including the 0.3s compiler delay.
        time.sleep(1 - time.time() % 1 + 0.02)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda i: self.compile(hosts, f"cached{i}"), range(3)))
        # Fresh and cached topology/coefficients must select the same host.
        self.assertEqual([port for port, _ in results], [self.ports[0]] * 3)
        self.assertEqual(peer.requests, 1)
        self.assertEqual(other.requests, 1)

    def test_cpu_slowdown_including_single_job(self):
        slow = self.peer(stats_response(cpu_scale=.25, cpu_age=.5))
        steady = self.peer(stats_response())
        hosts = (self.host(0, f",stats={slow.port},speed=2") + " " +
                 self.host(1, f",stats={steady.port},speed=1"))
        # A fast but throttled idle core must lose even without contention.
        time.sleep(1 - time.time() % 1 + .02)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda i: self.compile(hosts, f"throttled{i}"), range(2)))
        self.assertEqual([port for port, _ in results], [self.ports[1]] * 2)
        self.assertEqual(slow.requests, 1)
        self.assertEqual(steady.requests, 1)

    def test_invalid_cpu_capacity_is_unavailable(self):
        for i, metrics in enumerate((dict(cpu_scale=0, cpu_age=0),
                                     dict(cpu_scale="nan", cpu_age=0),
                                     dict(cpu_scale=.5, cpu_age=-1),
                                     dict(cpu_scale=.5, cpu_age=0, cpu_probe=2))):
            with self.subTest(metrics=metrics):
                peer = self.peer(stats_response(**metrics))
                _, log = self.compile(self.host(0, f",stats={peer.port}"), f"invalid-cpu{i}")
                self.assertIn("adaptive stats unavailable", log)

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
                       "speed=1001", "speed=2x", "speed=", "h2", "tcp,h2"):
            result = subprocess.run([str(self.build / "h_hosts")],
                                    env=dict(self.base_env, DISTCC_HOSTS=self.host(0, "," + option)),
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 106, option)
        result = subprocess.run([str(self.build / "h_hosts")],
                                env=dict(self.base_env, DISTCC_HOSTS="@host,stats=3633"),
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 106)


class MemoryEstimateTest(unittest.TestCase):
    def test_outlier_keeps_spare_without_charging_every_job(self):
        with tempfile.TemporaryDirectory(prefix='distcc-memory-model-') as directory:
            root = Path(directory)
            source = root/'memory.c'
            source.write_text('''#include <assert.h>
#include <stdlib.h>
#include "resource.h"
#define M (1024ULL*1024)
int main(void) {
    setenv("DISTCC_JOB_MEMORY", "512", 1);
    setenv("DISTCC_MEMORY_LEARNING", "1", 1);
    assert(dcc_resource_job_bytes() == 512*M);
    for (int i=0; i<8; ++i) dcc_resource_observe(64*M, 1);
    assert(dcc_resource_job_bytes() == 256*M);
    dcc_resource_observe(1024*M, 1);
    assert(dcc_resource_job_bytes() == 256*M);
    assert(dcc_resource_spare_bytes() == 1024*M);
    dcc_resource_observe(64*M, 1);
    assert(dcc_resource_job_bytes() == 256*M);
    for (int i=0; i<7; ++i) dcc_resource_observe(64*M, 1);
    assert(dcc_resource_job_bytes() == 256*M);
    assert(dcc_resource_spare_bytes() == 1024*M);
    assert(dcc_resource_samples() == 17);
    dcc_resource_observe(1536*M, 0);
    assert(dcc_resource_spare_bytes() == 1536*M);
    assert(dcc_resource_samples() == 17);
    for (int i=0; i<64; ++i) dcc_resource_observe(160*M, 1);
    assert(dcc_resource_job_bytes() == 384*M);
    assert(dcc_resource_spare_bytes() == 1536*M);
    unsetenv("DISTCC_MEMORY_LEARNING");
    assert(dcc_resource_job_bytes() == 512*M);
    assert(dcc_resource_spare_bytes() == 128*M);
    return 0;
}''')
            executable=root/'memory'
            subprocess.run(['gcc','-Isrc',str(source),'src/resource.c','-o',str(executable)],
                           check=True,capture_output=True)
            subprocess.run([str(executable)],check=True,capture_output=True)


class CpuPerfTest(DistccTestCase):
    # Deliberately above hardware frequency, making a measurable reduction
    # without changing the machine's governor or frequency limits.
    daemon_env = {'DISTCC_CPU_FREQ_REF': '10000000,10000000,10000000'}

    def test_live_sampling_idle_and_global_recovery(self):
        if not list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*/cpuinfo_avg_freq')):
            self.skipTest('hardware frequency feedback is unavailable')
        self.source.write_text('int main(void) { return 0; }\n' + ''.join(
            f'int f{i}(int x) {{ return (x + {i}) * (x ^ {i}); }}\n' for i in range(10000)))
        host = self.host(0, f',stats={self.stats_ports[0]}')
        self.compile(host, 'warm')

        def metrics():
            return dict(line.split(' ', 1) for line in self.get_stats(self.stats_ports[0]).splitlines()
                        if line.startswith('dcc_'))

        warm = metrics()
        self.assertGreater(int(warm['dcc_cpu_samples']), 0, warm)
        self.assertGreater(float(warm['dcc_cpu_scale']), 0)
        self.assertLess(float(warm['dcc_cpu_scale']), .9)
        time.sleep(10.1)
        idle = metrics()
        self.assertEqual(idle['dcc_cpu_samples'], warm['dcc_cpu_samples'])
        self.assertEqual(idle['dcc_cpu_scale'], warm['dcc_cpu_scale'])
        self.assertEqual(idle['dcc_cpu_probe'], '1')
        self.assertEqual(idle['dcc_capacity'], '1')
        # Independent client processes must not start two recovery compilers.
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(self.compile, host, f'recover{i}') for i in range(2)]
            end = time.monotonic() + 1.5
            seen_busy = False
            while time.monotonic() < end:
                current = metrics()
                busy = int(current['dcc_busy'])
                seen_busy |= busy == 1
                self.assertLessEqual(busy, 1, current)
                time.sleep(.1)
            for job in jobs:
                job.result()
        self.assertTrue(seen_busy)
        self.assertGreater(int(metrics()['dcc_cpu_samples']), int(warm['dcc_cpu_samples']))
        # A tiny job cannot provide 100ms of busy feedback. Its trial must
        # still release the global one-worker limit, retaining the old scale.
        time.sleep(10.1)
        stale = metrics()
        self.assertEqual(stale['dcc_cpu_probe'], '1')
        self.source.write_text('int main(void) { return 0; }\n')
        self.compile(host, 'tiny-recovery')
        time.sleep(2.1)
        expired = metrics()
        self.assertEqual(expired['dcc_cpu_samples'], stale['dcc_cpu_samples'])
        self.assertEqual(expired['dcc_cpu_scale'], stale['dcc_cpu_scale'])
        self.assertEqual(expired['dcc_cpu_probe'], '0')
        self.assertGreater(int(expired['dcc_capacity']), 1)


class CpuCalibrationTest(DistccTestCase):
    daemon_env = {'DISTCC_CORE_SCALE': '.7', 'DISTCC_SMT_GAIN': '-.125'}

    def test_server_advertises_calibration(self):
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_core_scale 0.700\n', stats)
        self.assertIn('dcc_smt_gain -0.125\n', stats)
        # Exercise the actual response parser and compiler path as well.
        port, _ = self.compile(self.host(0, f',stats={self.stats_ports[0]}'))
        self.assertEqual(port, self.ports[0])


if __name__ == "__main__":
    unittest.main()
