#!/usr/bin/env python3
"""Rust/Cargo distributed compilation through real TCP workers."""
import os
import json
from pathlib import Path
import runpy
import re
import shutil
import subprocess
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
        result = self.rust('--crate-type=rlib', source, '-o', output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mismatched types', result.stderr)
        self.assertEqual(output.read_bytes(), b'previous artifact')

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
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(build, range(1, 4)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-rust: compiled on', result.stderr)
            counts = re.search(r'input cache: (\d+) of', result.stderr)
            assert counts is not None, result.stderr
            warm = int(counts[1])
            self.assertLess(warm, cold)
        for i in range(1, 4):
            path = self.work / f'libcache{i}.rlib'
            actual = path.read_bytes()
            result = self.rust('--crate-name=cache', '--crate-type=rlib', source,
                               '-o', path, remote=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(actual, path.read_bytes())

    def test_changed_snapshot_is_rejected(self):
        helper = runpy.run_path(str(self.build / 'distcc-rust'))
        source = self.work / 'changing.rs'
        source.write_text('pub fn answer() -> u32 { 1 }')
        job, inputs, _, _ = helper['plan'](self.rustc, ['--crate-type=rlib', str(source)], self.work)
        source.write_text('pub fn answer() -> u32 { 2 }')
        with self.assertRaises(helper['LocalOnly']):
            helper['metadata'](job, inputs, self.work)

    def test_worker_rejects_wrong_compiler_without_output(self):
        helper = runpy.run_path(str(self.build / 'distcc-rust'))
        source = self.work / 'version.rs'
        source.write_text('pub fn answer() -> u32 { 1 }')
        job, inputs, _, _ = helper['plan'](self.rustc, ['--crate-type=rlib', str(source)], self.work)
        job['compiler']['commit'] = 'wrong-build'
        metadata, snapshots = helper['metadata'](job, inputs, self.work)
        artifact = self.work / 'result.tar'
        artifact.write_bytes(b'old result')
        result = subprocess.run([str(self.build / 'distcc'), '--rust-job', str(metadata),
                                 str(snapshots), str(artifact)], env=self.env,
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 100, result.stderr)
        self.assertIn('no worker compiler matches', result.stderr)
        self.assertEqual(artifact.read_bytes(), b'old result')


if __name__ == '__main__':
    unittest.main()
