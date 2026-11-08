#!/usr/bin/env python3
"""Prepared-input TCP jobs through real broker, daemon and compilers."""
import concurrent.futures
import array
import ctypes
import errno
import importlib.util
import itertools
import os
from pathlib import Path
import re
import random
import resource
import runpy
import signal
import stat
import shutil
import shlex
import socket
import base64
import struct
import subprocess
import sys
import threading
import tempfile
import pwd
import time
import unittest
import xxhash

from test_broker import BrokerTestCase
from test_adaptive import free_port

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class FileCacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        extension = next(Path.cwd().glob('_include_server/lib*/include_server/distcc_pump_c_extensions*.so'))
        spec = importlib.util.spec_from_file_location('distcc_pump_c_extensions', extension)
        assert spec and spec.loader
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)
        pointer = ctypes.c_void_p
        size = ctypes.c_size_t
        class Api(ctypes.Structure):
            _fields_ = [('open', ctypes.CFUNCTYPE(pointer, ctypes.c_char_p)),
                        ('close', ctypes.CFUNCTYPE(None, pointer)),
                        ('lookup', ctypes.CFUNCTYPE(ctypes.c_int, pointer, pointer, size, ctypes.POINTER(size))),
                        ('contents', ctypes.CFUNCTYPE(ctypes.c_int, pointer, size, pointer, size,
                                                     ctypes.POINTER(pointer), ctypes.POINTER(size))),
                        ('write', ctypes.CFUNCTYPE(None, pointer, pointer, size, pointer, size))]
        capsule = ctypes.pythonapi.PyCapsule_GetPointer
        capsule.argtypes = (ctypes.py_object, ctypes.c_char_p)
        capsule.restype = pointer
        cls.api = Api.from_address(capsule(cls.module.file_cache, b'distcc.file_cache'))

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='distcc-record-cache-')
        self.addCleanup(directory.cleanup)
        self.cache_path = Path(directory.name)/'records'
        setting = os.environ.get('DISTCCD_FILE_CACHE')
        def restore():
            if setting is None: os.environ.pop('DISTCCD_FILE_CACHE', None)
            else: os.environ['DISTCCD_FILE_CACHE'] = setting
        self.addCleanup(restore)
        os.environ['DISTCCD_FILE_CACHE'] = str(self.cache_path)
        self.initial_fds = len(os.listdir('/proc/self/fd'))
        self.handle = self.api.open(directory.name.encode())
        self.assertTrue(self.handle)
        self.addCleanup(self.close_cache)

    def close_cache(self):
        self.api.close(self.handle)
        self.handle = None

    def write(self, key, data):
        self.api.write(self.handle, key, len(key), data, len(data))

    def read(self, key):
        extent = ctypes.c_size_t()
        fd = self.api.lookup(self.handle, key, len(key), ctypes.byref(extent))
        if fd < 0: return None, ctypes.c_void_p(), 0
        return self.load_record(fd, extent.value, key)

    def load_record(self, fd, extent, key):
        with os.fdopen(fd, 'rb') as stream: record = stream.read(extent)
        storage = ctypes.create_string_buffer(record)
        data, size = ctypes.c_void_p(), ctypes.c_size_t()
        if not self.api.contents(storage, len(record), key, len(key), ctypes.byref(data), ctypes.byref(size)):
            return None, data, 0
        return storage, data, size.value

    def get(self, key):
        storage, data, size = self.read(key)
        if not storage: return None
        assert data.value is not None
        self.assertEqual(ctypes.string_at(data.value+size, 1), b'\0')
        return ctypes.string_at(data, size)

    def shard_keys(self, count):
        candidates = (f'header-{index}'.encode() for index in itertools.count())
        return list(itertools.islice((key for key in candidates
                    if (xxhash.xxh3_64_intdigest(key) >> 56) % 64 == 0), count))

    def test_small_records_use_byte_capacity_instead_of_entry_count(self):
        keys = self.shard_keys(128)
        value = b'#define SHARED_HEADER 17\n' * 128
        for key in keys:
            self.write(key, value)
        for key in keys:
            self.assertEqual(self.get(key), value, key)

    def test_byte_pressure_retains_recent_reads_without_changing_mtime(self):
        keys = self.shard_keys(4)
        value = b'x' * (1024 * 1024)
        paths = []
        for index, key in enumerate(keys[:3]):
            self.write(key, value)
            path = self.cache_path/'00'/f'{xxhash.xxh3_64_intdigest(key):016x}'
            os.utime(path, ns=((index+1)*10**9, (index+1)*10**9))
            paths.append(path)
        # The first insertion is old, but it is still actively used.
        self.assertEqual(self.get(keys[0]), value)
        self.assertEqual(paths[0].stat().st_mtime_ns, 10**9)
        self.write(keys[3], value)
        self.assertEqual(self.get(keys[0]), value)
        self.assertIsNone(self.get(keys[1]))
        self.assertEqual(self.get(keys[2]), value)
        self.assertEqual(self.get(keys[3]), value)
        self.assertLessEqual(sum(p.stat().st_blocks * 512 for p in paths[0].parent.iterdir()),
                             4 * 1024 * 1024)

    def test_binary_records_and_open_snapshots_outlive_replacement_and_cache(self):
        for value in (b'', bytes(range(256))*16, os.urandom(131072)):
            self.write(b'binary', value)
            self.assertEqual(self.get(b'binary'), value)
        extent = ctypes.c_size_t()
        fd = self.api.lookup(self.handle, b'binary', len(b'binary'), ctypes.byref(extent))
        self.assertGreaterEqual(fd, 0)
        self.write(b'binary', b'replacement')
        self.assertEqual(self.get(b'binary'), b'replacement')
        self.close_cache()
        storage, data, size = self.load_record(fd, extent.value, b'binary')
        self.assertIsNotNone(storage)
        self.assertEqual(ctypes.string_at(data, size), value)
        self.assertEqual(len(os.listdir('/proc/self/fd')), self.initial_fds)

    def test_corrupt_or_symlink_records_are_misses_and_can_be_replaced(self):
        key, value = b'key', b'payload'
        self.write(key, value)
        path = next(p for p in self.cache_path.glob('*/*') if len(p.name) == 16)
        original = path.read_bytes()
        damaged = (b'', original[:-1], original+b'extra',
                   original[:24]+b'X'+original[25:], original[:-1]+b'X',
                   struct.pack('=3Q', 2**64-1, 2**64-1, 0))
        for content in damaged:
            with self.subTest(content=content):
                path.chmod(0o600)
                path.write_bytes(content)
                path.chmod(0o400)
                self.assertIsNone(self.get(key))
                self.write(key, value)
                self.assertEqual(self.get(key), value)
        outside = self.cache_path.parent/'outside'
        outside.write_bytes(original)
        path.unlink()
        path.symlink_to(outside)
        self.assertIsNone(self.get(key))
        self.write(key, value)
        self.assertFalse(path.is_symlink())
        self.assertEqual(self.get(key), value)
        self.assertEqual(outside.read_bytes(), original)

    def test_short_write_keeps_previous_complete_record(self):
        self.write(b'key', b'complete')
        pid = os.fork()
        if not pid:
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
            self.write(b'key', b'incomplete'*16384)
            os._exit(0)
        self.assertEqual(os.waitpid(pid, 0)[1], 0)
        self.assertEqual(self.get(b'key'), b'complete')
        self.assertFalse(list(self.cache_path.glob('*/.tmp-*')))

    def test_shared_snapshots_verify_empty_large_and_corrupt_contents(self):
        library = ctypes.CDLL(self.module.__file__)
        library.dcc_file_cache_put.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                               ctypes.c_int, ctypes.c_char_p]
        library.dcc_file_cache_put.restype = None
        library.dcc_file_cache_get.argtypes = library.dcc_file_cache_put.argtypes
        library.dcc_file_cache_get.restype = ctypes.c_int
        root = self.cache_path.parent
        for index, payload in enumerate((b'', bytes(range(256))*513)):
            with self.subTest(size=len(payload)):
                source, output = root/f'source-{index}', root/f'output-{index}'
                source.write_bytes(payload)
                os.utime(source, ns=(10**9, 10**9))
                digest = xxhash.xxh3_64_digest(payload)
                library.dcc_file_cache_put(digest, os.fsencode(source), 1, os.fsencode(root))
                self.assertEqual(library.dcc_file_cache_get(
                    digest, os.fsencode(output), 1, os.fsencode(root)), 1)
                self.assertEqual(output.read_bytes(), payload)
                self.assertEqual(output.stat().st_ino, source.stat().st_ino)
                self.assertEqual(output.stat().st_mtime_ns, 10**9)
                output.unlink()
                cached = next(self.cache_path.glob('*/'+digest.hex()))
                cached.chmod(0o600)
                cached.write_bytes(b'corrupt'+payload)
                cached.chmod(0o400)
                self.assertEqual(library.dcc_file_cache_get(
                    digest, os.fsencode(output), 1, os.fsencode(root)), 0)
                self.assertFalse(output.exists())

    def test_retained_shards_prune_after_fork_and_publish_atomic_snapshots(self):
        keys = self.shard_keys(80)
        payload = b'entry'.ljust(65536, b'\0')
        self.write(keys[0], payload)
        oldest = next(p for p in self.cache_path.glob('*/*') if len(p.name) == 16)
        # Eviction compares file ages; creation within one timestamp tick
        # does not establish an order between otherwise equal-age entries.
        os.utime(oldest, (1, 1))
        for key in keys[1:40]: self.write(key, payload)
        pid = os.fork()
        if not pid:
            for key in keys[40:]: self.write(key, payload)
            self.close_cache()
            os._exit(0)
        self.assertEqual(os.waitpid(pid, 0)[1], 0)
        self.assertIsNone(self.get(keys[0]))
        self.assertEqual(self.get(keys[-1]), payload)
        key, values = b'atomic', (b'A'*65536, b'B'*65536)
        self.write(key, values[0])
        children = []
        for value in values:
            pid = os.fork()
            if not pid:
                for _ in range(40): self.write(key, value)
                os._exit(0)
            children.append(pid)
        try:
            for _ in range(80): self.assertTrue(self.get(key) in values)
        finally:
            statuses = [os.waitpid(pid, 0)[1] for pid in children]
        self.assertEqual(statuses, [0, 0])
        self.assertTrue(self.get(key) in values)


class GccFrontendTest(BrokerTestCase):
    worker_address = '127.0.0.1'

    def start_frontend(self, backoff=60, **settings):
        self.module = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if not self.module:
            directory = subprocess.check_output([self.cc, '-print-file-name=plugin'], text=True).strip()
            self.module = str(Path(directory)/'distcc_gcc_scan.so')
        self.assertTrue(Path(self.module).is_file(), 'GCC native module must be built before testing')
        self.restart_server(self.build/'distccd', DISTCC_GCC_SCAN_LIBRARY=self.module, **settings)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), backoff=backoff)
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_VERBOSE='1',
                        DISTCC_GCC_PLAN_LIBRARY=str(self.build/'gcc_plan.so'),
                        DISTCC_TESTING_INCLUDE_SERVER='1',
                        DISTCC_GCC_SCAN_LIBRARY=self.module, DISTCC_SKIP_LOCAL_RETRY='1',
                        INCLUDE_SERVER_PORT='/nonexistent/analyzer.sock')

    def compile(self, command):
        log = Path(self.env['DISTCC_LOG']) if self.env.get('DISTCC_LOG') else None
        offset = log.stat().st_size if log and log.exists() else 0
        result = subprocess.run([str(self.build/'distcc'), *command], cwd=self.work,
                                env=self.env, capture_output=True, text=True, timeout=30)
        trace = log.read_bytes()[offset:].decode() if log else result.stderr
        self.assertEqual(result.returncode, 0, trace + '\n' + Path(self.logs[0].name).read_text())
        self.assertIn('pooled TCP job completed', trace)
        return result

    def test_native_gcc_compiles_without_local_dependency_scan(self):
        self.start_frontend()
        (self.work/'value.h').write_text('#define VALUE 37\n')
        self.source.write_text('#include <stdio.h>\n#include "value.h"\n'
                               'int main(void) { printf("%d %s\\n", VALUE, __FILE__); return 0; }\n')
        command = [self.cc, '-O2', '-march=native', '-c', 'hello.c', '-o', 'out.o', '-MMD', '-MF', 'out.d']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = [(self.work/name).read_bytes() for name in ('out.o', 'out.d')]
        for name in ('out.o', 'out.d'): (self.work/name).unlink()
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected[0])
        self.assertEqual((self.work/'out.d').read_bytes().replace(b'\\\n', b'').split(),
                         expected[1].replace(b'\\\n', b'').split())
        subprocess.run([self.cc, 'out.o', '-o', 'program'], cwd=self.work, check=True)
        self.assertEqual(subprocess.check_output([str(self.work/'program')]), b'37 hello.c\n')

    def test_driver_resolves_subprograms_with_masquerade_path(self):
        self.start_frontend()
        aliases = self.work/'distcc-masquerade'
        aliases.mkdir()
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        for name in ('gcc', 'g++', target+'-gcc', target+'-g++'):
            (aliases/name).symlink_to(self.build/'distcc')
        self.env['PATH'] = str(aliases)+os.pathsep+self.env['PATH']
        for compiler, source in (('gcc', '#include <stddef.h>\nint value;\n'),
                                 ('g++', '#include <vector>\nstd::vector<int> value;\n')):
            with self.subTest(compiler=compiler):
                self.source.write_text(source)
                real = shutil.which(compiler)
                assert real is not None
                args = ['-c', str(self.source), '-o', 'out.o']
                subprocess.run([real, *args], cwd=self.work, check=True, capture_output=True)
                expected = (self.work/'out.o').read_bytes()
                (self.work/'out.o').unlink()
                result = self.compile([compiler, *args])
                self.assertIn('reused GCC driver status=0', result.stderr)
                self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_frontend_replies_survive_short_writes_and_disconnect(self):
        # Exercise the client's bounded retries, without the broker's separate
        # host-quarantine delay after the injected disconnect.
        self.start_frontend(backoff=0)
        library = self.work/'send-faults.so'
        subprocess.run([self.cc, '-shared', '-fPIC', '-I'+str(self.build/'src'),
                        str(self.build/'test/tcp_send_faults.c'), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        header = self.work/'large.h'
        header.write_text('/*'+base64.b64encode(random.Random(91).randbytes(600000)).decode()+'*/\n')
        self.source.write_text('#include "large.h"\nint value = 42;\n')
        command = [self.cc, '-O2', '-c', str(self.source), '-o', 'out.o']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        marker = self.work/'send-events'
        self.env.update(LD_PRELOAD=str(library), DISTCC_TEST_SEND_LOG=str(marker))
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        self.assertTrue({'B', 'S'} <= set(marker.read_text()))
        # A new file version forces another file reply, even with a warm cache.
        with header.open('a') as text:
            text.write('/* changed */\n')
        marker.unlink()
        self.env['DISTCC_TEST_SEND_DROP'] = '1'
        failed = subprocess.run([str(self.build/'distcc'), *command], cwd=self.work,
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0, failed.stderr)
        self.assertIn('D', marker.read_text())
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        for name in ('LD_PRELOAD', 'DISTCC_TEST_SEND_LOG', 'DISTCC_TEST_SEND_DROP'):
            self.env.pop(name)
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_planner_does_not_open_the_source(self):
        self.start_frontend()
        self.source.write_text('#error only the remote frontend should read this\n')
        libc = ctypes.CDLL(None, use_errno=True)
        descriptor = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
        self.assertGreaterEqual(descriptor, 0, os.strerror(ctypes.get_errno()))
        self.addCleanup(os.close, descriptor)
        self.assertGreaterEqual(libc.inotify_add_watch(descriptor, os.fsencode(self.source), 0x20), 0)
        with os.fdopen(os.memfd_create('gcc-plan-no-read', os.MFD_CLOEXEC), 'w+b') as output:
            result = subprocess.run([self.cc, '-E', str(self.source), '-o', '/dev/null',
                                     '-fplugin='+self.module,
                                     f'-fplugin-arg-distcc_gcc_scan-plan-fd={output.fileno()}'],
                                    env=self.env, pass_fds=(output.fileno(),),
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            output.seek(0)
            self.assertEqual(output.read(4), b'ARGC')
        try:
            events = os.read(descriptor, 4096)
        except BlockingIOError:
            events = b''
        self.assertEqual(events, b'', 'search planning opened the source')

    def test_gcc_assembler_preprocessing_and_assembler_inputs_are_remote(self):
        self.start_frontend()
        (self.work/'values.h').write_text('#ifndef __ASSEMBLER__\n#error wrong language\n#endif\n#define VALUE 42\n')
        (self.work/'tail.inc').write_text('.long 7\n')
        (self.work/'bytes.bin').write_bytes(struct.pack('=I', 9))
        (self.work/'values.S').write_text('#include "values.h"\n.data\n.globl values\n'
            'values:\n.long VALUE\n.include "tail.inc"\n.incbin "bytes.bin"\n')
        command = [self.cc, '-march=native', '-mtune=native', '-c', 'values.S',
                   '-o', 'out.o', '-MMD', '-MF', 'out.d']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = [(self.work/name).read_bytes() for name in ('out.o', 'out.d')]
        result = self.compile(command)
        self.assertIn('frontend filesystem:', result.stderr)
        self.assertEqual((self.work/'out.o').read_bytes(), expected[0])
        self.assertEqual((self.work/'out.d').read_bytes().replace(b'\\\n', b'').split(),
                         expected[1].replace(b'\\\n', b'').split())
        (self.work/'check.c').write_text('extern unsigned values[]; int main(void) { '
                                       'return values[0]!=42 || values[1]!=7 || values[2]!=9; }\n')
        subprocess.run([self.cc, 'check.c', 'out.o', '-o', 'program'], cwd=self.work,
                       check=True, capture_output=True)
        subprocess.run([str(self.work/'program')], check=True)

    def test_assembler_explicit_language_sysroot_and_symlink_refresh(self):
        self.start_frontend()
        headers = self.work/'root/usr/include'
        headers.mkdir(parents=True)
        (self.work/'alias').symlink_to(headers, target_is_directory=True)
        names = ('part.c', 'part.i', 'part.s')
        for name in names:
            (self.work/name).write_text('#include <value.h>\n.data\n.globl value\nvalue:\n.long VALUE\n')
        for value in (41, 42):
            if value == 42:
                headers = self.work/'replacement'
                headers.mkdir()
                (self.work/'alias').unlink()
                (self.work/'alias').symlink_to(headers, target_is_directory=True)
            (headers/'value.h').write_text(f'#define VALUE {value}\n')
            for name in names:
                command = [self.cc, '-x', 'assembler-with-cpp', '--sysroot='+str(self.work/'root'),
                           '-Ialias', '-c', name, '-o', 'out.o']
                subprocess.run(command, cwd=self.work, check=True, capture_output=True)
                expected = (self.work/'out.o').read_bytes()
                result = self.compile(command)
                self.assertIn('frontend filesystem:', result.stderr)
                self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_remote_assembly_reports_preprocessor_and_assembler_errors(self):
        self.start_frontend()
        source = self.work/'broken.S'
        for contents, diagnostic in (('#include "missing.h"\n', 'missing.h'),
                                      ('.data\n.incbin "missing.bin"\n', 'missing.bin')):
            with self.subTest(diagnostic=diagnostic):
                source.write_text(contents)
                result = subprocess.run([str(self.build/'distcc'), self.cc, '-c', str(source),
                                         '-o', str(self.work/'out.o')], cwd=self.work,
                                        env=self.env, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)
                self.assertIn('pooled TCP job completed', result.stderr)
                self.assertNotIn('retrying locally', result.stderr)
                self.assertFalse((self.work/'out.o').exists())

    def test_assembly_stays_local_without_the_native_filesystem(self):
        self.start_frontend()
        (self.work/'part.s').write_text('.data\n.globl value\nvalue:\n.incbin "bytes.bin"\n')
        (self.work/'part.S').write_text('#define VALUE 41\n.data\n.globl value\nvalue:\n.long VALUE\n')
        (self.work/'part.i').write_bytes((self.work/'part.S').read_bytes())
        (self.work/'bytes.bin').write_bytes(struct.pack('=I', 42))
        for name, enabled, language in (('part.s', '1', []), ('part.S', '0', []),
                                       ('part.i', '0', ['-x', 'assembler-with-cpp'])):
            command = [self.cc, *language, '-c', name, '-o', 'out.o']
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            result = subprocess.run([str(self.build/'distcc'), *command], cwd=self.work,
                env=dict(self.env, DISTCC_FRONTEND_REMOTE=enabled), capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('pooled TCP job completed', result.stderr)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_planner_shares_loaded_gcc_and_isolates_fresh_search_paths(self):
        self.start_frontend()
        log = self.work/'planner.log'
        self.env['DISTCC_LOG'] = str(log)
        cases = []
        for index in range(8):
            directory = self.work/f'job-{index}'
            directory.mkdir()
            include = self.work/f'headers-{index}'
            include.mkdir()
            (include/'value.h').write_text(f'#define VALUE {index}\n')
            (directory/'source.c').write_text('#include <value.h>\nint answer(void) { return VALUE+ADD; }\n')
            command = [self.cc, '-c', 'source.c', '-o', 'out.o', '-nostdinc',
                       '-I'+str(include), f'-DADD={index+1}']
            subprocess.run(command, cwd=directory, check=True, capture_output=True)
            cases.append((directory, command, (directory/'out.o').read_bytes()))
        def compile_case(case):
            directory, command, expected = case
            # Make exports target-specific flags even though the driver has
            # already received the complete options in argv. These values
            # must not create another copy of the same loaded compiler image.
            environment = dict(self.env, CFLAGS=' '.join(command[1:]),
                               CXXFLAGS=' '.join(command[1:]),
                               CPPFLAGS=command[-1], LDFLAGS=str(directory))
            result = subprocess.run([str(self.build/'distcc'), *command], cwd=directory,
                                    env=environment, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((directory/'out.o').read_bytes(), expected)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_case, cases))
        text = log.read_text()
        self.assertEqual(text.count('started reusable GCC driver pid='), 1, text)
        self.assertEqual(text.count('reused GCC driver status=0'), len(cases), text)
        self.assertEqual(text.count('started reusable GCC planner pid='), 1, text)
        self.assertEqual(text.count('reused GCC planner status=0'), len(cases), text)
        self.assertEqual(text.count('pooled TCP job completed'), len(cases), text)
        # An earlier missing search directory must be considered on the next
        # call, even though the loaded compiler image is the same process.
        directory, command, _ = cases[0]
        first = self.work/'generated'
        command += ['-I'+str(first)]
        (directory/'source.c').write_text('#include <new.h>\nint answer(void) { return NEW; }\n')
        failed = subprocess.run([str(self.build/'distcc'), *command], cwd=directory,
                                env=self.env, capture_output=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0)
        first.mkdir()
        (first/'new.h').write_text('#define NEW 42\n')
        subprocess.run(command, cwd=directory, check=True, capture_output=True)
        compile_case((directory, command, (directory/'out.o').read_bytes()))
        self.assertEqual(log.read_text().count('started reusable GCC planner pid='), 1)
        self.assertEqual(log.read_text().count('started reusable GCC driver pid='), 1)

    def test_planner_failure_preserves_compiler_diagnostics(self):
        self.start_frontend()
        missing = self.work/'missing-distcc_gcc_scan.so'
        self.env.update(DISTCC_GCC_SCAN_LIBRARY=str(missing), DISTCC_FALLBACK='1')
        self.source.write_text('int answer(void) { return 42; }\n')
        result = subprocess.run([str(self.build/'distcc'), self.cc, '-c', str(self.source),
                                 '-o', 'out.o'], cwd=self.work, env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(missing), result.stderr)
        self.assertIn('failed during compiler execution', result.stderr)
        self.assertIn('failed to distribute '+str(self.source), result.stderr)
        self.assertNotIn('pooled TCP job completed', result.stderr)
        self.assertTrue((self.work/'out.o').is_file())

    def test_slow_planner_startup_keeps_owner_and_waiters(self):
        self.start_frontend()
        # ELF constructors finish before the retained GCC image reports ready.
        # Six seconds crosses the broker's former five-second lease expiry,
        # while staying within this client's configured startup timeout.
        delay = self.work/'startup.c'
        delay.write_text('#include <stdlib.h>\n#include <time.h>\n'
                         '__attribute__((constructor)) static void startup(void) {\n'
                         ' if (getenv("DISTCC_GCC_PLAN_CONTROL")) {\n'
                         '  struct timespec left = {6, 0};\n'
                         '  while (nanosleep(&left, &left)) {}\n'
                         ' }\n}\n')
        library = self.work/'startup.so'
        subprocess.run([self.cc, '-shared', '-fPIC', str(delay), '-o', str(library)],
                       check=True, capture_output=True)
        self.source.write_text('int answer(void) { return 42; }\n')
        command = [self.cc, '-c', str(self.source)]
        subprocess.run([*command, '-o', 'expected.o'], cwd=self.work,
                       check=True, capture_output=True)
        expected = (self.work/'expected.o').read_bytes()
        log = self.work/'startup.log'
        self.env.update(LD_PRELOAD=str(library), DISTCC_IO_TIMEOUT='20', DISTCC_LOG=str(log))
        def compile_case(number):
            result = subprocess.run([str(self.build/'distcc'), *command, '-o', f'{number}.o'],
                                    cwd=self.work, env=self.env, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr + log.read_text())
            self.assertEqual((self.work/f'{number}.o').read_bytes(), expected)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(compile_case, range(2)))
        compile_case(2)
        trace = log.read_text()
        self.assertEqual(trace.count('pooled TCP job completed'), 3)
        self.assertEqual(trace.count('started reusable GCC driver pid='), 1)
        self.assertEqual(trace.count('started reusable GCC planner pid='), 1)

    def test_planner_preserves_explicit_gcc_wrapper(self):
        self.start_frontend()
        wrapper = self.work/'wrap'
        marker = self.work/'wrapped'
        environment_marker = self.work/'wrapper-environment'
        wrapper.write_text(f'#!/bin/sh\necho "${{_DISTCC_SAFEGUARD:-unset}}" >> "{marker}"\n'
                           f'printf "%s\\n" "$CFLAGS" >> "{environment_marker}"\nexec "$@"\n')
        wrapper.chmod(0o755)
        self.source.write_text('int answer(void) { return 42; }\n')
        command = [self.cc, '-wrapper', str(wrapper), '-c', str(self.source), '-o', 'out.o']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        marker.unlink()
        environment_marker.unlink()
        logs = []
        observed = 0
        for value in ('first-request', 'second-request'):
            log = self.work/(value+'.log')
            self.env.update(CFLAGS=value, DISTCC_LOG=str(log))
            self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)
            values = environment_marker.read_text().splitlines()
            # Planning finishes before the remote compiler/as invocations.
            # Only the local driver receives this client build environment.
            self.assertEqual(values[observed], value)
            observed = len(values)
            logs.append(log.read_text())
        self.assertTrue(marker.read_text().strip())
        self.assertEqual(marker.read_text().splitlines()[0], '1')
        self.assertEqual(''.join(logs).count('started reusable GCC driver pid='), 1)
        self.assertTrue(all('reused GCC driver status=0' in log for log in logs))

    def test_planner_cancels_only_its_request_and_recovers_dead_parent(self):
        self.start_frontend()
        log = self.work/'planner.log'
        self.env['DISTCC_LOG'] = str(log)
        # A real GCC plugin holds one invocation before the plan completes.
        # It is loaded in the invocation child, never in the shared parent.
        marker = self.work/'entered'
        plugin = self.work/'hold.c'
        plugin.write_text('#include <stdio.h>\n#include <unistd.h>\n'
                          'int plugin_is_GPL_compatible;\n'
                          'int plugin_init(void *a, void *b) { '
                          f'FILE *f=fopen("{marker}.tmp","w"); '
                          'fprintf(f,"%d",getpid()); fclose(f); '
                          f'rename("{marker}.tmp","{marker}"); '
                          'for (;;) pause(); }\n')
        library = self.work/'hold.so'
        subprocess.run([self.cc, '-shared', '-fPIC', str(plugin), '-o', str(library)],
                       check=True, capture_output=True)
        frontend = subprocess.check_output([self.cc, '-print-prog-name=cc1'], text=True).strip()
        self.source.write_text('int answer(void) { return 42; }\n')
        with os.fdopen(os.memfd_create('gcc-plan-test', os.MFD_CLOEXEC), 'w+b') as output:
            assert self.module is not None
            command = [str(self.build/'distcc'), '--gcc-plan', frontend, '-E', '-quiet',
                       str(self.source), '-o', '/dev/null', '-fplugin='+self.module,
                       f'-fplugin-arg-distcc_gcc_scan-plan-fd={output.fileno()}']
            held = subprocess.Popen([*command, '-fplugin='+str(library)], env=self.env,
                                    pass_fds=(output.fileno(),), stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            def stop():
                if held.poll() is None: held.kill()
                held.wait(timeout=5)
            self.addCleanup(stop)
            deadline = time.monotonic()+10
            while not marker.exists():
                self.assertIsNone(held.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            child = int(marker.read_text())
            match = re.search(r'started reusable GCC planner pid=(\d+)', log.read_text())
            assert match is not None, log.read_text()
            parent = int(match[1])
            # Another job can complete while the first is blocked.
            result = subprocess.run(command, env=self.env, pass_fds=(output.fileno(),),
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(log.read_text().count('started reusable GCC planner pid='), 1)
            stop()
            deadline = time.monotonic()+5
            while Path(f'/proc/{child}').exists():
                self.assertLess(time.monotonic(), deadline, 'cancelled GCC remains alive')
                time.sleep(.01)
            os.kill(parent, signal.SIGKILL)
            deadline = time.monotonic()+5
            while Path(f'/proc/{parent}/stat').exists():
                if Path(f'/proc/{parent}/stat').read_text().rsplit(')', 1)[1].split()[0] == 'Z': break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            result = subprocess.run(command, env=self.env, pass_fds=(output.fileno(),),
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            output.seek(0)
            self.assertEqual(output.read(4), b'ARGC')
            self.assertEqual(log.read_text().count('started reusable GCC planner pid='), 2)

    def test_driver_cancellation_and_parent_replacement(self):
        self.start_frontend()
        log = self.work/'driver.log'
        self.env['DISTCC_LOG'] = str(log)
        marker, fifo, wrapper = (self.work/name for name in ('entered', 'gate', 'wrapper'))
        os.mkfifo(fifo)
        wrapper.write_text(f'#!/bin/sh\ntouch "{marker}"\nread value < "{fifo}"\nexec "$@"\n')
        wrapper.chmod(0o755)
        command = [self.cc, '-wrapper', str(wrapper), '-c', str(self.source), '-o', str(self.work/'out.o')]
        process = subprocess.Popen([str(self.build/'distcc'), *command], env=self.env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic()+10
            while not marker.exists():
                self.assertIsNone(process.poll(), log.read_text() if log.exists() else '')
                self.assertLess(time.monotonic(), deadline, 'driver did not reach wrapper')
                time.sleep(.01)
            match = re.search(r'started reusable GCC driver pid=(\d+)', log.read_text())
            assert match is not None
            parent = int(match[1])
            children = [int(value) for value in Path(f'/proc/{parent}/task/{parent}/children').read_text().split()]
            self.assertTrue(children)
            process.kill(); process.wait(timeout=5)
            while any(Path(f'/proc/{child}').exists() for child in children):
                self.assertLess(time.monotonic(), deadline, 'cancelled driver remains')
                time.sleep(.01)
            self.assertTrue(Path(f'/proc/{parent}').exists(), 'cancellation killed shared driver')
            self.compile([self.cc, '-c', str(self.source), '-o', 'next.o'])
            self.assertEqual(log.read_text().count('started reusable GCC driver pid='), 1)
            os.kill(parent, signal.SIGKILL)
            deadline = time.monotonic()+5
            while Path(f'/proc/{parent}/stat').exists():
                if Path(f'/proc/{parent}/stat').read_text().rsplit(')', 1)[1].split()[0] == 'Z': break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            self.compile([self.cc, '-c', str(self.source), '-o', 'replacement.o'])
            self.assertEqual(log.read_text().count('started reusable GCC driver pid='), 2)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)

    def test_script_driver_keeps_interpreter_arguments(self):
        self.start_frontend()
        log = self.work/'script-driver.log'
        self.env['DISTCC_LOG'] = str(log)
        script = self.work/'wrapped-gcc'
        script.write_text('#!/bin/sh\nexec '+shlex.quote(self.cc)+' "$@"\n')
        script.chmod(0o755)
        command = [str(script), '-c', str(self.source), '-o', 'out.o']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        self.assertNotIn('started reusable GCC driver pid=', log.read_text())
        self.assertIn('pooled TCP job completed', log.read_text())

    def test_cpp_search_classes_macros_symlinks_and_warm_cache(self):
        self.start_frontend(DISTCCD_FILE_CACHE=str(self.work/'cache'))
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        headers = self.work/'headers'
        (headers/'inner').mkdir(parents=True)
        (self.work/'alias').symlink_to('headers/inner')
        (headers/'value.h').write_text('#define VALUE 39\n')
        (self.work/'extra.h').write_text('#define EXTRA 3\n')
        (self.work/'hello.cc').write_text(
            '#include <vector>\n#include <string>\n'
            'enum { seed = __COUNTER__ };\n'
            '#if __COUNTER__ == 1 && __has_include("alias/../value.h")\n'
            '#include "alias/../value.h"\n#else\n#error lost macro state\n#endif\n'
            'int main() { std::vector<std::string> v{std::to_string(VALUE+EXTRA)}; '
            'return v[0] != "42"; }\n')
        command = [compiler, '-O2', '-c', 'hello.cc', '-o', 'out.o', '-include', 'extra.h',
                   '-MMD', '-MF', 'out.d']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        for _ in range(2):
            (self.work/'out.o').unlink()
            self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)
            subprocess.run([compiler, 'out.o', '-o', 'program'], cwd=self.work, check=True)
            subprocess.run([str(self.work/'program')], check=True)

    @unittest.skipUnless(shutil.which('strace'), 'strace required')
    def test_directory_hints_follow_source_context_and_share_names_and_bodies(self):
        self.start_frontend()
        headers = self.work/'headers'
        headers.mkdir()
        for index in range(200):
            (headers/f'first-{index}.h').write_text(f'#define FIRST_{index} {index}\n')
        (headers/'common.h').write_text('#define COMMON 7\n')
        (headers/'second.h').write_text('#define SECOND 13\n')
        for name in ('first', 'second'):
            (self.work/name).mkdir()
        (self.work/'first/part.c').write_text(
            ''.join(f'#include <first-{index}.h>\n' for index in range(200)) +
            '#include <common.h>\nint value(void) { return FIRST_0 + COMMON; }\n')
        (self.work/'second/part.c').write_text(
            '#include <second.h>\n#include <common.h>\n'
            'int value(void) { return SECOND + COMMON; }\n')
        command = [self.cc, '-nostdinc', '-Iheaders', '-O2', '-c', 'first/part.c', '-o', 'out.o']
        first = self.compile(command)
        second = [*command[:5], 'second/part.c', *command[6:]]
        subprocess.run(second, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        trace = self.work/'contexts.trace'
        result = subprocess.run(['strace', '-f', '-yy', '-e',
                                 'trace=newfstatat,statx,getdents64,openat', '-o', str(trace),
                                 str(self.build/'distcc'), *second], cwd=self.work,
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        lines = trace.read_text().splitlines()
        metadata = '\n'.join(line for line in lines if 'newfstatat(' in line or 'statx(' in line)
        self.assertEqual(metadata.count('first-'), 0, 'another source context expanded this metadata batch')
        listings = '\n'.join(line for line in lines if 'getdents64(' in line)
        self.assertNotIn(f'<{headers}>', listings, 'directory names stopped being shared')
        reads = '\n'.join(line for line in lines if 'openat(' in line)
        self.assertNotIn('common.h', reads, 'an unchanged shared header was transferred again')
        again = self.compile(command)
        counts = [int(n) for n in re.findall(r'frontend filesystem: (\d+) requests',
                                             first.stderr + again.stderr)]
        self.assertEqual(len(counts), 2)
        self.assertGreater(counts[0] - counts[1], 200,
                           'another source context discarded the first context\'s hints')

    def test_native_search_directories_use_bounded_batches(self):
        self.start_frontend()
        directories = [self.work/'search'/str(index)/'nested'/'leaf' for index in range(256)]
        for directory in directories:
            directory.mkdir(parents=True)
        header = directories[-1]/'value.h'
        header.write_text('#define VALUE 41\n')
        (self.work/'part.c').write_text('#include <value.h>\nint value(void) { return VALUE; }\n')
        command = [self.cc, '-nostdinc', *('-I'+str(path) for path in directories),
                   '-O2', '-c', 'part.c', '-o', 'out.o']
        self.compile(command)
        for value in (41, 42):
            header.write_text(f'#define VALUE {value}\n')
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            result = self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)
            counts = re.findall(r'frontend filesystem: (\d+) requests', result.stderr)
            self.assertEqual(len(counts), 1)
            self.assertLess(int(counts[0]), len(directories),
                            'search directories still require individual round trips')

    def test_warm_directory_hints_batch_each_tree_level(self):
        self.start_frontend()
        # The long shared prefix makes the deepest 256 queries exceed the
        # 64 KiB request limit while the short child hint names still fit.
        headers = self.work/'headers'/('h' * 240)
        branches = 256
        for index in range(branches):
            directory = headers/f'branch{index}/nested'
            directory.mkdir(parents=True)
            (directory/'value.h').write_text(f'#define VALUE_{index} {index + 1}\n')
        counts = []
        for name, width in (('narrow', 1), ('wide', 12), ('split', branches)):
            directory = self.work/name
            directory.mkdir()
            (directory/'part.c').write_text(
                ''.join(f'#include <branch{i}/nested/value.h>\n' for i in range(width)) +
                'int value(void) { return ' + '+'.join(f'VALUE_{i}' for i in range(width)) + '; }\n')
            command = [self.cc, '-nostdinc', '-I'+str(headers), '-O2', '-c', f'{name}/part.c', '-o', 'out.o']
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            self.compile(command)
            warm = self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)
            requests = re.search(r'frontend filesystem: (\d+) requests', warm.stderr)
            assert requests is not None, warm.stderr
            counts.append(int(requests[1]))
        # Both trees have the same depth. Their previously observed directories
        # should require the same round trips regardless of the branch count.
        self.assertEqual(counts[0], counts[1], f'width-dependent directory waits: {counts}')
        self.assertGreater(counts[2], counts[1], 'fixture did not reach request splitting')
        self.assertLess(counts[2], branches, 'split batches reverted to per-directory requests')
        print(f'Warm directory tree RPC counts (1, 12, 256 branches): {counts}')
        nested = headers/'branch0/nested'
        for action in ('modify', 'symlink'):
            if action == 'modify':
                (nested/'value.h').write_text('#define VALUE_0 73\n')
            else:
                nested.rename(nested.with_name('moved'))
                nested.symlink_to('moved', target_is_directory=True)
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected, action)

    def test_directory_batch_replies_and_failed_prefetch_release_mount(self):
        compiler = shutil.which('c++')
        assert compiler is not None, 'C++ compiler required'
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'fuse3'], text=True))
        executable = self.work/'gcc-files'
        result = subprocess.run([compiler, '-std=c++17', '-I'+str(self.build), '-I'+str(self.build/'src'),
                                 str(self.build/'test/gcc_files_contract.cc'), *flags, '-pthread',
                                 '-o', str(executable)], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(executable), str(self.work)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which('strace'), 'strace required')
    def test_directory_hints_skip_unused_attributes_and_refresh_each_job(self):
        self.start_frontend()
        headers = self.work/'headers'
        headers.mkdir()
        for index in range(200):
            (headers/f'unused-{index}.h').write_text('#error unused header\n')
        used = headers/'used.h'
        used.write_text('#define VALUE 17\n')
        generated = headers/'generated.h'
        self.source.write_text('#include <used.h>\n#if __has_include(<generated.h>)\n'
                               '#include <generated.h>\n#else\n#define EXTRA 0\n#endif\n'
                               'int value(void) { return VALUE + EXTRA; }\n')
        command = [self.cc, '-nostdinc', '-Iheaders', '-O2', '-c', 'hello.c', '-o', 'out.o']
        cold = self.compile(command)
        trace = self.work/'metadata.trace'
        result = subprocess.run(['strace', '-f', '-yy', '-e', 'trace=newfstatat,statx,getdents64', '-o', str(trace),
                                 str(self.build/'distcc'), *command], cwd=self.work,
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        self.assertNotIn('unused-', trace.read_text())
        self.assertIn('used.h', trace.read_text())
        listings = '\n'.join(line for line in trace.read_text().splitlines() if 'getdents64(' in line)
        self.assertNotIn(f'<{headers}>', listings, 'unchanged header directory was enumerated again')
        counts = [int(n) for n in re.findall(r'frontend filesystem: (\d+) requests',
                                             cold.stderr + result.stderr)]
        self.assertEqual(len(counts), 2)
        # Reusing the two file bodies alone saves two READs. Shared hints must
        # also avoid their individual metadata round trips under default setup.
        self.assertGreater(counts[0] - counts[1], 2, 'directory hints did not survive job cleanup')
        for action in ('create', 'modify', 'replace', 'delete', 'directory'):
            previous = headers.stat()
            if action == 'create': generated.write_text('#define EXTRA 25\n')
            elif action == 'modify': used.write_text('#define VALUE 19\n')
            elif action == 'replace':
                (headers/'replacement.h').write_text('#define VALUE 23\n')
                os.replace(headers/'replacement.h', used)
            elif action == 'delete': generated.unlink()
            else:
                headers.rename(self.work/'old-headers')
                headers.mkdir()
                used.write_text('#define VALUE 43\n')
            # Restoring mtime must not make a changed directory look unchanged.
            os.utime(headers, ns=(previous.st_atime_ns, previous.st_mtime_ns))
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected, action)

    @unittest.skipIf(os.geteuid() == 0, 'root bypasses directory read permission')
    def test_cached_directory_names_preserve_traversal_and_file_permissions(self):
        self.start_frontend()
        headers = self.work/'headers'
        headers.mkdir()
        used = headers/'used.h'
        used.write_text('#define VALUE 17\n')
        self.source.write_text('#include <used.h>\nint value(void) { return VALUE; }\n')
        command = [self.cc, '-nostdinc', '-Iheaders', '-c', 'hello.c', '-o', 'out.o']
        self.compile(command)
        self.addCleanup(headers.chmod, 0o700)
        headers.chmod(0o100)
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        used.chmod(0)
        local = subprocess.run(command, cwd=self.work, capture_output=True, text=True)
        self.assertNotEqual(local.returncode, 0)
        remote = subprocess.run([str(self.build/'distcc'), *command], cwd=self.work,
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(remote.returncode, 0, remote.stderr)

    def test_parallel_directory_hints_preserve_changed_inputs(self):
        self.start_frontend()
        headers = self.work/'headers'
        headers.mkdir()
        for phase in range(2):
            commands, expected = [], []
            for name, value in (('first', 17+phase), ('second', 42-phase)):
                (headers/(name+'.h')).write_text(f'#define VALUE {value}\n')
                (self.work/(name+'.c')).write_text(
                    f'#include "headers/{name}.h"\nint {name}(void) {{ return VALUE; }}\n')
                command = [self.cc, '-nostdinc', '-O2', '-c', name+'.c', '-o', name+'.o']
                subprocess.run(command, cwd=self.work, check=True, capture_output=True)
                expected.append((self.work/(name+'.o')).read_bytes())
                commands.append(command)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(self.compile, commands))
            self.assertEqual(len(results), 2)
            for name, content in zip(('first', 'second'), expected):
                self.assertEqual((self.work/(name+'.o')).read_bytes(), content)

    def test_source_diagnostic_is_produced_by_remote_gcc(self):
        self.start_frontend()
        self.source.write_text('#error remote frontend reached\n')
        result = subprocess.run([str(self.build/'distcc'), self.cc, '-c', 'hello.c', '-o', 'out.o'],
                                cwd=self.work, env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('remote frontend reached', result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        self.assertNotIn('gave up analyzing', result.stderr)
        self.assertFalse((self.work/'out.o').exists())

    def test_default_cache_survives_jobs_and_refreshes_changed_headers(self):
        self.start_frontend()
        header = self.work/'value.h'
        header.write_text('#define VALUE 41\n/*' + 'header contents\n' * 8192 + '*/\n')
        self.source.write_text('#include "value.h"\nint value(void) { return VALUE; }\n')
        command = [self.cc, '-nostdinc', '-c', 'hello.c', '-o', 'out.o']

        def compile_and_measure():
            result = self.compile(command)
            match = re.search(r'frontend filesystem: (\d+) requests, (\d+) bytes', result.stderr)
            self.assertIsNotNone(match, result.stderr)
            assert match is not None
            return tuple(map(int, match.groups()))

        cold = compile_and_measure()
        expected = (self.work/'out.o').read_bytes()
        warm = compile_and_measure()
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        self.assertLess(warm[0], cold[0], 'default cache did not survive job cleanup')
        self.assertGreater(cold[1] - warm[1], header.stat().st_size)

        # Equal size and restored mtime must not hide a changed header.
        before = header.stat()
        header.write_text(header.read_text().replace('VALUE 41', 'VALUE 42'))
        os.utime(header, ns=(before.st_atime_ns, before.st_mtime_ns))
        changed = compile_and_measure()
        self.assertGreater(changed[1] - warm[1], before.st_size)
        actual = (self.work/'out.o').read_bytes()
        self.assertNotEqual(actual, expected)
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        self.assertEqual(actual, (self.work/'out.o').read_bytes())
        refreshed = compile_and_measure()
        self.assertLess(refreshed[0], changed[0])

    def test_environment_search_path_preserves_user_header_dependencies(self):
        self.start_frontend()
        headers = self.work/'environment'
        headers.mkdir()
        (headers/'value.h').write_text('#define VALUE 11\nconst char *header_path = __FILE__;\n')
        self.source.write_text('#include <value.h>\nint value(void) { return VALUE; }\n')
        self.env['CPATH'] = 'environment/../environment'
        command = [self.cc, '-c', 'hello.c', '-o', 'out.o', '-MMD', '-MF', 'out.d']
        subprocess.run(command, cwd=self.work, env=self.env, check=True, capture_output=True)
        expected = (self.work/'out.d').read_text().replace('\\\n', '').split()
        expected_object = (self.work/'out.o').read_bytes()
        (self.work/'out.o').unlink()
        self.compile(command)
        self.assertEqual((self.work/'out.d').read_text().replace('\\\n', '').split(), expected)
        self.assertEqual((self.work/'out.o').read_bytes(), expected_object)

    def test_external_symlink_and_hardlink_header_identity(self):
        self.start_frontend()
        external = self.root/(self._testMethodName+'-headers')
        external.mkdir()
        (external/'once.h').write_text('#pragma once\nstruct once { int value; };\n')
        os.link(external/'once.h', external/'again.h')
        (self.work/'headers').symlink_to(external)
        self.source.write_text('#include "headers/once.h"\n#include "headers/again.h"\n'
                               'int main(void) { struct once x = {0}; return x.value; }\n')
        command = [self.cc, '-O2', '-c', 'hello.c', '-o', 'out.o']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        (self.work/'out.o').unlink()
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_precompiled_header_and_cache_invalidation_use_native_gcc(self):
        self.start_frontend()
        header = self.work/'prefix.h'
        header.write_text('#define VALUE 41\n')
        subprocess.run([self.cc, '-x', 'c-header', 'prefix.h', '-o', 'prefix.h.gch'],
                       cwd=self.work, check=True, capture_output=True)
        header.unlink()
        self.source.write_text('int value(void) { return VALUE; }\n')
        command = [self.cc, '-include', 'prefix.h', '-Winvalid-pch', '-c', 'hello.c', '-o', 'out.o']
        subprocess.run(command, cwd=self.work, check=True, capture_output=True)
        expected = (self.work/'out.o').read_bytes()
        (self.work/'out.o').unlink()
        self.compile(command)
        self.assertEqual((self.work/'out.o').read_bytes(), expected)
        (self.work/'prefix.h.gch').unlink()
        for value in (42, 43):
            header.write_text(f'#define VALUE {value}\n')
            subprocess.run(command, cwd=self.work, check=True, capture_output=True)
            expected = (self.work/'out.o').read_bytes()
            (self.work/'out.o').unlink()
            self.compile(command)
            self.assertEqual((self.work/'out.o').read_bytes(), expected)

    def test_disconnect_reaps_gcc_and_filesystem_and_allows_next_job(self):
        self.start_frontend()
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        (self.work/'slow.cc').write_text('#include <regex>\nstd::regex expression("[a-z]+[0-9]*");\n')
        def descendants(pid):
            try:
                children = [int(p) for p in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
            except FileNotFoundError:
                return []
            return children + [child for parent in children for child in descendants(parent)]
        with (self.work/'cancel.log').open('wb') as log:
            process = subprocess.Popen([str(self.build/'distcc'), compiler, '-O2', '-c', 'slow.cc', '-o', 'slow.o'],
                                       cwd=self.work, env=self.env, stdout=log, stderr=log)
            stopped = []
            try:
                deadline = time.monotonic()+10
                while not stopped:
                    self.assertIsNone(process.poll(), log.name)
                    self.assertLess(time.monotonic(), deadline)
                    for child in descendants(self.daemons[0].pid):
                        try:
                            if Path(f'/proc/{child}/comm').read_text().strip() == 'cc1plus':
                                os.kill(child, signal.SIGSTOP)
                                stopped.append(child)
                        except (FileNotFoundError, ProcessLookupError):
                            pass
                    if not stopped: time.sleep(.002)
                process.kill(); process.wait(timeout=5)
                deadline = time.monotonic()+5
                while any(Path(f'/proc/{child}').exists() for child in stopped):
                    self.assertLess(time.monotonic(), deadline, 'cancelled GCC child remains')
                    time.sleep(.01)
                while 'dcc_busy 0\n' not in self.get_stats(self.stats_ports[0]):
                    self.assertLess(time.monotonic(), deadline, 'cancelled job retains worker capacity')
                    time.sleep(.01)
                self.assertFalse(list(self.root.glob('distcc-tcp-*/files')))
            finally:
                if process.poll() is None: process.kill()
                process.wait(timeout=5)
                for child in stopped:
                    try: os.kill(child, signal.SIGCONT)
                    except ProcessLookupError: pass
        self.compile([self.cc, '-c', 'hello.c', '-o', 'next.o'])


class FrontendTest(BrokerTestCase):
    worker_address = '127.0.0.2'

    def start_tcp(self):
        return self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))

    def start_frontend(self, jobs=4, **settings):
        clang = shutil.which('clang')
        if not clang:
            self.skipTest('Clang unavailable')
        self.restart_server(self.build / 'distccd', DISTCC_CLANG_DRIVER=str(self.build / 'distcc-clang'),
                            DISTCCD_FILE_CACHE=str(self.work/'cache'), jobs=jobs, **settings)
        self.start_tcp()
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_VERBOSE='1',
                        DISTCC_CLANG_LOCAL='1', PATH=str(self.build)+os.pathsep+self.env['PATH'],
                        DISTCC_SKIP_LOCAL_RETRY='1', INCLUDE_SERVER_PORT='/nonexistent/analyzer.sock')
        return clang

    def test_frontend_preparation_shares_resident_compiler_and_isolates_requests(self):
        clang = self.start_frontend()
        log = self.work/'client.log'
        self.env['DISTCC_LOG'] = str(log)
        # A local preprocessing command and concurrent remote preparations use
        # one library owner. Each preparation must retain its own cwd/macros.
        query = subprocess.run([str(self.build/'distcc'), clang, '-E', '-P', '-x', 'c', '-'],
                               input=b'23\n', env=self.env, capture_output=True, timeout=30)
        self.assertEqual((query.returncode, query.stdout.strip()), (0, b'23'), query.stderr)
        def compile(index):
            directory = self.work/str(index)
            directory.mkdir()
            (directory/'value.h').write_text(f'#define VALUE {index}\n')
            (directory/'source.c').write_text('#include "value.h"\nint main(void) { return VALUE != EXPECTED; }\n')
            command = [clang, '-O2', '-c', 'source.c', '-o', 'out.o', '-MMD', '-MF', 'out.d',
                       f'-DEXPECTED={index}']
            subprocess.run(command, cwd=directory, check=True, capture_output=True, timeout=30)
            expected = [(directory/name).read_bytes() for name in ('out.o', 'out.d')]
            for name in ('out.o', 'out.d'): (directory/name).unlink()
            result = subprocess.run([str(self.build/'distcc'), *command], cwd=directory,
                                    env=self.env, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual([(directory/name).read_bytes() for name in ('out.o', 'out.d')], expected)
            subprocess.run([clang, 'out.o', '-o', 'out'], cwd=directory, check=True, capture_output=True)
            subprocess.run([str(directory/'out')], check=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile, range(8)))
        text = log.read_text()
        self.assertEqual(text.count('started local compiler libraries'), 1)
        self.assertEqual(text.count('operation=P'), 8, '\n'.join(
            line for line in text.splitlines() if 'compiler libraries' in line or 'frontend' in line))
        self.assertIn('operation=C', text)
        self.assertEqual(text.count('frontend filesystem:'), 8)

    def test_remote_codegen_shares_ipc_and_preserves_cpp_results(self):
        self.check_remote_codegen(4)

    @unittest.skipUnless(os.uname().machine in ('x86_64', 'i686'), 'x86 target required')
    def test_remote_codegen_preserves_32bit_shared_library_calls(self):
        self.check_remote_codegen(4, shared=True)

    def test_remote_codegen_respects_single_cpu_limit(self):
        self.check_remote_codegen(1)

    def test_remote_codegen_uses_requested_llvm_release(self):
        compiler = shutil.which('clang-22')
        if not compiler: self.skipTest('Alternate LLVM release unavailable')
        self.check_remote_codegen(4, compiler)

    def test_remote_codegen_disconnect_reaps_partitions_and_releases_capacity(self):
        # Use the host's real lanes for this lifecycle test. Capping the daemon
        # at four slots can leave no spare lane under unrelated desktop load.
        clang = self.start_frontend(jobs=len(os.sched_getaffinity(0)),
                                   DISTCC_CLANG_CODEGEN='1', DISTCC_VERBOSE='1')
        trace_offset = Path(self.logs[0].name).stat().st_size
        source, output = self.work/'cancel-codegen.c', self.work/'cancel-codegen.o'
        source.write_text('\n'.join(
            f'unsigned f{i}(unsigned n) {{ '
            '__asm__ volatile(".rept 100000\\nnop\\n.endr\\n"); '
            f'for (unsigned j=0;j<100;j++) n=n*33+j+{i}; return n; }}' for i in range(8)))
        command = [str(self.build/'distcc'), clang, '-O3', '-c', str(source), '-o', str(output)]
        warm = subprocess.run(command, env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(warm.returncode, 0, warm.stderr)
        trace = Path(self.logs[0].name).read_bytes()[trace_offset:].decode()
        helpers = re.findall(r'started reusable frontend pid=(\d+)', trace)
        self.assertEqual(len(helpers), 1)
        def children(pid):
            try: return [int(value) for value in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
            except FileNotFoundError: return []
        output.unlink()
        with (self.work/'cancel-codegen.log').open('wb') as log:
            process = subprocess.Popen(command, env=self.env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic()+10
                parents, partitions = [], []
                while not partitions:
                    self.assertIsNone(process.poll(), 'parallel backend was not reached')
                    self.assertLess(time.monotonic(), deadline)
                    parents = children(int(helpers[0]))
                    partitions = [child for parent in parents for child in children(parent)]
                    time.sleep(.001)
                # Hold an actual backend child to prove that cancellation
                # reaps descendants, rather than merely observing a fast exit.
                for child in partitions: os.kill(child, signal.SIGSTOP)
                self.assertIn('dcc_busy 2\n', self.get_stats(self.stats_ports[0]))
                process.kill(); process.wait(timeout=5)
                deadline = time.monotonic()+5
                while True:
                    alive = []
                    for child in parents + partitions:
                        try:
                            if Path(f'/proc/{child}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z':
                                alive.append(child)
                        except FileNotFoundError: pass
                    state = self.get_stats(self.stats_ports[0])
                    if not alive and 'dcc_busy 0\n' in state: break
                    self.assertLess(time.monotonic(), deadline, (alive, state))
                    time.sleep(.01)
                self.assertIn('dcc_memory_reserved_bytes 0\n', state)
                self.assertFalse(output.exists())
            finally:
                if process.poll() is None: process.kill()
                process.wait(timeout=5)
        recovered = subprocess.run(command, env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(recovered.returncode, 0, recovered.stderr)

    def check_remote_codegen(self, jobs, compiler=None, shared=False):
        clang = self.start_frontend(jobs=jobs, DISTCC_CLANG_CODEGEN='1', DISTCC_VERBOSE='1')
        if compiler: clang = compiler
        source = self.work/'partition.cpp'
        payload = self.work/'backend.bin'
        payload.write_bytes(random.Random(911).randbytes(256))
        asm = r'__asm__ volatile(".pushsection .rodata\n.incbin \"" PAYLOAD "\"\n.popsection\n");'
        source.write_text('#include <cstdio>\n#include <vector>\n'
                          f'#define PAYLOAD "{payload}"\n'
                          'static std::vector<unsigned> values{3,7};\nthread_local unsigned bias=2;\n' +
                          '\n'.join(f'__attribute__((noinline)) unsigned f{i}(unsigned n) {{ {asm} '
                                    f'for (unsigned j=0;j<100;j++) n=n*33+j+{i}; '
                                    'try { if (n&1) throw n; return n+values[0]+bias; } '
                                    'catch (unsigned v) { return v+values[1]; }}' for i in range(8)) +
                          '\nint main() { std::printf("%u\\n", ' +
                          '+'.join(f'f{i}(2)' for i in range(8)) + '); return 0; }\n')
        flags = ['-m32', '-fPIC', '-fno-semantic-interposition'] if shared else []
        if shared:
            source.write_text(source.read_text().replace('int main()', 'int entry()'))
            (self.work/'main.cpp').write_text('int entry(); int main() { return entry(); }\n')
        output, deps = self.work/'partition.o', self.work/'partition.d'
        command = [clang, '--driver-mode=g++', *flags, '-O3', '-g', '-ffunction-sections', '-fdata-sections',
                   '-MMD', '-MF', str(deps), '-c', str(source), '-o', str(output)]
        executable = self.work/'partition'
        def result():
            link_input = [str(output)]
            if shared:
                linked = subprocess.run([clang, '--driver-mode=g++', *flags, '-fuse-ld=lld',
                                         '-shared', '-Wl,-z,text', str(output), '-o', 'libpartition.so'],
                                        cwd=self.work, capture_output=True, timeout=30)
                self.assertEqual(linked.returncode, 0, linked.stderr)
                link_input = ['main.cpp', '-L.', '-lpartition', '-Wl,-rpath,$ORIGIN']
            subprocess.run([clang, '--driver-mode=g++', *flags, *link_input, '-o', str(executable)],
                           cwd=self.work,
                           check=True, capture_output=True, timeout=30)
            return subprocess.check_output([str(executable)]), deps.read_bytes()
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        expected = result()
        warm = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                              capture_output=True, text=True, timeout=30)
        self.assertEqual(warm.returncode, 0, warm.stderr)
        self.assertIn('distcc codegen plan:', warm.stderr)
        self.assertNotIn('distcc codegen: ', warm.stderr)
        compiled = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        self.assertEqual(result(), expected)
        if jobs > 1: self.assertIn('distcc codegen: 2 parts', compiled.stderr)
        else: self.assertNotIn('distcc codegen: ', compiled.stderr)
        # Missing assembler input fails after the frontend, including when
        # multiple partitions share the same request/reply stream.
        output.unlink(); payload.unlink()
        failed = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0, failed.stderr)
        self.assertFalse(output.exists())
        payload.write_bytes(random.Random(911).randbytes(256))
        restored = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(restored.returncode, 0, restored.stderr)
        self.assertEqual(result(), expected)
        state = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_busy 0\n', state)
        self.assertIn('dcc_memory_reserved_bytes 0\n', state)

    def test_remote_frontend_advertises_its_learned_memory_class(self):
        clang = self.start_frontend(DISTCC_JOB_MEMORY='1536', DISTCC_MEMORY_LEARNING='1')
        source = self.work/'frontend-memory.c'
        source.write_text('int main(void) { return 0; }\n')
        instrumented, profile = self.work/'instrumented', self.work/'default.profdata'
        subprocess.run([clang, '-fprofile-generate='+str(self.work), str(source),
                        '-o', str(instrumented)], check=True, capture_output=True)
        subprocess.run([str(instrumented)], check=True)
        subprocess.run([str(Path(clang).parent/'llvm-profdata'), 'merge',
                        *map(str, self.work.glob('*.profraw')), '-o', str(profile)],
                       check=True, capture_output=True)
        before = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_frontend_memory 1610612736\n', before)
        for index in range(2):
            result = subprocess.run([str(self.build/'distcc'), clang, '-O3', '-c', str(source),
                                     '-fprofile-use='+str(profile),
                                     '-o', str(self.work/f'frontend-memory-{index}.o')],
                                    env=self.env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('frontend filesystem:', result.stderr)
        after = self.get_stats(self.stats_ports[0])
        match = re.search(r'dcc_frontend_memory (\d+)', after)
        assert match is not None, after
        estimate = int(match[1])
        self.assertGreaterEqual(estimate, 256 * 1024**2)
        self.assertLess(estimate, 1536 * 1024**2)
        self.assertIn('dcc_job_memory_samples 2\n', after)

    def test_frontend_preserves_driver_errors_during_configure_probes(self):
        clang = self.start_frontend()
        source = self.work/'probe.c'
        output = self.work/'probe.o'
        source.write_text('int answer(void) { return 42; }\n')
        for option in ('/wd4189', '-fno-such-distcc-test-option'):
            with self.subTest(option=option):
                command = [clang, '-c', str(source), option, '-o', str(output)]
                native = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(native.returncode, 0, native.stderr)
                distributed = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                             capture_output=True, text=True, timeout=30)
                self.assertNotEqual(distributed.returncode, 0, distributed.stderr)
                self.assertFalse(output.exists())

    def test_frontend_reuses_libraries_without_sharing_compiler_state(self):
        self.base_env['DISTCC_VERBOSE'] = '1'
        clang = self.start_frontend()
        trace_offset = Path(self.logs[0].name).stat().st_size
        source = self.work/'reused.c'
        output = self.work/'reused.o'
        dependencies = self.work/'reused.d'
        source.write_text('int answer(void) { return VALUE + sizeof(void*) + __COUNTER__; }\n')
        for language, abi, value in (('c', '-m64', 3), ('c++', '-m32', 9), ('c', '-m64', 12)):
            command = [clang, '-x', language, abi, '-O3', '-DVALUE='+str(value),
                       '-MMD', '-MF', str(dependencies), '-c', str(source), '-o', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            expected = output.read_bytes(), dependencies.read_bytes()
            result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('frontend filesystem:', result.stderr)
            self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)
        self.logs[0].flush()
        trace = Path(self.logs[0].name).read_bytes()[trace_offset:].decode()
        helpers = re.findall(r'started reusable frontend pid=(\d+)', trace)
        self.assertEqual(len(helpers), 1, trace[-6000:])
        self.assertEqual(len(re.findall(r'reused frontend status=', trace)), 3)
        usage = re.findall(r'job cost: .* cpu=([0-9.]+)', (self.work/'broker.log').read_text())
        self.assertEqual(len(usage), 3)
        self.assertTrue(all(float(value) > 0 for value in usage))
        sockets = [fd for fd in Path('/proc', helpers[0], 'fd').iterdir()
                   if os.readlink(fd).startswith('socket:')]
        self.assertEqual(len(sockets), 1, 'completed job filesystem descriptor was retained')
        broken = [arg for arg in command if not arg.startswith('-DVALUE=')]
        failed = subprocess.run([str(self.build/'distcc'), *broken], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0, failed.stderr)
        restored = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(restored.returncode, 0, restored.stderr)
        self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)
        # An idle compiler helper may die independently. Its next job must
        # start a replacement, rather than reuse a closed IPC connection.
        os.kill(int(helpers[0]), signal.SIGKILL)
        result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)

    def test_frontend_shares_parallel_slots_and_cancels_only_owner(self):
        clang = self.start_frontend(DISTCC_VERBOSE='1')
        trace_offset = Path(self.logs[0].name).stat().st_size
        sources = []
        for i in range(2):
            source = self.work/f'parallel-{i}.c'
            source.write_text('int main(void) { __asm__ volatile(".rept 1000000\\nnop\\n.endr\\n"); '
                              f'return VALUE != {i}; }}\n')
            sources.append(source)
        def command(i):
            return [str(self.build/'distcc'), clang, '-O2', f'-DVALUE={i}', '-c',
                    str(sources[i]), '-o', str(sources[i].with_suffix('.o'))]
        # Load the frontend once, then hold two actual compiler invocations to
        # prove concurrent use of distinct execution slots and the same helper.
        warm = subprocess.run(command(0), env=self.env, capture_output=True, timeout=30)
        self.assertEqual(warm.returncode, 0, warm.stderr)
        trace = Path(self.logs[0].name).read_bytes()[trace_offset:].decode()
        helpers = re.findall(r'started reusable frontend pid=(\d+)', trace)
        self.assertEqual(len(helpers), 1, trace[-6000:])
        helper = int(helpers[0])
        held, clients = [], []
        def children():
            return [int(value) for value in Path(f'/proc/{helper}/task/{helper}/children').read_text().split()]
        try:
            for i in range(2):
                process = subprocess.Popen(command(i), env=self.env, stdout=subprocess.DEVNULL,
                                           stderr=subprocess.PIPE)
                clients.append(process)
                deadline = time.monotonic()+10
                while True:
                    active = set(children())-set(held)
                    if active:
                        self.assertEqual(len(active), 1)
                        child = active.pop()
                        os.kill(child, signal.SIGSTOP)
                        held.append(child)
                        break
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.001)
            groups = [os.getpgid(pid) for pid in held]
            self.assertEqual(len(set(groups)), 2)
            daemon_children = Path(f'/proc/{self.daemons[0].pid}/task/{self.daemons[0].pid}/children').read_text().split()
            self.assertTrue(all(str(group) in daemon_children for group in groups), groups)
            clients[0].kill(); clients[0].wait(timeout=5)
            deadline = time.monotonic()+5
            while held[0] in children():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            self.assertIn(held[1], children())
            self.assertEqual(os.getpgid(held[1]), groups[1])
            os.kill(held[1], signal.SIGCONT)
            _, errors = clients[1].communicate(timeout=30)
            self.assertEqual(clients[1].returncode, 0, errors)
            self.assertIn(b'frontend filesystem:', errors)
            binary = self.work/'parallel-result'
            subprocess.run([clang, str(sources[1].with_suffix('.o')), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)
            trace = Path(self.logs[0].name).read_bytes()[trace_offset:].decode()
            self.assertEqual(len(re.findall(r'started reusable frontend pid=', trace)), 1, trace[-6000:])
            # Stop another real invocation before shutting down its daemon.
            # The helper must reap it before the input tree is reclaimed.
            active_client = subprocess.Popen(command(0), env=self.env, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.PIPE)
            clients.append(active_client)
            deadline = time.monotonic()+10
            while not (active := children()):
                self.assertIsNone(active_client.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.001)
            self.assertEqual(len(active), 1)
            os.kill(active[0], signal.SIGSTOP)
            held.append(active[0])
            self.restart_server(self.build/'distccd')
            self.assertFalse(Path(f'/proc/{helper}').exists())
            self.assertFalse(Path(f'/proc/{active[0]}').exists())
            # The client may retry after a daemon restart. Its cleanup below
            # cancels that retry after server-side reclamation was observed.
        finally:
            for pid in held:
                try: os.kill(pid, signal.SIGCONT)
                except ProcessLookupError: pass
            for process in clients:
                if process.poll() is None: process.kill()
                process.communicate(timeout=5)

    def test_frontend_keeps_requested_compilers_in_separate_helpers(self):
        alternate = shutil.which('clang-22')
        if not alternate: self.skipTest('Alternate LLVM release unavailable')
        clang = self.start_frontend(DISTCC_VERBOSE='1')
        trace_offset = Path(self.logs[0].name).stat().st_size
        source, output = self.work/'release.c', self.work/'release.o'
        source.write_text('int release(void) { return __clang_major__; }\n')
        for compiler in (clang, alternate, clang):
            command = [compiler, '-O2', '-c', str(source), '-o', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            expected = output.read_bytes()
            result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                    capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'frontend filesystem:', result.stderr)
            self.assertEqual(output.read_bytes(), expected)
        trace = Path(self.logs[0].name).read_bytes()[trace_offset:].decode()
        self.assertEqual(len(re.findall(r'started reusable frontend pid=', trace)), 2, trace[-6000:])

    def test_remote_frontend_default_dependency_path_follows_object_output(self):
        clang = self.start_frontend()
        source_dir, output_dir = self.work/'sources', self.work/'objects'
        source_dir.mkdir(); output_dir.mkdir()
        source = source_dir/'input.c'
        source.write_text('#include "value.h"\nint value(void) { return VALUE; }\n')
        (source_dir/'value.h').write_text('#define VALUE 42\n')
        for flags, joined, absolute in itertools.product(
                ([], ['-march=native', '-mtune=native', '-flto=thin']),
                (False, True), (False, True)):
            with self.subTest(flags=flags, joined=joined, absolute=absolute):
                output = output_dir/'input.c.26.o'
                deps = output.with_suffix('.d')
                name = str(output if absolute else output.relative_to(self.work))
                output_args = ['-o' + name] if joined else ['-o', name]
                command = [clang, *flags, '-MMD', '-c', 'sources/input.c', *output_args]
                subprocess.run(command, cwd=self.work, check=True, capture_output=True)
                expected = output.read_bytes(), deps.read_bytes()
                output.unlink(); deps.unlink()
                result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                        cwd=self.work, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('frontend filesystem:', result.stderr)
                self.assertTrue(deps.is_file(), 'Remote compile lost the default dependency path')
                self.assertEqual((output.read_bytes(), deps.read_bytes()), expected)

    def test_remote_frontend_preserves_warning_errors(self):
        clang = self.start_frontend(DISTCC_CLANG_CODEGEN='1', DISTCC_VERBOSE='1')
        source = self.work/'source.c'
        output = self.work/'source.o'
        for flags in ([], ['-flto=thin']):
            with self.subTest(flags=flags):
                source.write_text('int value(void) { return 42; }\n')
                command = [clang, *flags, '-c', 'source.c', '-o', str(output)]
                valid = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                       cwd=self.work, capture_output=True, timeout=30)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                self.assertIn(b'frontend filesystem:', valid.stderr)
                if not flags: self.assertIn(b'distcc codegen plan:', valid.stderr)
                source.write_text('#warning remote diagnostic must be an error\nint value;\n')
                command += ['-Werror']
                results = []
                for distributed in (False, True):
                    output.unlink(missing_ok=True)
                    result = subprocess.run(([str(self.build/'distcc')] if distributed else []) + command,
                                            env=self.env, cwd=self.work, capture_output=True, timeout=30)
                    self.assertIn(b'error: remote diagnostic must be an error', result.stderr)
                    if distributed: self.assertIn(b'frontend filesystem:', result.stderr)
                    results.append((result.returncode, output.exists()))
                self.assertEqual(results, [(1, False), (1, False)])

    def test_remote_frontend_owns_preprocessing_and_dependency_output(self):
        clang = self.start_frontend()
        include = self.work / 'headers'
        include.mkdir()
        header = include / 'value.h'
        forced = self.root / 'forced-frontend-headers'
        forced.mkdir()
        (forced / 'prelude.h').write_text('#define FORCED 2\n')
        (forced / 'macros.h').write_text('#define MACROS 3\n')
        source = self.work / 'frontend.cpp'
        source.write_text('#include <cstdio>\n#include "value.h"\n'
                          '#if __has_include("absent.h")\n#error absent header found\n#endif\n'
                          'constexpr int a = __COUNTER__;\nconstexpr int b = __COUNTER__;\n'
                          'int main() { std::printf("%s %s %d\\n", __FILE__, __BASE_FILE__, '
                          'VALUE + b - a + FORCED - 2 + MACROS - 3); }\n')
        output = self.work / 'frontend.o'
        dependencies = self.work / 'frontend.d'
        command = [clang, '-O3', '-march=native', '-I', str(include), '-MMD', '-MF', str(dependencies),
                   '-include', str(forced/'prelude.h'), '-imacros', str(forced/'macros.h'),
                   '-c', str(source), '-o', str(output)]
        for value in (7, 19):
            with self.subTest(value=value):
                header.write_text(f'#define VALUE {value}\n')
                subprocess.run(command, check=True, capture_output=True, timeout=30)
                expected = output.read_bytes(), dependencies.read_bytes()
                output.unlink(); dependencies.unlink()
                client_log = self.work / f'frontend-{value}.log'
                self.env['DISTCC_LOG'] = str(client_log)
                result = subprocess.run([str(self.build / 'distcc'), *command], env=self.env,
                                        capture_output=True, text=True, timeout=30)
                result.stderr += client_log.read_text()
                self.assertEqual(result.returncode, 0, result.stderr[-6000:])
                self.assertIn('frontend filesystem:', result.stderr)
                self.assertIn('pooled TCP job completed', result.stderr)
                self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)
                executable = self.work / 'frontend'
                subprocess.run([clang, str(output), '-o', str(executable)], check=True, capture_output=True)
                actual = subprocess.check_output([str(executable)], text=True).strip()
                self.assertEqual(actual, f'{source} {source} {value + 1}')
                first_bytes = int(re.findall(r'frontend filesystem: \d+ requests, (\d+) bytes', result.stderr)[-1])
                transfers = []
                for corrupt in (False, True):
                    if corrupt:
                        entries = [p for p in (self.work/'cache').glob('*/*') if len(p.name)==16]
                        self.assertTrue(entries, 'shared cache was not populated')
                        for entry in entries:
                            data = bytearray(entry.read_bytes()); data[-1] ^= 1
                            entry.chmod(0o600); entry.write_bytes(data); entry.chmod(0o400)
                    again = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                           capture_output=True, text=True, timeout=30)
                    self.assertEqual(again.returncode, 0, again.stderr[-5000:])
                    self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)
                    transferred = int(re.findall(r'frontend filesystem: \d+ requests, (\d+) bytes',
                                                 client_log.read_text())[-1])
                    transfers.append(transferred)
                if value == 7:
                    self.assertLess(transfers[0], first_bytes, 'unchanged headers were retransmitted')
                self.assertGreater(transfers[1], transfers[0], 'corrupt cache was accepted')

    def test_remote_frontend_skips_missing_candidates_outside_roots(self):
        clang = self.start_frontend()
        source_root = self.work/'project'/'source'
        (source_root/'deep').mkdir(parents=True)
        (source_root/'headers').mkdir()
        (source_root/'headers'/'value.h').write_text('#define VALUE 42\n')
        (source_root/'main.c').write_text('#include "../headers/value.h"\nint value(void) { return VALUE; }\n')
        output, dependencies = source_root/'main.o', source_root/'main.d'
        command = [clang, '-I', str(source_root/'deep'), '-O2', '-c', 'main.c',
                   '-o', str(output), '-MMD', '-MF', str(dependencies)]
        # The first candidate is missing outside the source root; a later -I
        # candidate finds the permitted header. A synthetic EACCES stops Clang.
        subprocess.run(command, check=True, capture_output=True, cwd=source_root)
        expected = output.read_bytes(), dependencies.read_bytes()
        output.unlink(); dependencies.unlink()
        result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                cwd=source_root, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        self.assertIn('frontend filesystem:', result.stderr)
        self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)

    def test_portage_frontend_exports_package_work_tree(self):
        clang = self.start_frontend()
        package = self.work/'package work'
        source_dir = package/'source'/'src'
        platform = package/'source'/'mkspecs'/'platform'
        common = platform.parent/'common'
        generated = package/'build'
        extra, private = self.work/'extra', self.work/'private'
        for directory in (source_dir, platform, common, generated, extra, private):
            directory.mkdir(parents=True, exist_ok=True)
        (common/'value.h').write_text('#define VALUE 42\n')
        (platform/'platform.h').write_text('#include "../common/value.h"\n')
        (private/'secret.h').write_text('#define SECRET 1\n')
        source = source_dir/'main.c'
        source.write_text('#include <platform.h>\nint value(void) { return VALUE; }\n')
        output, deps = generated/'main.o', generated/'main.d'
        command = [clang, '-I', str(platform), '-O2', '-c', str(source),
                   '-o', str(output), '-MMD', '-MF', str(deps)]
        subprocess.run(command, check=True, capture_output=True, cwd=generated)
        expected = output.read_bytes(), deps.read_bytes()
        hook = self.build/'packaging/gentoo/pump.bashrc'
        script = '''
has() { local word=$1; shift; [[ " $* " == *" $word "* ]]; }
register_die_hook() { :; }
die() { echo "$*" >&2; exit 1; }
source "$1"
shift
trap distcc_pump_stop EXIT
distcc_pump_start
distcc_pump_start
[[ $DISTCC_FRONTEND_ROOTS == "$EXPECTED_ROOTS" ]] || exit 98
"$@"
'''
        for pump in ('0', '1'):
            with self.subTest(pump=pump):
                output.unlink(missing_ok=True); deps.unlink(missing_ok=True)
                env = dict(self.env, WORKDIR=str(package), T=str(generated), FEATURES='distcc',
                           DISTCC_PUMP=pump, DISTCC_FRONTEND_ROOTS=str(extra),
                           EXPECTED_ROOTS=str(extra) + ':' + str(package),
                           PATH=str(self.build) + os.pathsep + self.env['PATH'])
                result = subprocess.run(['bash', '-c', script, 'portage-frontend', str(hook),
                                         str(self.build/'distcc'), *command],
                                        env=env, cwd=generated, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('frontend filesystem:', result.stderr)
                self.assertEqual((output.read_bytes(), deps.read_bytes()), expected)
        source.write_text('#include "../../../private/secret.h"\n')
        output.unlink()
        result = subprocess.run(['bash', '-c', script, 'portage-frontend', str(hook),
                                 str(self.build/'distcc'), *command],
                                env=env, cwd=generated, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Permission denied', result.stderr)
        self.assertFalse(output.exists())

    def test_remote_frontend_explicit_roots_support_generated_sources(self):
        clang = self.start_frontend()
        generated = self.work/'build'
        sources = self.work/'source tree'
        private = self.work/'unrelated'
        for directory in (generated, sources, private): directory.mkdir()
        (sources/'implementation.c').write_text('int value(void) { return 42; }\n')
        (private/'secret.h').write_text('#define SECRET 1\n')
        source = generated/'unified.c'
        source.write_text('#include "../source tree/implementation.c"\n')
        output, dependencies = generated/'unified.o', generated/'unified.d'
        command = [clang, '-O2', '-c', str(source), '-o', str(output),
                   '-MMD', '-MF', str(dependencies)]
        subprocess.run(command, check=True, capture_output=True, cwd=generated)
        expected = output.read_bytes(), dependencies.read_bytes()
        output.unlink(); dependencies.unlink()
        denied = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                cwd=generated, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(denied.returncode, 0)
        self.assertIn('Permission denied', denied.stderr)
        self.assertFalse(output.exists())
        for roots in (str(sources), ':../source tree/.:'):
            with self.subTest(roots=roots):
                result = subprocess.run([str(self.build/'distcc'), *command],
                                        env=dict(self.env, DISTCC_FRONTEND_ROOTS=roots),
                                        cwd=generated, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr[-6000:])
                self.assertIn('frontend filesystem:', result.stderr)
                self.assertEqual((output.read_bytes(), dependencies.read_bytes()), expected)
        source.write_text('#include "../unrelated/secret.h"\n')
        denied = subprocess.run([str(self.build/'distcc'), *command],
                                env=dict(self.env, DISTCC_FRONTEND_ROOTS=str(sources)),
                                cwd=generated, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(denied.returncode, 0)
        self.assertIn('Permission denied', denied.stderr)

    def test_remote_frontend_preserves_symlink_parent_and_trailing_slash(self):
        clang = self.start_frontend()
        (self.work/'physical'/'deep').mkdir(parents=True)
        (self.work/'alias').symlink_to('physical/deep', target_is_directory=True)
        (self.work/'value.h').write_text('#define VALUE 99\n')
        (self.work/'physical'/'value.h').write_text('#define VALUE 7\n')
        source = self.work/'paths.c'
        source.write_text('#include "alias/../value.h"\n'
                          '#if __has_include("value.h/")\n#error not a directory\n#endif\n'
                          'int main(void) { return VALUE != 7; }\n')
        output = self.work/'paths.o'
        command = [clang, '-O3', '-c', str(source), '-o', str(output)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        expected = output.read_bytes(); output.unlink()
        result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-5000:])
        self.assertIn('frontend filesystem:', result.stderr)
        self.assertEqual(output.read_bytes(), expected)
        executable = self.work/'paths'
        subprocess.run([clang, str(output), '-o', str(executable)], check=True, capture_output=True)
        subprocess.run([str(executable)], check=True, timeout=5)

    def test_remote_frontend_transfers_compressed_and_raw_chunk_tails(self):
        clang = self.start_frontend()
        source = self.work/'chunks.c'
        (self.work/'chunks.h').write_text('/*' + 'a'*(256*1024) +
                                        base64.b64encode(random.Random(821).randbytes(256*1024+13)).decode() + '*/\n')
        (self.work/'empty.h').write_bytes(b'')
        source.write_text('#include "chunks.h"\n#include "empty.h"\nint main(void) { return 0; }\n')
        output = self.work/'chunks.o'
        command = [clang, '-O3', '-c', str(source), '-o', str(output)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        expected = output.read_bytes(); output.unlink()
        transfers = []
        for _ in range(2):
            result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr[-5000:])
            self.assertIn('frontend filesystem:', result.stderr)
            self.assertEqual(output.read_bytes(), expected)
            transfers.append(int(re.findall(r'frontend filesystem: \d+ requests, (\d+) bytes',
                                            result.stderr)[-1]))
        self.assertLess(transfers[1], transfers[0])

    def test_frontend_buffers_share_storage_and_outlive_closed_files(self):
        clang = self.start_frontend()
        config = Path(clang).resolve().parent/'llvm-config'
        if not config.exists(): self.skipTest('LLVM development files unavailable')
        source = self.work/'buffer.cc'
        header = self.build/'include_server/c_extensions/remote_files.h'
        source.write_text(f'#include "{header}"\n' + r'''
int main(int argc, char **argv) {
    auto loaded = llvm::MemoryBuffer::getFile(argv[1]);
    auto info = llvm::vfs::getRealFileSystem()->status(argv[1]);
    if (!loaded || !info) return 1;
    std::shared_ptr<llvm::MemoryBuffer> storage(std::move(*loaded));
    std::weak_ptr<llvm::MemoryBuffer> lifetime(storage);
    auto file = std::make_unique<SnapshotFile>(*info, storage);
    auto first = file->getBuffer("first", -1, true, false);
    auto second = file->getBuffer("second", -1, true, false);
    if (!first || !second) return 2;
    if ((*first)->getBufferStart() != storage->getBufferStart() ||
        (*second)->getBufferStart() != storage->getBufferStart()) return 3;
    auto size = storage->getBufferSize();
    file->close(); file.reset(); storage.reset();
    if (lifetime.expired() || (*first)->getBufferSize() != size ||
        (*first)->getBufferIdentifier() != "first" ||
        (*second)->getBufferIdentifier() != "second" ||
        (*first)->getBufferStart()[size] != '\0') return 4;
    first->reset();
    if (lifetime.expired() || (*second)->getBuffer().count('\n') == 0) return 5;
    second->reset();
    return lifetime.expired() ? 0 : 6;
}
''')
        includedir = subprocess.check_output([config, '--includedir'], text=True).strip()
        libdir = subprocess.check_output([config, '--libdir'], text=True).strip()
        executable = self.work/'buffer'
        subprocess.run([clang, '--driver-mode=g++', '-std=c++17', str(source),
                        '-I'+includedir, '-L'+libdir, '-Wl,-rpath,'+libdir, '-lLLVM',
                        '-o', str(executable)], check=True, capture_output=True, timeout=30)
        subprocess.run([str(executable), str(source)], check=True, timeout=5)

    def test_remote_files_partial_ipc_cache_and_snapshot_ownership(self):
        clang = self.start_frontend()
        config = Path(clang).resolve().parent/'llvm-config'
        if not config.exists(): self.skipTest('LLVM development files unavailable')
        includedir = subprocess.check_output([config, '--includedir'], text=True).strip()
        libdir = subprocess.check_output([config, '--libdir'], text=True).strip()
        executable = self.work/'remote-files'
        subprocess.run([clang, '--driver-mode=g++', '-std=c++17',
                        str(self.build/'test/remote_files_contract.cc'), '-I'+str(self.build),
                        '-I'+includedir, '-L'+libdir, '-Wl,-rpath,'+libdir, '-lLLVM', '-pthread',
                        '-o', str(executable)], check=True, capture_output=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=10)

    def test_frontend_directory_snapshots_keep_freshness_and_permissions(self):
        if os.geteuid() == 0:
            self.skipTest('directory read permissions require an unprivileged caller')
        compiler = shutil.which('cc')
        assert compiler is not None, 'C compiler required for filesystem reply tests'
        executable = self.work/'metadata'
        compiled = subprocess.run([compiler, '-D_GNU_SOURCE', '-ffunction-sections', '-fdata-sections',
                        '-I'+str(self.build/'src'), str(self.build/'test/frontend_metadata_contract.c'),
                        str(self.build/'src/tcp_job.c'), str(self.build/'src/argutil.c'),
                        '-Wl,--gc-sections,--wrap=fstat', '-llz4', '-o', str(executable)],
                        capture_output=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        subprocess.run([str(executable), str(self.work)], check=True, timeout=5)

    def test_frontend_reply_stream_owns_open_file_and_checks_truncation(self):
        # Exercise the actual reply producer, chunk encoder and decoder,
        # including binary payloads that a C source fixture cannot contain.
        source = self.work/'reply.c'
        source.write_text(r'''
#include "tcp_client.c"
int main(int argc, char **argv) {
    if (argc < 4) return 1;
    char *roots[] = {argv[1], NULL};
    struct dcc_job job = {.fs_roots = roots};
    size_t length = strlen(argv[2]);
    int batch = !strncmp(argv[3], "batch", 5);
    int lstat_query = batch || !strcmp(argv[3], "lstat") || !strcmp(argv[3], "invalid") ||
                      !strcmp(argv[3], "unterminated");
    size_t payload = length + (lstat_query && strcmp(argv[3], "unterminated") ? 1+DCC_FS_METADATA : 0);
    for (int i = 4; i < argc; ++i) payload += strlen(argv[i])+1;
    unsigned char *query = malloc(payload + 4);
    unsigned operation = lstat_query ? DCC_FS_LSTAT : !strcmp(argv[3], "list") ? DCC_FS_LIST : DCC_FS_READ;
    dcc_tcp_put32(query, operation); memcpy(query + 4, argv[2], length);
    size_t offset = 4+length;
    if (lstat_query && strcmp(argv[3], "unterminated")) {
        memset(query+offset, 0, 1+DCC_FS_METADATA); offset += 1+DCC_FS_METADATA;
    }
    for (int i = 4; i < argc; ++i) {
        memcpy(query+offset, argv[i], strlen(argv[i])+1); offset += strlen(argv[i])+1;
    }
    if (batch) {
        size_t entry = payload+4, total = 4+2*(4+entry);
        unsigned char *combined = malloc(total+1);
        operation = DCC_FS_DIRECTORIES;
        dcc_tcp_put32(combined, operation);
        for (unsigned i = 0; i < 2; ++i) {
            size_t at = 4+i*(4+entry);
            dcc_tcp_put32(combined+at, entry);
            memcpy(combined+at+4, query, entry);
        }
        if (!strcmp(argv[3], "batch-mixed")) combined[4+(4+entry)+8+length-1] = '!';
        if (!strcmp(argv[3], "batch-invalid-short")) --total;
        if (!strcmp(argv[3], "batch-invalid-tail")) combined[total++] = 0;
        if (!strcmp(argv[3], "batch-invalid-size")) dcc_tcp_put32(combined+4, UINT_MAX);
        if (!strcmp(argv[3], "batch-invalid-kind")) dcc_tcp_put32(combined+8, DCC_FS_READ);
        if (!strcmp(argv[3], "batch-invalid-nested")) dcc_tcp_put32(combined+8, DCC_FS_DIRECTORIES);
        if (!strcmp(argv[3], "batch-invalid-empty")) total = 4;
        free(query); query = combined; payload = total-4;
    }
    struct dcc_frontend_reply reply = {.fd = -1};
    int rejected = dcc_frontend_fs_reply(&job, query, payload + 4, &reply);
    free(query);
    if (!strcmp(argv[3], "invalid") || !strcmp(argv[3], "unterminated") ||
        !strncmp(argv[3], "batch-invalid-", 14)) {
        dcc_frontend_fs_clear(&reply);
        return rejected ? 0 : 2;
    }
    if (rejected) return 2;
    unsigned error = dcc_tcp_get32(reply.data);
    unsigned expected_error = !strcmp(argv[3], "denied") ? EACCES :
        !strcmp(argv[3], "missing") ? ENOENT : !strcmp(argv[3], "notdir") ? ENOTDIR : 0;
    if (expected_error) {
        int ok = error == expected_error && reply.size == 8 && reply.fd < 0 && !reply.file_size;
        dcc_frontend_fs_clear(&reply); return ok ? 0 : 3;
    }
    if (error) return 4;
    if (operation == DCC_FS_READ) {
        struct stat actual, expected;
        if (reply.fd < 0 || fstat(reply.fd, &actual) || stat(argv[2], &expected) ||
            actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino) return 4;
    } else if (reply.fd >= 0 || reply.file_size) return 4;
    if (!strcmp(argv[3], "unlink") && unlink(argv[2])) return 5;
    if (!strcmp(argv[3], "truncate") && truncate(argv[2], 0)) return 6;
    unsigned char raw[DCC_TCP_CHUNK], decoded[DCC_TCP_CHUNK];
    unsigned char *chunk = NULL;
    size_t capacity = 0;
    uint64_t stream_offset = 0, total = reply.size + reply.file_size;
    while (stream_offset < total) {
        size_t size = 0;
        int failed = next_fs_chunk(&reply, &stream_offset, raw, &chunk, &capacity, &size);
        if (!strcmp(argv[3], "truncate")) {
            free(chunk); dcc_frontend_fs_clear(&reply);
            return failed ? 0 : 7;
        }
        if (failed || size != 8 + dcc_tcp_get32(chunk + 4)) return 8;
        struct dcc_tcp_frame frame = {.type = dcc_tcp_get32(chunk), .data = chunk + 8, .size = size - 8};
        const void *data; size_t plain; unsigned index;
        if (dcc_tcp_unpack(&frame, &index, decoded, &data, &plain) || index != UINT_MAX ||
            !!(frame.type & DCC_TCP_FINAL) != (stream_offset == total) ||
            fwrite(data, 1, plain, stdout) != plain) return 9;
    }
    free(chunk);
    int fd = reply.fd;
    dcc_frontend_fs_clear(&reply);
    return reply.data || reply.fd != -1 || fcntl(fd, F_GETFD) != -1 || errno != EBADF ? 10 : 0;
}
''')
        executable = self.work/'reply'
        compiler = shutil.which('cc')
        assert compiler is not None, 'C compiler required for filesystem reply tests'
        compiled = subprocess.run([compiler, '-D_GNU_SOURCE', '-ffunction-sections', '-fdata-sections',
                        '-I'+str(self.build/'src'), str(source), str(self.build/'src/frontend_fs.c'),
                        str(self.build/'src/tcp_job.c'), str(self.build/'src/argutil.c'),
                        '-Wl,--gc-sections', '-llz4', '-o', str(executable)], capture_output=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        path = self.work/'payload'
        for length in (0, 1, 256*1024-60, 256*1024-59, 2*256*1024+13):
            with self.subTest(length=length):
                payload = random.Random(length).randbytes(length)
                path.write_bytes(payload)
                result = subprocess.run([str(executable), str(self.work), str(path), 'unlink'],
                                        check=True, capture_output=True, timeout=5)
                self.assertEqual(result.stdout[60:], payload)
                self.assertEqual(struct.unpack('!II', result.stdout[:8]), (0, len(payload)+52))
                self.assertFalse(path.exists())
        path.write_bytes(b'truncated source')
        subprocess.run([str(executable), str(self.work), str(path), 'truncate'], check=True, timeout=5)
        subprocess.run([str(executable), str(self.work/'other-root'), str(path), 'denied'], check=True, timeout=5)
        for candidate, expected in ((self.work/'absent'/'header.h', 'missing'), (path/'header.h', 'notdir')):
            subprocess.run([str(executable), str(self.work/'other-root'), str(candidate), expected],
                           check=True, timeout=5)
        listing = self.work/'listing'
        listing.mkdir()
        (listing/'file').write_bytes(b'')
        (listing/'directory').mkdir()
        (listing/'dangling').symlink_to('absent')
        os.mkfifo(listing/'fifo')
        data = subprocess.check_output([str(executable), str(self.work), str(listing), 'list'], timeout=5)
        self.assertEqual(struct.unpack('!II', data[:8]), (0, len(data)-8))
        actual, offset = {}, 8
        while offset < len(data):
            length, mode = struct.unpack_from('!II', data, offset)
            offset += 8
            actual[data[offset:offset+length].decode()] = stat.S_IFMT(mode)
            offset += length
        self.assertEqual(actual, {p.name: stat.S_IFMT(p.lstat().st_mode) for p in listing.iterdir()})
        data = subprocess.check_output([str(executable), str(self.work), str(listing),
                                        'lstat', 'dangling', 'file'], timeout=5)
        self.assertEqual(struct.unpack_from('!II', data, 60), (2, 4))
        actual, offset = {}, 68
        while offset < len(data):
            length, error = struct.unpack_from('!II', data, offset)
            offset += 8
            name = data[offset:offset+length].decode()
            offset += length
            actual[name] = None
            if not error:
                actual[name] = stat.S_IFMT(struct.unpack_from('!I', data, offset+32)[0])
                offset += 52
            else: self.assertEqual(error, 0xffffffff)
        self.assertEqual(actual, {'file': stat.S_IFREG, 'dangling': stat.S_IFLNK,
                                  'directory': None, 'fifo': None})
        batch = subprocess.check_output([str(executable), str(self.work), str(listing),
                                         'batch', 'dangling', 'file'], timeout=5)
        self.assertEqual(struct.unpack_from('!II', batch), (0, len(batch)-8))
        self.assertEqual(batch[8:], data+data)
        mixed = subprocess.check_output([str(executable), str(self.work), str(listing),
                                         'batch-mixed', 'dangling', 'file'], timeout=5)
        self.assertEqual(struct.unpack_from('!II', mixed), (0, len(mixed)-8))
        self.assertEqual(mixed[8:], data+struct.pack('!II', errno.ENOENT, 0))
        for root, requested, error in ((self.work/'other-root', listing, errno.EACCES),
                                       (self.work, self.work/'absent', errno.ENOENT)):
            batch = subprocess.check_output([str(executable), str(root), str(requested), 'batch'], timeout=5)
            self.assertEqual(batch, struct.pack('!IIIIII', 0, 16, error, 0, error, 0))
        for invalid in ('short', 'tail', 'size', 'kind', 'nested', 'empty'):
            subprocess.run([str(executable), str(self.work), str(listing), 'batch-invalid-'+invalid],
                           check=True, timeout=5)
        for names in (['z', 'a'], ['file', 'file'], ['../file'], [''], ['.'], ['..']):
            subprocess.run([str(executable), str(self.work), str(listing), 'invalid', *names],
                           check=True, timeout=5)
        subprocess.run([str(executable), str(self.work), str(listing), 'unterminated'],
                       check=True, timeout=5)

    def test_remote_frontend_preserves_large_directory_snapshots(self):
        clang = self.start_frontend()
        directory = self.work/'headers'
        directory.mkdir()
        names = [f'header_{i:04d}_with_a_long_directory_entry_name.h' for i in range(3000)]
        for name in names:
            (directory/name).write_text('#define VALUE 42\n')
        (directory/'dangling.h').symlink_to('absent.h')
        source = self.work/'directory.c'
        source.write_text(f'#include "headers/{names[-1]}"\n'
                          '#if __has_include("headers/dangling.h")\n#error dangling header\n#endif\n'
                          'int main(void) { return VALUE != 42; }\n')
        output, deps = self.work/'directory.o', self.work/'directory.d'
        command = [clang, '-O3', '-c', str(source), '-o', str(output), '-MMD', '-MF', str(deps)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        expected = output.read_bytes(), deps.read_bytes()
        output.unlink(); deps.unlink()
        result = subprocess.run([str(self.build/'distcc'), *command], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[-5000:])
        self.assertIn('frontend filesystem:', result.stderr)
        self.assertEqual((output.read_bytes(), deps.read_bytes()), expected)
        executable = self.work/'directory'
        subprocess.run([clang, str(output), '-o', str(executable)], check=True, capture_output=True)
        subprocess.run([str(executable)], check=True, timeout=5)

    def test_remote_frontend_disconnect_reclaims_worker(self):
        # Reserve within the one-job-only interval (half to all of the
        # measured budget), away from its fluctuating boundary. While one
        # job is active, additional capacity is zero, but total occupancy must
        # still include that job, as on every other RPC path.
        initial = self.get_stats(self.stats_ports[0])
        headroom = int(re.search(r'dcc_memory_headroom_bytes (\d+)', initial)[1])
        reservation_mib = max(1, 3 * headroom // (4 * 1024**2))
        clang = self.start_frontend(DISTCC_JOB_MEMORY=str(reservation_mib), DISTCC_MEMORY_LEARNING='0')
        source = self.work/'cancel.c'
        source.write_text('unsigned values[] = {' + ','.join(map(str, range(400000))) + '};\n')
        command = [str(self.build/'distcc'), clang, '-O3', '-c', str(source), '-o', str(self.work/'cancel.o')]
        with (self.work/'cancel.log').open('wb') as log:
            process = subprocess.Popen(command, env=self.env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic()+10
                while 'dcc_busy 0\n' in (active := self.get_stats(self.stats_ports[0])):
                    self.assertIsNone(process.poll(), 'worker did not execute the job')
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                busy = int(re.search(r'dcc_busy (\d+)', active)[1])
                capacity = int(re.search(r'dcc_frontend_capacity (\d+)', active)[1])
                self.assertEqual(capacity, busy, active)
                process.kill(); process.wait(timeout=5)
                deadline = time.monotonic()+5
                while 'dcc_busy 0\n' not in self.get_stats(self.stats_ports[0]):
                    self.assertLess(time.monotonic(), deadline, 'disconnected compiler still holds capacity')
                    time.sleep(.01)
            finally:
                if process.poll() is None: process.kill()
                process.wait(timeout=5)

class TcpJobTestCase(BrokerTestCase):
    def start_tcp(self):
        # Fixed transport slots keep the client on the RPC path. Live local
        # statistics deliberately enable direct compilation, tested separately.
        return self.start_broker(self.host(0, ',tcp'))


class TcpJobsTest(TcpJobTestCase):
    def test_explicit_launcher_unwraps_absolute_masquerade_compilers(self):
        self.start_tcp()
        wrappers = self.work / 'masquerade'
        wrappers.mkdir()
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        for name, suffix in (('gcc', 'c'), ('g++', 'cc'),
                             (target + '-gcc', 'c'), (target + '-g++', 'cc')):
            with self.subTest(compiler=name):
                compiler = wrappers / name
                compiler.symlink_to(self.build / 'distcc')
                source = self.work / ('source.' + suffix)
                source.write_text('int main(void) { return 0; }\n')
                obj = self.work / (name + '.o')
                env = dict(self.env, DISTCC_VERBOSE='1',
                           PATH=str(wrappers) + os.pathsep + os.environ['PATH'])
                result = subprocess.run([str(self.build / 'distcc'), str(compiler),
                                         '-c', str(source), '-o', str(obj)], env=env,
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr[-4000:])
                self.assertIn('pooled TCP job completed', result.stderr)
                # Client-only wrapper paths must never reach a worker, even
                # when that worker permits absolute compiler paths.
                self.logs[0].flush()
                log = Path(self.logs[0].name).read_text()
                self.assertNotIn(str(compiler), log)
                exe = self.work / (name + '.exe')
                subprocess.run([self.cc, str(obj), '-o', str(exe)], check=True,
                               capture_output=True, timeout=10)
                subprocess.run([str(exe)], check=True, timeout=5)

    def test_compiler_execution_outlives_transport_idle_timeout(self):
        wrapper = self.work / 'slow-compiler'
        wrapper.mkdir()
        compiler = wrapper / 'gcc'
        compiler.write_text('#!/bin/sh\nsleep 7\nexec ' + self.cc + ' "$@"\n')
        compiler.chmod(0o755)
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        (wrapper / (target + '-gcc')).symlink_to('gcc')
        self.restart_server(self.build / 'distccd', DISTCC_IO_TIMEOUT='3',
                            DISTCCD_PATH=str(wrapper) + os.pathsep + os.defpath)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        self.env['DISTCC_IO_TIMEOUT'] = '3'
        self.env['DISTCC_VERBOSE'] = '1'
        self.job('long-running')
        subprocess.run(['gcc', str(self.work / 'job-long-running.o'),
                        '-o', str(self.work / 'long-running')], check=True)
        subprocess.run([str(self.work / 'long-running')], check=True)

    def test_memory_refusal_is_a_reusable_unstarted_boundary(self):
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='1048576')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        body = b'int value(void) { return 7; }\n'
        net = None
        for identity in (37, 38):
            net = self.wire_job(body, cached=False, identity=identity, net=net)
            self.upload(net, body)
            self.assertEqual(self.frame(net), (0x54000307, struct.pack('!Q', identity)))
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_compile_ok 0\n', stats)
        self.assertIn('dcc_busy 0\n', stats)
        self.assertIn('dcc_queued 0\n', stats)
        deadline = time.monotonic() + 5
        while list(self.root.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline, 'refused input tree was not reclaimed')
            time.sleep(.01)

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_sealed_input_maps_and_sends_raw_tail(self):
        self.start_tcp()
        wrappers = self.work / 'client-bin'
        wrappers.mkdir()
        compiler = wrappers / 'gcc'
        # Real preprocessing and compilation, with an incompressible comment
        # added to CPP stdout to exercise raw sealed extents and a short tail.
        compiler.write_text(f'''#!/usr/bin/python3
import random, subprocess, sys
result = subprocess.run([{self.cc!r}, *sys.argv[1:]])
if result.returncode == 0 and '-E' in sys.argv:
    data = random.Random(718).randbytes(1024 * 1024 + 17).replace(b'\\x00', b'x').replace(b'*', b'x')
    sys.stdout.buffer.write(b'\\n/*' + data + b'*/\\n')
sys.exit(result.returncode)
''')
        compiler.chmod(0o755)
        target = subprocess.check_output([self.cc, '-dumpmachine'], text=True).strip()
        (wrappers / (target + '-gcc')).symlink_to('gcc')
        env = dict(self.env, PATH=str(wrappers) + os.pathsep + os.environ['PATH'])
        trace = self.work / 'input.trace'
        actual, expected = self.work / 'raw.o', self.work / 'native.o'
        subprocess.run([self.cc, '-c', str(self.source), '-o', str(expected)], check=True)
        subprocess.run(['strace', '-f', '-yy', '-e', 'trace=pread64,mmap,sendfile',
                        '-o', str(trace), str(self.build / 'distcc'), 'gcc', '-c',
                        str(self.source), '-o', str(actual)], env=env, check=True,
                       capture_output=True, timeout=20)
        self.assertEqual(actual.read_bytes(), expected.read_bytes())
        calls = trace.read_text()
        self.assertTrue(re.search(r'mmap\([^\n]*distcc-preprocessed', calls), 'sealed input was not mapped')
        self.assertTrue(re.search(r'sendfile\([^\n]*distcc-preprocessed', calls), 'raw input did not use sendfile')
        self.assertFalse(re.search(r'pread64\([^\n]*distcc-preprocessed', calls), 'sealed input was copied')

    def test_local_error_result_preserves_object_and_reuses_connection(self):
        prefix = str(self.root / 'local-error')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        source, obj = self.work / 'bad.c', self.work / 'bad.o'
        source.write_text('int = ;\n')
        obj.write_bytes(b'previous object')
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source),
                                 '-o', str(obj)], env=self.env, capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'expected identifier', result.stderr)
        self.assertEqual(obj.read_bytes(), b'previous object')
        self.job('after-local-error')

    def test_local_input_descriptor_and_pool_reuse(self):
        prefix = str(self.root / 'local-jobs')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        before = self.get_stats(self.stats_ports[0])
        for i in range(4):
            self.job(f'local-{i}', 'cc' if i % 2 else 'c')
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        with socket.socket(fileno=descriptors[0]) as connection:
            self.assertEqual(connection.family, socket.AF_UNIX)
            self.assertEqual(connection.getpeername(), f'{prefix}-{self.ports[0]}.sock')
        lease.close()
        after = self.get_stats(self.stats_ports[0])
        self.assertEqual(re.search(r'dcc_tcp_accept (\d+)', before)[1],
                         re.search(r'dcc_tcp_accept (\d+)', after)[1])
        self.assertEqual(int(re.search(r'dcc_compile_ok (\d+)', after)[1]) -
                         int(re.search(r'dcc_compile_ok (\d+)', before)[1]), 4)
        self.assertIn('dcc_local_input_jobs 4\n', after)
        self.assertGreater(int(re.search(r'dcc_local_input_bytes (\d+)', after)[1]), 0)

    def test_cache_and_cleanup_leave_event_loop_responsive(self):
        hook, library = self.work / 'file-gate.c', self.work / 'file-gate.so'
        arm, marker, release = (self.work / name for name in ('arm', 'entered', 'release'))
        hook.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static pid_t daemon_pid;
__attribute__((constructor)) static void identify_daemon(void) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (n > 0) {
        path[n] = 0;
        const char *name = strrchr(path, '/');
        if (name && !strcmp(name + 1, "distccd")) daemon_pid = getpid();
    }
}
static void gate(const char *kind) {
    if (getpid() != daemon_pid) return;
    const char *mode = getenv("TEST_FILE_KIND"), *arm = getenv("TEST_FILE_ARM");
    if (!mode || strcmp(mode, kind) || !arm || access(arm, F_OK)) return;
    int fd = open(getenv("TEST_FILE_ENTERED"), O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (fd < 0) return;
    dprintf(fd, "%ld %ld", (long)getpid(), (long)syscall(SYS_gettid)); close(fd);
    for (int i = 0; i < 1000 && access(getenv("TEST_FILE_RELEASE"), F_OK); ++i) usleep(10000);
}
static void cache_input(int fd) {
    char link[64], path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path)-1);
    if (n > 0) {
        path[n] = 0;
        const char *cache = getenv("DISTCCD_FILE_CACHE");
        if (cache && *cache && !strncmp(path, cache, strlen(cache)) && path[strlen(cache)] == '/')
            gate("cache");
    }
}
ssize_t read(int fd, void *data, size_t size) {
    ssize_t (*real_read)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    cache_input(fd);
    return real_read(fd, data, size);
}
int unlinkat(int fd, const char *path, int flags) {
    int (*real_unlinkat)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    if ((flags & AT_REMOVEDIR) && strstr(path, "/distcc-tcp-")) gate("cleanup");
    return real_unlinkat(fd, path, flags);
}
int ioctl(int fd, unsigned long request, ...) {
    va_list args; va_start(args, request);
    unsigned long argument;
    if (request == FICLONE) {
        argument = va_arg(args, int);
        /* Reflink filesystems can satisfy cache hits without any read(). */
        cache_input(argument); gate("copy"); gate("store");
    }
    else argument = (unsigned long)va_arg(args, void *);
    va_end(args);
    return syscall(SYS_ioctl, fd, request, argument);
}
''')
        subprocess.run([self.cc, '-shared', '-fPIC', str(hook), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        for kind in ('cache', 'store', 'cleanup', 'copy'):
            with self.subTest(kind=kind):
                for path in (arm, marker, release): path.unlink(missing_ok=True)
                self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library),
                                    DISTCCD_FILE_CACHE='' if kind == 'copy' else str(self.work / 'cache'),
                                    TEST_FILE_KIND=kind, TEST_FILE_ARM=str(arm),
                                    TEST_FILE_ENTERED=str(marker), TEST_FILE_RELEASE=str(release))
                body = f'int asynchronous_{kind};\n'.encode()
                if kind == 'cache':
                    first = self.wire_job(body)
                    self.assertEqual(self.frame(first), (0x54000301, bytes(4)))
                    self.upload(first, body)
                    self.result(first)
                    first.close()
                    # A job result can precede asynchronous cache publication.
                    # Arm the cache-read gate only after a complete input exists.
                    deadline = time.monotonic()+5
                    while not any(p.read_bytes() == body for p in (self.work/'cache').glob('*/*')
                                  if len(p.name) == 16):
                        self.assertLess(time.monotonic(), deadline, 'cache input was not published')
                        time.sleep(.01)
                arm.touch()
                owner = None
                if kind == 'copy':
                    before = int(re.search(r'dcc_tcp_parallel_tasks (\d+)', self.get_stats(self.stats_ports[0]))[1])
                    owner = self.wire_job(body)
                    self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
                net = self.wire_job(body, cached=kind != 'cleanup')
                if owner:
                    deadline = time.monotonic() + 5
                    while True:
                        stats = self.get_stats(self.stats_ports[0])
                        if ('dcc_preparing 2\n' in stats and
                            int(re.search(r'dcc_tcp_parallel_tasks (\d+)', stats)[1]) >= before + 2): break
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    self.upload(owner, body)
                if kind == 'store':
                    self.assertEqual(self.frame(net), (0x54000301, bytes(4)))
                if kind in ('store', 'cleanup'): self.upload(net, body)
                try:
                    deadline = time.monotonic() + 5
                    while not marker.exists() or not marker.read_text():
                        self.assertLess(time.monotonic(), deadline, 'file operation was not reached')
                        time.sleep(.01)
                    pid, tid = map(int, marker.read_text().split())
                    self.assertEqual(pid, self.daemons[0].pid)
                    self.assertNotEqual(pid, tid, 'file operation ran in the event loop')
                    if owner: owner.close()
                    self.assertIn('dcc_cpus ', self.get_stats(self.stats_ports[0]))
                    if kind == 'cleanup': self.result(net)
                finally:
                    release.touch()
                if kind != 'cleanup': self.result(net)
                net.close()
                deadline = time.monotonic() + 5
                while list(self.root.glob('distcc-tcp-*')):
                    self.assertLess(time.monotonic(), deadline, 'async cleanup leaked a tree')
                    time.sleep(.01)

    def test_local_input_matches_native_and_rejects_writable_fd(self):
        prefix = str(self.root / 'local-validation')
        self.restart_server(self.build / 'distccd', DISTCC_LOCAL_SOCKET=prefix)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.base_env = dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix)
        self.start_tcp()
        source = self.work / 'large.c'
        source.write_text('const char payload[] = "' + 'abcdefgh' * 262144 + '";\n')
        expected, actual = self.work / 'native.o', self.work / 'remote.o'
        subprocess.run([self.cc, '-c', str(source), '-o', str(expected)], check=True, capture_output=True)
        subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source), '-o', str(actual)],
                       env=self.env, check=True, capture_output=True, timeout=15)
        self.assertEqual(actual.read_bytes(), expected.read_bytes())
        # Observe the real client/CPP boundary: preprocessing owns the shared
        # memfd, and local transport must not allocate/copy a second snapshot.
        tracer = shutil.which('strace')
        if tracer:
            trace = self.work / 'local-input.trace'
            subprocess.run([tracer, '-f', '-yy', '-e', 'trace=memfd_create,sendfile,recvmsg,sendmsg',
                            '-o', str(trace), str(self.build / 'distcc'), 'gcc',
                            '-c', str(source), '-o', str(actual)],
                           env=self.env, check=True, capture_output=True, timeout=15)
            calls = trace.read_text()
            self.assertIn('memfd_create("distcc-preprocessed"', calls)
            self.assertNotIn('memfd_create("distcc-local-input"', calls)
            self.assertIn('SCM_RIGHTS', calls)
            self.assertIn('sendfile(', calls)  # Result publication, not an input snapshot.
            self.assertNotRegex(calls, r'sendfile\([^\n]*distcc-preprocessed')
            returned = re.search(r'sendmsg\([^\n]*iov_len=40[^\n]*SCM_RIGHTS', calls)
            self.assertIsNotNone(returned, 'pooled connection was not returned')
            self.assertLess(returned.start(), calls.index('sendfile('), 'copy held the pooled connection')
            self.assertEqual(actual.read_bytes(), expected.read_bytes())
        self.assertGreater(int(re.search(r'dcc_local_input_bytes (\d+)',
                                        self.get_stats(self.stats_ports[0]))[1]), 2 * 1024 * 1024)
        # A writable handle must not be inherited into a compiler. Include a
        # valid manifest so rejection tests FD validation, not framing.
        def string(s): return struct.pack('!Q', len(s)) + s
        args = [b'gcc', b'-c', b'/input.i', b'-o', b'/out.o']
        body = b'int input;\n'
        manifest = struct.pack('!4Q', 7, 0, len(args), 1) + string(b'/')
        manifest += b''.join(map(string, args))
        manifest += struct.pack('!5Q', 0, 0, 0, len(body), len(body)) + bytes(8) + string(b'/input.i') + string(b'')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as net, (self.work/'bad.i').open('w+b') as f:
            f.write(body); f.flush()
            net.settimeout(3); net.connect(f'{prefix}-{self.ports[0]}.sock')
            net.sendmsg([struct.pack('!II', 0x54000300, len(manifest)) + manifest],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [f.fileno()]))])
            self.assertEqual(net.recv(1), b'')
        self.job('after-invalid-local-fd')

    def test_memory_estimate_uses_completed_compiler_rss(self):
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='512', DISTCC_MEMORY_LEARNING='1')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        before = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_job_memory_bytes 536870912\n', before)
        for i in range(8):
            self.job(f'memory-{i}')
        after = self.get_stats(self.stats_ports[0])
        estimate = int(re.search(r'dcc_job_memory_bytes (\d+)', after)[1])
        self.assertGreaterEqual(estimate, 256 * 1024 * 1024)
        self.assertLess(estimate, 512 * 1024 * 1024)
        self.assertIn('dcc_job_memory_samples 8\n', after)

    def test_compile_and_pool_reuse(self):
        self.start_tcp()
        before = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        for i in range(5):
            self.job(i, 'cc' if i % 2 else 'c')
        stats = self.get_stats(self.stats_ports[0])
        self.assertEqual(int(re.search(r'dcc_tcp_accept (\d+)', stats)[1]) - before, 1)
        self.assertIn('dcc_busy 0\n', stats)

    def test_parallel_compile(self):
        self.start_tcp()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(self.job, range(24)))

    def test_worker_crossing_retirement_deadline_stays_available(self):
        # Advance daemon wall time during compilation, after its retirement
        # decision. Avoid a minute-long, timing-dependent deadline test.
        clock = self.work / 'clock.c'
        library = self.work / 'clock.so'
        marker = self.work / 'advance'
        trigger = self.work / 'trigger'
        worker = self.work / 'worker'
        clock.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
time_t time(time_t *out) {
    time_t (*real_time)(time_t *) = dlsym(RTLD_NEXT, "time");
    time_t value = real_time(NULL);
    const char *marker = getenv("TEST_CLOCK_MARKER");
    if (marker && !access(marker, F_OK)) value += 120;
    if (out) *out = value;
    return value;
}
''')
        subprocess.run([self.cc, '-shared', '-fPIC', str(clock), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        wrapper = self.root / 'bin/gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text(f'#!/bin/sh\necho "$PPID" > "{worker}"\n'
                           f'if test -e "{trigger}"; then touch "{marker}"; fi\n'
                           f'exec "{self.cc}" "$@"\n')
        self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library),
                            TEST_CLOCK_MARKER=str(marker))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        for i in range(50):
            self.job(f'warm-retirement-{i}')
        trigger.touch()
        self.job('cross-retirement')
        pid = int(worker.read_text())
        time.sleep(.2)
        self.assertTrue(Path(f'/proc/{pid}').exists(),
                        'worker exited after reporting itself reusable')
        self.job('announced-retirement')
        self.job('replacement-worker')

    def test_failed_reused_connection_does_not_quarantine_worker(self):
        self.start_tcp()
        self.job('warm-pool')
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        self.assertEqual(len(descriptors), 1)
        # A peer may close after pool_take's liveness check. The client then
        # reports a failed RPC; only that pooled connection is known bad.
        with socket.socket(fileno=descriptors[0]) as net:
            net.shutdown(socket.SHUT_RDWR)
        lease.sendall(struct.pack('=II', self.protocol_version, 11))
        self.assertEqual(lease.recv(1), b'')
        lease.close()
        self.job('fresh-connection-after-failure')

    def test_failed_fresh_connection_still_quarantines_worker(self):
        self.start_tcp()
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        for fd in descriptors:
            os.close(fd)
        lease.sendall(struct.pack('=II', self.protocol_version, 11))
        self.assertEqual(lease.recv(1), b'')
        lease.close()
        retry = self.request(6)
        status, _, descriptors = self.receive(retry)
        self.assertEqual(status, 103)
        self.assertEqual(descriptors, [])

    def test_preparing_and_ready_are_distinct(self):
        reservations = []
        for _ in range(4):
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
            self.addCleanup(net.close)
            net.sendall(b'SLOT00000001')
            self.assertEqual(net.recv(12, socket.MSG_WAITALL), b'GRNT00000001')
            reservations.append(net)
        body = b'int phase_accounting;\n'
        net = self.wire_job(body)
        self.assertEqual(self.frame(net), (0x54000301, bytes(4)))
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_preparing 1\n', stats)
        self.assertIn('dcc_queued 0\n', stats)
        self.upload(net, body)
        deadline = time.monotonic() + 3
        while True:
            stats = self.get_stats(self.stats_ports[0])
            if 'dcc_queued 1\n' in stats:
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        self.assertIn('dcc_preparing 0\n', stats)
        self.assertIn('dcc_busy 4\n', stats)
        for lease in reservations:
            lease.close()
        self.result(net)

    def test_publication_failure_keeps_completed_connection_reusable(self):
        self.start_tcp()
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_VERBOSE='1')
        destination = self.work / 'directory.o'
        destination.mkdir()
        before = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(destination)], env=self.env, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'pooled TCP job completed', result.stderr)
        self.assertTrue(destination.is_dir())
        self.assertFalse(list(self.work.glob('directory.o.distcc-*')))
        self.job('after-publication-failure')
        after = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        self.assertEqual(after - before, 1)

    def test_parallel_compressed_inputs_and_disconnect(self):
        # Hold a decoder in a real worker thread so the next frame must be
        # received while the first one still owns its decompression buffer.
        shim = self.work / 'slow-decode.c'
        shim.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
ssize_t write(int fd, const void *data, size_t size) {
    ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    if (size >= 65536 && syscall(SYS_gettid) != getpid()) usleep(100000);
    return real_write(fd, data, size);
}
''')
        library = self.work / 'slow-decode.so'
        subprocess.run([self.cc, '-shared', '-fPIC', str(shim), '-ldl', '-o', str(library)],
                       check=True, capture_output=True)
        self.restart_server(self.build / 'distccd', LD_PRELOAD=str(library))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        body = b'/*' + b'bounded expansion ' * 65536 + b'*/\nint parallel_input;\n'
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        streams = [self.wire_job(body, cached=False) for _ in range(6)]
        cancelled = self.wire_job(body, cached=False)
        first = codec.encode(body[:262144])
        self.send(cancelled, 0x54010302, struct.pack('!II', 0, 262144) + first[8:])
        self.send(cancelled, 0x54010302, struct.pack('!II', 0, 262144) + first[8:])
        deadline = time.monotonic() + 3
        while not int(re.search(r'dcc_tcp_input_prefetch_bytes (\d+)',
                               self.get_stats(self.stats_ports[0]))[1]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        cancelled.close()
        for offset in range(0, len(body), 262144):
            chunk = body[offset:offset + 262144]
            packed = codec.encode(chunk)
            plain, size = struct.unpack('!II', packed[:8])
            self.assertGreater(size, 0)
            kind = 0x54010302 | (0x20000 if offset + plain == len(body) else 0)
            for net in streams:
                self.send(net, kind, struct.pack('!II', 0, plain) + packed[8:])
        for net in streams:
            self.result(net)
        stats = self.get_stats(self.stats_ports[0])
        self.assertGreaterEqual(int(re.search(r'dcc_tcp_parallel_tasks (\d+)', stats)[1]), 24)
        self.assertIn(int(re.search(r'dcc_tcp_parallel_peak (\d+)', stats)[1]), (1, 2))

    def test_partial_manifest_does_not_reserve_worker(self):
        self.start_tcp()
        sockets = []
        try:
            for _ in range(8):
                net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=2)
                sockets.append(net)
                net.sendall(struct.pack('!II', 0x54000300, 100) + b'partial')
            self.job('while-receiving')
        finally:
            for net in sockets:
                net.close()
        self.job('after-cancel')

    def wire_job(self, body, cached=True, paths=(b'/src/source.c',), identity=1, receive_buffer=None,
                 verification: bytes | None = b'DCSI\0\0\0\0', net=None):
        def string(value):
            return struct.pack('!Q', len(value)) + value
        args = [b'gcc', b'-c', paths[0], b'-o', b'/src/result.o']
        count = len(paths) + (verification is not None)
        manifest = struct.pack('!Q3Q', identity, 1, len(args), count) + string(b'/src')
        manifest += b''.join(map(string, args))
        for path in paths:
            manifest += (struct.pack('!5Q', 0, 0, cached, len(body), len(body)) +
                         xxhash.xxh3_64(body).digest() + string(path) + string(b''))
        if verification is not None:
            # These wire fixtures contain no system includes. Production
            # analyzer manifests are exercised by test_preprocess.
            manifest += (struct.pack('!5Q', 0, 0, 0, len(verification), len(verification)) +
                         xxhash.xxh3_64(verification).digest() +
                         string(b'/.distcc-system/check') + string(b''))
        if net is None:
            net = socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5)
        net.settimeout(5)
        if receive_buffer:
            net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.addCleanup(net.close)
        self.send(net, 0x54000300, manifest)
        if verification is not None:
            self.upload(net, verification, len(paths))
        return net

    @staticmethod
    def send(net, kind, body):
        net.sendall(struct.pack('!II', kind, len(body)) + body)

    def frame(self, net):
        def read(size):
            result = bytearray()
            while len(result) < size:
                part = net.recv(size - len(result))
                self.assertTrue(part, 'unexpected connection EOF')
                result.extend(part)
            return bytes(result)
        kind, size = struct.unpack('!II', read(8))
        self.assertLessEqual(size, 256 * 1024 + 8)
        return kind, read(size)

    def upload(self, net, body, index=0):
        self.send(net, 0x54000302 | 0x20000, struct.pack('!II', index, len(body)) + body)

    def result(self, net, identity=1, expected_status=0):
        codec = runpy.run_path(str(self.build / 'distcc-link'))['Lz4Chunks']()
        kind, description = self.frame(net)
        while kind == 0x54000308:
            self.assertEqual(description, struct.pack('!Q', identity))
            kind, description = self.frame(net)
        self.assertEqual(kind, 0x54000303)
        self.assertEqual(len(description), 72)
        status, mode, *sizes = struct.unpack('!II4Q', description[:40])
        prepare, queued, execute, cpu = struct.unpack('!4Q', description[40:])
        self.assertGreater(execute, 0)
        if not expected_status:
            self.assertGreater(cpu, 0)
        self.assertLess(prepare + queued + execute, 60 * 1000000000)
        outputs = [bytearray() for _ in sizes]
        while True:
            kind, body = self.frame(net)
            if kind == 0x54000305:
                self.assertEqual(body, struct.pack('!Q', identity))
                break
            self.assertEqual(kind & ~0x30000, 0x54000304)
            index, plain = struct.unpack('!II', body[:8])
            data = codec.decode(body[8:], plain) if kind & 0x10000 else body[8:]
            outputs[index].extend(data)
        self.assertEqual(list(map(len, outputs)), sizes)
        self.assertEqual(status, expected_status, outputs[0].decode(errors='replace'))
        if not status:
            self.assertTrue(outputs[2].startswith(b'\x7fELF'))
        return outputs

    def test_missing_or_invalid_system_include_manifest_is_rejected(self):
        body = b'int unchecked_headers;\n'
        for verification in (None, b'bad format', b'DCSI\0\0\0\1',
                             b'DCSI' + struct.pack('!I', 4097),
                             b'DCSI' + struct.pack('!III', 1, 0, 1) + b'x',
                             b'DCSI' + struct.pack('!I', 0) + b'extra'):
            with self.subTest(verification=verification):
                net = self.wire_job(body, cached=False, verification=verification)
                self.upload(net, body)
                # Intact jobs with invalid metadata return a completed protocol
                # error. EOF would incorrectly quarantine a healthy worker.
                outputs = self.result(net, expected_status=109 << 8)
                self.assertEqual(outputs[2], b'')

    def test_single_upload_and_owner_disconnect(self):
        body = b'int cache_owner_recovery(void) { return 901; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
        waiter = self.wire_job(body)
        waiter.settimeout(.15)
        with self.assertRaises(socket.timeout):
            waiter.recv(8)
        owner.close()
        waiter.settimeout(5)
        self.assertEqual(self.frame(waiter), (0x54000301, bytes(4)))
        self.upload(waiter, body)
        self.result(waiter)
        hit = self.wire_job(body)
        self.result(hit)  # No NEED on the warm cache path.

    def test_stalled_upload_does_not_expire_healthy_waiter(self):
        timeout = 2
        self.restart_server(self.build / 'distccd', DISTCC_IO_TIMEOUT=str(timeout))
        self.addCleanup(self.restart_server, self.build / 'distccd')
        body = b'int recovered_from_stalled_peer(void) { return 904; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
        waiter = self.wire_job(body, identity=2)
        # The owner stays connected but sends no input, as with a lost network
        # path. A healthy waiter must receive progress before that owner expires.
        self.assertEqual(self.frame(waiter), (0x54000308, struct.pack('!Q', 2)))
        # Simulate a delayed event-loop turn. The owner now expires, but the
        # queued takeover notification must reach the healthy waiting peer.
        parent = self.daemons[0].pid
        os.kill(parent, signal.SIGSTOP)
        try:
            time.sleep(timeout)
        finally:
            os.kill(parent, signal.SIGCONT)
        while True:
            kind, data = self.frame(waiter)
            if kind == 0x54000301:
                self.assertEqual(data, bytes(4))
                break
            self.assertEqual((kind, data), (0x54000308, struct.pack('!Q', 2)))
        self.assertEqual(owner.recv(1), b'')
        self.upload(waiter, body)
        self.result(waiter, identity=2)

    def test_owner_disconnect_during_waiter_lookup_requests_input_once(self):
        body = b'int cancel_during_lookup(void) { return 903; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
        paths = [b'/anchor/source.c', b'/progress/source.c']
        paths += [f'/remaining_{i}/source.c'.encode() for i in range(3000)]
        paths += [b'/done/source.c']
        waiter = self.wire_job(body, paths=paths)
        deadline = time.monotonic() + 5
        while not (roots := list(self.root.glob('distcc-tcp-*/progress'))):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.001)
        # The second directory appears after the first cache miss registered
        # this job as a waiter. Later lookups are still pending.
        self.assertFalse((roots[0].parent / 'done').exists())
        owner.close()
        kind, requested = self.frame(waiter)
        self.assertEqual(kind, 0x54000301)
        self.assertEqual(len(requested), 4)
        self.upload(waiter, body, struct.unpack('!I', requested)[0])
        self.result(waiter)  # A duplicate NEED is a protocol error here.

    def test_parallel_cache_waiters_share_upload(self):
        body = b'int single_upload_for_all(void) { return 902; }\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
        waiters = [self.wire_job(body) for _ in range(7)]
        self.upload(owner, body)
        self.result(owner)
        for waiter in waiters:
            self.result(waiter)

    def test_path_ancestor_rejected_even_with_intervening_name(self):
        # Checking only adjacent sorted names misses /a as parent of /a/b.
        net = self.wire_job(b'int invalid;', paths=(b'/a', b'/a.b', b'/a/b'), verification=None)
        self.assertEqual(net.recv(8), b'')

    def test_bad_digest_discards_connection_and_releases_waiter(self):
        body = b'int verified_upload;\n'
        owner = self.wire_job(body)
        self.assertEqual(self.frame(owner), (0x54000301, bytes(4)))
        waiter = self.wire_job(body)
        self.upload(owner, b'x' * len(body))
        self.assertEqual(owner.recv(8), b'')
        self.assertEqual(self.frame(waiter), (0x54000301, bytes(4)))
        self.upload(waiter, body)
        self.result(waiter)

    def test_invalid_chunks_close_connection_without_dispatch(self):
        body = b'int bounded_input;\n'
        cases = [
            (0x54020302, struct.pack('!II', 0, 262145)),
            (0x54020302, struct.pack('!II', 1, len(body)) + body),
            (0x54030302, struct.pack('!II', 0, len(body)) + b'\xff'),
            (0x54020302, struct.pack('!II', 0, len(body)) + body[:-1]),
        ]
        for kind, data in cases:
            with self.subTest(kind=kind, payload=len(data)):
                net = self.wire_job(body, cached=False)
                self.send(net, kind, data)
                self.assertEqual(net.recv(8), b'')
                self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))
        self.start_tcp()
        self.job('after-invalid-chunks')

    def test_cancel_running_job_reclaims_tree_and_worker(self):
        body = b'int cancel_running_job;\n'
        net = self.wire_job(body, cached=False)
        self.upload(net, body)
        deadline = time.monotonic() + 3
        while 'dcc_busy 1\n' not in self.get_stats(self.stats_ports[0]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        net.close()
        deadline = time.monotonic() + 5
        while list(self.root.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.start_tcp()
        self.job('after-running-cancel')

    def test_worker_death_kills_compiler_and_reclaims_inputs(self):
        marker = self.work / 'compiler-pids'
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text(f'#!/bin/sh\necho "$PPID $$" > "{marker}"\n'
                           'echo private-temporary > "$TMPDIR/compiler-owned"\nexec sleep 30\n')
        body = b'int killed_compiler;\n'
        net = self.wire_job(body, cached=False)
        self.upload(net, body)
        deadline = time.monotonic() + 5
        while not marker.exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        worker, compiler = map(int, marker.read_text().split())
        os.kill(worker, signal.SIGKILL)
        self.assertEqual(net.recv(8), b'')
        while True:
            try:
                with open(f'/proc/{compiler}/stat') as status:
                    state = status.read().rsplit(')', 1)[1].split()[0]
            except (FileNotFoundError, ProcessLookupError):
                break
            if state == 'Z':
                break
            self.assertLess(time.monotonic(), deadline, 'compiler outlived its worker')
            time.sleep(.01)
        while list(self.root.glob('distcc-tcp-*')):
            self.assertLess(time.monotonic(), deadline, 'input cleanup did not finish')
            time.sleep(.01)
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.assertFalse((self.root / 'compiler-owned').exists())
        wrapper.write_text(original)
        self.start_tcp()
        self.job('after-worker-death')

    def test_stalled_results_do_not_hold_compiler_workers(self):
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        wrapper.write_text('''#!/usr/bin/python3
import random, sys
from pathlib import Path
args = sys.argv[1:]
Path(args[args.index('-o') + 1]).write_bytes(random.Random(543).randbytes(4 * 1024 * 1024))
''')
        stalled = []
        try:
            for i in range(4):
                body = f'int stalled_{i};\n'.encode()
                net = self.wire_job(body, cached=False, receive_buffer=1024)
                stalled.append(net)
                self.upload(net, body)
                kind, description = self.frame(net)
                self.assertEqual(kind, 0x54000303)
                self.assertEqual(struct.unpack('!II4Q', description[:40])[4], 4 * 1024 * 1024)
            wrapper.write_text(original)
            stats = self.get_stats(self.stats_ports[0])
            self.assertIn('dcc_busy 0\n', stats)
            self.assertIn('dcc_returning 4\n', stats)
            # Replacement must fork only after joining transfer threads.
            parent = self.daemons[0].pid
            workers = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
            self.assertTrue(workers)
            os.kill(int(workers[0]), signal.SIGKILL)
            self.start_tcp()
            self.job('while-results-stalled')
        finally:
            for net in stalled:
                net.close()

    @unittest.skipUnless(shutil.which('objcopy'), 'objcopy is required')
    def test_hinted_broker_leases_release_cpu_during_real_result_transfer(self):
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        noise = self.work / 'noise'
        noise.write_bytes(random.Random(543).randbytes(6 * 1024 * 1024))
        wrapper.write_text(f'''#!/usr/bin/python3
import subprocess, sys
args = sys.argv[1:]
subprocess.run([{self.cc!r}, *args], check=True)
subprocess.run(['objcopy', '--add-section', {'noise=' + str(noise)!r}, args[args.index('-o') + 1]], check=True)
''')
        self.start_tcp()
        held = []
        try:
            for index in range(4):
                lease, net = self.acquire_reserved(10, (990 + index, 4 * 1024 * 1024, 1))
                body = f'int answer_{index}(void) {{ return {index}; }}\n'.encode()
                self.wire_job(body, cached=False, receive_buffer=65536, net=net)
                self.upload(net, body)
                held.append((lease, net, body))
            deadline = time.monotonic() + 10
            while 'dcc_returning 4\n' not in self.get_stats(self.stats_ports[0]):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            wrapper.write_text(original)
            # Include a fresh broker snapshot. All four leases still exist,
            # but the actual worker has completed their GCC compilations.
            time.sleep(1.1)
            self.assertIn('dcc_busy 0\n', self.get_stats(self.stats_ports[0]))
            started = time.monotonic()
            self.job('while-hinted-results-stalled')
            self.assertLess(time.monotonic() - started, 5)
            for index, (lease, net, body) in enumerate(held):
                net.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
                outputs = self.result(net)
                actual, expected = self.work / 'returned.o', self.work / 'native.o'
                source = self.work / 'source.c'
                actual.write_bytes(outputs[2])
                subprocess.run(['objcopy', '--remove-section', 'noise', str(actual)], check=True)
                source.write_bytes(body)
                subprocess.run([self.cc, '-c', str(source), '-o', str(expected)], check=True)
                self.assertEqual(actual.read_bytes(), expected.read_bytes())
                lease.close()
        finally:
            for lease, net, _ in held:
                lease.close()
                net.close()

    def test_daemon_shutdown_reclaims_preparing_and_running_jobs(self):
        waiting = self.wire_job(b'int waiting_for_upload;\n', cached=False)
        body = b'int running_at_shutdown;\n'
        running = self.wire_job(body, cached=False)
        self.upload(running, body)
        deadline = time.monotonic() + 3
        while 'dcc_busy 1\n' not in self.get_stats(self.stats_ports[0]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        self.restart_server(self.build / 'distccd')
        self.assertEqual(waiting.recv(8), b'')
        self.assertEqual(running.recv(8), b'')
        self.assertFalse(list(self.root.glob('distcc-tcp-*')))
        self.assertFalse(list(self.root.glob('distccd_*')))
        self.start_tcp()
        self.job('after-daemon-restart')

    def test_large_raw_result_keeps_the_last_partial_chunk(self):
        before = int(re.search(r'dcc_tcp_output_overlaps (\d+)',
                               self.get_stats(self.stats_ports[0]))[1])
        wrapper = self.root / 'bin' / 'gcc'
        original = wrapper.read_text()
        self.addCleanup(wrapper.write_text, original)
        size = 4 * 1024 * 1024 + 17
        wrapper.write_text(f'''#!/usr/bin/python3
import random, subprocess, sys
args = sys.argv[1:]
subprocess.run([{self.cc!r}, *args], check=True)
with open(args[args.index('-o') + 1], 'ab') as output:
    output.write(random.Random(764).randbytes({size}))
''')
        body = b'int main(void) { return 0; }\n'
        net = self.wire_job(body, cached=False, receive_buffer=65536)
        self.upload(net, body)
        outputs = self.result(net)
        self.assertEqual(outputs[2][-size:], random.Random(764).randbytes(size))
        after = int(re.search(r'dcc_tcp_output_overlaps (\d+)',
                              self.get_stats(self.stats_ports[0]))[1])
        self.assertGreater(after, before)
        obj = self.work / 'large.o'
        obj.write_bytes(outputs[2])
        executable = self.work / 'large'
        subprocess.run([self.cc, str(obj), '-o', str(executable)], check=True, capture_output=True)
        subprocess.run([str(executable)], check=True)

    def test_coalesced_results_cross_receive_turns(self):
        self.coalesced_result(False)

    def test_unsolicited_result_tail_preserves_previous_object(self):
        self.coalesced_result(True)

    def coalesced_result(self, trailing):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        errors = []
        payload = b''.join(bytes([i])*31 for i in range(12))

        def server():
            try:
                with listener.accept()[0] as net:
                    net.settimeout(5)
                    kind, manifest = self.frame(net)
                    self.assertEqual(kind, 0x54000300)
                    while True:
                        kind, body = self.frame(net)
                        self.assertEqual(kind & ~0x30000, 0x54000302)
                        if kind & 0x20000:
                            break
                    response = struct.pack('!IIII8Q', 0x54000303, 72, 0, 0,
                                           0, 0, len(payload), 0, 0, 0, 0, 0)
                    for i in range(12):
                        kind = 0x54020304 if i == 11 else 0x54000304
                        response += struct.pack('!IIII', kind, 39, 2, 31) + payload[i*31:(i+1)*31]
                    response += struct.pack('!II', 0x54000305, 8) + manifest[:8]
                    if trailing:
                        response += b'unsolicited bytes'
                    net.sendall(response)
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=server)
        thread.start()
        self.start_broker(f'127.0.0.1:{listener.getsockname()[1]}/1,tcp')
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1',
                        DISTCC_RECONNECT_TIMEOUT='0')
        output = self.work/'coalesced.o'
        output.write_bytes(b'previous object')
        result = subprocess.run([str(self.build/'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(output)], env=self.env, capture_output=True, timeout=10)
        thread.join(timeout=6)
        self.assertFalse(thread.is_alive())
        if errors:
            raise errors[0]
        if trailing:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b'previous object')
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), payload)

    def test_missing_complete_preserves_previous_object(self):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        errors = []
        reached = threading.Event()

        def server():
            try:
                with listener.accept()[0] as net:
                    net.settimeout(5)
                    kind, manifest = self.frame(net)
                    self.assertEqual(kind, 0x54000300)
                    while True:
                        kind, body = self.frame(net)
                        self.assertEqual(kind & ~0x30000, 0x54000302)
                        if kind & 0x20000:
                            break
                    # Deliberately fragment the response header.
                    response = struct.pack('!IIII8Q', 0x54000303, 72, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0)
                    net.sendall(response[:3])
                    time.sleep(.01)
                    net.sendall(response[3:])
                    self.send(net, 0x54020304, struct.pack('!II', 2, 4) + b'FAKE')
                    reached.set()  # Close after a complete artifact, before COMPLETE.
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=server)
        thread.start()
        self.start_broker(f'127.0.0.1:{listener.getsockname()[1]}/1,tcp')
        self.env.update(DISTCC_MAX_RETRIES='1', DISTCC_SKIP_LOCAL_RETRY='1',
                        DISTCC_RECONNECT_TIMEOUT='0')
        output = self.work / 'previous.o'
        output.write_bytes(b'original artifact')
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                 '-o', str(output)], env=self.env, capture_output=True, timeout=10)
        thread.join(timeout=6)
        self.assertFalse(thread.is_alive())
        if errors:
            raise errors[0]
        self.assertTrue(reached.is_set())
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b'original artifact')
        self.assertFalse(list(self.work.glob('previous.o.distcc-*')))

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_execution_ipc_is_one_request_and_one_result(self):
        trace = self.work / 'execution.trace'
        wrapper = self.work / 'traced-daemon'
        # Keep the tracked process as distccd; the tracer must not replace
        # the daemon's PID/process-group lifecycle in restart_server.
        wrapper.write_text(f'#!/bin/sh\nexec strace -D -f -qq -e trace=sendmsg -o "{trace}" '
                           f'"{self.build / "distccd"}" "$@"\n')
        wrapper.chmod(0o755)
        try:
            self.restart_server(wrapper)
            self.start_tcp()
            self.job('ipc')
            messages = [line for line in trace.read_text().splitlines()
                        if 'sendmsg(' in line and 'SCM_RIGHTS' in line]
            self.assertEqual(len(messages), 2, '\n'.join(messages))
        finally:
            self.restart_server(self.build / 'distccd')


class RemoteTcpJobsTest(TcpJobTestCase):
    # These tests need live capacity feedback and real worker execution.
    worker_address = '127.0.0.2'

    def test_memory_refusal_reschedules_without_fallback(self):
        self.start_memory_refusal()
        before = self.get_stats(self.stats_ports[1])
        self.job('memory-rescheduled')
        log = (self.work / 'memory-refusal.log').read_text()
        self.assertIn('rescheduling unstarted job', log)
        after = self.get_stats(self.stats_ports[1])
        count = lambda text: int(re.search(r'dcc_compile_ok (\d+)', text)[1])
        self.assertEqual(count(after) - count(before), 1)
        self.assertNotIn('job cost:', log.split('rescheduling unstarted job', 1)[0])

    def test_unseen_size_uses_available_memory_in_parallel(self):
        def metric(text, name):
            match = re.search(rf'^{name} (\d+)$', text, re.M)
            assert match is not None, (name, text)
            return int(match[1])
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='512', DISTCC_MEMORY_LEARNING='1')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        for i in range(8):
            self.job(f'small-{i}')
        source = self.work / 'larger.i'
        source.write_text('/*' + ' ' * 8192 + '*/\nint main(void) { return 0; }\n')
        bucket = source.stat().st_size.bit_length() - 1
        def capacity(text):
            row = next(line for line in text.splitlines() if line.startswith('dcc_compile_capacity '))
            return list(map(int, row.split(' ', 1)[1].split(',')))[bucket]
        before = self.get_stats(self.stats_ports[0])
        self.assertGreaterEqual(capacity(before), 2)
        completed = metric(before, 'dcc_compile_ok')
        # Expose cached client capacity to exercise the real daemon's
        # reservation accounting rather than client-side serialization.
        advertised = re.sub(r'dcc_compile_capacity [^\n]+',
                            'dcc_compile_capacity ' + ','.join(['4'] * 64), before)
        peer = self.peer(advertised.encode())
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'), name='cached-capacity')
        def compile_one(index):
            state = self.work / f'independent-{index}'
            state.mkdir()
            env = dict(self.env, DISTCC_DIR=str(state))
            obj = self.work / f'larger-{index}.o'
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c',
                                     str(source), '-o', str(obj)], env=env,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            return obj.read_bytes()
        rows = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(compile_one, i) for i in range(4)]
            while not all(job.done() for job in jobs):
                stats = self.get_stats(self.stats_ports[0])
                rows.append((metric(stats, 'dcc_compile_ok'), metric(stats, 'dcc_busy')))
                time.sleep(.01)
            outputs = [job.result() for job in jobs]
        cold = [busy for done, busy in rows if done == completed]
        self.assertTrue(cold)
        self.assertGreaterEqual(max(cold), 2)
        native = self.work / 'native.o'
        subprocess.run([self.cc, '-c', str(source), '-o', str(native)], check=True)
        self.assertTrue(all(output == native.read_bytes() for output in outputs))
        after = self.get_stats(self.stats_ports[0])
        self.assertGreater(capacity(after), 0)

    def test_known_job_runs_alongside_unmeasured_work(self):
        self.restart_server(self.build / 'distccd', DISTCC_JOB_MEMORY='512', DISTCC_MEMORY_LEARNING='1')
        self.addCleanup(self.restart_server, self.build / 'distccd')
        self.start_tcp()
        self.job('warm-small')
        before = self.get_stats(self.stats_ports[0])
        advertised = re.sub(r'dcc_compile_capacity [^\n]+',
                            'dcc_compile_capacity ' + ','.join(['4'] * 64), before)
        peer = self.peer(advertised.encode())
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'), name='mixed-memory')
        source = self.work / 'unmeasured.i'
        source.write_text('/*' + ' ' * 8192 + '*/\nint main(void) { return 0; }\n')
        entered, release = self.work / 'entered', self.work / 'release'
        wrapper = self.root / 'bin/gcc'
        original = wrapper.read_bytes()
        self.addCleanup(wrapper.write_bytes, original)
        wrapper.write_text(f'''#!/usr/bin/python3
import os, sys, time
from pathlib import Path
if any(os.path.isfile(arg) and os.stat(arg).st_size >= 8192 for arg in sys.argv[1:]):
    Path({str(entered)!r}).touch()
    while not Path({str(release)!r}).exists(): time.sleep(.01)
os.execv({self.cc!r}, [{self.cc!r}, *sys.argv[1:]])
''')
        def compile_large(index):
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source),
                                     '-o', str(self.work / f'large-{index}.o')], env=self.env,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            large = [pool.submit(compile_large, 0)]
            try:
                deadline = time.monotonic() + 5
                while not entered.exists():
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                large.append(pool.submit(compile_large, 1))
                while 'dcc_busy 2\n' not in self.get_stats(self.stats_ports[0]):
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                # Both unmeasured jobs remain held. A known small compile
                # must also run while their memory reservations fit.
                small = pool.submit(self.job, 'bypass-calibration')
                small.result(timeout=3)
                self.assertFalse(any(job.done() for job in large))
            finally:
                release.touch()
            for job in large:
                job.result()

    def test_multiple_brokers_share_workers(self):
        hosts = ' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2))
        environments = []
        for i in range(2):
            self.start_broker(hosts, name=f'shared-{i}')
            environments.append(dict(self.env, DISTCC_VERBOSE='1'))
        before = [int(re.search(r'dcc_compile_ok (\d+)', self.get_stats(port))[1])
                  for port in self.stats_ports]

        def compile_job(i):
            obj, exe = self.work / f'shared-{i}.o', self.work / f'shared-{i}'
            result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(self.source),
                                     '-o', str(obj)], env=environments[i % 2],
                                    capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn(b'pooled TCP job completed', result.stderr)
            subprocess.run([self.cc, str(obj), '-o', str(exe)], check=True,
                           capture_output=True, timeout=5)
            subprocess.run([str(exe)], check=True, timeout=5)

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(compile_job, range(32)))
        for i, port in enumerate(self.stats_ports):
            stats = self.get_stats(port)
            self.assertGreater(int(re.search(r'dcc_compile_ok (\d+)', stats)[1]), before[i])


@unittest.skipUnless(os.geteuid() == 0, 'cross-UID integration requires root')
class LocalCrossUidTest(unittest.TestCase):
    def test_private_input_can_be_compiled_by_another_uid(self):
        build = Path.cwd()
        account = pwd.getpwnam('nobody')
        with tempfile.TemporaryDirectory(prefix='distcc-cross-uid-') as directory:
            root = Path(directory)
            os.chown(root, account.pw_uid, account.pw_gid)
            client = root / 'client'
            client.mkdir(mode=0o700)
            source = client / 'private.c'
            source.write_text('const char *name = __FILE__; int private_input;\n')
            source.chmod(0o600)
            port, stats = free_port(), free_port()
            env = {k:v for k,v in os.environ.items() if not k.startswith('DISTCC')}
            env.update(DISTCC_JOB_MEMORY='64', DISTCC_LOCAL_SOCKET=str(root/'jobs'),
                       DISTCC_HOSTS=f'127.0.0.1:{port}/1,tcp',
                       DISTCC_DIR=str(client), DISTCC_BROKER=str(client/'broker.sock'),
                       DISTCC_FALLBACK='0', DISTCC_SKIP_LOCAL_RETRY='1', TMPDIR=str(client))
            server_env = dict(env, TMPDIR=str(root), DISTCCD_PATH=os.path.dirname(shutil.which('gcc'))+os.pathsep+os.defpath)
            processes = []
            with (root/'server.log').open('w+') as log:
                try:
                    processes.append(subprocess.Popen([str(build/'distccd'), '--daemon', '--no-detach',
                        '--enable-tcp-insecure', '--listen', '127.0.0.1', '--allow', '127.0.0.1',
                        '--port', str(port), '--stats', '--stats-port', str(stats), '--jobs', '1',
                        '--log-stderr'], env=server_env, user=account.pw_uid, group=account.pw_gid,
                        extra_groups=[], start_new_session=True, stdout=log, stderr=log))
                    deadline = time.monotonic()+5
                    while True:
                        try:
                            BrokerTestCase.get_stats(stats)
                            break
                        except OSError:
                            log.flush(); log.seek(0)
                            self.assertIsNone(processes[0].poll(), log.read())
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.02)
                    processes.append(subprocess.Popen([str(build/'distccd'), '--broker', env['DISTCC_BROKER']],
                        env=env, start_new_session=True, stdout=log, stderr=log))
                    while not Path(env['DISTCC_BROKER']).exists():
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    result = subprocess.run([str(build/'distcc'), 'gcc', '-c', str(source), '-o', str(client/'remote.o')],
                        env=env, capture_output=True, timeout=10)
                    log.flush();log.seek(0)
                    self.assertEqual(result.returncode, 0, result.stderr.decode()+log.read())
                    subprocess.run(['gcc', '-c', str(source), '-o', str(client/'native.o')], check=True, capture_output=True)
                    self.assertEqual((client/'remote.o').read_bytes(), (client/'native.o').read_bytes())
                    self.assertIn('dcc_local_input_jobs 1\n', BrokerTestCase.get_stats(stats))
                    self.assertEqual(source.stat().st_mode & 0o777, 0o600)
                finally:
                    for process in reversed(processes):
                        if process.poll() is None:os.killpg(process.pid, signal.SIGTERM)
                        process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
