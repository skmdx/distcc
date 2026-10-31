#!/usr/bin/env python3
"""Argument framing and TCP buffering through a real compiler/daemon."""
import os
import array
import contextlib
import importlib.util
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

from test_adaptive import DistccTestCase


class ArgumentFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        extension = next(Path.cwd().glob('_include_server/lib*/include_server/distcc_pump_c_extensions*.so'))
        spec = importlib.util.spec_from_file_location('distcc_pump_c_extensions', extension)
        assert spec and spec.loader
        cls.protocol = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.protocol)

    def setUp(self):
        self.fd = os.memfd_create('distcc-argv-test')
        self.addCleanup(os.close, self.fd)

    def test_file_groups_offsets_and_reused_command_storage(self):
        for values in ((['/working directory', '/compiler'], []),
                       (['', 'spaced value', 'quote"', '日本語'],),
                       (['-DARG%d=%d' % (i, i) for i in range(16000)], ['root']),
                       ([], [])):
            with self.subTest(lengths=list(map(len, values))):
                os.ftruncate(self.fd, 0)
                os.lseek(self.fd, 0, os.SEEK_SET)
                os.write(self.fd, b'prefix')
                for group in values: self.protocol.XArgv(self.fd, group)
                end = os.lseek(self.fd, 0, os.SEEK_CUR)
                os.lseek(self.fd, len(b'prefix'), os.SEEK_SET)
                self.assertEqual(self.protocol.RArgvFile(self.fd, len(values)), values)
                self.assertEqual(os.lseek(self.fd, 0, os.SEEK_CUR), end)

    def test_partial_or_invalid_file_never_publishes_arguments(self):
        frame = b'ARGC00000002ARGV00000003oneARGV00000003two'
        cases = [(frame[:i], 1) for i in range(len(frame))]
        cases += [(frame+b'extra', 1), (frame, 2), (b'ARGCffffffff', 1),
                  (b'ARGC00000001ARGVffffffff', 1), (b'ARGC0000000z', 1),
                  (b'XXXX00000001ARGV00000000', 1), (b'ARGC00000001XXXX00000000', 1)]
        for data, groups in cases:
            with self.subTest(data=data, groups=groups):
                os.ftruncate(self.fd, 0)
                os.lseek(self.fd, 0, os.SEEK_SET)
                os.write(self.fd, data)
                os.lseek(self.fd, 0, os.SEEK_SET)
                with self.assertRaises(self.protocol.Error): self.protocol.RArgvFile(self.fd, groups)
        # Preserve the existing stream decoder's padded hexadecimal syntax.
        os.ftruncate(self.fd, 0)
        os.lseek(self.fd, 0, os.SEEK_SET)
        os.write(self.fd, b'ARGC       1ARGV       3one')
        os.lseek(self.fd, 0, os.SEEK_SET)
        self.assertEqual(self.protocol.RArgvFile(self.fd, 1), (['one'],))

    def test_helpers_contain_bad_commands_and_accept_the_next_request(self):
        compiler = shutil.which('clang')
        if not compiler: self.skipTest('Clang unavailable')
        build = Path.cwd()
        env = dict(os.environ, PYTHONPATH=str(next(build.glob('_include_server/lib.*'))))
        for native in (False, True):
            with self.subTest(native=native), contextlib.ExitStack() as cleanup:
                work = Path(cleanup.enter_context(tempfile.TemporaryDirectory(prefix='distcc-worker-')))
                control, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                cleanup.callback(control.close); cleanup.callback(child.close)
                control.settimeout(15)
                process = subprocess.Popen([sys.executable, str(build/'distcc-clang'),
                    '--native-worker' if native else '--worker', str(child.fileno()), compiler],
                    pass_fds=(child.fileno(),), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                def finish():
                    control.close()
                    try: process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.communicate(timeout=5)
                cleanup.callback(finish)
                child.close()
                if native: self.assertEqual(control.recv(1), b'R')
                outputs = [os.memfd_create('distcc-worker-test') for _ in range(3)]
                for fd in outputs: cleanup.callback(os.close, fd)
                directory = [str(build), compiler]
                valid = [directory, [compiler, '--version']] if native else [[
                    'distcc-clang', str(work/'root'), str(work/'output.o'), str(work), 'remote', '0', str(work/'output.d'),
                    compiler, '--distcc-frontend', 'client', '-version']]
                # Broken framing and well-framed but invalid command shapes
                # must fail their invocation, never the resident process.
                cases = [(b'bad', b'C', False),
                         ([[], []] if native else [[]], b'C', False)]
                if native: cases.append((b'bad', b'P', False))
                cases.append((valid, b'C', True))
                for payload, operation, success in cases:
                    os.ftruncate(self.fd, 0); os.lseek(self.fd, 0, os.SEEK_SET)
                    if isinstance(payload, bytes): os.write(self.fd, payload)
                    else:
                        for group in payload: self.protocol.XArgv(self.fd, group)
                    os.lseek(self.fd, 0, os.SEEK_SET)
                    for fd in outputs: os.ftruncate(fd, 0); os.lseek(fd, 0, os.SEEK_SET)
                    reply, peer = socket.socketpair()
                    with reply, peer:
                        reply.settimeout(15)
                        fds = [self.fd, peer.fileno(), *outputs] if native else [self.fd, peer.fileno(), *outputs[1:]]
                        control.sendmsg([operation], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))])
                        channel = reply if native else control
                        response = channel.recv(16 if native else 24, socket.MSG_WAITALL)
                    self.assertEqual(len(response), 16 if native else 24)
                    status = struct.unpack_from('=Q', response)[0]
                    self.assertEqual(os.waitstatus_to_exitcode(status) == 0, success,
                                     os.pread(outputs[2], 65536, 0))
                    self.assertIsNone(process.poll())
                    if success:
                        self.assertIn(b'version', os.pread(outputs[1], 65536, 0)+os.pread(outputs[2], 65536, 0))


class ProtocolTest(DistccTestCase):
    daemon_env = {'DISTCCD_PATH': os.defpath}

    def setUp(self):
        super().setUp()
        self.env = dict(self.base_env, DISTCC_HOSTS=self.host(0),
                        DISTCC_BROKER='', DISTCC_DIR=str(self.ddir),
                        DISTCC_FALLBACK='0', DISTCC_VERBOSE='1', TMPDIR=str(self.work))
        self.source = self.work / 'source with spaces.c'
        self.source.write_text('int main(void) { return 0; }\n')

    def compile_source(self, flags=(), prefix=()):
        obj = self.work / 'result.o'
        result = subprocess.run([*prefix, str(self.build / 'distcc'), 'gcc',
                                 *flags, '-c', str(self.source), '-o', str(obj)],
                                env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('compiled on ', result.stderr)
        exe = self.work / 'result'
        subprocess.run(['gcc', str(obj), '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe)], check=True)
        return result

    def test_large_argument_frame(self):
        # The wire frame exceeds ordinary socket buffers; all flags must arrive.
        self.compile_source(['-fno-inline'] * 16000)

    def test_compression_round_trip(self):
        for codec in ('', ',lzo', ',lz4', ',lz4s'):
            self.env['DISTCC_HOSTS'] = self.host(0, codec)
            for body in ('', 'const char repeated[] = "' + '0' * 1000000 + '";\n',
                         'const char data[] = "' + os.urandom(32000).hex() + '";\n'):
                with self.subTest(codec=codec, size=len(body)):
                    self.source.write_text(body + '#warning compression-test\n'
                                           'int main(void) { return 0; }\n')
                    result = self.compile_source()
                    self.assertIn('compression-test', result.stderr)

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_diagnostics_without_client_spool_files(self):
        self.env.update(DISTCC_HOSTS=self.host(0, ',lz4'), DISTCC_SAVE_TEMPS='1')
        self.source.write_text('int main(void) { int direct_diagnostics; return 0; }\n')
        trace = self.work / 'files.trace'
        result = self.compile_source(['-Wunused-variable'],
                                     prefix=['strace', '-f', '-e', 'trace=openat', '-o', str(trace)])
        self.assertIn('direct_diagnostics', result.stderr)
        calls = trace.read_text()
        self.assertNotIn('distcc_server_stdout', calls)
        self.assertNotIn('distcc_server_stderr', calls)

    def test_lz4_rejects_invalid_blocks(self):
        def token(name, value):
            return f'{name}{value:08x}'.encode()

        args = [b'gcc', b'-c', b'source.i', b'-o', b'result.o']
        header = token('DIST', 6) + token('ARGC', len(args))
        header += b''.join(token('ARGV', len(arg)) + arg for arg in args)
        # Missing size, impossible output size, malformed block, size mismatch,
        # and an advertised block cut short by EOF must all fail closed.
        for data, extra in ((b'bad', 0), (struct.pack('!I', 0xffffffff) + b'\x00', 0),
                            (struct.pack('!I', 1) + b'\xff', 0),
                            (struct.pack('!I', 2) + b'\x10x', 0),
                            (struct.pack('!I', 1), 2)):
            with self.subTest(data=data, extra=extra):
                with socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5) as conn:
                    conn.sendall(header + token('DOTI', len(data) + extra) + data)
                    conn.shutdown(socket.SHUT_WR)
                    response = b''
                    try:
                        while chunk := conn.recv(4096):
                            response += chunk
                    except ConnectionResetError:
                        pass
                    self.assertNotIn(b'DOTO', response)
        self.env['DISTCC_HOSTS'] = self.host(0, ',lz4')
        self.compile_source()

    def test_lz4_rejects_oversized_input_before_reading(self):
        source = self.work / 'oversized.i'
        with source.open('wb') as stream:
            stream.truncate(0x7e000001)  # sparse; one byte above LZ4_MAX_INPUT_SIZE
        self.env.update(DISTCC_HOSTS=self.host(0, ',lz4'), DISTCC_MAX_RETRIES='1')
        result = subprocess.run([str(self.build / 'distcc'), 'gcc', '-c', str(source),
                                 '-o', str(self.work / 'oversized.o')],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('compiled on ', result.stderr)
        self.assertFalse((self.work / 'oversized.o').exists())

    def test_stream_rejects_invalid_chunks(self):
        args = [b'gcc', b'-c', b'source.i', b'-o', b'result.o']
        header = b'DIST00000008' + f'ARGC{len(args):08x}'.encode()
        header += b''.join(f'ARGV{len(arg):08x}'.encode() + arg for arg in args)
        # Invalid sentinel, zero plaintext with data, oversized chunk, expanded
        # "compressed" size, and malformed LZ4 must fail before publication.
        cases = [(1, b'x'), (0xffffffff, struct.pack('!II', 0, 1)),
                 (0xffffffff, struct.pack('!II', 262145, 0)),
                 (0xffffffff, struct.pack('!II', 1, 1)),
                 (0xffffffff, struct.pack('!II', 4, 1) + b'\xff')]
        for size, chunk in cases:
            with self.subTest(size=size, chunk=chunk):
                with socket.create_connection(('127.0.0.1', self.ports[0]), timeout=5) as conn:
                    conn.sendall(header + f'DOTI{size:08x}'.encode() + chunk)
                    conn.shutdown(socket.SHUT_WR)
                    reply = bytearray()
                    try:
                        while data := conn.recv(4096):
                            reply.extend(data)
                    except ConnectionResetError:
                        pass
                    self.assertNotIn(b'DOTO', reply)
        self.env['DISTCC_HOSTS'] = self.host(0, ',lz4s')
        self.compile_source()

    @unittest.skipUnless(shutil.which('strace'), 'strace is required')
    def test_batched_arguments_and_cork(self):
        trace = self.work / 'transport.trace'
        for enabled in ('0', '1'):
            with self.subTest(cork=enabled):
                self.env['DISTCC_TCP_CORK'] = enabled
                self.compile_source(['-fno-inline'] * 100, prefix=(
                    'strace', '-yy', '-s', '64', '-e', 'trace=write,setsockopt',
                    '-o', str(trace)))
                lines = trace.read_text().splitlines()
                writes = [line for line in lines if re.search(r'write\(\d+<TCP:', line)]
                frames = [line for line in writes if '"ARGC' in line]
                self.assertEqual(len(frames), 1)
                self.assertIn('ARGV', frames[0])
                corks = [line for line in lines if 'TCP_CORK' in line]
                self.assertEqual(len(corks), 2 if enabled == '1' else 0)
                self.assertTrue(all('= 0' in line for line in corks))


if __name__ == '__main__':
    unittest.main()
