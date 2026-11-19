#!/usr/bin/env python3
"""Rust/Cargo distributed compilation through real TCP workers."""
import os
import json
import hashlib
from pathlib import Path
import runpy
import re
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
        return '\n'.join((report[0], fields['commit-hash'], fields['LLVM version']))

    def wire_plan(self, source, *, flags=(), requested=None, logical=None, extra=(), host=''):
        """A real rustc closure for tests which hold or alter an individual TCP transaction."""
        from distcc_job import RustCommand, fingerprint, manifest
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
        command = RustCommand(phase=1, compiler=requested or self.version(self.rustc), crate_types='rlib', host=host)
        return manifest(3, self.work, args, entries, rust=command), paths

    def wire_result(self, net, identity, first=None):
        codec = runpy.run_path(str(self.build/'distcc-link'))['Lz4Chunks']()
        kind, data = first or self.frame(net)
        while kind == 0x54000308:
            kind, data = self.frame(net)
        self.assertEqual(kind, 0x54000303)
        status, _, *sizes = struct.unpack('!II4Q', data[:40])
        streams = [bytearray() for _ in sizes]
        names = []
        while True:
            kind, data = self.frame(net)
            if kind == 0x54000305:
                self.assertEqual(data[:8], struct.pack('!Q', identity))
                break
            if kind == 0x5400030b:
                while data:
                    size, mode = struct.unpack_from('!QI', data)
                    name, data = data[12:].split(b'\0', 1)
                    names.append(os.fsdecode(name))
                    sizes.append(size)
                    streams.append(bytearray())
                continue
            self.assertEqual(kind & ~0x30000, 0x54000304)
            index, plain = struct.unpack_from('!II', data)
            streams[index].extend(codec.decode(data[8:], plain) if kind & 0x10000 else data[8:])
        self.assertEqual(names, sorted(names))
        self.assertEqual(list(map(len, streams)), sizes)
        return status >> 8, streams[0].decode(errors='replace'), dict(zip(names, streams[4:]))

    def raw_compile(self, source, **kw):
        body, paths = self.wire_plan(source, **kw)
        with socket.create_connection((self.worker_address, self.ports[0]), timeout=15) as net:
            self.send(net, 0x54000300, struct.pack('!Q',1)+body)
            kind, needed = self.frame(net)
            if kind != 0x54000301:
                return self.wire_result(net, 1, (kind, needed))
            self.assertEqual(kind, 0x54000301)
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
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])

    def rust(self, *args, remote=True, env=None):
        cmd = ([str(self.build / 'distcc'), self.rustc] if remote else [self.rustc])
        return subprocess.run([*cmd, *map(str, args)], env=env or self.env,
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
            if (proc/'cmdline').read_bytes().startswith(b'/backend\0--execute\0'):
                return True
            children = (proc/'task'/str(pid)/'children').read_text().split()
        except (FileNotFoundError, ProcessLookupError):
            return False
        return any(RustTest.cgu_executing(child) for child in children)

    def completed_jobs(self):
        return [int(next(line.split()[1] for line in self.get_stats(port).splitlines()
                         if line.startswith('dcc_compile_ok '))) for port in self.stats_ports]

    def assert_one_job_per_cgu(self, messages, counts):
        modules = re.findall(r'^distcc-rust-cgu: completed \S+ (\S+)$', messages, re.M)
        self.assertGreater(len(modules), 1, messages)
        self.assertEqual(len(modules), len(set(modules)), modules)
        self.assertCountEqual(modules, re.findall(r'^distcc-rust-cgu: emitted (\S+)$', messages, re.M))
        self.assertEqual(sum(self.completed_jobs()) - sum(counts), len(modules), messages)

    def test_started_cgu_failure_reaches_the_compiler(self):
        version, target = self.cgu_toolchain()
        request = self.work/'invalid-module.bc'
        request.write_bytes(b'{"llvm":"'+version.encode()+b'"}\ninvalid bitcode')
        with request.open('rb') as ir, (self.work/'object').open('wb') as obj:
            result = subprocess.run([self.build/'distcc', '--rust-cgu', version, target, 'object'],
                                    cwd=self.work, stdin=ir, stdout=obj, stderr=subprocess.PIPE,
                                    env=dict(self.env, DISTCC_FALLBACK='0'), text=True, timeout=15)
        self.assertEqual(result.returncode, 100, result.stderr)
        self.assertIn('missing CGU flags', result.stderr)
        self.assertEqual((self.work/'object').stat().st_size, 0)

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
            modules.append(f'pub mod m{i} {{ {funcs} pub fn all(x:u64)->u64 {{ {expr} }} }}')
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
        os.link(self.build/'distccd', daemon)
        shim = folder/'version.c'
        shim.write_text('#include <stdio.h>\n#include <unistd.h>\n'
                        'void __real_LLVMGetVersion(unsigned*,unsigned*,unsigned*);\n'
                        'void __wrap_LLVMGetVersion(unsigned *a,unsigned *b,unsigned *c) { '
                        f'__real_LLVMGetVersion(a,b,c); *c={patch}; '
                        'if (!access("/input", R_OK)) fprintf(stderr, "LLVM worker %u.%u.%u\\n", *a, *b, *c); }\n')
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
                self.assertIn('dcc_rust_host '+host, stats)
                self.assertIn(llvm+' '+backend, next(l for l in stats.splitlines() if l.startswith('dcc_llvm_cgu ')))
                for incompatible in ('0.0.0 '+backend, llvm+' absent-backend'):
                    request = self.request(10, work=(1, 65536, 11), memory_class=1+8*64+16,
                                           rust_host=incompatible.encode())
                    self.assertEqual(struct.unpack_from('=i', request.recv(1032))[0], 121)
                counts = self.completed_jobs()
                result = subprocess.run(['strace', '-f', '-s', '256', '-e', 'execve', '-o', trace,
                                         self.build/'distcc', self.rustc, *args], cwd=self.work, env=self.env,
                                        capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-rust-cgu: finished module-opt', result.stderr)
                self.assertIn('distcc-rust-cgu: completed', result.stderr)
                self.assert_one_job_per_cgu(result.stderr, counts)
                if host == foreign_host:
                    self.assertEqual(result.stderr.count(f'LLVM worker {llvm}.{worker_patch}\n'),
                                     result.stderr.count('distcc-rust-cgu: completed'))
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
        result = self.rust('--crate-type=rlib', source, '-o', output)
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
        # The producer runs locally. rustc itself diagnoses its invalid exported item.
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
        driver.write_text('#!/bin/sh\nexec cc "$@" -Wl,--defsym,verified_client_link=42\n')
        driver.chmod(0o755)
        output=self.work/'client-linked'
        inner,outer=self.work/'inner args',self.work/'outer args'
        inner.write_text('"-Wl,--defsym,verified_response_arg=17"\n')
        outer.write_text(shlex.quote('@'+str(inner))+'\n')
        result=self.rust(src,'-Clinker='+str(driver),'-Clink-arg=@'+str(outer),'-o',output)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('compiled on',result.stderr)
        symbols=subprocess.check_output(['nm',output],text=True)
        self.assertIn('verified_client_link',symbols)
        self.assertIn('verified_response_arg',symbols)

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
                          'assert_eq!(env!("DISTCC_RUST_TEST_VALUE"), "value with space"); '
                          'assert!(option_env!("DISTCC_RUST_ABSENT").is_none()); '
                          'module::value() + INCLUDED + include_bytes!("payload.bin").len() as u32 }')
        env = dict(self.env, DISTCC_RUST_TEST_VALUE='value with space')
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
                self.assertEqual(first.recv(4, socket.MSG_WAITALL), struct.pack('!I', 0x54000300))
            second, _ = listener.accept()
            with second, socket.create_connection((self.worker_address, self.ports[0]), timeout=10) as worker:
                peers = {second: worker, worker: second}
                while not stopped.is_set():
                    for source in select.select(list(peers), [], [], .1)[0]:
                        data = source.recv(256 * 1024)
                        if not data:
                            return
                        peers[source].sendall(data)
        self.start_broker(f'{self.worker_address}:{listener.getsockname()[1]}/1,tcp', name='retry', backoff=0)
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
        profdata = shutil.which('llvm-profdata')
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

    def test_worker_rejects_plugin_producer_hidden_by_later_crate_type(self):
        src = self.work/'plugin.rs'
        src.write_text('pub fn answer() {}')
        body, _ = self.wire_plan(src, flags=['--crate-type=proc-macro', '--crate-type=rlib'])
        with socket.create_connection((self.worker_address, self.ports[0])) as net:
            self.send(net, 0x54000300, struct.pack('!Q', 1) + body)
            # A second encoding of a typed field is rejected at BEGIN.
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

    def test_pure_macro_consumer_runs_on_matching_worker(self):
        source = self.work / 'macros.rs'
        source.write_text('extern crate proc_macro; use proc_macro::TokenStream; '
                          '#[proc_macro] pub fn answer(_: TokenStream) -> TokenStream { "42".parse().unwrap() }')
        library = self.work / 'libmacros.so'
        result = self.rust('--crate-type=proc-macro', source, '-o', library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: local:', result.stderr)
        source = self.work / 'consumer.rs'
        source.write_text('extern crate macros; pub fn value() -> u32 { macros::answer!() }')
        result = self.rust('--crate-type=rlib', source, '--extern', 'macros=' + str(library),
                           '-o', self.work / 'libconsumer.rlib')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('proc-macro host ABI', result.stderr)
        self.assertIn('compiled on', result.stderr)

    def test_broker_and_worker_refuse_mismatching_macro_host(self):
        source=self.work/'host.rs'
        source.write_text('pub fn value() {}')
        self.assertEqual(self.rust('--crate-type=rlib',source).returncode,0)
        foreign='incompatible-test-host'
        request=self.request(10,work=(1,1024,7),work_units=1,rust_host=foreign.encode())
        status,=struct.unpack_from('=i',request.recv(1032))
        self.assertEqual(status,121)
        status,err,outputs=self.raw_compile(source,host=foreign)
        self.assertEqual(status,121,err)
        self.assertFalse(outputs)

    def test_macro_private_shared_library_and_observed_data(self):
        library=self.work/'libhelper.so'
        c=self.work/'helper.c'
        c.write_text('int add(void) {return 2;}')
        subprocess.run(['cc','-shared','-fPIC',c,'-o',library],check=True)
        data=self.work/'number.txt'
        data.write_text('40')
        source=self.work/'derive.rs'
        source.write_text('extern crate proc_macro; #[link(name="helper")] extern "C" { fn add()->i32; } '
            '#[proc_macro] pub fn value(_:proc_macro::TokenStream)->proc_macro::TokenStream { '
            'let n:i32=std::fs::read_to_string('+json.dumps(str(data))+').unwrap().parse().unwrap(); '
            '(n+unsafe{add()}).to_string().parse().unwrap() }')
        plugin=self.work/'libderive.so'
        result=self.rust(source,'--crate-type=proc-macro','-Lnative='+str(self.work),
                         '-Clink-arg=-Wl,-rpath,$ORIGIN','-o',plugin,remote=False)
        self.assertEqual(result.returncode,0,result.stderr)
        src=self.work/'app.rs'
        src.write_text('extern crate derive; fn main(){assert_eq!(derive::value!(),42)}')
        output=self.work/'app'
        result=self.rust(src,'--extern','derive='+str(plugin),'-o',output)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('proc-macro host ABI',result.stderr)
        self.assertIn('compiled on',result.stderr)
        subprocess.run([output],check=True)

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
            owner.sendall(struct.pack('!II', 0x54000300, len(manifest)) + manifest)
            header = bytearray()
            while len(header) < 8:
                part = owner.recv(8 - len(header))
                self.assertTrue(part)
                header.extend(part)
            kind, size = struct.unpack('!II', header)
            self.assertEqual(kind, 0x54000301)
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
        from distcc_job import manifest as job_manifest
        args = [b'gcc', b'-c', b'/src/held.c', b'-o', b'/src/held.o']
        space = os.statvfs(self.root)
        size = space.f_bavail * space.f_frsize - 8 * 1024 * 1024
        digest = struct.pack('!Q', identity)
        manifest = digest + job_manifest(1, '/src', args, [
            ('/src/held.c', size, digest, 0), ('/src/marker', 1, digest, 1)])
        net = socket.create_connection((self.worker_address, self.ports[0]), timeout=5)
        self.addCleanup(net.close)
        net.sendall(struct.pack('!II', 0x54000300, len(manifest)) + manifest)
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
            self.send(net, 0x54000300, struct.pack('!Q', identity) + body)
            if identity == 1:
                kind, requested = self.frame(net)
                self.assertEqual(kind, 0x54000301)
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
                self.assertEqual(self.frame(held), (0x54000301, b'\x02'))

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
        self.assertEqual(kind, 0x54000301)
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
        self.assertEqual(held.recv(9, socket.MSG_WAITALL), struct.pack('!IIB', 0x54000301, 1, 2))
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
        r = self.rust('--crate-type=rlib', src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('local: worker has no matching Rust toolchain', r.stderr)
        self.restart_server(self.build/'distccd', DISTCCD_RUSTC=os.pathsep.join((str(other), self.rustc)))
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
        for marker in (None, 'b'*64):
            if marker: (root/'.distcc-complete').write_text(marker+'\n')
            status, _, files = self.raw_compile(source)
            self.assertEqual(status, 121)
            self.assertFalse(files)
        (root/'.distcc-complete').write_text('c'*64 + '\n')
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
