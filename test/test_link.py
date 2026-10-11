#!/usr/bin/env python3
"""Exercise sysroot publication and object linking through real distccd jobs."""
import json
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import tarfile
import unittest

from test_broker import BrokerTestCase


class LinkTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('rsync') or not shutil.which('g++'):
            raise unittest.SkipTest('rsync and GCC C++ are required')
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
        cls.link_env = dict(os.environ, PATH=str(Path.cwd()) + os.pathsep + os.defpath)
        cls.helper = Path.cwd() / 'distcc-link'
        cls.profile_file = cls.link_root / 'profile.json'
        subprocess.run([str(cls.helper), '--sync', str(cls.snapshot), 'gcc', str(cls.profile_file), str(cls.cache)],
                                env=cls.link_env, check=True, capture_output=True, text=True, timeout=30)
        cls.profile = json.loads(cls.profile_file.read_text())
        cls.daemon_env = {'DISTCCD_LINK_ROOT': str(cls.cache),
                          'DISTCCD_PATH': str(Path.cwd()) + os.pathsep + os.defpath}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.start_broker()
        self.env.update(DISTCC_LINK_SYSROOT=str(self.profile_file), DISTCC_VERBOSE='1',
                        DISTCC_LINK_SOURCE=str(self.snapshot),
                        PATH=str(self.build) + os.pathsep + os.defpath)
        self.cpp = self.work / 'hello.cc'
        self.cpp.write_text('#include <iostream>\nint main() { std::cout << "linked\\n"; }\n')
        self.obj = self.work / 'hello.o'
        subprocess.run([str(self.build / 'distcc'), 'g++', '-c', str(self.cpp), '-o', str(self.obj)],
                       env=self.env, check=True, capture_output=True, timeout=15)

    def link(self, args=()):
        output = self.work / 'hello'
        result = subprocess.run([str(self.build / 'distcc'), 'g++', str(self.obj),
                                 *args, '-o', str(output)], env=self.env, capture_output=True,
                                text=True, timeout=15)
        return output, result

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
        subprocess.run(['ar', 'rcs', str(directory / 'libanswer.a'), str(obj)], check=True)
        self.cpp.write_text('extern int answer(); int main() { return answer() != 42; }\n')
        subprocess.run(['g++', '-c', str(self.cpp), '-o', str(self.obj)], check=True)
        output, result = self.link(['-L' + str(directory), '-lanswer'])
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


if __name__ == '__main__':
    unittest.main()
