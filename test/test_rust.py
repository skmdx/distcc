#!/usr/bin/env python3
"""Rust/Cargo distributed compilation through real TCP workers."""
import os
import json
from pathlib import Path
import runpy
import re
import shlex
import shutil
import subprocess
import socket
import struct
import time
import unittest

from test_broker import BrokerTestCase


@unittest.skipUnless(shutil.which('rustc') and shutil.which('bwrap'), 'rustc and bubblewrap required')
class RustTest(BrokerTestCase):
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
        self.assertEqual(float(records[0]['predicted']), 0)
        self.assertGreater(float(records[1]['predicted']), 0)
        self.assertGreater(int(records[0]['source']), 0)
        self.assertGreater(int(records[0]['bytes']), int(records[0]['source']))
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_rust_capacity ', stats)

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
                self.assertEqual('worker compilation failed; retrying locally' in result.stderr,
                                 fallback == '1')
                self.assertEqual(output.read_bytes(), b'previous artifact')

    def test_disconnected_transport_retries_without_local_fallback(self):
        import concurrent.futures
        import select
        import threading
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(10)
        stopped = threading.Event()
        def proxy():
            # Drop an actual BEGIN after the broker has leased the connection.
            first, _ = listener.accept()
            with first:
                first.settimeout(10)
                self.assertEqual(first.recv(4, socket.MSG_WAITALL), struct.pack('!I', 0x54000200))
            second, _ = listener.accept()
            with second, socket.create_connection(('127.0.0.1', self.ports[0]), timeout=10) as worker:
                peers = {second: worker, worker: second}
                while not stopped.is_set():
                    for source in select.select(list(peers), [], [], .1)[0]:
                        data = source.recv(256 * 1024)
                        if not data:
                            return
                        peers[source].sendall(data)
        self.start_broker(f'127.0.0.1:{listener.getsockname()[1]}/1,tcp', name='retry', backoff=0)
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
        self.assertIn('retrying Rust job on a fresh connection', result.stderr)
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
        cache = self.ddir / 'rust-native.json'
        before = cache.stat().st_mtime_ns
        result = self.rust(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(cache.stat().st_mtime_ns, before)
        cache.unlink()
        # An earlier native is overridden by the last explicit CPU setting.
        result = self.rust(*args, '-Ctarget-cpu=generic')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        self.assertFalse(cache.exists())

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
        self.assertIn('procedural macro executed through a transitive dependency', result.stderr)
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

    def test_proc_macro_stays_local(self):
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
        self.assertIn('distcc-rust: local:', result.stderr)

    def test_transitive_procedural_macro_executes_on_client(self):
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
        result = self.rust('--crate-type=rlib', '-Ctarget-cpu=native', source,
                           '--extern', 'bridge=' + str(self.work / 'libbridge.rlib'), *search, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('procedural macro executed through a transitive dependency', result.stderr)
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
        self.restart_server(self.build / 'distccd', DISTCC_MEMORY_LEARNING='1')
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

    def reserve_input_space(self, identity):
        def string(value):
            return struct.pack('!Q', len(value)) + value
        args = [b'gcc', b'-c', b'/src/held.c', b'-o', b'/src/held.o']
        space = os.statvfs(self.root)
        size = space.f_bavail * space.f_frsize - 8 * 1024 * 1024
        manifest = struct.pack('!4Q', identity, 1, len(args), 2) + string(b'/src')
        manifest += b''.join(map(string, args))
        manifest += (struct.pack('!6Q', 0, 0, 0, size, size, identity) +
                     string(b'/src/held.c') + string(b''))
        manifest += (struct.pack('!6Q', 0, 0, 1, 1, 1, identity) +
                     string(b'/src/marker') + string(b''))
        net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5)
        self.addCleanup(net.close)
        net.sendall(struct.pack('!II', 0x54000200, len(manifest)) + manifest)
        return net

    def test_shared_inputs_survive_cancel_and_eviction_and_are_read_only(self):
        import select
        import xxhash
        helper = runpy.run_path(str(self.build / 'distcc-rust'))
        source, payload = self.work / 'shared.rs', self.work / 'shared.bin'
        source.write_text('pub fn size() -> usize { include_bytes!("shared.bin").len() }')
        payload.write_bytes(b'abcde' * (1024 * 1024))
        output = self.work / 'libshared.rlib'
        job, inputs, outdir, outputs = helper['plan'](self.rustc,
            ['--crate-name=shared', '--crate-type=rlib', str(source), '-o', str(output)], self.work)
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
        inputs[source] = helper['stamp'](source.stat())
        metadata, snapshots = helper['metadata'](job, inputs, self.work)
        manifest_index = len(inputs)

        def send(net, kind, data):
            net.sendall(struct.pack('!II', kind, len(data)) + data)

        def frame(net):
            def read(size):
                result = bytearray()
                while len(result) < size:
                    data = net.recv(size - len(result))
                    self.assertTrue(data, 'unexpected connection EOF')
                    result.extend(data)
                return bytes(result)
            kind, size = struct.unpack('!II', read(8))
            return kind, read(size)

        def upload(net, index):
            data = (snapshots / str(index)).read_bytes()
            for offset in range(0, len(data) or 1, 256 * 1024):
                chunk = data[offset:offset + 256 * 1024]
                final = offset + len(chunk) == len(data)
                send(net, 0x54000202 | (0x20000 if final else 0),
                     struct.pack('!II', index, len(chunk)) + chunk)

        sockets = []
        held = None
        for identity in range(1, 4):
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=15)
            self.addCleanup(net.close)
            sockets.append(net)
            send(net, 0x54000200, struct.pack('!Q', identity) + metadata.read_bytes())
            if identity == 1:
                kind, requested = frame(net)
                self.assertEqual(kind, 0x54000201)
                for (index,) in struct.iter_unpack('!I', requested):
                    if index != manifest_index:
                        upload(net, index)
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
                self.assertEqual(frame(held), (0x54000201, struct.pack('!I', 1)))

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
        cache = self.root / f'distcc-cache-{os.getuid()}' / f'{digest[0] % 64:02x}' / digest.hex()
        cache.unlink()
        sockets[0].close()
        promoted = select.select(sockets[1:], [], [], 5)[0]
        self.assertEqual(len(promoted), 1)
        self.assertEqual(frame(promoted[0]), (0x54000201, struct.pack('!I', manifest_index)))
        upload(promoted[0], manifest_index)
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        for identity, net in enumerate(sockets[1:], 2):
            kind, description = frame(net)
            self.assertEqual(kind, 0x54000203)
            status, mode, *sizes = struct.unpack('!II4Q', description[:40])
            buffers = [bytearray() for _ in sizes]
            while True:
                kind, data = frame(net)
                if kind == 0x54000205:
                    self.assertEqual(data, struct.pack('!Q', identity))
                    break
                self.assertEqual(kind & ~0x30000, 0x54000204)
                index, plain = struct.unpack('!II', data[:8])
                buffers[index].extend(codec.decode(data[8:], plain) if kind & 0x10000 else data[8:])
            self.assertEqual(status, 0, buffers[0].decode(errors='replace'))
            self.assertEqual(list(map(len, buffers)), sizes)
            received = self.work / f'result-{identity}'
            received.mkdir()
            artifact = received / 'shared-result.tar'
            artifact.write_bytes(buffers[2])
            helper['install_outputs'](artifact, job, outdir, outputs, received)
            main = self.work / 'shared-main.rs'
            main.write_text(f'fn main() {{ assert!(shared::read_only()); assert_eq!(shared::size(), {payload.stat().st_size}); }}')
            result = self.rust(main, '--extern', 'shared=' + str(output), '-o', self.work / 'shared-check', remote=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([self.work / 'shared-check'], check=True)
        assert held is not None
        held.close()
        deadline = time.monotonic() + 5
        while list(self.root.glob('distcc-tcp-*')) or any(
                p.stat().st_size > 4 * 1024 * 1024
                for p in cache.parent.parent.glob('*/*') if len(p.name) == 16):
            self.assertLess(time.monotonic(), deadline, 'released large inputs were retained')
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

        held = self.reserve_input_space(101)
        self.assertEqual(held.recv(12, socket.MSG_WAITALL), struct.pack('!3I', 0x54000201, 4, 1))
        cancelled = self.reserve_input_space(102)
        preparing(2)
        cancelled.close()
        preparing(1)
        source = self.work / 'budget.rs'
        source.write_text('pub fn answer() -> u32 { 42 }')
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

    def test_changed_snapshot_is_rejected(self):
        helper = runpy.run_path(str(self.build / 'distcc-rust'))
        source = self.work / 'changing.rs'
        source.write_text('pub fn answer() -> u32 { 1 }')
        job, inputs, _, _ = helper['plan'](self.rustc, ['--crate-type=rlib', str(source)], self.work)
        source.write_text('pub fn answer() -> u32 { 2 }')
        with self.assertRaises(helper['LocalOnly']):
            helper['metadata'](job, inputs, self.work)

    def test_worker_accepts_different_build_metadata_when_artifact_links(self):
        helper = runpy.run_path(str(self.build / 'distcc-rust'))
        source = self.work / 'version.rs'
        source.write_text('pub fn answer() -> u32 { 1 }')
        output = self.work / 'libversion.rlib'
        job, inputs, outdir, outputs = helper['plan'](self.rustc,
            ['--crate-name=version', '--crate-type=rlib', str(source), '-o', str(output)], self.work)
        job['compiler'] = dict(version='rustc 0.0.0 (different build)', commit='different', llvm='0.0.0')
        metadata, snapshots = helper['metadata'](job, inputs, self.work)
        artifact = self.work / 'result.tar'
        result = subprocess.run([str(self.build / 'distcc'), '--rust-job', str(metadata),
                                 str(snapshots), str(artifact)], env=self.env,
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-rust: compiled on', result.stderr)
        helper['install_outputs'](artifact, job, outdir, outputs, self.work)
        main = self.work / 'main.rs'
        main.write_text('fn main() { assert_eq!(version::answer(), 1); }')
        executable = self.work / 'linked'
        result = self.rust(main, '--extern', 'version=' + str(output), '-o', executable, remote=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True)


if __name__ == '__main__':
    unittest.main()
