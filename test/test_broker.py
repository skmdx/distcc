#!/usr/bin/env python3
"""Broker IPC, lifetime and scheduling with real distccd/GCC workers."""
import array
import fcntl
import concurrent.futures
import os
from pathlib import Path
import socket
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import unittest

from test_adaptive import DistccTestCase, free_port, stats_response


class BrokerTestCase(DistccTestCase):
    def restart_server(self, binary, **settings):
        old = self.daemons[0]
        os.killpg(old.pid, signal.SIGTERM)
        old.wait(timeout=5)
        env = dict(self.base_env, TMPDIR=str(self.root),
                   DISTCCD_PATH=str(self.root / 'bin') + os.pathsep + os.defpath)
        env.update(getattr(self, 'daemon_env', {}))
        env.update(settings)
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
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.02)

    def start_broker(self, hosts=None, idle=10, name="broker", backoff=60):
        path = self.root / f"{name}.sock"
        env = dict(self.base_env, DISTCC_HOSTS=hosts or self.host(0),
                   DISTCC_DIR=str(self.ddir), DISTCC_BROKER_IDLE=str(idle),
                   DISTCC_BACKOFF_PERIOD=str(backoff))
        log = open(self.work / f"{name}.log", "w+")
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

    def request(self, command=1, work=(0, 0, 0), source_bytes=0):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sock.settimeout(5)
        sock.connect(self.env["DISTCC_BROKER"])
        payload = struct.pack("=II", 2, command)
        if command in (6, 9, 10):
            payload += struct.pack('=QQII', *work, source_bytes)
        sock.sendall(payload)
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

class BrokerTest(BrokerTestCase):
    def test_client_waits_for_broker_start(self):
        self.env = dict(self.base_env, DISTCC_HOSTS=self.host(0),
                        DISTCC_DIR=str(self.ddir), TMPDIR=str(self.work),
                        DISTCC_BROKER=str(self.root / 'broker.sock'),
                        DISTCC_FALLBACK='0', DISTCC_RECONNECT_TIMEOUT='5')
        with concurrent.futures.ThreadPoolExecutor() as pool:
            job = pool.submit(self.job, 900)
            time.sleep(.3)
            self.assertFalse(job.done())
            self.start_broker()
            job.result(timeout=10)

    def test_client_reconnects_after_broker_dies_while_queued(self):
        broker = self.start_broker(self.host(0) + ' --localslots_cpp=1')
        self.env['DISTCC_RECONNECT_TIMEOUT'] = '5'
        held = self.request(12)
        self.assertEqual(self.receive(held), (0, 1, []))
        with concurrent.futures.ThreadPoolExecutor() as pool:
            job = pool.submit(self.job, 901)
            time.sleep(.3)
            self.assertFalse(job.done())
            broker.kill()
            broker.wait(timeout=5)
            self.start_broker()
            job.result(timeout=10)

    def test_unavailable_broker_reconnect_is_bounded(self):
        env = dict(self.base_env, DISTCC_HOSTS=self.host(0),
                   DISTCC_BROKER=str(self.root / 'absent.sock'),
                   DISTCC_RECONNECT_TIMEOUT='1')
        start = time.monotonic()
        result = subprocess.run([str(self.build / 'distcc'), '-j'], env=env,
                                capture_output=True, timeout=4)
        self.assertNotEqual(result.returncode, 0)
        self.assertGreaterEqual(time.monotonic() - start, .9)

    def test_worker_restart_during_compile(self):
        self.start_broker(self.host(0, ',tcp'), backoff=1)
        self.env['DISTCC_RECONNECT_TIMEOUT'] = '10'
        # Observe actual execution before dropping the TCP connection.
        with concurrent.futures.ThreadPoolExecutor() as pool:
            job = pool.submit(self.job, 902)
            deadline = time.monotonic() + 5
            while '\ndcc_busy 1\n' not in self.get_stats(self.stats_ports[0]):
                self.assertFalse(job.done())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            self.restart_server(self.build / 'distccd')
            job.result(timeout=15)

    def test_reused_cpp_requests_do_not_inherit_old_job_priority(self):
        peer = self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                         queued=0, preparing=0, returning=0, capacity=1))
        self.start_broker(self.host(0, f',tcp,stats={peer.port}') + ' --localslots_cpp=1')
        held = self.request(12)
        self.assertEqual(self.receive(held), (0, 1, []))
        first_seed, first_net = self.acquire_reserved(10, (201, 65536, 1))
        first_seed.close()
        first_net.close()
        first = self.request(12)
        first.settimeout(.05)
        with self.assertRaises(socket.timeout):
            first.recv(1032)
        second_seed, second_net = self.acquire_reserved(10, (202, 64 * 1024 * 1024, 2))
        second_seed.close()
        second_net.close()
        second = self.request(12)
        second.settimeout(.05)
        with self.assertRaises(socket.timeout):
            second.recv(1032)
        held.close()
        first.settimeout(.5)
        self.assertEqual(self.receive(first), (0, 1, []))
        first.close()
        second.settimeout(.5)
        self.assertEqual(self.receive(second), (0, 1, []))

    def test_local_cpp_lease_wakes_waiter_without_reserving_worker(self):
        self.start_broker(self.host(0, ',tcp') + ' --localslots_cpp=1')
        first = self.request(12)
        self.assertEqual(self.receive(first), (0, 1, []))
        waiter = self.request(12)
        waiter.settimeout(.05)
        with self.assertRaises(socket.timeout):
            waiter.recv(1032)
        # CPP admission does not reserve a remote worker.
        remote = self.request(10)
        status, _, descriptors = self.receive(remote)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        for fd in descriptors:
            os.close(fd)
        first.close()
        waiter.settimeout(.5)
        self.assertEqual(self.receive(waiter), (0, 1, []))
        waiter.close()
        self.assertEqual(self.receive(self.request(12)), (0, 1, []))

    def acquire_reserved(self, command=6, work=(0, 0, 0), source_bytes=0):
        lease = self.request(command, work, source_bytes)
        status, capabilities, fds = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertTrue(capabilities & (1 << 30))
        self.assertEqual(len(fds), 1)
        net = socket.socket(fileno=fds[0])
        self.addCleanup(net.close)
        lease.sendall(struct.pack('=II', 2, 3))
        return lease, net

    def admission_hosts(self):
        # Intentionally stale, idle metrics: admission must still prevent
        # separate brokers from queuing behind a full real server.
        stats = stats_response().replace(b'</distccstats>',
            b'dcc_running 0\ndcc_busy 0\ndcc_queued 0\ndcc_admission 1\n</distccstats>')
        peers = [self.peer(stats), self.peer(stats)]
        return ' '.join(self.host(i, f',stats={p.port},speed={2 if i == 0 else 1}')
                        for i, p in enumerate(peers)) + ' --localslots_cpp=8'

    def test_idle_fast_cores_before_slow_host(self):
        self.start_broker(self.admission_hosts())
        for _ in range(4):
            _, net = self.acquire_reserved()
            self.assertEqual(net.getpeername()[1], self.ports[0])
        _, net = self.acquire_reserved()
        self.assertEqual(net.getpeername()[1], self.ports[1])

    def test_multiple_brokers_reserve_global_capacity(self):
        hosts, environments, assigned, leases = self.admission_hosts(), [], [], []
        for i in range(3):
            self.start_broker(hosts, name=f'broker-{i}')
            environments.append(self.env)
        for i in range(8):
            self.env = environments[i % 3]
            lease, net = self.acquire_reserved()
            leases.append((lease, net))
            assigned.append(net.getpeername()[1])
        self.assertEqual([assigned.count(port) for port in self.ports], [4, 4])
        for port in self.stats_ports:
            stats = self.get_stats(port)
            self.assertIn('dcc_busy 4\n', stats)
            self.assertIn('dcc_queued 0\n', stats)
        # The lease and network close together on cancellation, freeing the
        # server reservation; completion must also release it after real GCC.
        for lease, net in leases:
            net.close()
            lease.close()
        self.job('after-cancel')

    def test_links_have_separate_admission_and_no_cpp_reservation(self):
        self.start_broker(self.admission_hosts() + ' --localslots_cpp=1')
        _, first = self.acquire_reserved(8)
        _, second = self.acquire_reserved(8)
        self.assertNotEqual(first.getpeername()[1], second.getpeername()[1])
        # Both links can remain reserved while a compile gets the cpp slot.
        _, compile_net = self.acquire_reserved()
        self.assertEqual(compile_net.getpeername()[1], self.ports[0])

    def test_invalid_probe_preserves_last_good_metrics(self):
        peer = self.peer(stats_response(cpus=8, jobs=8))
        self.start_broker(f'127.0.0.1:{self.ports[0]},stats={peer.port}')
        def jobs():
            request = self.request(2)
            status, slots, fds = self.receive(request)
            self.assertEqual((status, fds), (0, []))
            return slots
        self.assertEqual(jobs(), 8)
        peer.response = b'garbage'
        time.sleep(1.05)
        self.assertEqual(jobs(), 8)

    def test_jobs_use_effective_capacity_without_changing_admission(self):
        peer = self.peer(stats_response(cpus=8, jobs=8, capacity=2, busy=2, available=0))
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'))
        status, slots, fds = self.receive(self.request(2))
        self.assertEqual((status, slots, fds), (0, 2, []))

    def test_job_cost_uses_idle_cpu_before_queueing_on_fast_worker(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        first, net = self.acquire_reserved(10, (1, 4 * 1024 * 1024, 1))
        self.assertEqual(net.getpeername()[1], self.ports[0])
        heavy, other = self.acquire_reserved(10, (2, 16 * 1024 * 1024, 1))
        self.assertEqual(other.getpeername()[1], self.ports[1])
        # With both CPUs occupied, finish-time prediction can still wait for
        # the faster worker. Empty independent execution capacity comes first.
        small = self.request(10, (3, 64 * 1024, 1))
        small.settimeout(.05)
        with self.assertRaises(socket.timeout):
            small.recv(1032)
        first.close()
        small.settimeout(1)
        status, _, descriptors = self.receive(small)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        with socket.socket(fileno=descriptors[0]) as selected:
            self.assertEqual(selected.getpeername()[1], self.ports[0])

    def test_independent_slow_cores_precede_fast_smt_siblings(self):
        peers = [self.peer(stats_response(cpus=16, cores=8, jobs=16,
                    running=0, busy=0, queued=0, preparing=0, returning=0, capacity=16)),
                 self.peer(stats_response(cpus=4, cores=2, jobs=4,
                    running=0, busy=0, queued=0, preparing=0, returning=0, capacity=2))]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={1 if i == 0 else .32}',
                                  slots=16 if i == 0 else 4)
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        for index, expected in enumerate([0] * 8 + [1] * 2 + [0]):
            _, net = self.acquire_reserved(10, (1000 + index, 4 * 1024 * 1024, 1))
            self.assertEqual(net.getpeername()[1], self.ports[expected], index)

    def test_job_cost_learns_execution_cost(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        seed, net = self.acquire_reserved(10, (11, 4 * 1024 * 1024, 1))
        seed.sendmsg([struct.pack('=II3Q', 2, 7, 0, 90_000_000_000, 10_000_000_000)],
                     [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        # An exact repeat learns 20 normalized seconds from ten execution
        # seconds on speed=2. The unrelated C++ class remains untrained.
        _, repeated = self.acquire_reserved(10, (11, 4 * 1024 * 1024, 1))
        self.assertEqual(repeated.getpeername()[1], self.ports[0])
        _, other = self.acquire_reserved(10, (12, 2 * 1024 * 1024, 2))
        self.assertEqual(other.getpeername()[1], self.ports[1])

    def test_marginal_capacity_accounts_for_competing_cpu_work(self):
        peers = [self.peer(stats_response(cpus=8, cores=4, jobs=8, capacity=8,
                    running=4, busy=0, queued=0, preparing=0, returning=0)),
                 self.peer(stats_response(cpus=2, cores=2, jobs=2, capacity=2,
                    running=0, busy=0, queued=0, preparing=0, returning=0))]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={1 if i == 0 else .5}',
                                  slots=8 if i == 0 else 2)
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        # Four preprocessors already occupy the faster host's physical cores.
        # Adding a compiler there gains only SMT throughput and slows them.
        _, selected = self.acquire_reserved(10, (1100, 4 * 1024 * 1024, 1))
        self.assertEqual(selected.getpeername()[1], self.ports[1])

    def test_job_cost_overrun_does_not_starve_idle_worker(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        first, _ = self.acquire_reserved(10, (31, 4 * 1024 * 1024, 1))
        time.sleep(1.5)
        waiting = self.request(10, (32, 4 * 1024 * 1024, 1))
        waiting.settimeout(1)
        _, _, descriptors = self.receive(waiting)
        with socket.socket(fileno=descriptors[0]) as selected:
            self.assertEqual(selected.getpeername()[1], self.ports[1])

    def test_job_cost_prioritizes_heavy_waiter_on_fast_cpu(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        first, _ = self.acquire_reserved(10, (41, 40 * 1024 * 1024, 1))
        _, second = self.acquire_reserved(10, (42, 8 * 1024 * 1024, 1))
        self.assertEqual(second.getpeername()[1], self.ports[1])
        small = self.request(10, (43, 2 * 1024 * 1024, 1))
        heavy = self.request(10, (44, 64 * 1024 * 1024, 1))
        for waiter in (small, heavy):
            waiter.settimeout(.05)
            with self.assertRaises(socket.timeout):
                waiter.recv(1032)
        first.close()
        heavy.settimeout(1)
        _, _, descriptors = self.receive(heavy)
        with socket.socket(fileno=descriptors[0]) as selected:
            self.assertEqual(selected.getpeername()[1], self.ports[0])

    def test_job_cost_keeps_colliding_histories(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        # Large and small execution costs with the same size and old hash slot.
        for key, duration in ((51, 10_000_000_000), (51 + 4096, 100_000_000)):
            seed, net = self.acquire_reserved(10, (key, 4 * 1024 * 1024, 1))
            seed.sendmsg([struct.pack('=II3Q', 2, 7, 0, 0, duration)],
                         [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        self.acquire_reserved(10, (51, 4 * 1024 * 1024, 1))
        _, other = self.acquire_reserved(10, (52, 18 * 1024 * 1024, 2))
        self.assertEqual(other.getpeername()[1], self.ports[1])

    def test_job_cost_excludes_queue_time(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        seed, net = self.acquire_reserved(10, (21, 4 * 1024 * 1024, 1))
        seed.sendmsg([struct.pack('=II3Q', 2, 7, 0, 90_000_000_000, 100_000_000)],
                     [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        self.acquire_reserved(10, (21, 4 * 1024 * 1024, 1))
        self.acquire_reserved(10, (23, 40 * 1024 * 1024, 2))
        # Ninety seconds of old queueing must not turn this repeat into a
        # long-running job. With both CPUs occupied, the fast CPU's short
        # remaining work must beat the other worker's long-running job.
        _, other = self.acquire_reserved(10, (22, 2 * 1024 * 1024, 2))
        self.assertEqual(other.getpeername()[1], self.ports[0])

    def test_job_cost_distinguishes_source_work_from_shared_headers(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        for key, size, duration in ((61, 16384, 50_000_000), (62, 1048576, 2_000_000_000)):
            seed, net = self.acquire_reserved(10, (key, 4 * 1024 * 1024, 1), size)
            seed.sendmsg([struct.pack('=II3Q', 2, 7, 0, 0, duration)],
                         [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        first, _ = self.acquire_reserved(10, (63, 1024 * 1024, 2))
        self.acquire_reserved(10, (66, 4 * 1024 * 1024, 1), 16384)
        heavy = self.request(10, (64, 4 * 1024 * 1024, 1), 1048576)
        heavy.settimeout(.05)
        with self.assertRaises(socket.timeout):
            heavy.recv(1032)
        _, small = self.acquire_reserved(10, (65, 4 * 1024 * 1024, 1), 16384)
        self.assertEqual(small.getpeername()[1], self.ports[1])
        first.close()
        heavy.settimeout(1)
        _, _, descriptors = self.receive(heavy)
        with socket.socket(fileno=descriptors[0]) as selected:
            self.assertEqual(selected.getpeername()[1], self.ports[0])

    def test_local_prepare_delay_is_not_scaled_by_input_size(self):
        peers = [self.peer(stats_response(cpus=1, jobs=1, running=0, busy=0,
                    queued=0, preparing=0, returning=0, capacity=1)) for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={2 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        seed, net = self.acquire_reserved(10, (71, 65536, 2))
        seed.sendmsg([struct.pack('=II3Q', 2, 7, 20_000_000, 0, 10_000_000)],
                     [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
        # A delayed small FD handoff must not imply a slow bulk upload for
        # larger inputs on this otherwise idle, faster local CPU.
        _, selected = self.acquire_reserved(10, (72, 1024 * 1024, 2))
        self.assertEqual(selected.getpeername()[1], self.ports[0])

    def test_tcp_smt_contention_prefers_spare_physical_cores(self):
        first = self.peer(stats_response(cpus=8, cores=4, jobs=8, running=4, busy=4,
                                        queued=0, preparing=0, returning=0, capacity=8))
        second = self.peer(stats_response(cpus=8, cores=8, jobs=8, running=0, busy=0,
                                         queued=0, preparing=0, returning=0, capacity=8))
        hosts = self.host(0, f',tcp,stats={first.port},speed=1') + ' ' + self.host(1, f',tcp,stats={second.port},speed=.9')
        self.start_broker(hosts)
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[1])

    def test_tcp_throttled_host_gets_only_one_recovery_trial(self):
        first = self.peer(stats_response(cpu_scale=.2, cpu_age=20, cpu_probe=1,
                                        busy=0, queued=0, preparing=0, returning=0, capacity=1))
        second = self.peer(stats_response(busy=0, queued=0, preparing=0, returning=0, capacity=4))
        hosts = self.host(0, f',tcp,stats={first.port}') + ' ' + self.host(1, f',tcp,stats={second.port}')
        self.start_broker(hosts)
        lease, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[0])
        net.close()
        lease.close()
        # Even a completed/cancelled short trial cannot trigger another probe
        # from the same stale stats snapshot.
        _, next_net = self.acquire_reserved(10)
        self.assertEqual(next_net.getpeername()[1], self.ports[1])

    def test_tcp_global_pipeline_and_resource_limits(self):
        for name, metrics in (
                ('memory', dict(capacity=0)),
                ('pressure', dict(memory_pressure=10)),
                ('pipeline', dict(busy=4, preparing=4)),
                ('reduced_capacity', dict(capacity=1, busy=1, preparing=1))):
            values = dict(running=0, busy=0, queued=0, preparing=0,
                          returning=0, capacity=4)
            values.update(metrics)
            peer = self.peer(stats_response(**values))
            hosts = (self.host(0, f',tcp,stats={peer.port},speed=100') + ' ' +
                     self.host(1, ',tcp'))
            self.start_broker(hosts, name=name)
            _, net = self.acquire_reserved(10)
            self.assertEqual(net.getpeername()[1], self.ports[1], name)
            self.job(name)

    def test_tcp_snapshot_does_not_recount_local_leases(self):
        peer = self.peer(stats_response(running=0, busy=0, queued=0,
                                       preparing=0, returning=0, capacity=4))
        hosts = self.host(0, f',tcp,stats={peer.port}')
        self.start_broker(hosts)
        for _ in range(4):
            _, net = self.acquire_reserved(10)
            self.assertEqual(net.getpeername()[1], self.ports[0])
        # The same four leases now appear in the server snapshot.
        peer.response = stats_response(running=0, busy=1, queued=3,
                                       preparing=0, returning=0, capacity=4)
        time.sleep(1.1)
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[0])

    def test_tcp_unknown_job_uses_execution_capacity_before_pipeline(self):
        peers = [self.peer(stats_response(running=0, busy=0, queued=0,
                                         preparing=0, returning=0, capacity=4))
                 for _ in range(2)]
        hosts = ' '.join(self.host(i, f',tcp,stats={p.port},speed={4 if i == 0 else 1}')
                         for i, p in enumerate(peers))
        self.start_broker(hosts)
        for _ in range(4):
            _, net = self.acquire_reserved(10)
            self.assertEqual(net.getpeername()[1], self.ports[0])
        # A missing cost hint must not hide the throughput of idle CPUs.
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[1])

    def test_tcp_preparing_snapshot_does_not_recount_local_leases(self):
        peer = self.peer(stats_response(running=0, busy=0, queued=0,
                                       preparing=0, returning=0, capacity=4))
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'))
        held = [self.acquire_reserved(10) for _ in range(4)]
        peer.response = stats_response(running=0, busy=0, queued=0,
                                       preparing=4, returning=0, capacity=4)
        time.sleep(1.1)
        _, selected = self.acquire_reserved(10)
        self.assertEqual(selected.getpeername()[1], self.ports[0])

    def test_tcp_returning_leases_do_not_fill_cpu_pipeline(self):
        peer = self.peer(stats_response(running=0, busy=0, queued=0,
                                       preparing=0, returning=0, capacity=4))
        hosts = self.host(0, f',tcp,stats={peer.port},speed=100')
        self.start_broker(hosts)
        for _ in range(8):
            _, net = self.acquire_reserved(10)
            self.assertEqual(net.getpeername()[1], self.ports[0])
        peer.response = stats_response(running=0, busy=0, queued=0,
                                       preparing=0, returning=8, capacity=4)
        time.sleep(1.1)
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[0])

    def test_tcp_completed_snapshot_work_releases_pipeline(self):
        peer = self.peer(stats_response(running=0, busy=0, queued=0,
                                       preparing=0, returning=0, capacity=4))
        hosts = (self.host(0, f',tcp,stats={peer.port}') + ' ' +
                 self.host(1, ',tcp,speed=0.5', slots=1))
        self.start_broker(hosts)
        held = [self.acquire_reserved(10) for _ in range(4)]
        peer.response = stats_response(running=0, busy=4, queued=4,
                                       preparing=0, returning=0, capacity=4)
        time.sleep(1.1)
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[1])
        for lease, net in held:
            lease.sendmsg([struct.pack('=II3Q', 2, 7, 0, 0, 0)],
                          [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [net.fileno()]))])
            net.close()
        _, net = self.acquire_reserved(10)
        self.assertEqual(net.getpeername()[1], self.ports[0])

    def memo(self, key):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sock.settimeout(2)
        sock.connect(self.env['DISTCC_BROKER'])
        sock.sendall(struct.pack('=II', 2, 4) + key + b'\0')
        self.addCleanup(sock.close)
        return sock

    def test_memo_coalesces_and_recovers_owner_exit(self):
        self.start_broker()
        owner = self.memo(b'probe')
        self.assertEqual(owner.recv(65536), b'\0')
        waiter = self.memo(b'probe')
        waiter.settimeout(.1)
        with self.assertRaises(socket.timeout):
            waiter.recv(65536)
        owner.sendall(struct.pack('=II', 2, 5) + b'resolved\0')
        waiter.settimeout(2)
        self.assertEqual(waiter.recv(65536), b'resolved\0')
        self.assertEqual(self.memo(b'probe').recv(65536), b'resolved\0')
        owner = self.memo(b'aborted')
        self.assertEqual(owner.recv(65536), b'\0')
        waiter = self.memo(b'aborted')
        waiter.settimeout(.1)
        with self.assertRaises(socket.timeout):
            waiter.recv(65536)
        owner.close()
        waiter.settimeout(2)
        self.assertEqual(waiter.recv(65536), b'')
        self.assertEqual(self.memo(b'aborted').recv(65536), b'\0')

    def test_empty_setting_uses_direct_path(self):
        self.env = dict(self.base_env, DISTCC_HOSTS=self.host(0), DISTCC_BROKER="",
                        DISTCC_DIR=str(self.ddir), DISTCC_FALLBACK="0",
                        TMPDIR=str(self.work))
        self.job(0)

    def test_parallel_real_compiles_and_no_children(self):
        proc = self.start_broker(self.host(0) + " " + self.host(1))
        initial_fds = len(list(Path(f"/proc/{proc.pid}/fd").iterdir()))
        with concurrent.futures.ThreadPoolExecutor(max_workers=28) as pool:
            objects = list(pool.map(self.job, range(28)))
        self.assertEqual(len(objects), 28)
        self.job(29, "cc")
        children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children")
        self.assertEqual(children.read_text().strip(), "")
        # Eight compile slots can now retain eight reusable TCP sockets, plus
        # at most two transient statistics connections.
        self.assertLessEqual(len(list(Path(f"/proc/{proc.pid}/fd").iterdir())), initial_fds + 10)

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

    def test_preprocessing_reservation_released_before_compile_finishes(self):
        self.start_broker(self.host(0) + " --localslots_cpp=1")
        first = self.request()
        status, _, fds = self.receive(first)
        self.assertEqual(status, 0)
        for fd in fds:
            self.assertTrue(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK)
            os.close(fd)
        second = self.request()
        second.settimeout(.15)
        with self.assertRaises(socket.timeout):
            second.recv(1032)
        first.sendall(struct.pack("=II", 2, 3))
        second.settimeout(2)
        status, _, fds = self.receive(second)
        self.assertEqual(status, 0)
        for fd in fds:
            os.close(fd)

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
        # After an idle period, -j must refresh expired metrics before replying.
        time.sleep(5.2)
        result = subprocess.run([str(self.build / "distcc"), "-j"], env=self.env,
                                check=True, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout.strip(), "6")

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


@unittest.skipUnless(os.geteuid() == 0, 'requires real cross-UID peers')
class BrokerIdentityTest(unittest.TestCase):
    def test_root_build_phase_and_foreign_user(self):
        build = Path.cwd()
        owner = build.stat().st_uid
        self.assertNotEqual(owner, 0, 'checkout must belong to the build user')
        with tempfile.TemporaryDirectory(prefix='distcc-identity-') as directory:
            root = Path(directory)
            root.chmod(0o777)
            path = root / 'broker.sock'
            env = dict(os.environ, DISTCC_BROKER=str(path), DISTCC_DIR=str(root),
                       DISTCC_HOSTS='127.0.0.1/2 --localslots_cpp=2',
                       DISTCC_BROKER_IDLE='10', DISTCC_RECONNECT_TIMEOUT='30',
                       DISTCC_JOB_MEMORY='64')
            broker = subprocess.Popen([str(build / 'distccd'), '--broker', str(path)],
                                      env=env, user=owner)
            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    self.assertIsNone(broker.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                # Exercise broker credential checks, not filesystem permissions.
                path.chmod(0o777)
                result = subprocess.run([str(build / 'distcc'), 'gcc', '-E', '-P', '-'],
                                        env=env, input=b'42\n', capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), b'42')
                result = subprocess.run([str(build / 'distcc'), '-j'], env=env,
                                        user=65534, capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'Permission denied', result.stderr)
            finally:
                broker.terminate()
                broker.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
