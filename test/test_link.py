#!/usr/bin/env python3
"""Exercise sysroot publication and object linking through real distccd jobs."""
import json
import array
from concurrent.futures import ThreadPoolExecutor
import fcntl
import io
import os
from pathlib import Path
import re
import runpy
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import tarfile
import time
import unittest
from unittest import mock

from test_broker import BrokerTestCase


class LinkArchiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        extension = str(next(Path.cwd().glob('_include_server/lib.*')))
        sys.path.insert(0, extension)
        cls.addClassCleanup(sys.path.remove, extension)
        cls.helper = runpy.run_path(str(Path.cwd() / 'distcc-link'))

    def test_identity_is_queried_only_after_link_validation(self):
        prepare = self.helper['prepare_job']
        globals_ = prepare.__globals__
        identity = {'target': 'test-target', 'version': 'test-version'}
        with tempfile.TemporaryDirectory(prefix='distcc-link-map-') as directory:
            obj = Path(directory)/'input.o'
            obj.write_bytes(b'\x7fELF' + bytes(60))
            profile = {'key': '0' * 64, 'family': 'gcc', 'state': {}}
            query = mock.Mock(return_value=identity)
            with mock.patch.dict(globals_, compiler_identity=query):
                for suffix in ('.os', '.so', '.so.1'):
                    other = obj.with_suffix(suffix)
                    other.write_bytes(obj.read_bytes())
                    with self.assertRaises(self.helper['LocalOnly']):
                        prepare(['gcc', str(obj), str(other), '-o', 'out'], profile)
                with self.assertRaises(self.helper['LocalOnly']):
                    prepare(['gcc', str(obj), '-funsupported-link-option', '-o', 'out'], profile)
                query.assert_not_called()
                job, _, output, _ = prepare(['gcc', str(obj), '-o', 'out.so'], profile)
                self.assertEqual((job['target'], job['version'], output),
                                 (identity['target'], identity['version'], Path('out.so')))
                query.assert_called_once_with('gcc', None)
                query.reset_mock()
                prepare(['gcc', str(obj)], profile, identity=identity)
                query.assert_not_called()
                target = 'aarch64-unknown-linux-gnu'
                with mock.patch.dict(globals_, clang_link_command=mock.Mock(
                        return_value=(['ld.lld', str(obj), '-o', 'out'], ''))):
                    prepare(['clang', '--target=' + target, str(obj)], dict(profile, family='clang'))
                query.assert_called_once_with('clang', target)

    def test_tcp_progress_and_local_artifact_descriptor(self):
        from include_server.distcc_pump_c_extensions import TCP_BEGIN, TCP_PROGRESS, TCP_RESULT_FD
        archive = self.helper['LinkArchive']({}, {})
        payload = b'linked artifact\n' * 4096
        client, server = socket.socketpair()
        client.settimeout(5)
        server.settimeout(5)

        def peer():
            def exact(size):
                data = bytearray()
                while len(data) < size:
                    chunk = server.recv(size - len(data))
                    if not chunk:
                        raise EOFError('client disconnected')
                    data.extend(chunk)
                return bytes(data)

            kind, size = struct.unpack('!II', exact(8))
            self.assertEqual(kind, TCP_BEGIN)
            identity = exact(size)[:8]
            while True:
                kind, size = struct.unpack('!II', exact(8))
                exact(size)
                if kind & 0x20000:
                    break
            server.sendall(struct.pack('!II', TCP_PROGRESS, 8) + identity)
            result = struct.pack('!II8Q', 0, 0o755, 0, 0, len(payload), 0, 0, 0, 0, 0)
            server.sendall(struct.pack('!II', TCP_BEGIN + 3, len(result)) + result)
            with tempfile.TemporaryFile() as artifact:
                artifact.write(payload)
                artifact.flush()
                fd = os.open('/proc/self/fd/' + str(artifact.fileno()), os.O_RDONLY)
                try:
                    os.lseek(fd, len(payload), os.SEEK_SET)
                    server.sendmsg([struct.pack('!III', TCP_RESULT_FD, 4, 2)],
                                   [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                finally:
                    os.close(fd)
            server.sendall(struct.pack('!II', TCP_BEGIN + 5, 8) + identity)

        with client, server, tempfile.TemporaryDirectory() as directory, ThreadPoolExecutor(1) as pool:
            future = pool.submit(peer)
            output = Path(directory) / 'linked'
            self.assertEqual(self.helper['tcp_link'](client, archive, output), 0)
            future.result(timeout=5)
            self.assertEqual(output.read_bytes(), payload)

    def test_changed_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.o'
            path.write_bytes(b'original')
            archive = self.helper['LinkArchive']({}, {'input-0.o': path})
            path.write_bytes(b'changed size')
            sender, receiver = socket.socketpair()
            with sender, receiver:
                with self.assertRaisesRegex(OSError, 'input changed'):
                    archive.send(sender)

    def test_protocol_limit_is_checked_without_reading_inputs(self):
        with tempfile.TemporaryFile() as source:
            source.truncate(0xffffffff)
            with self.assertRaises(self.helper['LocalOnly']):
                self.helper['LinkArchive']({}, {'input-0.o': source.fileno()})


class LinkTestCase(BrokerTestCase):
    transport = ''
    compiler = 'g++'
    sync_compiler = 'gcc'

    @classmethod
    def setUpClass(cls):
        if not shutil.which('rsync') or not shutil.which('g++'):
            raise unittest.SkipTest('rsync and GCC C++ are required')
        compiler_path = shutil.which(cls.compiler)
        if not compiler_path:
            raise unittest.SkipTest(cls.compiler + ' is required')
        if cls.sync_compiler == 'clang':
            linker = subprocess.check_output(['clang', '-print-prog-name=ld.lld'], text=True).strip()
            if not shutil.which(linker):
                raise unittest.SkipTest('LLD is required for Clang remote links')
        cls.link_storage = tempfile.TemporaryDirectory(prefix='distcc-link-tests-')
        cls.addClassCleanup(cls.link_storage.cleanup)
        cls.link_root = Path(cls.link_storage.name)
        cls.snapshot = cls.link_root / 'snapshot'
        cls.cache = cls.link_root / 'cache'
        cls.snapshot.mkdir()
        for directory in ('usr/lib', 'usr/lib64'):
            (cls.snapshot / directory).mkdir(parents=True, exist_ok=True)
        # Copy only the native toolchain's C/C++ runtime closure for this fixture.
        for directory in ('lib', 'lib64'):
            original = Path('/') / directory
            if original.is_symlink():
                (cls.snapshot / directory).symlink_to(os.readlink(original))
        names = ('crt1.o', 'Scrt1.o', 'crti.o', 'crtn.o', 'crtbegin.o', 'crtbeginS.o',
                 'crtend.o', 'crtendS.o', 'libgcc.a', 'libgcc_s.so', 'libgcc_s.so.1',
                 'libc.so', 'libc.so.6', 'libc_nonshared.a', 'libstdc++.so',
                 'libm.so', 'libm.so.6', 'libmvec.so.1')
        files = []
        for name in names:
            found = subprocess.check_output(['g++', '-print-file-name=' + name], text=True).strip()
            if Path(found).is_absolute() and Path(found).is_file():
                files.append(Path(os.path.normpath(found)))
        # libc/libm linker scripts name the runtime loader and companion files.
        import re
        for file in list(files):
            if file.name in ('libc.so', 'libm.so'):
                text = file.read_bytes()
                if not text.startswith(b'\x7fELF'):
                    files.extend(Path(p.decode()) for p in re.findall(rb'/[\w/+.\-]+', text)
                                 if Path(os.fsdecode(p)).is_file())
        for file in files:
            target = cls.snapshot / str(file).lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, target)
        if cls.sync_compiler == 'clang':
            runtime = Path(subprocess.check_output(['clang', '-print-runtime-dir'], text=True).strip())
            for file in runtime.glob('libclang_rt.builtins*.a'):
                target = cls.snapshot / os.path.normpath(str(file)).lstrip('/')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, target)
        cls.link_path = os.pathsep.join((str(Path.cwd()), str(Path(compiler_path).parent), os.defpath))
        cls.link_env = dict(os.environ, PATH=cls.link_path)
        cls.helper = Path.cwd() / 'distcc-link'
        cls.profile_file = cls.link_root / 'profile.json'
        sync = subprocess.run([str(cls.helper), '--sync', str(cls.snapshot), cls.sync_compiler,
                               str(cls.profile_file), str(cls.cache)],
                              env=cls.link_env, capture_output=True, text=True, timeout=30)
        if sync.returncode:
            raise RuntimeError(sync.stderr)
        cls.profile = json.loads(cls.profile_file.read_text())
        cls.daemon_env = {'DISTCCD_LINK_ROOT': str(cls.cache),
                          'DISTCCD_PATH': cls.link_path}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, f',stats={self.stats_ports[0]}' + self.transport))
        self.env.update(DISTCC_LINK_SYSROOT=str(self.profile_file), DISTCC_VERBOSE='1',
                        DISTCC_LINK_SOURCE=str(self.snapshot),
                        PATH=self.link_path)
        self.cpp = self.work / 'hello.cc'
        self.cpp.write_text('#include <iostream>\nint main() { std::cout << "linked\\n"; }\n')
        self.obj = self.work / 'hello.o'
        subprocess.run([str(self.build / 'distcc'), self.compiler, '-c', str(self.cpp), '-o', str(self.obj)],
                       env=self.env, check=True, capture_output=True, timeout=15)

    def link(self, args=(), prefix=()):
        output = self.work / 'hello'
        result = subprocess.run([*prefix, str(self.build / 'distcc'), self.compiler, str(self.obj),
                                 *args, '-o', str(output)], env=self.env, capture_output=True,
                                text=True, timeout=15)
        return output, result

    def test_remote_link_bypasses_occupied_local_slot(self):
        self.env['DISTCC_HOSTS'] += ' --localslots=1'
        locks = Path(self.env['DISTCC_DIR']) / 'lock'
        locks.mkdir(exist_ok=True)
        with (locks / 'cpu_localhost_0').open('w') as held:
            fcntl.lockf(held, fcntl.LOCK_EX)
            output, result = self.link()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-link: linked on ', result.stderr)
            subprocess.run([str(output)], check=True, capture_output=True)

    def test_local_fallback_acquires_local_slot(self):
        self.env['DISTCC_HOSTS'] += ' --localslots=1'
        self.env.pop('DISTCC_PAUSE_TIME_MSEC', None)
        locks = Path(self.env['DISTCC_DIR']) / 'lock'
        locks.mkdir(exist_ok=True)
        log, output = self.work / 'fallback.log', self.work / 'fallback'
        self.env['DISTCC_LOG'] = str(log)
        with (locks / 'cpu_localhost_0').open('w') as held:
            fcntl.lockf(held, fcntl.LOCK_EX)
            proc = subprocess.Popen([str(self.build / 'distcc'), self.compiler, str(self.obj),
                                     '-Wl,-Map=' + str(self.work / 'link.map'), '-o', str(output)],
                                    env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while not log.exists() or 'cpu_localhost_0 is busy' not in log.read_text():
                    self.assertIsNone(proc.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                self.assertFalse(output.exists())
                fcntl.lockf(held, fcntl.LOCK_UN)
                _, error = proc.communicate(timeout=15)
                self.assertEqual(proc.returncode, 0, error)
                self.assertIn(b'distcc-link: local:', error)
                self.assertIn('nothing available, sleeping 20ms', log.read_text())
                subprocess.run([str(output)], check=True, capture_output=True)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()

    def test_input_suffixes_bypass_helper_only_for_gcc(self):
        probe = self.work/'link-probe'
        probe.mkdir()
        helper = probe/'distcc-link'
        helper.write_text('#!/bin/sh\n: > "$DISTCC_LINK_PROBE"\nexit 77\n')
        helper.chmod(0o755)
        marker = probe/'called'
        self.env.update(PATH=str(probe) + os.pathsep + self.link_path,
                        DISTCC_LINK_PROBE=str(marker))
        # An output named .so must not disqualify an ordinary object link.
        result = subprocess.run([str(self.build/'distcc'), self.compiler, '-o', str(self.work/'out.so'),
                                 str(self.obj)], env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 77, result.stderr)
        self.assertTrue(marker.exists())
        for suffix in ('.os', '.so', '.so.1'):
            with self.subTest(suffix=suffix):
                marker.unlink(missing_ok=True)
                extra = self.work/('extra' + suffix)
                subprocess.run([self.compiler, '-x', 'c++', '-fPIC',
                                '-c' if suffix == '.os' else '-shared', '-', '-o', str(extra)],
                               input=b'int extra() { return 42; }\n', check=True, capture_output=True, timeout=10)
                output, result = self.link([str(extra)])
                if self.sync_compiler == 'gcc':
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertFalse(marker.exists())
                    subprocess.run([str(output)], check=True, capture_output=True, timeout=5)
                else:
                    self.assertEqual(result.returncode, 77, result.stderr)
                    self.assertTrue(marker.exists())

    def test_metadata_only_sysroot_change_keeps_remote_link(self):
        snapshot = self.work / 'metadata-snapshot'
        subprocess.run(['cp', '-a', '--reflink=auto', str(self.snapshot), str(snapshot)], check=True)
        path = self.work / 'metadata.json'
        subprocess.run([str(self.helper), '--sync', str(snapshot), self.sync_compiler,
                        str(path), str(self.cache)], env=self.link_env,
                       check=True, capture_output=True, timeout=30)
        self.env.update(DISTCC_LINK_SYSROOT=str(path), DISTCC_LINK_SOURCE=str(snapshot))
        file = snapshot / self.profile['gccdir'] / 'crtbegin.o'
        os.chmod(file, file.stat().st_mode ^ 0o200)
        for _ in range(2):
            output, result = self.link()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-link: linked on ', result.stderr)
            subprocess.run([str(output)], check=True, capture_output=True)

    def test_shared_library_permissions_match_local(self):
        source = self.work / 'library.cc'
        source.write_text('extern "C" int answer() { return 42; }\n')
        obj = self.work / 'library.o'
        subprocess.run([self.compiler, '-fPIC', '-c', str(source), '-o', str(obj)],
                       check=True, capture_output=True)
        flags = ['-fuse-ld=lld'] if self.sync_compiler == 'clang' else []
        command = [self.compiler, *flags, '-shared', str(obj)]
        for mask in (0o022, 0o077):
            with self.subTest(umask=oct(mask)):
                local = self.work / f'local-{mask}.so'
                remote = self.work / f'remote-{mask}.so'
                subprocess.run([*command, '-o', str(local)], umask=mask,
                               check=True, capture_output=True)
                result = subprocess.run([str(self.build / 'distcc'), *command,
                                         '-o', str(remote)], env=self.env, umask=mask,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-link: linked on ', result.stderr)
                self.assertEqual(remote.stat().st_mode & 0o777,
                                 local.stat().st_mode & 0o777)
                subprocess.run([sys.executable, '-c',
                                'import ctypes,sys; assert ctypes.CDLL(sys.argv[1]).answer() == 42',
                                str(remote)], check=True, capture_output=True)

    def test_compatible_sysroot_metadata_and_missing_search_directory(self):
        profile = dict(self.profile, version='0.0-compatible-build')
        triple = profile['target'].split('-')
        if len(triple) >= 3:
            triple[1] = 'unknown'
            profile['target'] = '-'.join(triple)
        client_profile = self.work / 'compatible.json'
        client_profile.write_text(json.dumps(profile))
        self.env['DISTCC_LINK_SYSROOT'] = str(client_profile)
        cached_profile = self.cache / profile['key'] / '.distcc-link.json'
        original = cached_profile.read_bytes()
        try:
            cached_profile.write_text(json.dumps(profile))
            output, result = self.link(['-L' + str(self.work / 'absent')])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-link: linked on ', result.stderr)
            self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')
        finally:
            cached_profile.write_bytes(original)


class LinkTest(LinkTestCase):
    def test_worker_prefers_sysroot_target_over_generic_gcc(self):
        directory = self.work / 'worker-compilers'
        directory.mkdir()
        wrong = directory / 'g++'
        wrong.write_text('#!/bin/sh\necho "wrong worker default target" >&2\nexit 1\n')
        wrong.chmod(0o755)
        actual = shutil.which('g++')
        assert actual
        (directory / (self.profile['target'] + '-g++')).symlink_to(actual)
        self.restart_server(self.build / 'distccd',
                            DISTCCD_PATH=str(directory) + os.pathsep + self.link_path)
        output, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertNotIn('wrong worker default target', result.stderr)
        self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')

    def test_gcc_undefined_symbol_policy_is_forwarded(self):
        output, result = self.link(['-Wl,-no-undefined,--undefined-version'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')
        output, result = self.link(['-Wl,--no-undefined-version'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')

    def test_refresh_skips_unchanged_and_publishes_changed_or_missing_cache(self):
        snapshot = self.work / 'snapshot'
        subprocess.run(['cp', '-a', '--reflink=auto', str(self.snapshot), str(snapshot)], check=True)
        path = self.work / 'refresh.json'
        command = [str(self.helper), '--refresh', str(snapshot), 'gcc', str(path), str(self.cache)]
        subprocess.run(command, env=self.link_env, check=True, capture_output=True, timeout=30)
        original = path.read_bytes()
        stamp = path.stat().st_mtime_ns
        result = subprocess.run(command, env=self.link_env, check=True, capture_output=True,
                                text=True, timeout=30)
        self.assertIn('unchanged', result.stdout)
        self.assertEqual(path.stat().st_mtime_ns, stamp)
        extra = self.work / 'extra-cache'
        subprocess.run([*command, str(extra)], env=self.link_env, check=True,
                       capture_output=True, timeout=30)
        self.assertTrue((extra / json.loads(original)['key'] / '.distcc-link.json').is_file())
        with (snapshot / self.profile['gccdir'] / 'crtbegin.o').open('ab') as stream:
            stream.write(b'\0')
        blocked = self.work / 'blocked-cache'
        blocked.write_text('not a directory')
        result = subprocess.run([*command, str(extra), str(blocked)], env=self.link_env,
                                capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(path.read_bytes(), original)
        # A failed destination must not publish a partial client profile, and
        # other transfers must have finished before their metadata is removed.
        self.assertTrue((extra / 'current' / '.distcc-link.json').is_file())
        updated = json.loads((extra / 'current' / '.distcc-link.json').read_text())
        self.assertNotEqual(updated['key'], json.loads(original)['key'])
        self.assertEqual(list(extra.glob('.incoming-*')), [])
        subprocess.run([*command, str(extra)], env=self.link_env, check=True,
                       capture_output=True, timeout=30)
        self.assertNotEqual(json.loads(path.read_text())['key'], json.loads(original)['key'])
        self.env.update(DISTCC_LINK_SOURCE=str(snapshot), DISTCC_LINK_SYSROOT=str(path))
        output, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_link_without_archive_spool(self):
        spool = self.work / 'spool'
        spool.mkdir()
        self.env['TMPDIR'] = str(spool)
        trace = self.work / 'files.trace'
        output, result = self.link(prefix=['strace', '-f', '-e', 'trace=openat', '-o', str(trace)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')
        calls = trace.read_text()
        # Anonymous state publication is independent of link input transport.
        state = str(Path(self.env['DISTCC_DIR'])/'state')
        anonymous = [call for call in calls.splitlines() if 'O_TMPFILE' in call]
        self.assertTrue(all('"' + state + '"' in call for call in anonymous), anonymous)
        # tempfile's directory probe and the compiler-query cache remain valid.
        self.assertNotRegex(calls, re.escape(str(spool)) + r'/tmp[\w-]{8}"')

    def test_native_gcc_link_stays_local(self):
        output, result = self.link(['-march=native'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('native object links require client-side CPU expansion', result.stderr)
        self.assertNotIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    def test_cpp_remote_link_matches_local(self):
        output, result = self.link(['-O3', '-pipe', '-fomit-frame-pointer', '-fno-plt',
                                   '-fno-asynchronous-unwind-tables', '-fno-common',
                                   '-fdata-sections', '-ffunction-sections'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')
        local = self.work / 'local'
        sysroot = self.cache / self.profile['key']
        subprocess.run(['g++', '--sysroot=' + str(sysroot),
                        '-B' + str(sysroot / self.profile['gccdir']) + '/',
                        str(self.obj), '-o', str(local)], check=True, capture_output=True)
        self.assertEqual(local.read_bytes(), output.read_bytes())

    def test_response_file(self):
        response = self.work / 'flags'
        response.write_text('-Wl,--as-needed -pthread')
        _, result = self.link(['@' + str(response)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)

    def test_client_linker_defaults_are_preserved(self):
        # Model a client GCC build with different default linker specs while
        # executing the real compiler and the ordinary synchronization path.
        compiler = self.work / 'gcc'
        real = shutil.which('gcc')
        compiler.write_text('#!' + sys.executable + '\nimport os, subprocess, sys\n'
                            'real = ' + repr(real) + '\n'
                            'if sys.argv[1:] == ["-dumpspecs"]:\n'
                            '    specs = subprocess.check_output([real, "-dumpspecs"], text=True)\n'
                            '    sys.stdout.write(specs.replace("*link:\\n", "*link:\\n--defsym=distcc_specs_marker=42 "))\n'
                            'else:\n    os.execv(real, [real, *sys.argv[1:]])\n')
        compiler.chmod(0o755)
        profile = self.work / 'specs.json'
        subprocess.run([str(self.helper), '--sync', str(self.snapshot), str(compiler),
                        str(profile), str(self.cache)], env=self.link_env,
                       check=True, capture_output=True, timeout=30)
        self.env['DISTCC_LINK_SYSROOT'] = str(profile)
        output, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        symbols = subprocess.check_output(['nm', str(output)], text=True)
        self.assertRegex(symbols, r'0*2a A distcc_specs_marker')

    def test_build_library_directory_is_transferred(self):
        directory = self.work / 'build libraries'
        directory.mkdir()
        source = directory / 'answer.cc'
        source.write_text('int answer() { return 42; }\n')
        obj = directory / 'answer.o'
        subprocess.run(['g++', '-c', str(source), '-o', str(obj)], check=True)
        # Requires a PAX extended header, which must be included in LINK's size.
        library = 'answer' + 'x' * 120
        subprocess.run(['ar', 'rcs', str(directory / ('lib' + library + '.a')), str(obj)], check=True)
        self.cpp.write_text('extern int answer(); int main() { return answer() != 42; }\n')
        subprocess.run(['g++', '-c', str(self.cpp), '-o', str(self.obj)], check=True)
        output, result = self.link(['-L' + str(directory), '-l' + library])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        subprocess.run([str(output)], check=True)

    def test_unsupported_side_output_stays_local(self):
        mapping = self.work / 'link.map'
        output, result = self.link(['-Wl,-Map,' + str(mapping)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: local:', result.stderr)
        self.assertTrue(mapping.is_file())
        self.assertTrue(output.is_file())

    def test_missing_cache_does_not_replace_existing_output(self):
        profile = dict(self.profile, key='0' * 64)
        path = self.work / 'missing.json'
        path.write_text(json.dumps(profile))
        self.env['DISTCC_LINK_SYSROOT'] = str(path)
        output = self.work / 'hello'
        output.write_bytes(b'previous result')
        _, result = self.link()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b'previous result')
        self.assertIn('remote link worker could not execute', result.stderr)

    def test_repeated_sync_preserves_published_generation(self):
        old = self.cache / self.profile['key']
        obj = old / self.profile['gccdir'] / 'crtbegin.o'
        inode = obj.stat().st_ino
        current = os.readlink(self.cache / 'current')
        subprocess.run([str(self.helper), '--sync', str(self.snapshot), 'gcc', str(self.profile_file), str(self.cache)],
                                env=self.link_env, check=True, capture_output=True, text=True, timeout=30)
        self.assertEqual(json.loads(self.profile_file.read_text()), self.profile)
        self.assertEqual(obj.stat().st_ino, inode)
        self.assertEqual(os.readlink(self.cache / 'current'), current)
        self.assertEqual(list(self.cache.glob('.incoming-*')), [])

    def test_stale_profile_uses_local_libraries(self):
        snapshot = self.work / 'snapshot'
        subprocess.run(['cp', '-a', '--reflink=auto', str(self.snapshot), str(snapshot)], check=True)
        path = self.work / 'stale.json'
        subprocess.run([str(self.helper), '--sync', str(snapshot), 'gcc', str(path), str(self.cache)],
                       env=self.link_env, check=True, capture_output=True, timeout=30)
        self.env['DISTCC_LINK_SYSROOT'] = str(path)
        self.env['DISTCC_LINK_SOURCE'] = str(snapshot)
        _, before = self.link()
        self.assertEqual(before.returncode, 0, before.stderr)
        self.assertIn('distcc-link: linked on ', before.stderr)
        file = snapshot / self.profile['gccdir'] / 'crtbegin.o'
        info = file.stat()
        data = file.read_bytes()
        file.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        os.utime(file, ns=(info.st_atime_ns, info.st_mtime_ns))
        output, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('sysroot changed', result.stderr)
        self.assertNotIn('distcc-link: linked on ', result.stderr)
        subprocess.run([str(output)], check=True, capture_output=True)
        # A preserved mtime and size must not fool rsync's quick comparison.
        previous = json.loads(path.read_text())
        subprocess.run([str(self.helper), '--sync', str(snapshot), 'gcc', str(path), str(self.cache)],
                       env=self.link_env, check=True, capture_output=True, timeout=30)
        updated = json.loads(path.read_text())
        self.assertNotEqual(previous['key'], updated['key'])
        relative = Path(self.profile['gccdir']) / 'crtbegin.o'
        self.assertEqual((self.cache / updated['key'] / relative).read_bytes(), file.read_bytes())
        self.assertEqual((self.cache / previous['key'] / relative).read_bytes(), data)

    def test_worker_rejects_archive_path_traversal(self):
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w') as tar:
            member = tarfile.TarInfo('../escaped')
            member.size = 3
            tar.addfile(member, io.BytesIO(b'bad'))
        data = archive.getvalue()
        with socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5) as net:
            net.sendall(b'DIST00000004' + f'LINK{len(data):08x}'.encode() + data)
            reply = b''
            while chunk := net.recv(65536):
                reply += chunk
        self.assertEqual(reply[:16], b'DONE00000004STAT')
        self.assertEqual(int(reply[16:24], 16) >> 8, 100)
        self.assertIn(b'invalid link archive member', reply)
        self.assertFalse((self.root / 'escaped').exists())


class ClangLinkTest(LinkTestCase):
    compiler = 'clang++'
    sync_compiler = 'clang'

    def link(self, args=(), prefix=()):
        return super().link(['-fuse-ld=lld', *args], prefix)

    def test_cpp_remote_link_matches_local(self):
        output, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        local = self.work / 'local'
        subprocess.run([self.compiler, '-fuse-ld=lld', str(self.obj), '-o', str(local)],
                       check=True, capture_output=True)
        self.assertEqual(local.read_bytes(), output.read_bytes())
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    def test_lto(self):
        for mode in ('thin', 'full'):
            with self.subTest(mode=mode):
                flag = '-flto=' + mode
                subprocess.run([self.compiler, flag, '-c', str(self.cpp), '-o', str(self.obj)],
                               check=True, capture_output=True)
                output, result = self.link([flag])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-thinlto: compiled on ' if mode == 'thin' and self.transport else
                              'distcc-link: linked on ', result.stderr)
                self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    def test_client_lld_can_differ_from_clang(self):
        current = subprocess.check_output(['clang', '-dumpversion'], text=True).strip()
        for linker in Path('/usr/lib/llvm').glob('*/bin/ld.lld'):
            version = subprocess.check_output([linker, '--version'], text=True)
            if 'LLD ' + current + ' ' not in version:
                output, result = self.link(['-fuse-ld=' + str(linker)])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('distcc-link: linked on ', result.stderr)
                self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')
                return
        self.skipTest('an additional LLD release is required')

    def test_compiler_rt_from_cache(self):
        self.assertTrue(any('libclang_rt.builtins' in name for name in self.profile['state']))
        output, result = self.link(['--rtlib=compiler-rt', '--unwindlib=libgcc'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    def test_lld_optimization_options(self):
        subprocess.run([self.compiler, '-flto=thin', '-c', str(self.cpp), '-o', str(self.obj)],
                       check=True, capture_output=True)
        for dash in ('-', '--'):
            flags = ['-flto=thin', '-Wl,--threads=2,--lto-O3,' + ','.join(
                     dash + option for option in ('icf=all', 'undefined-version'))]
            output, result = self.link(flags)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-thinlto: compiled on ' if self.transport else
                          'distcc-link: linked on ', result.stderr)
            local = self.work / 'local-optimized'
            subprocess.run([self.compiler, '-fuse-ld=lld', *flags, str(self.obj), '-o', str(local)],
                           check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([local]), subprocess.check_output([output]))
            self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

    def test_profile_directory_selects_clang(self):
        profiles = self.work / 'profiles'
        profiles.mkdir()
        shutil.copyfile(self.profile_file, profiles / 'clang.json')
        self.env['DISTCC_LINK_SYSROOT'] = str(profiles)
        _, result = self.link()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: linked on ', result.stderr)

    def test_other_linker_stays_local(self):
        output, result = self.link(['-fuse-ld=bfd'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-link: local:', result.stderr)
        self.assertNotIn('distcc-link: linked on ', result.stderr)
        self.assertEqual(subprocess.check_output([output], text=True), 'linked\n')

    def test_worker_rejects_linker_side_output(self):
        outside = self.work / 'unauthorized.map'
        job = dict(compiler=self.compiler, target=self.profile['target'],
                   version=self.profile['version'], key=self.profile['key'],
                   args=[{'input': 'input-0.o'}, '--Map=' + str(outside)])
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w') as tar:
            for name, data in [('job.json', json.dumps(job).encode()),
                               ('input-0.o', self.obj.read_bytes())]:
                member = tarfile.TarInfo(name)
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
        data = archive.getvalue()
        with socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5) as net:
            net.sendall(b'DIST00000004' + f'LINK{len(data):08x}'.encode() + data)
            reply = b''
            while chunk := net.recv(65536):
                reply += chunk
        self.assertEqual(reply[:16], b'DONE00000004STAT')
        self.assertEqual(int(reply[16:24], 16) >> 8, 100)
        self.assertIn(b'unsupported LLD option', reply)
        self.assertFalse(outside.exists())


class TcpLinkTest(LinkTest):
    transport = ',tcp'


class TcpClangLinkTest(ClangLinkTest):
    transport = ',tcp'


if __name__ == '__main__':
    unittest.main()
