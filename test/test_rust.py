#!/usr/bin/env python3
"""Rust/Cargo distributed compilation through real TCP workers."""
import os
import json
from pathlib import Path
import runpy
import re
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
        self.start_broker(self.host(0, ',tcp'))
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])

    def rust(self, *args, remote=True, env=None):
        cmd = ([str(self.build / 'distcc'), self.rustc] if remote else [self.rustc])
        return subprocess.run([*cmd, *map(str, args)], env=env or self.env,
                              cwd=self.work, capture_output=True, text=True, timeout=90)

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
        env = dict(self.env, RUSTC_WRAPPER=str(self.build / 'distcc'), RUSTC=self.rustc)
        result = subprocess.run(['cargo', 'run', '--offline'], cwd=self.work, env=env,
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('rust-distcc-ok', result.stdout)
        self.assertIn('distcc-rust: compiled on', result.stderr)

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

    def test_cached_inputs_and_parallel_crates(self):
        import concurrent.futures
        source = self.work / 'cache.rs'
        source.write_text('pub fn answer() -> usize { include_bytes!("data.bin").len() }')
        (self.work / 'data.bin').write_bytes(b'abcde' * (1024 * 1024))
        def build(index):
            return self.rust('--crate-name=cache', '--crate-type=rlib', source,
                             '-o', self.work / ('libcache' + str(index) + '.rlib'))
        first = build(0)
        self.assertEqual(first.returncode, 0, first.stderr)
        counts = re.search(r'input cache: (\d+) of', first.stderr)
        assert counts is not None, first.stderr
        cold = int(counts[1])
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            results = list(pool.map(build, range(1, 6)))
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

    def test_input_budget_waits_then_compiles_and_reclaims_cancelled_jobs(self):
        import concurrent.futures

        # Reserve most of the real worker budget without sending a large file.
        # The NEED response proves admission; the real Rust job must then wait
        # for that reservation to be released instead of losing its connection.
        def reserve(identity):
            def string(value):
                return struct.pack('!Q', len(value)) + value
            args = [b'gcc', b'-c', b'/src/held.c', b'-o', b'/src/held.o']
            size = 500 * 1024 * 1024
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

        held = reserve(101)
        self.assertEqual(held.recv(12, socket.MSG_WAITALL), struct.pack('!3I', 0x54000201, 4, 1))
        cancelled = reserve(102)
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
