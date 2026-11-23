#!/usr/bin/env python3
"""Rust/Cargo distributed compilation through real TCP workers."""
import os
import json
import hashlib
from pathlib import Path
import runpy
import re
import resource
import shlex
import shutil
import subprocess
import socket
import struct
import sys
import time
import unittest
import test_tcp_jobs as tcp

from test_broker import BrokerTestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


from distcc_job import JOB_RUST_CGU


@unittest.skipUnless(shutil.which('rustc') and shutil.which('bwrap'), 'rustc and bubblewrap required')
class RustTest(BrokerTestCase):
    # A separate loopback address exercises TCP worker placement. A local
    # interface address is eligible for TRY_LOCAL before the dependency pass.
    worker_address = '127.0.0.2'
    def test_dependency_parent_traversal_preserves_empty_directories(self):
        (self.work/'empty'/'bits').mkdir(parents=True)
        (self.work/'value.rs').write_text('pub fn answer() -> u32 { 42 }')
        source = self.work/'traversal.rs'
        source.write_text('#[path="empty/bits/../../value.rs"] mod value; '
                          'pub fn answer() -> u32 { value::answer() }')
        output = self.work/'libtraversal.rlib'
        result = self.rust('--crate-type=rlib', source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        main = self.work/'main.rs'
        main.write_text('fn main() { assert_eq!(traversal::answer(), 42); }')
        executable = self.work/'main'
        result = self.rust(main, '--extern', 'traversal='+str(output),
                           '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, timeout=5)

    def test_dependency_symlink_parent_is_not_lexically_rewritten(self):
        (self.work/'target'/'child').mkdir(parents=True)
        (self.work/'alias').symlink_to('target/child')
        (self.work/'target'/'value.rs').write_text('pub const N: usize = 42;')
        (self.work/'value.rs').write_text('compile_error!("wrong source");')
        src = self.work/'check.rs'
        src.write_text('#[path="alias/../value.rs"] mod value; pub use value::N;')
        result = self.rust('--crate-type=rlib', src)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('crosses a symlink', result.stderr)

    send = staticmethod(tcp.TcpJobsTest.send)
    frame = tcp.TcpJobsTest.frame
    upload = tcp.TcpJobsTest.upload

    def version(self, compiler):
        report = subprocess.check_output([compiler, '-vV'], text=True).splitlines()
        fields = dict(line.split(': ', 1) for line in report[1:] if ': ' in line)
        return '\n'.join((report[0], fields['commit-hash'], '.'.join(fields['LLVM version'].split('.')[:2])))

    def wire_plan(self, source, *, flags=(), requested=None, logical=None, extra=(), host='', crate_types='rlib'):
        """A real rustc closure for tests which hold or alter an individual TCP transaction."""
        from distcc_job import JOB_RUST, RustCommand, fingerprint, manifest
        root = subprocess.check_output([self.rustc, '--print=sysroot'], text=True).strip()
        depfile = self.work/'wire.d'
        subprocess.run([self.rustc, '--crate-type=rlib', '-Zbinary-dep-depinfo',
                        '--emit=dep-info='+str(depfile), source], check=True,
                       env=dict(self.env, RUSTC_BOOTSTRAP='1'), cwd=self.work)
        paths = [Path(line[:-1].replace('\\ ', ' ')) for line in depfile.read_text().split('\n\n',1)[1].splitlines()
                 if line.endswith(':')] + list(extra)
        args = ['--sysroot', root, logical or source,
                '-o', '/__distcc_rust/out/libwire.rlib', *flags]
        entries = [(logical if logical and path==source else path, *fingerprint(path), 1) for path in paths]
        command = RustCommand(phase=1, compiler=requested or self.version(self.rustc), crate_types=crate_types, host=host)
        return manifest(JOB_RUST, self.work, args, entries, rust=command), paths

    def wire_result(self, net, identity, first=None):
        codec = runpy.run_path(str(self.build/'distcc-link'))['Lz4Chunks']()
        kind, data = first or self.frame(net)
        while kind == 0x54000408:
            kind, data = self.frame(net)
        self.assertEqual(kind, 0x54000403)
        status, _, *sizes = struct.unpack('!II4Q', data[:40])
        streams = [bytearray() for _ in sizes]
        names = []
        while True:
            kind, data = self.frame(net)
            if kind == 0x54000405:
                self.assertEqual(data[:8], struct.pack('!Q', identity))
                break
            if kind == 0x5400040b:
                while data:
                    size, mode = struct.unpack_from('!QI', data)
                    name, data = data[12:].split(b'\0', 1)
                    names.append(os.fsdecode(name))
                    sizes.append(size)
                    streams.append(bytearray())
                continue
            self.assertEqual(kind & ~0x30000, 0x54000404)
            index, plain = struct.unpack_from('!II', data)
            streams[index].extend(codec.decode(data[8:], plain) if kind & 0x10000 else data[8:])
        self.assertEqual(names, sorted(names))
        self.assertEqual(list(map(len, streams)), sizes)
        return status >> 8, streams[0].decode(errors='replace'), dict(zip(names, streams[4:]))

    def raw_compile(self, source, **kw):
        body, paths = self.wire_plan(source, **kw)
        with socket.create_connection((self.worker_address, self.ports[0]), timeout=15) as net:
            self.send(net, 0x54000400, struct.pack('!Q',1)+body)
            kind, needed = self.frame(net)
            if kind != 0x54000401:
                return self.wire_result(net, 1, (kind, needed))
            self.assertEqual(kind, 0x54000401)
            for i, path in enumerate(paths):
                if needed[i//8] & (1 << (i%8)): self.upload(net, path.read_bytes(), i)
            return self.wire_result(net, 1)

    def test_source_below_tmp_survives_worker_isolation(self):
        source = self.work/'temporary.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        status, err, outputs = self.raw_compile(source, logical='/tmp/distcc-client/temporary.rs')
        self.assertEqual(status, 0, err)
        output = self.work/'libwire.rlib'
        output.write_bytes(outputs['libwire.rlib'])
        main = self.work/'main.rs'
        main.write_text('fn main() { assert_eq!(temporary::answer(), 42); }')
        executable = self.work/'main'
        result = self.rust(main, '--extern', 'temporary='+str(output), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)

    def test_memory_refusal_reschedules_rust_without_fallback(self):
        self.start_memory_refusal()
        source = self.work / 'rescheduled.rs'
        source.write_text('pub fn answer() -> i32 { 42 }')
        output = self.work / 'librescheduled.rlib'
        result = self.rust('--crate-type=rlib', source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('rescheduling unstarted job', (self.work / 'memory-refusal.log').read_text())
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(rescheduled::answer(), 42); }')
        executable = self.work / 'rescheduled'
        result = self.rust(main, '--extern', 'rescheduled=' + str(output), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, timeout=5)

    @classmethod
    def setUpClass(cls):
        rustc = shutil.which('rustc')
        assert rustc is not None
        cls.rustc = rustc
        cls.daemon_env = {'DISTCCD_PATH': str(Path.cwd()) + os.pathsep + os.environ['PATH']}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.broker_process = self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])

    def rust(self, *args, remote=True, env=None, prefix=()):
        cmd = ([str(self.build / 'distcc'), self.rustc] if remote else [self.rustc])
        return subprocess.run([*prefix, *cmd, *map(str, args)], env=env or self.env,
                              cwd=self.work, capture_output=True, text=True, timeout=90)

    def cgu_toolchain(self):
        tool = self.build/'llvm-lto/distcc-rust-cgu'
        if not tool.is_file() or not (tool.parent/'distcc-rust-cgu.so').is_file():
            self.skipTest('build does not include the LLVM CGU backend')
        fields = dict(line.split(': ', 1) for line in subprocess.check_output([self.rustc, '-vV'],
                      text=True).splitlines() if ': ' in line)
        offered = subprocess.check_output([tool, '--capabilities'], text=True).split()
        if '.'.join(fields['LLVM version'].split('.')[:2]) != offered[0]:
            self.skipTest('rustc uses another LLVM version')
        target = subprocess.check_output([tool, '--target-name', offered[0], fields['host']], text=True)
        return offered[0], target

    @staticmethod
    def cgu_executing(pid):
        proc = Path('/proc')/str(pid)
        try:
            children = (proc/'task'/str(pid)/'children').read_text().split()
            if (proc/'cmdline').read_bytes().split(b'\0')[:2] == [b'/backend', b'--worker']:
                return bool(children)
        except (FileNotFoundError, ProcessLookupError):
            return False
        return any(RustTest.cgu_executing(child) for child in children)

    def completed_jobs(self):
        return [int(next(line.split()[1] for line in self.get_stats(port).splitlines()
                         if line.startswith('dcc_compile_ok '))) for port in self.stats_ports]

    def test_whole_crate_pins_over_1024_inputs(self):
        _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        budget = 65536 + 1024
        if hard != resource.RLIM_INFINITY and hard < budget:
            self.skipTest('hard descriptor limit below the full job budget')
        for process in (self.daemons[0], self.broker_process):
            soft, _ = resource.prlimit(process.pid, resource.RLIMIT_NOFILE)
            self.assertTrue(soft == resource.RLIM_INFINITY or soft >= budget)
        limited = ['prlimit', '--nofile=1024:', '--']
        compiler = self.work/'rustc'
        compiler.write_text('#!/bin/sh\n[ "$(ulimit -Sn)" = 1024 ] || exit 1\nexec ' +
                            shlex.quote(self.rustc) + ' "$@"\n')
        compiler.chmod(0o755)
        self.rustc = str(compiler)
        self.rust('--version', prefix=limited).check_returncode()

        count = 1100
        for i in range(count):
            (self.work/f'input-{i}').write_bytes(bytes([i % 251]))
        source = self.work/'many.rs'
        inputs = ','.join(f'include_bytes!("input-{i}")[0]' for i in range(count))
        source.write_text(f'static DATA: [u8; {count}] = [{inputs}];\n'
                          'fn main() { println!("{}", DATA.iter().map(|&x| u32::from(x)).sum::<u32>()); }')
        output = self.work/'many'
        before = self.completed_jobs()
        result = self.rust(source, '-Ccodegen-units=1', '-o', output, prefix=limited)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertEqual(sum(self.completed_jobs()) - sum(before), 1)
        self.assertEqual(int(subprocess.check_output([output])), sum(i % 251 for i in range(count)))
        local = subprocess.check_output([*limited, self.build/'distcc', '--local-command',
                                         '/bin/sh', '-c', 'ulimit -Sn'], env=self.env)
        self.assertEqual(int(local), 1024)

    def assert_one_job_per_cgu(self, messages, counts):
        modules = re.findall(r'^distcc-rust-cgu: completed \S+ (\S+)$', messages, re.M)
        self.assertGreater(len(modules), 1, messages)
        self.assertEqual(len(modules), len(set(modules)), modules)
        self.assertCountEqual(modules, re.findall(r'^distcc-rust-cgu: emitted (\S+)$', messages, re.M))
        self.assertEqual(sum(self.completed_jobs()) - sum(counts), len(modules), messages)

    def test_cgu_constructor_exports_are_checked_before_compilation(self):
        self.cgu_toolchain()
        shim = self.build/'llvm-lto/distcc-rust-cgu.so'
        # objcopy needs an ELF object even when the normal build uses linker-plugin LTO.
        native = self.work/'native-hook'
        subprocess.run(['make', '-s', f'LTO_BUILD={native}', 'CXXFLAGS=-O1 -fno-lto', native/'cgu_hook.o'],
                       cwd=self.build, check=True)
        exports = subprocess.check_output(['nm', '-DP', '--defined-only', shim], text=True).splitlines()
        for ctor in ('_ZN4llvm11PassBuilderC1', '_ZN4llvm14raw_fd_ostreamC1'):
            with self.subTest(ctor=ctor):
                # Re-link so that the broken export has a valid ELF hash and relocations.
                name, = [line.split()[0] for line in exports if line.startswith(ctor)]
                missing = name.replace(ctor, ctor[:-2] + 'X1', 1)
                folder = self.work/ctor
                folder.mkdir()
                shutil.copy2(native/'cgu-libcpp.h', folder)
                subprocess.run(['objcopy', '--redefine-sym', f'{name}={missing}',
                                native/'cgu_hook.o', folder/'cgu_hook.o'], check=True)
                target = folder/shim.name
                check = subprocess.run(['make', '-s', f'LTO_BUILD={folder}', target], cwd=self.build,
                                       capture_output=True, text=True)
                self.assertNotEqual(check.returncode, 0)
                self.assertIn(missing, check.stderr)
                self.assertFalse(target.exists())
                env = dict(self.env, LD_PRELOAD=str(target)+'.tmp', DISTCC_RUST_CGU_CLIENT=str(self.build/'distcc'))
                load = self.rust('-vV', remote=False, env=env)
                self.assertEqual(load.returncode, 100, load.stderr)
                self.assertIn('missing LLVM entry point ' + missing, load.stderr)

    @unittest.skipUnless(shutil.which('strace'), 'CGU process tracing required')
    def test_parallel_cgus_share_resident_sandbox_and_dispatcher(self):
        self.cgu_toolchain()
        server_trace = self.trace_backend()
        offset = Path(self.logs[0].name).stat().st_size
        source = self.work/'resident.rs'
        steps = ''.join(f'x = std::hint::black_box(x).wrapping_add({i});' for i in range(64))
        modules = [f'mod m{j} {{ #[inline(never)] pub fn f(mut x:u64)->u64 {{ {steps} x }} }}'
                   for j in range(8)]
        total = '+'.join(f'm{j}::f({j})' for j in range(8))
        source.write_text('\n'.join(modules) + f'\nfn main() {{println!("{{}}", {total});}}')
        client_trace = self.work/'client.exec'
        result = self.rust(source, '-Ccodegen-units=8', '-Copt-level=2', '-o', 'resident',
                           remote=False, env=dict(self.env, LD_PRELOAD=str(self.build/'llvm-lto/distcc-rust-cgu.so'),
                                                 DISTCC_RUST_CGU_CLIENT=str(self.build/'distcc')),
                           prefix=['strace', '-f', '-s', '256', '-e', 'execve', '-o', str(client_trace)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreater(result.stderr.count('distcc-rust-cgu: compiled on'), 1, result.stderr)
        self.assertEqual(int(subprocess.check_output([self.work/'resident'])), 8*sum(range(64))+sum(range(8)))
        pids = re.findall(r'^(\d+)\s+execve\(.*?"--rust-cgu-worker".*?= 0$', client_trace.read_text(), re.M)
        self.assertEqual(len(pids), 1)
        self.assertFalse((Path('/proc')/pids[0]).exists(), 'dispatcher still alive after rustc')
        self.assertEqual(len(re.findall(r'execve\("/usr/bin/bwrap"', server_trace.read_text())), 1)
        started = re.findall(rb'started reusable sandboxed Rust CGU worker pid=(\d+)',
                             Path(self.logs[0].name).read_bytes()[offset:])
        self.assertEqual(len(started), 1)
        self.assert_resident_sandbox(int(started[0]), [b'/backend', b'--worker'])

    def test_started_cgu_failure_reaches_the_compiler(self):
        version, target = self.cgu_toolchain()
        request = self.work/'invalid-module.bc'
        request.write_bytes(b'{"llvm":"'+version.encode()+b'"}\ninvalid bitcode')
        control, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        with request.open('rb') as ir, (self.work/'object').open('wb') as obj, control, child, subprocess.Popen(
                [self.build/'distcc', '--rust-cgu-worker'], cwd=self.work, stdin=child.fileno(),
                stderr=subprocess.PIPE, env=dict(self.env, DISTCC_FALLBACK='0'), text=True) as process:
            child.close()
            descriptors = Path('/proc')/str(process.pid)/'fd'
            def idle_fds():
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if len(list((descriptors.parent/'task').iterdir())) == 1:
                        try:
                            opened = sorted(os.readlink(fd) for fd in descriptors.iterdir())
                        except FileNotFoundError:
                            continue
                        if 'anon_inode:[eventfd]' in opened:
                            return opened
                    time.sleep(.005)
                self.fail('CGU dispatcher did not become idle')
            baseline = idle_fds()
            for abi, status in [(version, 100), ('absent', 121)] * 4:
                packet = struct.pack('96s96s14s', abi.encode(), target.encode(), b'object')
                reply, peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                with reply, peer:
                    reply.settimeout(10)
                    socket.send_fds(control, [packet], [ir.fileno(), obj.fileno(), peer.fileno()])
                    peer.close()
                    self.assertEqual(reply.recv(1), bytes([status]))
                    self.assertEqual(reply.recv(1), b'')
                self.assertEqual(idle_fds(), baseline)
            control.close()
            _, diagnostics = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, diagnostics)
            self.assertIn('missing CGU flags', diagnostics)
        self.assertEqual((self.work/'object').stat().st_size, 0)

    def test_cgu_dispatcher_disconnect_cancels_pending_acquisition(self):
        version, target = self.cgu_toolchain()
        path = str(self.work/'pending-broker')
        control, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        reply, peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as broker, control, child, reply, peer:
            broker.bind(path)
            broker.listen(1)
            broker.settimeout(5)
            with subprocess.Popen([self.build/'distcc', '--rust-cgu-worker'], stdin=child.fileno(),
                                  stderr=subprocess.PIPE, env=dict(self.env, DISTCC_BROKER=path)) as process:
                child.close()
                with Path(__file__).open('rb') as source, (self.work/'unused').open('wb') as output:
                    packet = struct.pack('96s96s14s', version.encode(), target.encode(), b'object')
                    socket.send_fds(control, [packet], [source.fileno(), output.fileno(), peer.fileno()])
                peer.close()
                with broker.accept()[0] as pending:
                    pending.settimeout(5)
                    self.assertTrue(pending.recv(4096))
                    control.close()
                    _, errors = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0, errors)
                    self.assertEqual(pending.recv(1), b'')
                self.assertNotIn(reply.recv(1), (b'', b'\x00'))
                self.assertEqual(reply.recv(1), b'')

    def test_saturated_cgu_worker_waits_then_returns_object(self):
        import select
        import threading
        version, target = self.cgu_toolchain()
        requested, publish = threading.Event(), threading.Event()
        def worker_stats():
            requested.set()
            publish.wait(timeout=5)
            return self.get_stats(self.stats_ports[0]).encode()

        peer = self.peer(worker_stats)
        self.start_broker(self.host(0, f',tcp,stats={peer.port}', slots=1), name='one-lane')
        def request_cgu(command):
            return self.request(command, work=(1, 65536, 11), memory_class=1+JOB_RUST_CGU*64+16,
                                cgu_backend=f'{version} {target}'.encode())

        cold = request_cgu(20)
        try:
            self.assertTrue(requested.wait(timeout=5))
            self.assertFalse(select.select([cold], [], [], 0)[0])
        finally:
            publish.set()
        self.assertEqual(self.receive(cold), (0, 1, []))
        lease = request_cgu(10)
        status, _, fds = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertEqual(len(fds), 1)
        connection = socket.socket(fileno=fds[0])
        self.addCleanup(connection.close)

        # Discovery must still advertise this occupied backend. The next
        # real rustc codegen unit then competes for the same single lease.
        capacity = request_cgu(20)
        self.assertEqual(self.receive(capacity), (0, 1, []))
        source = self.work / 'queued.rs'
        steps = ''.join(f'x = std::hint::black_box(x).wrapping_add({i});\n' for i in range(64))
        source.write_text('fn main() { let mut x = 0u64;\n' + steps + 'println!("{x}"); }')
        started = self.work / 'submitting'
        submit = self.work / 'submit'
        submit.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(started)) + '\nexec ' +
                          shlex.quote(str(self.build/'distcc')) + ' "$@"\n')
        submit.chmod(0o755)
        executable = self.work / 'queued'
        env = dict(self.env, LD_PRELOAD=str(self.build/'llvm-lto/distcc-rust-cgu.so'),
                   DISTCC_RUST_CGU_CLIENT=str(submit), DISTCC_RUST_CGU_OBJECT_ONLY='1')
        before = self.completed_jobs()
        with subprocess.Popen([self.rustc, '-Copt-level=2', '-Ccodegen-units=1', source, '-o', executable],
                              env=env, cwd=self.work, stderr=subprocess.PIPE, text=True) as compiler:
            try:
                deadline = time.monotonic() + 10
                while not started.exists() and compiler.poll() is None and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertTrue(started.exists(), 'rustc did not submit its CGU')
                with self.assertRaises(subprocess.TimeoutExpired):
                    compiler.wait(timeout=.3)
            finally:
                connection.close()
                lease.close()
            _, log = compiler.communicate(timeout=30)
        self.assertEqual(compiler.returncode, 0, log)
        self.assertIn('distcc-rust-cgu: compiled on ', log)
        self.assertGreater(self.completed_jobs()[0], before[0])
        self.assertEqual(subprocess.check_output([executable], text=True).strip(), str(sum(range(64))))

    def test_small_cargo_crates_use_both_whole_crate_workers(self):
        self.cgu_toolchain()
        self.start_broker(' '.join(self.host(i, f',tcp,stats={p}', slots=1)
                                   for i, p in enumerate(self.stats_ports)), name='small-workspace')
        counts = self.completed_jobs()
        crates = [f'leaf{n}' for n in range(8)]
        (self.work/'Cargo.toml').write_text('[workspace]\nresolver="2"\nmembers=' + json.dumps(crates))
        for crate in crates:
            directory = self.work/crate
            directory.mkdir()
            (directory/'Cargo.toml').write_text(f'[package]\nname="{crate}"\nversion="0.1.0"\n'
                                                'edition="2021"\n[lib]\npath="lib.rs"\n')
            (directory/'lib.rs').write_text('pub fn answer(x: u64) -> u64 { x + 42 }')
        for unrelated in ('tests', 'benches', 'examples', 'nested/target', 'nested/.cache'):
            ignored = self.work / crates[0] / unrelated
            ignored.mkdir(parents=True)
            (ignored/'large.rs').write_text('//' + 'x' * 65536)
        env = dict(self.env, RUSTC=self.rustc, RUSTC_WRAPPER=str(self.build/'distcc'),
                   CARGO_INCREMENTAL='1', RUSTFLAGS='-Ccodegen-units=256')
        result = subprocess.run(['cargo', 'build', '-vv', '--offline', '-j8'], cwd=self.work,
                                env=env, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('-L dependency=', result.stderr)
        self.assertIn('incremental=', result.stderr)
        self.assertEqual(result.stderr.count('distcc-rust: compiled on'), len(crates), result.stderr)
        self.assertNotIn('distcc-rust-cgu: completed', result.stderr)
        after = self.completed_jobs()
        self.assertTrue(all(b > a for a, b in zip(counts, after)), (counts, after))
        self.assertEqual(sum(after) - sum(counts), len(crates))

    def test_small_build_script_beside_large_crate(self):
        self.cgu_toolchain()
        self.start_broker(' '.join(self.host(i, f',tcp,stats={p}', slots=2)
                                   for i, p in enumerate(self.stats_ports)), name='build-script')
        (self.work/'Cargo.toml').write_text('[package]\nname="consumer"\nversion="0.1.0"\nedition="2021"\n')
        (self.work/'build.rs').write_text('fn main(){println!("cargo:rustc-env=BUILT=42");}')
        src = self.work / 'src'
        src.mkdir()
        (src/'main.rs').write_text('mod large; fn main(){assert_eq!(large::answer(), env!("BUILT"));}')
        (src/'large.rs').write_text('pub fn answer()->&\'static str {"42"}\n//' + 'x' * 65536)
        env = dict(self.env, RUSTC=self.rustc, RUSTC_WRAPPER=str(self.build/'distcc'),
                   CARGO_INCREMENTAL='0', RUSTFLAGS='-Ccodegen-units=8 -Copt-level=1')
        result = subprocess.run(['cargo', 'build', '-vv', '--offline'], cwd=self.work,
                                env=env, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        script = next(job for job in result.stderr.split('     Running `')
                      if ' --crate-name build_script_build ' in job)
        self.assertIn('distcc-rust: compiled on', script)
        subprocess.run([self.work/'target/debug/consumer'], check=True)

    def test_whole_crate_accepts_llvm_patch_update(self):
        report = subprocess.check_output([self.rustc, '-vV'], text=True)
        report, changes = re.subn(r'(?m)^(LLVM version: \d+\.\d+\.)(\d+)',
                                  lambda m: m[1] + str(int(m[2]) + 1), report)
        self.assertEqual(changes, 1)
        compiler = self.work/'rustc'
        compiler.write_text('#!/bin/sh\nif [ "$1" = -vV ]; then printf "%s" ' +
                            shlex.quote(report) + '; else exec ' + shlex.quote(self.rustc) + ' "$@"; fi\n')
        compiler.chmod(0o755)
        self.rustc = str(compiler)
        source = self.work/'main.rs'
        source.write_text('fn main(){ println!("patch"); }')
        output = self.work/'program'
        result = self.rust(source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertEqual(subprocess.check_output([output], text=True), 'patch\n')

    def test_host_feature_capture_respects_the_allowed_cores(self):
        cpus = sorted(os.sched_getaffinity(0))[:2]
        if len(cpus) != 2:
            self.skipTest('requires two allowed CPUs')
        # Exercise the real preload's pinning and LLVM ABI on a homogeneous
        # test machine. Only the per-core answer from LLVM is substituted.
        source = self.work/'features.c'
        source.write_text('#define _GNU_SOURCE\n#include <unistd.h>\n#include <sys/syscall.h>\n#include <string.h>\n'
                          'char *LLVMGetHostCPUFeatures(void) { unsigned cpu; syscall(SYS_getcpu, &cpu, 0, 0); '
                          'return strdup(cpu == '
                          f'{cpus[0]} ? "+left,+common,-right" : "-left,+right,+common"); }}')
        shim = self.work/'features.so'
        subprocess.run(['cc','-fPIC','-shared',source,'-o',shim], check=True)
        output = self.work/'features'
        preload = str(self.build/'distcc-rust-native.so') + ':' + str(shim)
        for mask, union, expected in [(cpus, False, 'common'), (cpus, True, 'common,left,right'),
                                      (cpus[:1], True, 'common,left')]:
            with self.subTest(mask=mask, union=union):
                env = dict(self.env, LD_PRELOAD=preload, DISTCC_RUST_HOST_PROFILE=str(output),
                           DISTCC_RUST_HOST_UNION=str(int(union)))
                result = self.rust('-Ctarget-cpu=native', '--print=cfg', remote=False, env=env,
                                   prefix=['taskset', '-c', ','.join(map(str,mask))])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output.read_text(), expected)
                output.unlink()

    def test_proc_macro_requires_host_features_only_for_whole_crates(self):
        self.cgu_toolchain()
        # Only the first worker's advertisement changes. All compiler, broker,
        # macro expansion and backend jobs still run through the real daemons.
        actual = self.get_stats(self.stats_ports[0])
        record = re.search(r'^dcc_rust_host_features (.+)$', actual, re.M)
        assert record is not None, 'worker did not advertise Rust host features'
        groups = []
        for group in record[1].split(';'):
            names, *keys = group.split()
            self.assertTrue(keys)
            groups.append(' '.join([','.join(names.split(',')[1:]) or '-', *keys]))
        narrow = self.peer(actual.replace(record[0], 'dcc_rust_host_features '+';'.join(groups)).encode())
        a = self.host(0, f',tcp,stats={narrow.port},speed=100', slots=1)
        b = self.host(1, f',tcp,stats={self.stats_ports[1]},speed=0.1', slots=1)
        macro = self.work/'maker.rs'
        macro.write_text('extern crate proc_macro; #[proc_macro] '
                         'pub fn number(_: proc_macro::TokenStream) -> proc_macro::TokenStream '
                         '{ "42".parse().unwrap() }')
        plugin = self.work/'libmaker.so'
        built = self.rust(macro, '--crate-type=proc-macro', '-Ctarget-cpu=native', '-o', plugin, remote=False)
        self.assertEqual(built.returncode, 0, built.stderr)
        main = self.work/'main.rs'
        program = self.work/'program'
        args = ['-Copt-level=2','-Ccodegen-units=1',main,'-o',program]
        self.start_broker(a+' '+b, name='host-features')
        main.write_text('fn main() { assert_eq!(std::hint::black_box(42), 42); }')
        before = self.completed_jobs()
        plain = self.rust(*args)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertIn('distcc-rust: compiled on', plain.stderr)
        self.assertEqual(self.completed_jobs(), [before[0]+1,before[1]])
        subprocess.run([program],check=True)
        # Keep enough IR for the normal 128-instruction CGU admission threshold.
        main.write_text('extern crate maker; fn main() { let mut x: u32 = maker::number!();' +
                        'x = std::hint::black_box(x).wrapping_add(1);' * 160 +
                        'assert_eq!(x, 202); }')
        args.extend(['--extern', 'maker='+str(plugin)])
        before = self.completed_jobs()
        log = self.work/'sandbox.log'
        prefix = ['sandbox', 'env', 'SANDBOX_WRITE='+str(self.work)] if shutil.which('sandbox') else []
        loaded = self.rust(*args, prefix=prefix, env=dict(self.env, SANDBOX_LOG=str(log),
                                                         SANDBOX_DEBUG_LOG=str(self.work/'sandbox.debug')))
        self.assertEqual(loaded.returncode, 0, loaded.stderr)
        self.assertFalse(log.exists(), loaded.stderr)
        self.assertIn('distcc-rust: compiled on', loaded.stderr)
        self.assertEqual(self.completed_jobs(), [before[0],before[1]+1])
        subprocess.run([program],check=True)
        self.start_broker(a, name='insufficient-host')
        before = self.completed_jobs()
        fallback = self.rust(*args)
        self.assertEqual(fallback.returncode, 0, fallback.stderr)
        self.assertIn('no worker with the required Rust host ABI', fallback.stderr)
        self.assertIn('distcc-rust-cgu: completed ', fallback.stderr)
        self.assertNotIn('distcc-rust: compiled on', fallback.stderr)
        self.assertGreater(self.completed_jobs()[0], before[0])
        self.assertEqual(self.completed_jobs()[1], before[1])
        subprocess.run([program],check=True)

    def test_same_triple_mixed_compilers_route_whole_crates_and_cgus(self):
        deploy = runpy.run_path(str(self.build/'packaging/gentoo/deploy.py'))
        abi, backend = self.cgu_toolchain()
        report = subprocess.check_output([self.rustc, '-vV'], text=True)
        host = next(line[6:] for line in report.splitlines() if line.startswith('host: '))
        identity = self.version(self.rustc)
        candidates = [*Path('/opt').glob('rust-bin-*/bin/rustc'), *Path('/usr/lib/rust').glob('*/bin/rustc')]
        other = next((p for p in sorted(candidates, reverse=True)
                      if self.version(p) != identity and 'host: '+host+'\n' in
                      subprocess.check_output([p, '-vV'], text=True)), None)
        if not other:
            self.skipTest('requires a second installed Rust build for the same triple')
        def key(version):
            identity = dict(zip(('version', 'commit', 'llvm'), version.splitlines(), strict=True))
            return deploy['rust_compiler_key'](dict(identity=identity, host=host))
        self.restart_server(self.build/'distccd', DISTCCD_RUSTC=str(other))
        self.addCleanup(self.restart_server, self.build/'distccd')
        hosts = [self.host(i, f',tcp,stats={p}', slots=1) for i, p in enumerate(self.stats_ports)]
        self.start_broker(' '.join(hosts), name='mixed')
        offers = [deploy['rust_worker_capabilities'](endpoint) for endpoint in hosts]
        self.assertEqual(offers[0][1], {key(self.version(other))})
        self.assertIn(key(identity), offers[1][1])
        self.assertTrue(all((abi, backend) in backends for _, _, backends in offers))
        capacity = self.request(20, work=(1, 65536, 11), memory_class=1+JOB_RUST_CGU*64+16,
                                cgu_backend=f'{abi} {backend}'.encode(), rust_compiler=int(key(identity), 16))
        self.assertEqual(struct.unpack_from('=iiIi', capacity.recv(1036)), (0, 2, 0, 1))
        (self.work/'Cargo.toml').touch()
        source = self.work/'src'
        source.mkdir()
        main = source/'main.rs'
        modules = []
        for i in range(16):
            functions = ''.join(f'#[inline(never)] pub fn f{j}(x:u64)->u64 {{ x.wrapping_add({i+j}) }}\n'
                                for j in range(100))
            (source/f'm{i}.rs').write_text(functions + 'pub fn sum(x:u64)->u64 { ' +
                '+'.join(f'f{j}(x)' for j in range(100)) + ' }')
            modules.append(f'mod m{i};')
        main.write_text('\n'.join(modules) + '\nfn main(){ assert_eq!(' +
                        '+'.join(f'm{i}::sum(std::hint::black_box(1))' for i in range(16)) +
                        f', {sum(1+i+j for i in range(16) for j in range(100))}); }}')
        build = self.work/'build.rs'
        build.write_text('fn main(){println!("cargo:rustc-cfg=checked");}')
        program = self.work/'program'
        counts = self.completed_jobs()
        result = self.rust(build, '-Ccodegen-units=8', '-o', program)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertEqual(subprocess.check_output([program], text=True).strip(), 'cargo:rustc-cfg=checked')
        self.assertEqual(self.completed_jobs(), [counts[0], counts[1]+1])
        plugin = ['-Clinker-plugin-lto', '-Clinker=clang', '-Clink-arg=-fuse-ld=lld']
        result = self.rust(main, '-Copt-level=2', '-Ccodegen-units=1', *plugin, '-o', program)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertNotIn('distcc-rust-cgu:', result.stderr)
        subprocess.run([program], check=True, timeout=5)
        for flags in ([], plugin):
            with self.subTest(flags=flags):
                counts = self.completed_jobs()
                trace = self.work/'frontend.trace'
                result = self.rust(main, '-Copt-level=2', '-Ccodegen-units=8', *flags, '-o', program,
                                   prefix=['strace', '-f', '-s', '2048', '-e', 'execve', '-o', str(trace)])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn('-Zbinary-dep-depinfo', trace.read_text())
                if flags:
                    modules = re.findall(r'^distcc-rust-cgu: completed thin-prelink (\S+)$', result.stderr, re.M)
                    self.assertGreater(len(modules), 1, result.stderr)
                    self.assertEqual(len(set(modules)), len(modules))
                    self.assertEqual(sum(self.completed_jobs()) - sum(counts), len(modules))
                else:
                    self.assert_one_job_per_cgu(result.stderr, counts)
                self.assertTrue(all(b > a for a, b in zip(counts, self.completed_jobs())), result.stderr)
                subprocess.run([program], check=True, timeout=5)
        self.start_broker(hosts[0], name='same-triple-cgu-only')
        counts = self.completed_jobs()
        result = self.rust(main, '-Copt-level=2', '-Ccodegen-units=1', *plugin, '-o', program)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count('distcc-rust-cgu: completed thin-prelink '), 1, result.stderr)
        self.assertEqual(self.completed_jobs(), [counts[0]+1, counts[1]])
        subprocess.run([program], check=True, timeout=5)
        counts = self.completed_jobs()
        result = self.rust(main, '-Copt-level=2', '-Ccodegen-units=8', '-o', program)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_job_per_cgu(result.stderr, counts)
        self.assertEqual(self.completed_jobs()[1], counts[1])
        subprocess.run([program], check=True, timeout=5)

    def test_parallel_cgu_backend(self):
        version, backend = self.cgu_toolchain()
        hosts = ' '.join(self.host(i, f',tcp,stats={p}', slots=3) for i,p in enumerate(self.stats_ports))
        self.start_broker(hosts, name='cgu')
        for p in self.stats_ports:
            row = next(s for s in self.get_stats(p).splitlines() if s.startswith('dcc_llvm_cgu '))
            offers = [s.split() for s in row.removeprefix('dcc_llvm_cgu ').split(';')]
            self.assertTrue(any(x[0] == version and backend in x[1:] for x in offers))
        src = self.work/'wide.rs'
        modules = []
        for i in range(16):
            funcs = '\n'.join(f'#[inline(never)] pub fn f{j}(x:u64)->u64 {{ x.wrapping_mul({j+1}).rotate_left({j%61}) ^ {i} }}'
                              for j in range(100))
            expr = 'x' + ''.join(f'.wrapping_add(f{j}(x))' for j in range(100))
            (src.parent / f'm{i}.rs').write_text(f'{funcs} pub fn all(x:u64)->u64 {{ {expr} }}')
            modules.append(f'pub mod m{i};')
        src.write_text('\n'.join(modules) + '\nfn main(){println!("{}",' +
                       '^'.join(f'm{i}::all(std::hint::black_box(19))' for i in range(16)) + ');}')
        args = (src, '-Ccodegen-units=8', '-Copt-level=z', '-Cllvm-args=-import-instr-limit=5',
                '-Cdebuginfo=1', '-Cembed-bitcode=yes')
        expected = self.work/'reference'
        actual = self.work/'parallel'
        local = self.rust(*args, '-o', expected, remote=False)
        self.assertEqual(local.returncode, 0, local.stderr)

        overlap = False
        counts = self.completed_jobs()
        trace = self.work/'compiler.log'
        with trace.open('w') as log, subprocess.Popen(
                [self.build/'distcc', self.rustc, *args, '-o', actual],
                env=self.env, cwd=self.work, stderr=log) as build:
            deadline = time.monotonic() + 90
            while build.poll() is None:
                overlap |= sum(self.cgu_executing(d.pid) for d in self.daemons) >= 2
                if time.monotonic() > deadline:
                    build.kill()
                    self.fail('parallel compiler timed out')
                time.sleep(.005)
        messages = trace.read_text()
        self.assertEqual(build.returncode, 0, messages)
        self.assertIn('distcc-rust-cgu: finished thin-postlink', messages)
        self.assertIn('distcc-rust-cgu: completed', messages)
        self.assert_one_job_per_cgu(messages, counts)
        self.assertTrue(overlap, messages)
        self.assertEqual(subprocess.check_output([actual]), subprocess.check_output([expected]))

    def test_cargo_pipelines_local_frontend_with_remote_cgus(self):
        self.cgu_toolchain()
        self.start_broker(' '.join(self.host(i, f',tcp,stats={p}') for i,p in enumerate(self.stats_ports)),
                          name='pipeline-cgus')
        for crate in ('producer', 'consumer'):
            directory = self.work/crate
            directory.mkdir()
            manifest = f'[package]\nname="{crate}"\nversion="0.1.0"\nedition="2021"\n[lib]\npath="lib.rs"\n'
            if crate == 'consumer':
                manifest += '[dependencies]\nproducer={path="../producer"}\n'
            (directory/'Cargo.toml').write_text(manifest)
        (self.work/'Cargo.toml').write_text('[workspace]\nmembers=["producer","consumer"]\nresolver="2"\n')
        (self.work/'producer/lib.rs').write_text('\n'.join(
            f'pub mod m{i} {{' + ''.join(
                f'#[inline(never)] pub fn f{j}(mut x:u64)->u64 {{for k in 1..20 {{x=x.wrapping_mul({j+3}).rotate_left(k)}} x}}'
                for j in range(128)) + '}' for i in range(16)))
        (self.work/'consumer/lib.rs').write_text('pub fn result(x:u64)->u64{producer::m1::f2(x)}')
        # Wrap the actual distcc command to record Cargo's per-crate start and
        # finish boundaries. The diagnostic stream passes through immediately.
        wrapper = self.work/'observe-cargo'
        wrapper.write_text('#!/bin/sh\ncase " $* " in\n'
            '  *" --crate-name producer "*) crate=producer;;\n'
            '  *" --crate-name consumer "*) crate=consumer;;\n  *) exec "$DISTCC_UNDER_TEST" "$@";;\nesac\n'
            'date +%s%N >"$EVENTS/$crate.start"\n"$DISTCC_UNDER_TEST" "$@"\nrc=$?\n'
            'date +%s%N >"$EVENTS/$crate.end"\nexit "$rc"\n')
        wrapper.chmod(0o755)
        env = dict(self.env, EVENTS=str(self.work), DISTCC_UNDER_TEST=str(self.build/'distcc'),
                   RUSTC=self.rustc, RUSTC_WRAPPER=str(wrapper), CARGO_INCREMENTAL='0',
                   RUSTFLAGS='-Ccodegen-units=8 -Copt-level=2')
        log = self.work/'cargo.log'
        # Only producer modules exceed the CGU threshold. Observe actual
        # worker LLVM processes after Cargo has started the metadata consumer.
        overlap = False
        with log.open('w') as output, subprocess.Popen(['cargo','build','-vv','--offline','-j4','-p','consumer'],
                env=env, cwd=self.work, stdout=output, stderr=output) as cargo:
            deadline = time.monotonic() + 100
            while cargo.poll() is None:
                if (self.work/'consumer.start').exists() and not (self.work/'producer.end').exists():
                    overlap |= any(self.cgu_executing(d.pid) for d in self.daemons)
                if time.monotonic() > deadline:
                    cargo.kill()
                    self.fail('Cargo pipeline timed out')
                time.sleep(.005)
        messages = log.read_text()
        self.assertEqual(cargo.returncode, 0, messages)
        self.assertRegex(messages, r'--extern producer=[^\s]+\.rmeta')
        self.assertIn('distcc-rust-cgu: completed', messages)
        self.assertTrue(overlap, messages)

    def worker_with_patch_level(self, patch):
        # Only the patch reported by the real LLVM C API changes. The daemon
        # advertises this helper and executes it inside the ordinary sandbox.
        folder = self.work/'patch-worker'
        folder.mkdir()
        daemon = folder/'distccd'
        shutil.copy2(self.build/'distccd', daemon)
        shim = folder/'version.c'
        shim.write_text('#include <stdio.h>\n#include <unistd.h>\n'
                        'void __real_LLVMGetVersion(unsigned*,unsigned*,unsigned*);\n'
                        'void __wrap_LLVMGetVersion(unsigned *a,unsigned *b,unsigned *c) { '
                        f'__real_LLVMGetVersion(a,b,c); *c={patch}; '
                        'if (!access("/backend", X_OK)) fprintf(stderr, "LLVM worker %u.%u.%u\\n", *a, *b, *c); }\n')
        obj = shim.with_suffix('.o')
        subprocess.run(['cc', '-c', shim, '-o', obj], check=True)
        helper = folder/'distcc-rust-cgu'
        subprocess.run(['make', '-s', 'LTO_BUILD=' + str(folder), 'CXXFLAGS=-O0',
                        f'LDFLAGS={obj} -Wl,--wrap=LLVMGetVersion', str(helper)],
                       cwd=self.build, check=True)
        return daemon

    def test_foreign_host_cgu_with_impure_macro_and_embedded_bitcode(self):
        llvm, backend = self.cgu_toolchain()
        version = subprocess.check_output([self.rustc, '-vV'], text=True)
        native = next(line.removeprefix('host: ') for line in version.splitlines() if line.startswith('host: '))
        foreign_host = 'aarch64-unknown-linux-gnu' if native.startswith('x86') else 'x86_64-unknown-linux-gnu'
        foreign = self.work/'foreign-rustc'
        match = re.search(r'^LLVM version: \d+\.\d+\.(\d+)', version, re.M)
        assert match is not None, version
        client_patch = int(match[1])
        worker_patch = 3 if client_patch != 3 else 5
        daemon = self.worker_with_patch_level(worker_patch)
        foreign_version = version.replace('host: '+native, 'host: '+foreign_host)
        foreign_version = re.sub(r'(?m)^(LLVM version: \d+\.\d+)\.\d+', rf'\g<1>.{worker_patch}', foreign_version)
        foreign.write_text('#!/bin/sh\n[ "$1" = -vV ] || exit 121\nprintf "%s" ' +
                           shlex.quote(foreign_version) + '\n')
        foreign.chmod(0o755)

        writes = self.work/'side-effect'
        plugin_src = self.work/'writer.rs'
        plugin_src.write_text('extern crate proc_macro; use std::io::Write; #[proc_macro] '
            'pub fn counter(_:proc_macro::TokenStream)->proc_macro::TokenStream { '
            'std::fs::OpenOptions::new().append(true).create(true).open(' + json.dumps(str(writes)) +
            ').unwrap().write_all(b"x").unwrap(); "42".parse().unwrap() }')
        plugin = self.work/'libwriter.so'
        r = self.rust(plugin_src, '--crate-type=proc-macro', '-o', plugin, remote=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        src = self.work/'library.rs'
        src.write_text('extern crate writer; pub const N:u64 = writer::counter!();\n' +
            '\n'.join(f'pub mod m{i} {{' + ''.join(
                f'#[inline(never)] pub fn f{j}(x:u64)->u64 {{ x.wrapping_mul({j+1}).rotate_left({j%32}) }}'
                for j in range(100)) + '}' for i in range(8)))
        out = self.work/'liblibrary.rlib'
        trace = self.work/'frontend.trace'
        args = ['--crate-type=rlib', src, '--extern', 'writer='+str(plugin), '-Clto=off',
                '-Cembed-bitcode=yes', '-Copt-level=2', '-Ccodegen-units=256',
                '-Cincremental='+str(self.work/'incremental'), '-Cdebuginfo=2', '-o', out]
        # Run the same impure crate on one matching worker and one foreign
        # worker independently. The local frontend must execute once in each case.
        for host in (native, foreign_host):
            with self.subTest(worker_rust_host=host):
                if host == foreign_host:
                    self.restart_server(daemon, DISTCCD_RUSTC=str(foreign))
                    self.addCleanup(self.restart_server, self.build/'distccd')
                self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='foreign' if host == foreign_host else 'native')
                stats = self.get_stats(self.stats_ports[0])
                self.assertIn('dcc_rust_host '+(native if host == native else '-'), stats)
                self.assertIn(llvm+' '+backend, next(l for l in stats.splitlines() if l.startswith('dcc_llvm_cgu ')))
                for incompatible in ('0.0.0 '+backend, llvm+' absent-backend'):
                    request = self.request(10, work=(1, 65536, 11), memory_class=1+JOB_RUST_CGU*64+16,
                                           cgu_backend=incompatible.encode())
                    self.assertEqual(self.receive(request)[0], 121)
                counts = self.completed_jobs()
                result = subprocess.run(['strace', '-f', '-s', '256', '-e', 'execve', '-o', trace,
                                         self.build/'distcc', self.rustc, *args], cwd=self.work, env=self.env,
                                        capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust-cgu: finished module-opt', result.stderr)
                self.assertIn('distcc-rust-cgu: completed', result.stderr)
                self.assert_one_job_per_cgu(result.stderr, counts)
                if host == foreign_host:
                    daemon_log = Path(self.logs[0].name).read_text()
                    self.assertEqual(daemon_log.count(f'LLVM worker {llvm}.{worker_patch}\n'), 1)
                self.assertEqual(writes.read_bytes(), b'x')
                writes.unlink()
                calls = trace.read_text()
                self.assertEqual('-Zbinary-dep-depinfo' in calls, host == native)
                frontend = 'execve("' + str(Path(self.rustc).resolve()) + '",'
                full = [line for line in calls.splitlines() if frontend in line and
                        re.search(r'"--crate-type(?:=rlib"|", "rlib")', line) and '-Zbinary-dep-depinfo' not in line]
                self.assertEqual(len(full), 1, calls)
                self.assertIn('"codegen-units=16"', full[0])
                self.assertNotIn('incremental=', full[0])
                bitcode = self.work / 'libplugin.rlib'
                result = self.rust(*args[:-2], '-Clinker-plugin-lto', '-o', bitcode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust-cgu: completed ', result.stderr)
                self.assertEqual(writes.read_bytes(), b'x')
                writes.unlink()
                members = subprocess.check_output(['ar', 't', bitcode], text=True).splitlines()
                member = next(name for name in members if name.endswith('.rcgu.o'))
                module = subprocess.check_output(['ar', 'p', bitcode, member])
                self.assertTrue(module.startswith(b'BC\xc0\xde'))
                subprocess.run(['llvm-dis', '-o', os.devnull, '-'], input=module, check=True)
        members = subprocess.check_output(['ar', 't', out], text=True).splitlines()
        self.assertTrue(any('rmeta' in name for name in members))
        obj = self.work/'unit.o'
        obj.write_bytes(subprocess.check_output(['ar', 'p', out, next(n for n in members if n.endswith('.rcgu.o'))]))
        sections = subprocess.check_output(['readelf', '-SW', obj], text=True)
        for section in ('.llvmbc', '.llvmcmd', '.debug_info'):
            self.assertIn(section, sections)
        main = self.work/'uses.rs'
        main.write_text('fn main(){ assert_eq!(library::N + library::m2::f0(2), 44); }')
        r = self.rust(main, '--extern', 'library='+str(out), '-Ldependency='+str(self.work),
                      '-Clto=fat', '-o', self.work/'uses', remote=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        subprocess.run([self.work/'uses'], check=True)
        r = self.rust(main, '--extern', 'library='+str(self.work/'libplugin.rlib'),
                      '-Ldependency='+str(self.work), '-Clinker=clang', '-Clinker-plugin-lto',
                      '-Clink-arg=-fuse-ld=lld', '-o', self.work/'plugin-uses', remote=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        subprocess.run([self.work/'plugin-uses'], check=True)

    def test_plain_probe_inherits_only_its_journal(self):
        source = self.work / 'plain.rs'
        source.write_text('pub fn value() {}')
        compiler = self.work / 'rustc'
        compiler.write_text('#!/bin/sh\nif test -n "$DISTCC_RUST_LOADS_FD"; then\n'
                            '  test -e "/proc/self/fd/$DISTCC_RUST_LOADS_FD" || exit 1\n'
                            '  test "$(readlink "/proc/self/fd/$CHECK_FD")" != "$CHECK_PIPE" || exit 1\n'
                            '  echo checked > checked-probe\nfi\nexec ' + shlex.quote(self.rustc) + ' "$@"\n')
        compiler.chmod(0o755)
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, 'rb'), os.fdopen(write_fd, 'wb'):
            env = dict(self.env, CHECK_FD=str(read_fd), CHECK_PIPE=os.readlink(f'/proc/self/fd/{read_fd}'))
            result = subprocess.run([self.build / 'distcc', compiler, '--crate-type=rlib', source],
                                    env=env, cwd=self.work, pass_fds=(read_fd,),
                                    capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('compiled on', result.stderr)
        self.assertTrue((self.work / 'checked-probe').is_file())

    def test_probe_executes_no_tracer_or_unpretty_pass(self):
        src = self.work/'attr.rs'
        src.write_text('#[link(wasm_import_module="js")] unsafe extern "C" { fn unused(); } pub fn ok(){}')
        log = self.work/'executed'
        r = subprocess.run(['strace', '-f', '-s', '256', '-e', 'execve', '-o', log, self.build/'distcc',
                            self.rustc, '--crate-type=rlib', src, '-Ldependency='+str(self.work),
                            '-Lnative='+str(self.work)], env=self.env, cwd=self.work,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('distcc-rust: compiled on', r.stderr)
        calls = log.read_text()
        self.assertIn('-Zbinary-dep-depinfo', calls)
        self.assertNotRegex(calls, r'execve\("[^"]*/strace"')
        self.assertNotIn('-Zunpretty', calls)

    def test_rust_uses_measured_workload_feedback(self):
        self.source = self.work / 'library.rs'
        self.source.write_text('pub fn value() -> u32 { 42 }')
        for index in range(2):
            result = self.rust('--crate-type=rlib', self.source, '-o', self.work / f'libsample{index}.rlib')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-rust: compiled on', result.stderr)
        records = [dict(re.findall(r'(\w+)=([^ ]+)', line))
                   for line in (self.work / 'broker.log').read_text().splitlines()
                   if 'job cost:' in line and 'kind=7 ' in line]
        self.assertEqual(len(records), 2)
        self.assertGreater(float(records[0]['predicted']), 0)
        self.assertGreater(float(records[1]['predicted']), 0)
        self.assertGreater(int(records[0]['units']), 0)
        self.assertGreater(int(records[0]['bytes']), int(records[0]['units']))
        self.assertNotEqual(int(records[0]['family'], 16), 0)
        self.assertEqual(records[0]['family'], records[1]['family'])
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_rust_capacity ', stats)

    def test_tiny_crate_uses_local_preflight(self):
        address = '127.0.0.1'
        self.restart_server(self.build/'distccd', address=address)
        self.addCleanup(self.restart_server, self.build/'distccd')
        self.start_broker(f'{address}:{self.ports[0]}/1,tcp,stats={self.stats_ports[0]}', name='local')
        source = self.work/'tiny.rs'
        output = self.work/'libtiny.rlib'
        source.write_text('pub fn tiny() {}')
        result = self.rust('--crate-type=rlib', '-L', 'dependency='+str(self.work),
                           '-Clinker-plugin-lto', source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreater(output.stat().st_size, 0)
        stats = subprocess.check_output([self.build/'distcc','--lto-capacity'], env=self.env)
        self.assertEqual(json.loads(stats)['direct_completed'], 1)
        self.assertNotIn('input preparation command=', (self.work/'local.log').read_text())
        rsp=self.work/'long.rsp'
        rsp.write_text(f'--crate-type=rlib\n--cfg={"x"*140000}\n{source}\n-o\n{output}\n')
        result=self.rust('@'+str(rsp))
        self.assertEqual(result.returncode,0,result.stderr)
        stats=subprocess.check_output([self.build/'distcc','--lto-capacity'],env=self.env)
        self.assertEqual(json.loads(stats)['direct_completed'],2)
        self.start_broker(f'{address}:{self.ports[0]}/1,tcp,stats={self.stats_ports[0]} ' +
                          self.host(1, f',tcp,stats={self.stats_ports[1]}', slots=1), name='occupied')
        lease = self.request(15, (1, 1024, 1))
        self.assertEqual(self.receive(lease), (0, 1, []))
        result = self.rust('--crate-type=rlib', '-L', 'dependency='+str(self.work),
                           '-Clinker-plugin-lto', source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on '+self.worker_address, result.stderr)
        lease.close()

    def test_direct_crates_bypass_masqueraded_linker(self):
        address = '127.0.0.1'
        self.restart_server(self.build/'distccd', address=address)
        self.addCleanup(self.restart_server, self.build/'distccd')
        self.start_broker(f'{address}:{self.ports[0]}/1,tcp,stats={self.stats_ports[0]} --localslots=1',
                          name='host-link')
        mask = self.work/'bin'
        mask.mkdir()
        (mask/'clang').symlink_to(self.build/'distcc')
        env = dict(self.env, PATH=str(mask)+os.pathsep+self.env['PATH'])
        source = self.work/'build.rs'
        for name, kind in [('build_script_build','bin'),('macro_plugin','proc-macro'),('tiny','bin')]:
            source.write_text('extern crate proc_macro;' if kind=='proc-macro' else 'fn main() {}')
            output = self.work/(name+'.so' if kind=='proc-macro' else name)
            result = self.rust('--crate-name',name,'--crate-type',kind,'-Clinker='+str(mask/'clang'),
                               '-Clink-arg=-fuse-ld=lld',source,'-o',output,env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('distcc-thinlto:', result.stderr)
            self.assertNotIn('dcc_lock_pause', result.stderr)
            if kind=='bin':
                subprocess.run([output], check=True)
            else:
                self.assertTrue(output.stat().st_size)
        stats = json.loads(subprocess.check_output([self.build/'distcc','--lto-capacity'], env=env))
        self.assertEqual(stats['direct_completed'], 3)
        self.assertNotIn('kind=8 ', (self.work/'host-link.log').read_text())

    def test_small_thinlto_links_use_distributor(self):
        for name, kind, body in [('build_script_build', 'bin', 'fn main() {}'),
                                 ('derived', 'proc-macro', 'extern crate proc_macro;')]:
            with self.subTest(kind=kind):
                source = self.work/(name+'.rs')
                source.write_text(body)
                output = self.work/(name if kind=='bin' else 'lib'+name+'.so')
                result = self.rust('--crate-name', name, '--crate-type', kind, '-Clinker=clang',
                                   '-Clinker-plugin-lto', '-Clink-arg=-fuse-ld=lld', source, '-o', output)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-thinlto: compiled on ', result.stderr)
                if kind=='bin':
                    subprocess.run([output], check=True)
                else:
                    consumer = self.work/'consumer.rs'
                    consumer.write_text('extern crate derived;')
                    subprocess.run([self.rustc, '--crate-type=lib', '--emit=metadata', consumer,
                                    '--extern', 'derived='+str(output)], cwd=self.work, check=True)

    def test_warm_compiler_and_rlib_reuse(self):
        dep, artifact = self.work/'leaf.rs', self.work/'libleaf.rlib'
        # Include incompressible extents so the cold transfer exercises sendfile.
        (self.work/'data.bin').write_bytes(hashlib.shake_256(b'leaf').digest(1048576))
        dep.write_text('pub static DATA: [u8; 1048576] = *include_bytes!("data.bin");')
        subprocess.run([self.rustc, '--crate-type=rlib', dep, '-o', artifact], check=True)
        src = self.work/'caller.rs'
        src.write_text('extern crate leaf; pub fn answer() -> u8 { leaf::DATA[0] }')
        args = ['--crate-type=rlib', '-Ctarget-cpu=native', '--extern', 'leaf='+str(artifact),
                src, '--emit=dep-info,metadata,link']
        for label in ('cold', 'warm'):
            outdir = self.work/label
            outdir.mkdir()
            result = self.rust(*args, '--out-dir', outdir)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('compiled on', result.stderr)
            self.assertEqual(sorted(p.name for p in outdir.iterdir()), ['caller.d','libcaller.rlib','libcaller.rmeta'])
        self.assertIn('input cache: 0 of', result.stderr)
        self.assertEqual((self.work/'broker.log').read_text().count('input preparation command=12 '), 2)

    def test_warm_digest_table_has_constant_ipc_and_descriptor_counts(self):
        source=self.work/'tree.rs'
        def compile_with(n):
            source.write_text(''.join(f'pub mod m{i};\n' for i in range(n)))
            for i in range(n):
                (self.work/f'm{i}.rs').write_text(f'pub const V:u32={i};')
            args=[self.rustc,'--crate-type=rlib','--emit=metadata',source,'--out-dir',self.work]
            compiled=self.rust(*args[1:])
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            trace=self.work/'client.trace'
            result=subprocess.run(['strace','-o',trace,'-e','trace=sendmsg,memfd_create,inotify_add_watch',
                                   self.build/'distcc',*args],env=self.env,cwd=self.work,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('compiled on',result.stderr)
            events=trace.read_text()
            self.assertNotIn('inotify_add_watch(',events)
            self.assertNotIn('memfd_create("distcc-rust-input-digests"',events)
            return [events.count(call+'(') for call in ('sendmsg','memfd_create')]
        self.assertEqual(compile_with(1),compile_with(150))

    def test_empty_named_results_and_explicit_dependency_filename(self):
        source=self.work/'none.rs'
        source.write_text('pub fn value() {}')
        out=self.work/'products'
        out.mkdir()
        result=self.rust(source,'--crate-type=rlib','-Zno-codegen','--emit=obj','--out-dir',out,
                         env=dict(self.env,RUSTC_BOOTSTRAP='1'))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('compiled on',result.stderr)
        self.assertEqual(list(out.iterdir()),[])
        dep=out/'dependencies.make'
        result=self.rust(source,'--crate-type=rlib','--emit=metadata,dep-info='+str(dep),'--out-dir',out)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('compiled on',result.stderr)
        self.assertIn(str(dep)+':',dep.read_text())
        self.assertNotIn('/__distcc_rust',dep.read_text())

    def test_local_rustc_diagnostics_and_large_original_response(self):
        for args in ([self.work/'absent.rs'], ['--explain','E0514']):
            plain=self.rust(*args,remote=False)
            result=self.rust(*args)
            self.assertEqual((result.returncode,result.stdout),(plain.returncode,plain.stdout))
            self.assertIn(plain.stderr.strip(),result.stderr)
        src=self.work/'args.rs'
        src.write_text('pub fn value() {}')
        rsp=self.work/'args.rsp'
        rsp.write_text('--crate-type=proc-macro\n'+str(src)+'\n--cfg='+'x'*140000+'\n')
        # rustc itself diagnoses the invalid invocation from the original response file.
        plain=self.rust('@'+str(rsp),remote=False)
        result=self.rust('@'+str(rsp))
        self.assertEqual(result.returncode,plain.returncode)
        self.assertIn(plain.stderr.strip(),result.stderr)

    def test_remote_codegen_and_local_native_links(self):
        src = self.work/'products.rs'
        src.write_text('pub fn value() -> u32 { 42 } fn main(){assert_eq!(value(),42)} '
                       '#[test] fn check(){assert_eq!(value(),42)}')
        for flags, name in ((['--crate-type=bin'],'program'), (['--test'],'tests'),
                            (['--crate-type=cdylib'],'libcproduct.so'), (['--crate-type=dylib'],'libproduct.so'),
                            (['--crate-type=staticlib'],'libproduct.a')):
            with self.subTest(flags=flags):
                output = self.work/name
                if name in ('program', 'libproduct.so'):
                    flags = [*flags, '-Cstrip=symbols', '-Crpath']
                result = self.rust(*flags, src, '-o', output)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('compiled on', result.stderr)
                self.assertGreater(output.stat().st_size, 4096)
                if name in ('program', 'libproduct.so'):
                    native = self.rust(*flags, src, '-o', self.work/'local-product', remote=False)
                    self.assertEqual(native.returncode, 0, native.stderr)
                    def rpath(p):
                        dynamic = subprocess.check_output(['readelf', '-d', p], text=True)
                        return [line.split('[', 1)[1] for line in dynamic.splitlines()
                                if '(RUNPATH)' in line or '(RPATH)' in line]
                    self.assertEqual(rpath(output), rpath(self.work/'local-product'))
                if name in ('program','tests'):
                    subprocess.run([output], check=True, capture_output=True)
        driver=self.work/'client-cc'
        driver.write_text('#!/bin/sh\nexec "${MOZ_CARGO_WRAP_LD:?}" "$@" -Wl,--defsym,verified_client_link=42\n')
        driver.chmod(0o755)
        output=self.work/'client-linked'
        inner,outer=self.work/'inner args',self.work/'outer args'
        inner.write_text('"-Wl,--defsym,verified_response_arg=17"\n')
        outer.write_text(shlex.quote('@'+str(inner))+'\n')
        mask = self.work/'masquerade'
        mask.mkdir()
        (mask/'cc').symlink_to(self.build/'distcc')
        env = dict(self.env, MOZ_CARGO_WRAP_LD='cc', PATH=str(mask)+os.pathsep+self.env['PATH'])
        result=self.rust(src,'-Clinker='+str(driver),'-Clink-arg=@'+str(outer),'-o',output,env=env)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('compiled on',result.stderr)
        symbols=subprocess.check_output(['nm',output],text=True)
        self.assertIn('verified_client_link',symbols)
        self.assertIn('verified_response_arg',symbols)

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'Clang and LLD required')
    def test_linker_plugin_lto_distributes_from_confined_linker(self):
        source = self.work/'thin.rs'
        source.write_text('mod padding; fn main() { println!("distributed Rust ThinLTO"); }')
        (self.work / 'padding.rs').write_text('// ' + 'x' * 65536)
        self.start_broker(' '.join(self.host(i, f',tcp,stats={p}') for i, p in enumerate(self.stats_ports)),
                          name='plugin-lto')
        output = self.work/'thin'
        wrapper = self.work/'cargo-linker'
        wrapper.write_text('#!/bin/sh\nexec "${MOZ_CARGO_WRAP_LD:?}" "$@"\n')
        wrapper.chmod(0o755)
        mask = self.work/'masquerade'
        mask.mkdir()
        (mask/'clang').symlink_to(self.build/'distcc')
        log = self.work / 'distcc.log'
        env = dict(self.env, MOZ_CARGO_WRAP_LD='clang', DISTCC_LOG=str(log),
                   PATH=str(mask)+os.pathsep+self.env['PATH'])
        prefix = []
        if shutil.which('sandbox'):
            env.update(SANDBOX_LOG=str(self.work/'sandbox.log'), SANDBOX_DEBUG_LOG=str(self.work/'sandbox.debug'))
            prefix = ['sandbox', 'env', 'SANDBOX_WRITE='+str(self.work), 'TMPDIR='+str(self.work),
                      'PYTHONDONTWRITEBYTECODE=1']
        for driver in ('clang', str(wrapper)):
            with self.subTest(driver=driver):
                result = self.rust(source, '-Ccodegen-units=8', '-Clinker='+driver, '-Clinker-plugin-lto',
                                   '-Clink-arg=-fuse-ld=lld', '-o', output, env=env, prefix=prefix)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-thinlto: compiled on ', result.stderr)
                self.assertIn('distcc-rust: compiled on ', result.stderr)
                self.assertNotIn('distcc-rust-cgu: completed ', result.stderr)
                self.assertNotIn('Read-only file system', result.stderr)
                self.assertGreater(log.stat().st_size, 0)
                self.assertEqual(subprocess.check_output([output], text=True), 'distributed Rust ThinLTO\n')

    def test_single_cgu_thinlto_partitions_with_size_optimization(self):
        clang = shutil.which('clang')
        if not clang:
            self.skipTest('Clang required')
        self.clang_slot(clang)
        hosts = [self.host(i, f',tcp,stats={self.stats_ports[i]}', slots=1) for i in range(2)]
        self.start_broker(' '.join(hosts), name='thinlto')
        source = self.work/'functions.rs'
        functions = [f'#[inline(never)] fn f{i}(x:u32)->u32 {{ let y=unsafe {{core::ptr::read_volatile(&x)}}; '
                     f'y.wrapping_mul({i+1}) + (y ^ x) }}'
                     for i in range(32)]
        total = ' + '.join(f'f{i}(x)' for i in range(len(functions)))
        source.write_text('#![no_std]\n' + '\n'.join(functions) + '\n#[inline(never)] pub fn total(x:u32)->u32 {'
                          + total + '}')
        library = self.work/'libfunctions.rlib'
        codegen = ['-Clinker-plugin-lto', '-Ccodegen-units=1', '-Copt-level=z']
        compiled = self.rust(source, '--crate-type=rlib', *codegen, '-o', library)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        main = self.work/'main.rs'
        main.write_text('extern crate functions; fn main() { let n=std::env::args().count() as u32; '
                        'println!("{}", functions::total(n)); }')
        output = self.work/'program'
        result = self.rust(main, '--extern', 'functions='+str(library), '-Clinker='+clang, *codegen,
                           '-Clink-arg=-fuse-ld=lld', '-Clink-arg=-Wl,--thinlto-remote-compiler-arg=-Oz',
                           '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        partitions = re.findall(r'distcc-thinlto: partitioned .* into (\d+)', result.stderr)
        self.assertTrue(any(int(count) > 1 for count in partitions), result.stderr)
        weights = re.findall(r'distcc-thinlto: submitting .*\.part-\d+\.bc instructions=(\d+)', result.stderr)
        self.assertGreaterEqual(sum(int(weight) > 0 for weight in weights), 2, result.stderr)
        self.assertGreaterEqual(result.stderr.count('distcc-thinlto: compiled on '), len(weights))
        self.assertEqual(subprocess.check_output([output, 'argument'], text=True), '1056\n')

    def test_thinlto_output_directories(self):
        import sys
        work, final, temporary = (self.work/p for p in ('inputs', 'final', 'lto'))
        for directory in (work, final, temporary):
            directory.mkdir()
        (work/'dtlto-inputs').touch()
        linked = final/'program'
        (temporary/'alias').symlink_to(work, target_is_directory=True)
        description = temporary/'dtlto.json'
        common = dict(linker_output=str(linked), args=['clang', '-c'], inputs=[])

        def job(destination):
            return dict(args=['primary', '-fthinlto-index=index', '-o', str(destination)],
                        inputs=['primary', 'index'], outputs=[str(destination)])

        # Act as the sandboxed linker on the inherited control socket.
        sender = ('import os,sys,socket; s=socket.socket(fileno=int(os.environ["DISTCC_RUST_DTLTO_FD"])); '
                  's.send(os.fsencode(sys.argv[1])); sys.exit(0 if s.recv(1)==bytes([0]) else 1)')
        def rejected(request):
            description.write_text(json.dumps(request))
            with open(os.devnull, 'rb') as fd:
                result = subprocess.run([self.build/'distcc-thinlto', '--rust-link',
                    work, linked, temporary, 'clang', str(fd.fileno()), sys.executable, '-c', sender, description],
                    pass_fds=(fd.fileno(),), capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((final/'ok.o').exists())
            return result.stderr

        for invalid_arg in ('@more-options', 1):
            with self.subTest(arg=invalid_arg):
                self.assertIn('compiler arguments', rejected(dict(common=dict(common, args=['clang', '-c', invalid_arg]),
                                                                  jobs=[job(final/'ok.o')])))
        for invalid in (work/'bad.o', temporary/'..'/'bad.o', temporary/'alias'/'bad.o'):
            with self.subTest(path=invalid):
                self.assertRegex(rejected(dict(common=common, jobs=[job(final/'ok.o'), job(invalid)])),
                                 'outside link directories|invalid ThinLTO path|unexpected ThinLTO backend job|Not a directory')

    @unittest.skipUnless(shutil.which('cargo') and shutil.which('clang'), 'Cargo and Clang required')
    def test_cargo_link_driver_skips_masquerade_symlinks(self):
        import ctypes
        links = []
        for binary in (self.build / 'distcc', shutil.which('ccache')):
            if binary:
                directory = self.work / (Path(binary).name + '-bin')
                directory.mkdir()
                alias = directory / 'clang'
                alias.symlink_to(binary)
                links.append(alias)
        src = self.work / 'src'
        src.mkdir()
        (self.work/'Cargo.toml').write_text('[package]\nname="wrapped"\nversion="0.1.0"\nedition="2021"\n'
                                            '[lib]\ncrate-type=["cdylib"]\n')
        (self.work/'build.rs').write_text('fn main() { println!("cargo:rustc-env=ANSWER=42"); }')
        (src/'main.rs').write_text('fn main() { println!("{}", env!("ANSWER")); }')
        (src/'lib.rs').write_text('#[no_mangle] pub extern "C" fn answer() -> i32 { '
                                  'env!("ANSWER").parse().unwrap() }')
        path = os.pathsep.join([*(str(alias.parent) for alias in links), self.env['PATH']])
        for i, driver in enumerate(['clang', *links]):
            with self.subTest(driver=driver):
                out = self.work / f'cargo-{i}'
                env = dict(self.env, PATH=path, RUSTC=self.rustc, RUSTC_WRAPPER=str(self.build/'distcc'),
                           CARGO_INCREMENTAL='0', CARGO_TARGET_DIR=str(out), RUSTFLAGS=f'-C linker={driver}')
                result = subprocess.run(['cargo', 'build', '--offline', '-v'], env=env, cwd=self.work,
                                        capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertGreaterEqual(result.stderr.count('distcc-rust: compiled on '), 3, result.stderr)
                self.assertEqual(subprocess.check_output([out/'debug/wrapped'], text=True).strip(), '42')
                self.assertEqual(ctypes.CDLL(str(out/'debug/libwrapped.so')).answer(), 42)

    def test_large_rust_artifacts_do_not_use_small_runtime_tmpfs(self):
        runtime = self.work / 'runtime'
        storage = self.work / 'rust-storage'
        runtime.mkdir()
        storage.mkdir()
        launcher = self.work / 'bounded-worker'
        command = ['bwrap', '--die-with-parent', '--unshare-user', '--uid', str(os.getuid()),
                   '--gid', str(os.getgid()), '--bind', '/', '/', '--dev', '/dev',
                   '--size', str(1024 * 1024), '--tmpfs', str(runtime),
                   str(self.build / 'distccd')]
        launcher.write_text('#!/bin/sh\nexec ' + shlex.join(command) + ' "$@"\n')
        launcher.chmod(0o755)
        self.restart_server(launcher, TMPDIR=str(runtime), DISTCCD_RUST_TMPDIR=str(storage))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        source = self.work / 'large.rs'
        source.write_text('pub static DATA: [u8; 16777216] = [7; 16777216];')
        output = self.work / 'liblarge.rlib'
        result = self.rust('--crate-type=rlib', source, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertGreater(output.stat().st_size, 16 * 1024 * 1024)
        main = self.work / 'large-main.rs'
        main.write_text('fn main() { assert_eq!(large::DATA[16777215], 7); }')
        result = self.rust(main, '--extern', 'large=' + str(output),
                           '-o', self.work / 'large-check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'large-check'], check=True)
        deadline = time.monotonic() + 5
        while list(storage.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline, 'Rust job tree was not cleaned')
            time.sleep(.01)

    def test_library_inputs_env_and_dependency_artifacts(self):
        source = self.work / 'library space.rs'
        nested = self.work / 'module.rs'
        included = self.work / 'included.rs'
        payload = self.work / 'payload.bin'
        nested.write_text('pub fn value() -> u32 { 7 }')
        included.write_text('pub const INCLUDED: u32 = 11;')
        payload.write_bytes(b'abc')
        source.write_text('mod module;\ninclude!(' + repr(str(included)).replace("'", '"') + ');\n'
                          'pub fn value() -> u32 { assert!(file!().ends_with("library space.rs")); '
                          'assert_eq!(env!("CRATE_TEST_VALUE"), "value with space"); '
                          'assert!(option_env!("CRATE_TEST_ABSENT").is_none()); '
                          'module::value() + INCLUDED + include_bytes!("payload.bin").len() as u32 }')
        env = dict(self.env, CRATE_TEST_VALUE='value with space')
        env.pop('CRATE_TEST_ABSENT', None)
        output = self.work / 'libsample.rlib'
        result = self.rust('--crate-name', 'sample', '--crate-type=rlib', '--emit=link,dep-info',
                           source, '-o', output, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        deps = (self.work / 'libsample.d').read_text()
        self.assertIn('payload.bin', deps)
        self.assertNotIn('/out/', deps)
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(sample::value(), 21); }')
        result = self.rust(main, '--extern', 'sample=' + str(output), '-o', self.work / 'check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'check'], check=True)

    def test_binary_dependency_closure_excludes_unrelated_crates(self):
        deps = self.work / 'deps'
        deps.mkdir()
        for name, body, args in (
            ('leaf', 'pub fn n<T: From<u8>>() -> T { 42.into() }', []),
            ('middle', 'pub fn n() -> u32 { leaf::n() }',
             ['--extern', 'leaf=' + str(deps / 'libleaf.rlib')]),
        ):
            source = self.work / (name + '.rs')
            source.write_text(body)
            result = self.rust('--edition=2021', '--crate-type=rlib', source,
                               *args, '-o', deps / ('lib' + name + '.rlib'), remote=False)
            self.assertEqual(result.returncode, 0, result.stderr)
        unrelated = deps / 'libunused.rlib'
        with unrelated.open('wb') as stream:
            stream.truncate(128 * 1024 * 1024)
        unused_archive = deps / 'libunused.a'
        unused_archive.write_bytes(b'unrelated native archive in a Rust-only search path')
        native = self.work / 'native'
        native.mkdir()
        (native / 'answer.c').write_text('unsigned answer(void) { return 1; }')
        subprocess.run(['gcc', '-c', native / 'answer.c', '-o', native / 'answer.o'], check=True)
        subprocess.run(['ar', 'rcs', native / 'libanswer.a', native / 'answer.o'], check=True)
        source = self.work / 'sample.rs'
        source.write_text('#[link(name="answer", kind="static")]\n'
                          'extern "C" { fn answer() -> u32; }\n'
                          'pub fn n() -> u32 { middle::n() + unsafe { answer() } }')
        output = self.work / 'libsample.rlib'
        args = ['--edition=2021', '--crate-type=rlib', str(source),
                '--extern', 'middle=' + str(deps / 'libmiddle.rlib'),
                '-Ldependency=' + str(deps), '-Lnative=' + str(native), '-Clto', '-o', str(output)]
        trace = self.work/'closure.trace'
        result = subprocess.run(['strace', '-o', trace, '-e', 'openat', self.build/'distcc',
                                 self.rustc, *args], env=self.env, cwd=self.work, capture_output=True, text=True)
        opened = trace.read_text()
        for path in [deps/'libleaf.rlib', deps/'libmiddle.rlib', native/'libanswer.a']:
            self.assertIn(str(path), opened)
        for path in [unrelated, unused_archive]:
            self.assertNotIn(str(path), opened)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(sample::n(), 43); }')
        executable = self.work / 'linked'
        result = self.rust(main, '--extern', 'sample=' + str(output),
                           '-Ldependency=' + str(deps), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)

    def test_dependency_probe_does_not_enable_unstable_compilation(self):
        source = self.work / 'unstable.rs'
        source.write_text('#![feature(rustc_private)]\npub fn n() -> u32 { 42 }')
        output = self.work / 'libunstable.rlib'
        env = dict(self.env, RUSTC_BOOTSTRAP='-1')
        args = ('--crate-type=rlib', source, '-o', output)
        local = self.rust(*args, remote=False, env=env)
        remote = self.rust(*args, env=env)
        self.assertNotEqual(local.returncode, 0, local.stderr)
        self.assertEqual(remote.returncode, local.returncode, remote.stderr)
        self.assertIn('E0554', remote.stderr)
        self.assertFalse(output.exists())

    def test_compiler_error_does_not_replace_output(self):
        source, output = self.work / 'bad.rs', self.work / 'libbad.rlib'
        source.write_text('pub fn bad() -> u32 { "not a number" }')
        output.write_bytes(b'previous artifact')
        for fallback in ('0', '1'):
            with self.subTest(fallback=fallback):
                env = dict(self.env, DISTCC_FALLBACK=fallback, DISTCC_SKIP_LOCAL_RETRY='0')
                result = self.rust('--crate-type=rlib', source, '-o', output, env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('mismatched types', result.stderr)
                self.assertNotIn('retrying locally', result.stderr)
                self.assertEqual(output.read_bytes(), b'previous artifact')

    def test_json_artifacts_exist_when_cargo_is_notified(self):
        import concurrent.futures
        source = self.work / 'published.rs'
        source.write_text('pub static DATA: [u8; 16777216] = [1; 16777216];\n' +
                          ''.join(f'pub const bad_name_{i}: usize = {i};\n' for i in range(128)))
        artifacts = []
        diagnostics = []
        def consume(stream):
            for line in stream:
                diagnostics.append(line)
                if line.startswith('{'):
                    message = json.loads(line)
                    if message.get('$message_type') == 'artifact':
                        path = Path(message['artifact'])
                        artifacts.append((path.name, path.is_file()))
        with subprocess.Popen([str(self.build / 'distcc'), self.rustc, source,
                               '--crate-type=rlib', '--emit=metadata,link', '--out-dir', self.work,
                               '--error-format=json', '--json=artifacts'], env=self.env,
                              cwd=self.work, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              text=True, pipesize=4096) as child:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                reader = pool.submit(consume, child.stderr)
                try:
                    code = child.wait(timeout=90)
                finally:
                    if child.poll() is None:
                        child.kill()
                reader.result(timeout=5)
        self.assertEqual(code, 0, ''.join(diagnostics))
        self.assertIn('distcc-rust: compiled on', ''.join(diagnostics))
        self.assertEqual({name for name, _ in artifacts}, {'libpublished.rmeta', 'libpublished.rlib'})
        self.assertTrue(all(exists for _, exists in artifacts), artifacts)

    def test_cargo_worker_codegen_overlaps_dependent_metadata_input(self):
        upstream, downstream = self.work/'upstream', self.work/'downstream'
        (self.work/'Cargo.toml').write_text('[workspace]\nmembers=["upstream","downstream"]\nresolver="2"\n')
        for directory in (upstream,downstream):
            directory.mkdir()
            manifest = f'[package]\nname="{directory.name}"\nversion="0.0.1"\nedition="2021"\n[lib]\npath="lib.rs"\n'
            if directory == downstream:
                manifest += '[dependencies]\nupstream={path="../upstream"}\n'
            (directory/'Cargo.toml').write_text(manifest)
        (upstream/'lib.rs').write_text(''.join(
            f'#[inline(never)] pub fn f{i}(mut x:u64)->u64 {{ for n in 0..99 {{ x=x.rotate_left(n)^({i}+x.wrapping_mul(3)); }} x }}\n'
            for i in range(800)))
        (downstream/'lib.rs').write_text(''.join(
            f'pub fn result{i}(x:u64)->u64 {{ let mut y=upstream::f{i}(x); for j in 1..99 '
            f'{{ y=(y^x).rotate_left(j).wrapping_add({i}); }} y }}\n' for i in range(650)))
        env = dict(self.env, RUSTC=self.rustc, RUSTC_WRAPPER=str(self.build/'distcc'),
                   CARGO_INCREMENTAL='0', RUSTC_BOOTSTRAP='1',
                   RUSTFLAGS='-Zllvm-time-trace -Copt-level=2 -Ccodegen-units=1')
        cargo = subprocess.run(['cargo','build','-vv','--offline','-j2','-p','downstream'],
                               cwd=self.work,env=env,capture_output=True,text=True,timeout=120)
        self.assertEqual(cargo.returncode,0,cargo.stderr)
        self.assertGreaterEqual(cargo.stderr.count('distcc-rust: compiled on'),2,cargo.stderr)
        self.assertRegex(cargo.stderr,r'--extern upstream=[^\s]+\.rmeta')
        def intervals(crate):
            trace,=(self.work/'target/debug/deps').glob(crate+'*.llvm_timings.json')
            profile=json.loads(trace.read_bytes())
            epoch=profile['beginningOfTime']
            spans=sorted((epoch+e['ts'],epoch+e['ts']+e['dur']) for e in profile['traceEvents']
                         if e['ph']=='X' and not e['name'].startswith('Total ') and
                         (e['name']=='OptModule' or e['name'].endswith('Pass')))
            merged=[]
            for start,end in spans:
                if merged and start<=merged[-1][1]:
                    merged[-1][1]=max(merged[-1][1],end)
                else:
                    merged.append([start,end])
            return merged
        up,down=intervals('upstream'),intervals('downstream')
        i=j=overlap=0
        while i<len(up) and j<len(down):
            a,b=up[i]
            c,d=down[j]
            overlap+=max(0,min(b,d)-max(a,c))
            if b<=d: i+=1
            else: j+=1
        self.assertGreater(overlap,0)
        print(f'Cargo dependent used upstream .rmeta with {overlap} us of overlapping LLVM passes')

    def test_disconnected_transport_retries_without_local_fallback(self):
        import concurrent.futures
        import select
        import threading
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind((self.worker_address, 0))
        listener.listen()
        listener.settimeout(10)
        stopped = threading.Event()
        def proxy():
            # Drop an actual BEGIN after the broker has leased the connection.
            first, _ = listener.accept()
            with first:
                first.settimeout(10)
                self.assertEqual(first.recv(4, socket.MSG_WAITALL), struct.pack('!I', 0x54000400))
            second, _ = listener.accept()
            with second, socket.create_connection((self.worker_address, self.ports[0]), timeout=10) as worker:
                peers = {second: worker, worker: second}
                while not stopped.is_set():
                    for source in select.select(list(peers), [], [], .1)[0]:
                        data = source.recv(256 * 1024)
                        if not data:
                            return
                        peers[source].sendall(data)
        self.start_broker(f'{self.worker_address}:{listener.getsockname()[1]}/1,tcp,stats={self.stats_ports[0]}',
                          name='retry', backoff=0)
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        source = self.work / 'retry.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(proxy)
            try:
                result = self.rust('--crate-type=rlib', '-Ctarget-cpu=native', source)
            finally:
                stopped.set()
            future.result(timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('retrying file job on a fresh connection', result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertTrue((self.work / 'libretry.rlib').is_file())

    def test_native_cpu_features_and_overrides_match_local_compiler(self):
        source = self.work / 'native.rs'
        source.write_text('#[no_mangle] pub extern "C" fn answer() -> u32 { 42 }')
        settings: list[tuple[str, ...]] = [('-Ctarget-cpu=native',)]
        cfg = subprocess.check_output([self.rustc, '-Ctarget-cpu=native', '--print=cfg'], text=True)
        features = re.findall(r'target_feature="([^"]+)"', cfg)
        source.write_text(source.read_text() + '\npub fn features() -> &\'static [&\'static str] { &[\n' +
                          ''.join(f'#[cfg(target_feature="{feature}")] "{feature}",\n' for feature in features) + '] }')
        if 'target_feature="avx2"' in cfg:
            settings.append(('--codegen=target-cpu=native', '-Ctarget-feature=-avx2'))
        for flags in settings:
            with self.subTest(flags=flags):
                artifacts = {}
                for remote in (False, True):
                    ir = self.work / f'native-{remote}.ll'
                    library = self.work / f'libnative-{remote}.rlib'
                    result = self.rust('--crate-name=native', '--crate-type=rlib', '-Dwarnings',
                                       *flags, '--emit=link=' + str(library) + ',llvm-ir=' + str(ir), source,
                                       remote=remote)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn('warning:', result.stderr)
                    if remote:
                        self.assertIn('distcc-rust: compiled on', result.stderr)
                    attributes = re.findall(r'"target-(?:cpu|features)"="[^"]*"', ir.read_text())
                    self.assertGreaterEqual(len(attributes), 2)
                    artifacts[remote] = attributes
                self.assertEqual(artifacts[False], artifacts[True])
                main = self.work / 'main.rs'
                expected_cfg = subprocess.check_output([self.rustc, *flags, '--print=cfg'], text=True)
                expected = [f for f in features if f'target_feature="{f}"' in expected_cfg]
                main.write_text('fn main() { assert_eq!(native::answer(), 42); '
                                'assert_eq!(native::features(), &' + json.dumps(expected) + '); }')
                result = self.rust(main, '--extern', 'native=' + str(library),
                                   '-o', self.work / 'check-native', remote=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([self.work / 'check-native'], check=True)

    def test_native_cache_reused_and_last_cpu_option_wins(self):
        source = self.work / 'native_cache.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        args = ('--crate-type=rlib', '-Ctarget-cpu=native', source)
        result = self.rust(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        for overrides in ([], ['-Ctarget-cpu=generic']):
            trace = self.work/'native.trace'
            result = subprocess.run(['strace', '-f', '-e', 'execve', '-o', trace, self.build/'distcc',
                                     self.rustc, *args, *overrides], env=self.env, cwd=self.work,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-rust: compiled on', result.stderr)
            self.assertNotIn('"--print=cfg"', trace.read_text())

    def test_native_discovery_serializes_and_refreshes_after_compiler_change(self):
        import concurrent.futures
        original = self.rustc
        compiler, calls = self.work / 'rustc', self.work / 'probes'
        preload = self.work / 'preload'
        compiler.write_text('#!/bin/sh\nif [ "$2" = "--print=cfg" ]; then echo probe >> ' +
                            shlex.quote(str(calls)) + '; printf "%s\\n" "$LD_PRELOAD" > ' +
                            shlex.quote(str(preload)) + '; fi\nexec ' + shlex.quote(original) + ' "$@"\n')
        compiler.chmod(0o755)
        self.rustc = str(compiler)
        self.env['LD_PRELOAD'] = 'libm.so.6'
        self.addCleanup(setattr, self, 'rustc', original)
        source = self.work / 'profile.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        def build(index):
            result = self.rust('--crate-type=rlib', '-Ctarget-cpu=native', source,
                               '-o', self.work / f'libprofile{index}.rlib')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-rust: compiled on', result.stderr)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(build, range(3)))
        self.assertEqual(calls.read_text().splitlines(), ['probe'])
        self.assertIn('libm.so.6', preload.read_text().split())
        compiler.write_text(compiler.read_text() + '# compiler updated\n')
        build(3)
        self.assertEqual(calls.read_text().splitlines(), ['probe', 'probe'])

    def test_client_working_directory_is_filesystem_root(self):
        source = self.work / 'root_cwd.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        result = subprocess.run([str(self.build / 'distcc'), self.rustc, '--crate-type=rlib',
                                 str(source), '-o', str(self.work / 'libroot.rlib')],
                                cwd='/', env=self.env, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)

    def test_long_lint_and_codegen_options_are_decided_by_rustc(self):
        source, library = self.work / 'options.rs', self.work / 'liboptions.rlib'
        source.write_text('pub fn answer() -> u32 { 42 }')
        result = self.rust('--crate-name=options', '--crate-type=rlib',
                           '--warn=unused_variables', '--allow', 'clippy::all',
                           '--deny=unused_imports', '--forbid', 'unsafe_code',
                           '--force-warn=dead_code', '-C', 'default-linker-libraries=yes',
                           source, '-o', library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        main, executable = self.work / 'main.rs', self.work / 'options'
        main.write_text('fn main() { assert_eq!(options::answer(), 42); }')
        result = self.rust(main, '--extern', 'options=' + str(library), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)

    def test_profile_use_is_transferred_and_linkable(self):
        config = os.environ.get('LLVM_CONFIG')
        bindir = subprocess.check_output([config, '--bindir'], text=True).strip() if config else None
        profdata = shutil.which('llvm-profdata', path=bindir)
        if not profdata:
            self.skipTest('llvm-profdata required')
        source = self.work / 'pgo.rs'
        source.write_text('pub fn answer() -> u32 { 42 }\nfn main() { assert_eq!(answer(), 42); }')
        result = self.rust(source, '-Cprofile-generate', '-o', self.work / 'training', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'training'], check=True,
                       env=dict(self.env, LLVM_PROFILE_FILE=str(self.work / 'training.profraw')))
        subprocess.run([profdata, 'merge', str(self.work / 'training.profraw'),
                        '-o', str(self.work / 'training.profdata')], check=True, capture_output=True)
        library = self.work / 'libpgo.rlib'
        result = self.rust('--crate-type=rlib', source, '-Cprofile-use=training.profdata', '-o', library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(pgo::answer(), 42); }')
        result = self.rust(main, '--extern', 'pgo=' + str(library), '-o', self.work / 'check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'check'], check=True)

    def test_new_options_outputs_and_unrelated_proc_macro_allow_distribution(self):
        macro = self.work / 'macro.rs'
        macro.write_text('extern crate proc_macro;')
        deps = self.work / 'deps'
        deps.mkdir()
        result = self.rust('--crate-type=proc-macro', macro, '-o', deps / 'libunused.so', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        source, library = self.work / 'source.input', self.work / 'liboptions.rlib'
        source.write_text('pub fn answer() -> u32 { 42 }')
        mir = self.work / 'options.mir'
        result = self.rust('--crate-name=options', '--crate-type=rlib', '-Z', 'threads=2',
                           '--codegen=default-linker-libraries=yes', '-L', 'dependency=' + str(deps),
                           '--emit=link,mir=' + str(mir), source, '-o', library,
                           env=dict(self.env, RUSTC_BOOTSTRAP='1'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertIn('fn answer', mir.read_text())
        main, executable = self.work / 'main.rs', self.work / 'check-options'
        main.write_text('fn main() { assert_eq!(options::answer(), 42); }')
        result = self.rust(main, '--extern', 'options=' + str(library), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)

    def test_transitive_macro_initializers_execute_on_client(self):
        deps = self.work / 'deps'
        deps.mkdir()
        macro, facade = self.work / 'derive.rs', self.work / 'facade.rs'
        marker = self.work / 'initialized'
        macro.write_text('extern crate proc_macro;\n'
                         '#[proc_macro_derive(Marker)]\n'
                         'pub fn marker(_: proc_macro::TokenStream) -> proc_macro::TokenStream { "".parse().unwrap() }\n'
                         'extern "C" fn initialize() { std::fs::write(' + json.dumps(str(marker)) + ', "client").unwrap(); }\n'
                         '#[used] #[unsafe(link_section=".init_array")] static INIT: extern "C" fn() = initialize;')
        shared = deps / 'libderive.so'
        result = self.rust('--crate-type=proc-macro', macro, '-o', shared, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        facade.write_text('extern crate derive; pub use derive::Marker; pub struct Value(pub u32);')
        rlib = deps / 'libfacade.rlib'
        result = self.rust('--crate-type=rlib', facade, '--extern', 'derive=' + str(shared),
                           '-o', rlib, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(), 'client')
        marker.unlink()
        source, library = self.work / 'consumer.rs', self.work / 'libconsumer.rlib'
        source.write_text('extern crate facade; pub fn answer() -> u32 { facade::Value(42).0 }')
        result = self.rust('--crate-type=rlib', source, '--extern', 'facade=' + str(rlib),
                           '-L', 'dependency=' + str(deps), '-o', library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('isolated dependency expansion failed', result.stderr)
        self.assertEqual(marker.read_text(), 'client')
        main, executable = self.work / 'main.rs', self.work / 'transitive'
        main.write_text('fn main() { assert_eq!(consumer::answer(), 42); }')
        result = self.rust(main, '--extern', 'consumer=' + str(library),
                           '-L', 'dependency=' + str(deps), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)

    def test_stdin_is_not_consumed_by_dependency_discovery(self):
        output = self.work / 'libstdin.rlib'
        result = subprocess.run([str(self.build / 'distcc'), self.rustc, '--crate-type=rlib',
                                 '--crate-name=stdin', '-', '-o', str(output)],
                                input='pub fn answer() -> u32 { 42 }', env=self.env,
                                cwd=self.work, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('standard input requires local compilation', result.stderr)
        self.assertTrue(output.is_file())

    def test_rust_dependency_filename_escaping(self):
        names = ['a space', 'a$$b', 'a#b', "a'b", 'a\\b', 'a: b', 'a\\ b']
        for name in names:
            (self.work / name).write_text('content')
        source = self.work / 'escaped.rs'
        source.write_text('pub fn length() -> usize { ' + ' + '.join(
            'include_str!(' + json.dumps(str(self.work / name)) + ').len()' for name in names) + ' }')
        result = self.rust('--crate-type=rlib', source, '-o', self.work / 'libescaped.rlib')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)

    def test_cargo_dependencies_build_script_and_local_link(self):
        if not shutil.which('cargo'):
            self.skipTest('cargo required')
        lib = self.work / 'library'
        lib.mkdir()
        (lib / 'Cargo.toml').write_text('[package]\nname="helper"\nversion="0.1.0"\nedition="2024"\n'
                                      '[lib]\npath="lib.rs"\n')
        (lib / 'build.rs').write_text('fn main() { std::fs::write(std::path::Path::new('
            '&std::env::var("OUT_DIR").unwrap()).join("value.rs"), "pub fn value() -> u32 { 42 }").unwrap(); }')
        (lib / 'lib.rs').write_text('include!(concat!(env!("OUT_DIR"), "/value.rs"));')
        (self.work / 'Cargo.toml').write_text('[package]\nname="app"\nversion="0.1.0"\nedition="2024"\n'
            '[[bin]]\nname="app"\npath="main.rs"\n[dependencies]\nhelper={path="library"}\n')
        (self.work / 'main.rs').write_text('fn main() { assert_eq!(helper::value(), 42); println!("rust-distcc-ok"); }')
        env = dict(self.env, RUSTC_WRAPPER=str(self.build / 'distcc'), RUSTC=self.rustc,
                   RUSTFLAGS='-Ctarget-cpu=native')
        result = subprocess.run(['cargo', 'run', '--offline'], cwd=self.work, env=env,
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('rust-distcc-ok', result.stdout)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertNotIn('failed to connect to jobserver', result.stderr)

    def test_repeated_crate_types_return_native_archive(self):
        source = self.work / 'combined.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        for types in (['--crate-type=staticlib', '--crate-type=rlib'],
                      ['--crate-type', 'rlib', '--crate-type', 'staticlib'],
                      ['--crate-type=staticlib,rlib']):
            with self.subTest(types=types):
                result = self.rust(*types, '--emit=link', '--emit=dep-info',
                                   source, '--out-dir', self.work)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust: compiled on', result.stderr)
                for name in ('libcombined.a', 'libcombined.rlib', 'combined.d'):
                    output = self.work / name
                    self.assertGreater(output.stat().st_size, 0)
                    output.unlink()

    def test_worker_validates_proc_macro_crate_types_and_host(self):
        src = self.work/'plugin.rs'
        src.write_text('pub fn answer() {}')
        for flags, types in ((['--crate-type=proc-macro', '--crate-type=rlib'], 'rlib'), ([], 'proc-macro')):
            with self.subTest(flags=flags, types=types):
                body, _ = self.wire_plan(src, flags=flags, crate_types=types)
                with socket.create_connection((self.worker_address, self.ports[0])) as net:
                    self.send(net, 0x54000400, struct.pack('!Q', 1) + body)
                    self.assertEqual(net.recv(1), b'')

    def test_repeated_emits_preserve_remote_outputs(self):
        source = self.work / 'emissions.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
        library, deps = self.work / 'libemissions.rlib', self.work / 'emissions.d'
        for emits in (['--emit=link', '--emit=dep-info=' + str(deps)],
                      ['--emit=dep-info=' + str(deps), '--emit=link']):
            with self.subTest(emits=emits):
                args = ['--crate-type=rlib', *emits, str(source), '-o', str(library)]
                result = self.rust(*args, remote=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = [path.read_bytes() for path in (library, deps)]
                library.unlink()
                deps.unlink()
                result = self.rust(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust: compiled on', result.stderr)
                self.assertEqual([path.read_bytes() for path in (library, deps)], expected)

    def build_pure_macro(self):
        source = self.work / 'macros.rs'
        source.write_text('extern crate proc_macro; use proc_macro::TokenStream; '
                          '#[proc_macro] pub fn answer(_: TokenStream) -> TokenStream { "42".parse().unwrap() }')
        library = self.work / 'libmacros.so'
        result = self.rust('--crate-type=proc-macro', source, '-o', library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('proc-macro host ABI', result.stderr)
        self.assertIn('compiled on', result.stderr)
        return library

    def test_pure_macro_producer_and_consumer_run_on_matching_worker(self):
        library = self.build_pure_macro()
        source = self.work / 'consumer.rs'
        source.write_text('extern crate macros; pub fn value() -> u32 { macros::answer!() }')
        result = self.rust('--crate-type=rlib', source, '--extern', 'macros=' + str(library),
                           '-o', self.work / 'libconsumer.rlib')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('proc-macro host ABI', result.stderr)
        self.assertIn('compiled on', result.stderr)

    def test_tracked_worker_owned_environment_uses_client(self):
        source = self.work/'owned.rs'
        source.write_text('fn main() { assert_eq!(env!("DISTCC_TEST_SETTING"), "client"); }')
        output = self.work/'owned'
        result = self.rust(source, '-o', output, env=dict(self.env, DISTCC_TEST_SETTING='client'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('crate depends on worker-owned environment: DISTCC_TEST_SETTING', result.stderr)
        self.assertNotIn('distcc-rust: compiled on', result.stderr)
        subprocess.run([output], check=True)

    def test_proc_macro_environment_on_whole_crate_worker(self):
        source = self.work/'context.rs'
        library = self.work/'libcontext.so'
        source.write_text('''extern crate proc_macro;
use std::{env, ffi::{CStr, c_char}};
extern "C" { fn secure_getenv(name: *const c_char) -> *const c_char; }
#[proc_macro]
pub fn context(_: proc_macro::TokenStream) -> proc_macro::TokenStream {
    assert_eq!(env::var("CARGO_CRATE_NAME").unwrap(), "consumer");
    assert_eq!(env::var("MACRO_OPTION").unwrap(), "value=42");
    assert_eq!(env::var_os("MACRO_EMPTY").unwrap(), "");
    assert_eq!(env::var("MACRO_MISSING"), Err(env::VarError::NotPresent));
    let ptr = unsafe { secure_getenv(c"MACRO_SECURE".as_ptr()) };
    assert!(!ptr.is_null());
    assert_eq!(unsafe { CStr::from_ptr(ptr) }.to_bytes(), b"secure");
    let all: std::collections::HashMap<_, _> = env::vars_os().collect();
    let key = std::ffi::OsStr::new;
    assert_eq!(all.get(key("CARGO_FEATURE_JOURNAL")).unwrap(), "1");
    assert_eq!(all.get(key("OUT_DIR")).unwrap(), "crate-output");
    let makeflags = env::var_os("CARGO_MAKEFLAGS");
    let libs = env::var("LD_LIBRARY_PATH").unwrap_or_default();
    let tmp = env::var("TMPDIR").unwrap_or_default();
    let clean = !all.contains_key(key("MACRO_UNREAD")) && makeflags.is_none()
        && !libs.contains("client-only") && !tmp.contains("client-only");
    clean.to_string().parse().unwrap()
}''')
        self.rust('--edition=2021', '--crate-type=proc-macro', source, '-o', library,
                  remote=False).check_returncode()
        consumer = self.work/'consumer.rs'
        consumer.write_text('extern crate context; fn main() { assert!(context::context!()); }')
        tmp = self.work/'client-only'
        tmp.mkdir()
        env = dict(self.env, CARGO_CRATE_NAME='consumer', MACRO_OPTION='value=42', MACRO_EMPTY='',
                   MACRO_SECURE='secure', CARGO_FEATURE_JOURNAL='1', OUT_DIR='crate-output',
                   MACRO_UNREAD='private', CARGO_MAKEFLAGS='-j2', LD_LIBRARY_PATH=str(tmp), TMPDIR=str(tmp))
        env.pop('MACRO_MISSING', None)
        output = self.work/'consumer'
        result = self.rust('-Ccodegen-units=1', consumer, '--extern', 'context='+str(library),
                           '-o', output, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        subprocess.run([output], check=True)

    @unittest.skipUnless(shutil.which('sandbox'), 'Portage sandbox required')
    def test_pure_macro_with_portage_and_jobserver_pipe(self):
        library = self.build_pure_macro()
        source = self.work / 'sandbox_consumer.rs'
        source.write_text('extern crate macros; fn main() { assert_eq!(macros::answer!(), 42); }')
        output = self.work / 'consumer'
        env = dict(self.env, SANDBOX_LOG=str(self.work / 'sandbox.log'),
                   SANDBOX_DEBUG_LOG=str(self.work / 'sandbox.debug'))
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, 'rb') as tokens, os.fdopen(write_fd, 'wb', buffering=0) as writer:
            writer.write(b'+')
            env['CARGO_MAKEFLAGS'] = f'-j --jobserver-auth=3,{write_fd}'
            result = subprocess.run(['sandbox', 'sh', '-ec',
                'test "$SANDBOX_ON" = 1; exec 3<&0; test -p /proc/self/fd/3; exec "$@"', 'sh',
                'env', 'SANDBOX_WRITE=' + str(self.work), 'TMPDIR=' + str(self.work),
                self.build / 'distcc', self.rustc, source, '--extern', 'macros=' + str(library),
                '-o', output], cwd=self.work, env=env, stdin=tokens, pass_fds=(write_fd,),
                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('proc-macro host ABI', result.stderr)
        self.assertIn('compiled on', result.stderr)
        subprocess.run([output], check=True)

    def test_broker_and_worker_refuse_mismatching_macro_host(self):
        source=self.work/'host.rs'
        source.write_text('pub fn value() {}')
        self.assertEqual(self.rust('--crate-type=rlib',source).returncode,0)
        foreign='incompatible-test-host'
        request=self.request(10,work=(1,1024,7),work_units=1,rust_compiler=1)
        status,_,_=self.receive(request)
        self.assertEqual(status,121)
        status,err,outputs=self.raw_compile(source,host=foreign)
        self.assertEqual(status,121,err)
        self.assertFalse(outputs)

    def test_cargo_upstream_span_source_text(self):
        home = self.work / 'home'
        home.mkdir()
        (self.work/'storage').mkdir()
        (home/'.cargo').symlink_to(self.work/'storage', target_is_directory=True)
        upstream = home/'.cargo/registry/src/fixture/source-1.0.0'
        macro = self.work/'text'
        app = self.work/'consumer'
        for path, name, manifest in ((upstream, 'source', ''), (macro, 'text', '[lib]\nproc-macro=true'),
                                     (app, 'consumer', f'[dependencies]\nsource={{path="{upstream}"}}\n'
                                                       f'text={{path="{macro}"}}')):
            (path/'src').mkdir(parents=True)
            (path/'Cargo.toml').write_text(f'[package]\nname="{name}"\nversion="1.0.0"\nedition="2021"\n' + manifest)
        (upstream/'src/lib.rs').write_text('#[macro_export] macro_rules! number { () => { text::value!(0x2a) } }')
        (macro/'src/lib.rs').write_text('extern crate proc_macro; #[proc_macro] '
            'pub fn value(input:proc_macro::TokenStream)->proc_macro::TokenStream {'
            'let s=input.into_iter().next().unwrap().span().source_text().unwrap();'
            'assert_eq!(s,"0x2a"); s.parse().unwrap()}')
        (app/'src/main.rs').write_text('fn main(){assert_eq!(source::number!(),42);}')
        env = dict(self.env, HOME=str(home), RUSTC=self.rustc, RUSTC_WRAPPER=str(self.build/'distcc'),
                   CARGO_INCREMENTAL='0')
        for key in ('CARGO_HOME', 'WORKDIR', 'DISTCC_FRONTEND_ROOTS'):
            env.pop(key, None)
        for action in ('check', 'build'):
            result = subprocess.run(['cargo', action, '--offline', '-vv', '-j1'], cwd=app, env=env,
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            consumer = next(job for job in result.stderr.split('     Running `') if ' --crate-name consumer ' in job)
            if action == 'check':
                self.assertRegex(consumer, r'--extern source=[^ ]+\.rmeta')
            self.assertIn('distcc-rust: compiled on', consumer)
        subprocess.run([app/'target/debug/consumer'], check=True)

    def test_macro_private_shared_library_and_observed_data(self):
        library=self.work/'libhelper.so'
        c=self.work/'helper.c'
        c.write_text('int add(void) {return 2;}')
        subprocess.run(['cc','-shared','-fPIC',c,'-o',library],check=True)
        home = self.work / 'cargo-home'
        home.symlink_to(self.work / 'cargo-storage', target_is_directory=True)
        (self.work / 'cargo-storage').mkdir()
        upstream = home / 'registry/src/example/upstream-1.0.0'
        upstream.mkdir(parents=True)
        data = upstream / 'number.txt'
        data.write_text('40')
        (upstream / 'marker').touch()
        source=self.work/'derive.rs'
        source.write_text('extern crate proc_macro; #[link(name="helper")] extern "C" { fn add()->i32; } '
            '#[proc_macro] pub fn value(_:proc_macro::TokenStream)->proc_macro::TokenStream { '
            'assert_eq!(std::fs::read("/rustc/distcc-test/library/core/src/span.rs").unwrap_err().kind(), '
            'std::io::ErrorKind::NotFound); '
            'assert_eq!(std::fs::read('+json.dumps(str(upstream/'marker'/'missing'))+
            ').unwrap_err().kind(), std::io::ErrorKind::NotADirectory); '
            'let n:i32=std::fs::read_to_string('+json.dumps(str(data))+').unwrap().parse().unwrap(); '
            '(n+unsafe{add()}).to_string().parse().unwrap() }')
        plugin=self.work/'libderive.so'
        result=self.rust(source,'--crate-type=proc-macro','-Lnative='+str(self.work),
                         '-Clink-arg=-Wl,-rpath,$ORIGIN','-o',plugin,remote=False)
        self.assertEqual(result.returncode,0,result.stderr)
        consumer = self.work / 'consumer'
        consumer.mkdir()
        src=consumer/'app.rs'
        src.write_text('extern crate derive; fn main(){assert_eq!(derive::value!(),42)}')
        output=self.work/'app'
        plugins = consumer / 'plugins'
        plugins.mkdir()
        shutil.copy2(plugin, plugins)
        shutil.copy2(library, plugins)
        for build_env in [dict(CARGO_HOME=str(home)), dict(WORKDIR=str(upstream))]:
            with self.subTest(build_env=build_env):
                env = dict(self.env, **build_env)
                env.pop('DISTCC_FRONTEND_ROOTS', None)
                result = self.rust(src, '--extern', 'derive='+str(plugins/plugin.name), '-o', output, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('proc-macro host ABI', result.stderr)
                self.assertIn('distcc-rust: compiled on', result.stderr)
                subprocess.run([output], check=True)

    def test_macro_network_attempt_executes_once_on_client(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0))
            listener.listen()
            source=self.work/'network.rs'
            source.write_text('extern crate proc_macro; #[proc_macro] pub fn number(_:proc_macro::TokenStream)'
                '->proc_macro::TokenStream {let _=std::net::TcpStream::connect(("127.0.0.1",'
                +str(listener.getsockname()[1])+')); "42".parse().unwrap()}')
            plugin=self.work/'libnetwork.so'
            self.assertEqual(self.rust(source,'--crate-type=proc-macro','-o',plugin,remote=False).returncode,0)
            src=self.work/'user.rs'
            src.write_text('extern crate network; pub const N:u32=network::number!();')
            result=self.rust(src,'--crate-type=rlib','--extern','network='+str(plugin))
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('distcc-rust: local:',result.stderr)
            listener.settimeout(2)
            listener.accept()[0].close()
            listener.settimeout(.05)
            with self.assertRaises(TimeoutError):
                listener.accept()

    def test_pure_transitive_macro_and_symlink_execute_on_worker(self):
        macro = self.work / 'macros.rs'
        macro.write_text('extern crate proc_macro; use proc_macro::TokenStream; '
                         '#[proc_macro] pub fn answer(_: TokenStream) -> TokenStream { "42".parse().unwrap() }')
        library = self.work / 'libmacros.so'
        result = self.rust('--crate-type=proc-macro', '-Ctarget-cpu=native', macro, '-o', library, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        bridge = self.work / 'bridge.rs'
        bridge.write_text('extern crate macros; pub use macros::answer;')
        result = self.rust('--crate-type=rlib', bridge, '--extern', 'macros=' + str(library),
                           '-o', self.work / 'libbridge.rlib', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        source = self.work / 'consumer.rs'
        source.write_text('extern crate bridge; pub fn value() -> u32 { bridge::answer!() }')
        output = self.work / 'libconsumer.rlib'
        search = ('-L', 'dependency=' + str(self.work))
        store = self.work / 'store'
        store.mkdir()
        library.rename(store / library.name)
        library.symlink_to(store / library.name)
        result = self.rust('--crate-type=rlib', '-Ctarget-cpu=native', source,
                           '--extern', 'bridge=' + str(self.work / 'libbridge.rlib'), *search, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('proc-macro host ABI', result.stderr)
        self.assertIn('compiled on', result.stderr)
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(consumer::value(), 42); }')
        result = self.rust(main, '--extern', 'consumer=' + str(output), *search,
                           '-o', self.work / 'check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'check'], check=True)
        source.write_text('pub fn value() -> u32 { 42 }')
        result = self.rust('--crate-type=rlib', source, *search, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)

    @unittest.skipUnless(shutil.which('sandbox'), 'Portage sandbox required')
    def test_portage_cargo_native_probe_in_build_tree(self):
        source = self.work / 'caller.rs'
        source.write_text('pub fn answer() -> u32 { '
                          'include!(concat!("answer-", env!("CRATE_BUILD_CONTEXT"), ".rs")) }')
        (self.work / 'answer-1.rs').write_text('42')
        log = self.work / 'sandbox.log'
        env = dict(self.env, SANDBOX_LOG=str(log), SANDBOX_DEBUG_LOG=str(self.work/'sandbox.debug'))
        # sandbox adds the machine defaults at startup. Install the package's
        # write list for the build command, as Portage does in the ebuild phase.
        check_sandbox = ('test "$SANDBOX_ON" = 1\n'
                         'case "$LD_PRELOAD" in *libsandbox.so*) ;; *) exit 1;; esac\n'
                         'export CRATE_BUILD_CONTEXT="$SANDBOX_ON"\n'
                         'exec "$@"')
        build = ['sandbox', 'sh', '-ec', check_sandbox, 'sh', 'env', '-u', 'TMPDIR', '-u', 'T',
                 'SANDBOX_WRITE=' + str(self.work), self.build/'distcc']
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(caller::answer(), 42); }')
        # Portage can select different tracing methods for source-built and
        # binary rustc. Both must load the native CPU and dependency hooks.
        compilers = {Path(self.rustc).resolve(), *Path('/opt').glob('rust-bin-*/bin/rustc')}
        for index, compiler in enumerate(sorted(compilers)):
            with self.subTest(compiler=compiler):
                out = self.work / f'deps-{index}'
                out.mkdir()
                result = subprocess.run([*build, compiler, '--crate-type=rlib', '--out-dir', out,
                                         '-L', 'dependency=' + str(out), '-Ctarget-cpu=native', source],
                                        cwd=self.work, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust: compiled on ', result.stderr)
                self.assertNotIn('distcc-rust: local:', result.stderr)
                self.assertFalse(log.exists(), result.stderr)
                executable = out / 'check'
                subprocess.run([compiler, main, '--extern', 'caller=' + str(out/'libcaller.rlib'),
                                '-o', executable], cwd=self.work, check=True, timeout=60)
                subprocess.run([executable], check=True, timeout=5)

    @unittest.skipUnless(shutil.which('sandbox'), 'Portage sandbox required')
    def test_dependency_probe_with_portage_sandbox(self):
        import signal
        source, library = self.work / 'sandbox.rs', self.work / 'libsandbox_test.rlib'
        source.write_text('pub fn answer() -> u32 { 42 }')
        env = dict(self.env, SANDBOX_WRITE=str(self.root),
                   SANDBOX_LOG=str(self.work / 'sandbox.log'),
                   SANDBOX_DEBUG_LOG=str(self.work / 'sandbox.debug'))
        process = subprocess.Popen(['sandbox', str(self.build / 'distcc'), self.rustc,
                                    '--crate-name=sandbox_test', '--crate-type=rlib',
                                    '-Ctarget-cpu=native', str(source), '-o', str(library)],
                                   env=env, cwd=self.work, start_new_session=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            stdout, stderr = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, stdout + stderr)
            self.assertIn('distcc-rust: compiled on', stderr)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        main = self.work / 'sandbox-main.rs'
        main.write_text('fn main() { assert_eq!(sandbox_test::answer(), 42); }')
        result = self.rust(main, '--extern', 'sandbox_test=' + str(library),
                           '-o', self.work / 'sandbox-check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'sandbox-check'], check=True)

    def test_independent_clients_share_cold_inputs(self):
        import concurrent.futures
        import threading
        source = self.work / 'cold.rs'
        source.write_text('pub fn size() -> usize { include_bytes!("cold.bin").len() }')
        (self.work / 'cold.bin').write_bytes(b'cold shared input\n' * (256 * 1024))
        path = self.env['PATH']
        clients = []
        for index in range(5):
            self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name=f'cold-{index}')
            state = self.work / f'client-{index}'
            state.mkdir()
            clients.append(dict(self.env, PATH=path, DISTCC_DIR=str(state), DISTCC_VERBOSE='1'))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        for attempt in range(3):
            self.restart_server(self.build / 'distccd', DISTCC_MEMORY_LEARNING='1',
                                DISTCCD_FILE_CACHE=str(self.work / f'cold-cache-{attempt}'))
            start = threading.Barrier(len(clients))
            def build(index):
                start.wait(timeout=5)
                return self.rust('--crate-name=cold', '--crate-type=rlib', '-Ctarget-cpu=native', source,
                                 '-o', self.work / f'libcold-{attempt}-{index}.rlib', env=clients[index])
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(clients)) as pool:
                results = list(pool.map(build, range(len(clients))))
            for result in results:
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust: compiled on', result.stderr)

    def test_cached_inputs_and_parallel_crates(self):
        import concurrent.futures
        # Earlier cases share the fixture daemon's cache. The cold/warm
        # comparison requires an empty cache owned by this test.
        self.restart_server(self.build / 'distccd', DISTCC_MEMORY_LEARNING='1',
                            DISTCCD_FILE_CACHE=str(self.work / 'cache'))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        source = self.work / 'cache.rs'
        source.write_text('pub fn answer() -> usize { include_bytes!("data.bin").len() }\n' +
                          '\n'.join(f'pub fn value_{i}(x: usize) -> usize {{ x.wrapping_add({i}) }}'
                                    for i in range(1000)))
        (self.work / 'data.bin').write_bytes(b'abcde' * (1024 * 1024))
        def build(index):
            return self.rust('--crate-name=cache', '--crate-type=rlib', source,
                             '-o', self.work / ('libcache' + str(index) + '.rlib'))
        first = build(0)
        self.assertEqual(first.returncode, 0, first.stderr)
        counts = re.search(r'input cache: (\d+) of', first.stderr)
        assert counts is not None, first.stderr
        cold = int(counts[1])
        peak_busy = 0
        peak_queued = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            futures = [pool.submit(build, i) for i in range(1, 6)]
            while not all(future.done() for future in futures):
                stats = self.get_stats(self.stats_ports[0])
                busy = re.search(r'dcc_busy (\d+)', stats)
                assert busy is not None, stats
                peak_busy = max(peak_busy, int(busy[1]))
                queued = re.search(r'dcc_queued (\d+)', stats)
                assert queued is not None, stats
                peak_queued = max(peak_queued, int(queued[1]))
                time.sleep(.01)
            results = [future.result() for future in futures]
        self.assertGreaterEqual(peak_busy, 2, f'calibrated Rust jobs must overlap; peak queued={peak_queued}')
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-rust: compiled on', result.stderr)
            counts = re.search(r'input cache: (\d+) of', result.stderr)
            assert counts is not None, result.stderr
            warm = int(counts[1])
            self.assertLess(warm, cold)
        for i in range(1, 6):
            path = self.work / f'libcache{i}.rlib'
            actual = path.read_bytes()
            result = self.rust('--crate-name=cache', '--crate-type=rlib', source,
                               '-o', path, remote=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(actual, path.read_bytes())

    def test_client_takes_over_inputs_from_unresponsive_peer(self):
        import concurrent.futures
        source, output = self.work / 'recovery.rs', self.work / 'librecovery.rlib'
        source.write_text('pub fn answer() -> u32 { 42 }')
        body, _ = self.wire_plan(source)
        self.restart_server(self.build / 'distccd', DISTCC_IO_TIMEOUT='2')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        env = dict(self.env, DISTCC_IO_TIMEOUT='2')
        with socket.create_connection((self.worker_address, self.ports[0]), timeout=5) as owner:
            # Reserve the actual Rust inputs, then stop sending as a disconnected
            # uploader would. The real file-job client has the same inputs.
            manifest = struct.pack('!Q', 999) + body
            owner.sendall(struct.pack('!II', 0x54000400, len(manifest)) + manifest)
            header = bytearray()
            while len(header) < 8:
                part = owner.recv(8 - len(header))
                self.assertTrue(part)
                header.extend(part)
            kind, size = struct.unpack('!II', header)
            self.assertEqual(kind, 0x54000401)
            while size:
                part = owner.recv(size)
                self.assertTrue(part)
                size -= len(part)
            def compile(index):
                return self.rust('--crate-type=rlib', source, '-o', output.with_name(f'lib{index}.rlib'), env=env)
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                for result in pool.map(compile, range(3)):
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('compiled on', result.stderr)
            self.assertEqual(owner.recv(1), b'')
        output = output.with_name('lib2.rlib')
        main = self.work / 'recovery-main.rs'
        main.write_text('fn main() { assert_eq!(recovery::answer(), 42); }')
        result = self.rust(main, '--extern', 'recovery=' + str(output),
                           '-o', self.work / 'recovery-check', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work / 'recovery-check'], check=True, timeout=5)

    def reserve_input_space(self, identity):
        from distcc_job import JOB_COMPILE, manifest as job_manifest
        args = [b'gcc', b'-c', b'/src/held.c', b'-o', b'/src/held.o']
        space = os.statvfs(self.root)
        size = space.f_bavail * space.f_frsize - 8 * 1024 * 1024
        digest = struct.pack('!Q', identity)
        manifest = digest + job_manifest(JOB_COMPILE, '/src', args, [
            ('/src/held.c', size, digest, 0), ('/src/marker', 1, digest, 1)])
        net = socket.create_connection((self.worker_address, self.ports[0]), timeout=5)
        self.addCleanup(net.close)
        net.sendall(struct.pack('!II', 0x54000400, len(manifest)) + manifest)
        return net

    def test_shared_inputs_survive_cancel_and_eviction_and_are_read_only(self):
        import select
        import xxhash
        source, payload = self.work / 'shared.rs', self.work / 'shared.bin'
        source.write_text('pub fn size() -> usize { include_bytes!("shared.bin").len() }')
        payload.write_bytes(b'abcde' * (1024 * 1024))
        output = self.work / 'libshared.rlib'
        marker = self.work/'ready'
        marker.write_text('wait until the owning uploader disconnects')
        # This worker fixture reads its own mount table, which deliberately
        # cannot be supplied by the client's dependency scanner.
        source.write_text(source.read_text() + '''
pub fn read_only() -> bool {
    include_str!("/proc/self/mountinfo").lines().any(|line| {
        let fields: Vec<_> = line.split_whitespace().collect();
        fields[4] == "/" && fields[5].split(',').any(|flag| flag == "ro")
    })
}
''')
        # The probe observes the client's /proc; the mount table is a worker runtime file.
        # Use a cfg to defer that expression until the isolated compilation.
        text = source.read_text().replace('pub fn read_only()', '#[cfg(worker_view)] pub fn read_only()')
        source.write_text(text)
        body, paths = self.wire_plan(source, flags=['--cfg','worker_view'], extra=[marker])
        inputs, manifest_index = paths[:-1], len(paths)-1
        sockets = []
        held = None
        for identity in range(1, 4):
            net = socket.create_connection((self.worker_address, self.ports[0]), timeout=15)
            self.addCleanup(net.close)
            sockets.append(net)
            # The planner's scheduling hint is consumed locally, before the
            # transport encodes the unchanged worker job description.
            self.send(net, 0x54000400, struct.pack('!Q', identity) + body)
            if identity == 1:
                kind, requested = self.frame(net)
                self.assertEqual(kind, 0x54000401)
                for index in range(len(requested) * 8):
                    if not requested[index // 8] & (1 << (index % 8)): continue
                    if index != manifest_index:
                        self.upload(net, paths[index].read_bytes(), index)
                deadline = time.monotonic() + 10
                while True:
                    roots = list(self.root.glob('distcc-tcp-*'))
                    if len(roots) == 1 and all(
                            (p := roots[0] / str(path).lstrip('/')).exists() and p.stat().st_nlink >= 2
                            for path in inputs):
                        break
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                # Leave only 8 MiB of unreserved capacity. The other jobs fit
                # only if admission counts shared files as already stored.
                held = self.reserve_input_space(999)
                self.assertEqual(self.frame(held), (0x54000401, b'\x02'))

        deadline = time.monotonic() + 10
        relative = str(payload).lstrip('/')
        while True:
            files = [root / relative for root in self.root.glob('distcc-tcp-*')
                     if (root / relative).exists()]
            roots = [root for root in self.root.glob('distcc-tcp-*') if (root / relative).exists()]
            ready = len(roots) == 3 and all(
                (root / str(path).lstrip('/')).exists() for root in roots for path in inputs)
            if ready and len(files) == 3 and len({p.stat().st_ino for p in files}) == 1:
                break
            self.assertLess(time.monotonic(), deadline, 'jobs did not share their input inode')
            time.sleep(.01)
        self.assertEqual(files[0].stat().st_size, payload.stat().st_size)
        self.assertGreaterEqual(files[0].stat().st_nlink, 4)
        for path in inputs:
            self.assertEqual(len({(root / str(path).lstrip('/')).stat().st_ino for root in roots}), 1,
                             f'input was duplicated: {path.name}')
        # Deleting the cache name and cancelling its uploading job must leave
        # the other jobs' inode references intact, without a retransmission.
        digest = xxhash.xxh3_64_digest(payload.read_bytes())
        cache = self.root / f'distcc-cache-{os.getuid()}' / f'{64 + digest[0] % 64:02x}' / digest.hex()
        cache.unlink()
        sockets[0].close()
        promoted = select.select(sockets[1:], [], [], 5)[0]
        self.assertEqual(len(promoted), 1)
        kind, requested = self.frame(promoted[0])
        self.assertEqual(kind, 0x54000401)
        self.assertEqual(int.from_bytes(requested, 'little'), 1 << manifest_index)
        self.upload(promoted[0], marker.read_bytes(), manifest_index)
        for identity, net in enumerate(sockets[1:], 2):
            status, err, outputs = self.wire_result(net, identity)
            self.assertEqual(status, 0, err)
            output.write_bytes(outputs['libwire.rlib'])
            main = self.work / 'shared-main.rs'
            main.write_text(f'fn main() {{ assert!(shared::read_only()); assert_eq!(shared::size(), {payload.stat().st_size}); }}')
            result = self.rust(main, '--extern', 'shared=' + str(output), '-o', self.work / 'shared-check', remote=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([self.work / 'shared-check'], check=True)
        assert held is not None
        held.close()
        deadline = time.monotonic() + 5
        # Cache capacity is global: large unused inputs may remain cached.
        # Cancellation/completion must release job links, not evict cache data.
        while list(self.root.glob('distcc-tcp-*')) or any(
                p.stat().st_nlink != 1
                for p in cache.parent.parent.glob('*/*') if len(p.name) == 16):
            self.assertLess(time.monotonic(), deadline, 'completed jobs retained input references')
            time.sleep(.01)

    def test_input_budget_waits_then_compiles_and_reclaims_cancelled_jobs(self):
        import concurrent.futures

        # Reserve most of the real input filesystem without sending a large file.
        # The NEED response proves admission; the real Rust job must then wait
        # for that reservation to be released instead of losing its connection.
        def preparing(count):
            deadline = time.monotonic() + 5
            while True:
                try:
                    if f'dcc_preparing {count}\n' in self.get_stats(self.stats_ports[0]):
                        return
                except ConnectionResetError:
                    pass  # Retry a reset within the same bounded progress check.
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)

        # Warm toolchain inputs require no storage reservation. Give this job
        # its own cold input larger than the 8 MiB left by the held request.
        with (self.work / 'budget.bin').open('wb') as payload:
            payload.write(os.urandom(32))
            payload.truncate(32 * 1024 * 1024)
        held = self.reserve_input_space(101)
        self.assertEqual(held.recv(9, socket.MSG_WAITALL), struct.pack('!IIB', 0x54000401, 1, 2))
        cancelled = self.reserve_input_space(102)
        preparing(2)
        cancelled.close()
        preparing(1)
        source = self.work / 'budget.rs'
        source.write_text('pub fn size() -> usize { include_bytes!("budget.bin").len() }')
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(self.rust, '--crate-type=rlib', source,
                                 '-o', self.work / 'libbudget.rlib')
            try:
                preparing(2)
                self.assertFalse(future.done(), 'Rust job did not wait for input capacity')
            finally:
                held.close()
            result = future.result(timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        result = self.rust('--crate-type=rlib', source, '-o', self.work / 'libafter.rlib')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)

    def test_response_files_expand_recursively_and_refresh_on_change(self):
        src = self.work/'response.rs'
        src.write_text('#[cfg(good)] pub fn answer() -> u32 { 42 }')
        inner = self.work/'inner args'
        inner.write_text('--cfg\ngood\n')
        outer = self.work/'outer'
        output = self.work/'libresponse.rlib'
        outer.write_text('\n'.join(['@'+str(inner),'--crate-type=rlib',str(src),'-o',str(output)])+'\n')
        result = self.rust('@'+str(outer))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('compiled on', result.stderr)
        quoted = self.work/'quoted'
        quoted.write_text(shlex.join(['--crate-type=rlib','--cfg','good',str(src),'-o',str(output)]))
        result = self.rust('-Zshell-argfiles','@shell:'+str(quoted), env=dict(self.env,RUSTC_BOOTSTRAP='1'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('compiled on', result.stderr)
        compiler = self.work/'rustc'
        compiler.write_text('#!/bin/sh\ncase " $* " in *" -Zbinary-dep-depinfo "*) echo --cfg=changed > '
                            + shlex.quote(str(inner)) + ';; esac\nexec '+shlex.quote(self.rustc)+' "$@"\n')
        compiler.chmod(0o755)
        self.rustc = str(compiler)
        result = self.rust('@'+str(outer))
        native = self.rust('@'+str(outer), remote=False)
        self.assertEqual(result.returncode, native.returncode, result.stderr)
        self.assertIn(native.stderr.strip(), result.stderr)
        self.assertIn('input changed:', result.stderr)
        self.assertNotIn('compiled on', result.stderr)

    def test_worker_refuses_unknown_build_without_compiling(self):
        source = self.work/'unknown.rs'
        source.write_text('pub fn unknown() {}')
        status, err, outputs = self.raw_compile(source, requested='rustc unknown build')
        self.assertEqual(status, 121, err)
        self.assertIn('worker has no matching Rust toolchain', err)
        self.assertFalse(outputs)

    def test_worker_selects_matching_build_without_incompatible_attempt(self):
        other = next((p for p in Path('/usr/lib/rust').glob('*/bin/rustc')
                      if self.version(p) != self.version(self.rustc)), None)
        if not other: self.skipTest('a second installed Rust compiler is required')
        self.addCleanup(self.restart_server, self.build/'distccd')
        src = self.work/'version.rs'
        src.write_text('pub fn answer() -> u32 { 42 }')
        self.restart_server(self.build/'distccd', DISTCCD_RUSTC=str(other))
        r = self.rust('--crate-type=rlib', '--emit=metadata', src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('local: worker has no matching Rust toolchain', r.stderr)
        self.restart_server(self.build/'distccd', DISTCCD_RUSTC=os.pathsep.join((str(other), self.rustc)))
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='refreshed-catalog')
        r = self.rust('--crate-type=rlib', src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('compiled on', r.stderr)
        src.write_text(src.read_text()+' // mixed metadata')
        status, err, outputs = self.raw_compile(src, requested=self.version(other))
        self.assertEqual(status, 1, err)
        self.assertIn('E0514', err)
        self.assertFalse(outputs)

    def test_worker_build_identity_is_not_release_number(self):
        src = self.work/'vendor.rs'
        src.write_text('pub fn vendor() {}')
        ident = self.version(self.rustc).replace('\n', ' (vendor)\n', 1)
        status, _, outputs = self.raw_compile(src, requested=ident)
        self.assertEqual(status, 121)
        self.assertFalse(outputs)



    def test_distributed_runtime_loader_executes_real_compiler(self):
        original = Path(self.rustc).resolve()
        root = self.work/('rust-distcc-' + 'c'*64)
        (root/'bin').mkdir(parents=True)
        (root/'distcc-runtime').mkdir()
        shutil.copy2(original, root/'bin/rustc')
        (root/'lib').mkdir()
        (root/'lib/rustlib').symlink_to(original.parent.parent/'lib/rustlib')
        subprocess.run(['cp', '-L', '--reflink=auto',
                        *map(str, (original.parent.parent/'lib').glob('*.so*')),
                        str(root/'lib')], check=True)
        loaded = subprocess.check_output(['ldd', str(original)], text=True,
                                         env=dict(os.environ, LC_ALL='C'))
        loader, = re.findall(r'^\s*(/\S+) \(', loaded, re.M)
        (root/'distcc-runtime/ld.so').symlink_to(loader)
        self.restart_server(self.build/'distccd', DISTCCD_RUSTC=str(root/'bin/rustc'))
        self.addCleanup(self.restart_server, self.build/'distccd')
        source = self.work/'runtime.rs'
        source.write_text('pub fn answer() -> String { String::from("runtime") }')
        output = self.work/'libruntime.rlib'
        for marker in (None, 'b'*64, 'c'*64):
            if marker: (root/'.distcc-complete').write_text(marker+'\n')
            status, _, files = self.raw_compile(source)
            self.assertEqual(status, 121)
            self.assertFalse(files)
        (root/'.distcc-complete').write_text('isa-checked ' + 'c'*64 + '\n')
        status, diagnostics, files = self.raw_compile(source)
        self.assertEqual(status, 0, diagnostics)
        output.write_bytes(files['libwire.rlib'])
        source.write_text('fn main() { assert_eq!(runtime::answer(), "runtime"); }')
        result = self.rust(source, '--extern', 'runtime='+str(output), '-o', self.work/'main', remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([self.work/'main'], check=True, timeout=5)

    def test_worker_does_not_retry_source_errors(self):
        src = self.work/'error.rs'
        src.write_text('#[cfg(broken)] compile_error!("original diagnostic");')
        status, err, outputs = self.raw_compile(src, flags=['--cfg','broken'])
        self.assertEqual(status, 1, err)
        self.assertIn('original diagnostic', err)
        self.assertFalse(outputs)


if __name__ == '__main__':
    unittest.main()
