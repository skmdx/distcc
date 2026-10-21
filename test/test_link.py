#!/usr/bin/env python3
"""Exercise sysroot publication and object linking through real distccd jobs."""
import json
import io
import os
from pathlib import Path
import re
import runpy
import shutil
import socket
import subprocess
import sys
import tempfile
import tarfile
import unittest

from test_broker import BrokerTestCase


class LinkArchiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = runpy.run_path(str(Path.cwd() / 'distcc-link'))

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
            version = subprocess.check_output(['clang', '-dumpversion'], text=True).strip()
            if ('LLD ' + version + ' ') not in subprocess.check_output([linker, '--version'], text=True):
                raise unittest.SkipTest('matching Clang and LLD versions are required')
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

class LinkTest(LinkTestCase):
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
        self.assertNotIn('O_TMPFILE', calls)
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
                self.assertIn('distcc-link: linked on ', result.stderr)
                self.assertEqual(subprocess.check_output([str(output)], text=True), 'linked\n')

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
            self.assertIn('distcc-link: linked on ', result.stderr)
            local = self.work / 'local-optimized'
            subprocess.run([self.compiler, '-fuse-ld=lld', *flags, str(self.obj), '-o', str(local)],
                           check=True, capture_output=True)
            self.assertEqual(local.read_bytes(), output.read_bytes())
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
        _, result = self.link(['-fuse-ld=bfd'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('single LLD invocation', result.stderr)
        self.assertNotIn('distcc-link: linked on ', result.stderr)

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
