#!/usr/bin/env python3
"""Remote preprocessing with mutable inputs, real compilers and pooled TCP."""
import os
import re
import select
import shlex
import shutil
import socket
import subprocess
import threading
import unittest

from test_broker import BrokerTestCase


class PreprocessTest(BrokerTestCase):
    compression = 'lzo'

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',' + self.compression) + ' --localslots_cpp=1', idle=60)
        self.env.update(TMPDIR=str(self.root), DISTCC_CLIENT_TMP=str(self.work),
                        DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_TESTING_INCLUDE_SERVER='1',
                        DISTCC_VERBOSE='1', DISTCC_LOG=str(self.work / 'client.log'))
        result = subprocess.run([str(self.build / 'pump'), '--startup'],
                                cwd=self.work, env=self.env, capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        for line in result.stdout.splitlines():
            words = shlex.split(line)
            if len(words) == 2 and words[0] == 'export':
                key, value = words[1].split('=', 1)
                self.env[key] = value
        self.assertIn('INCLUDE_SERVER_PORT', self.env)
        self.addCleanup(self.shutdown_pump)

    def shutdown_pump(self):
        subprocess.run([str(self.build / 'pump'), '--shutdown'], env=self.env,
                       cwd=self.work, capture_output=True, timeout=10, check=True)

    def run_cc(self, compiler, args, remote=True):
        prefix = [str(self.build / 'distcc')] if remote else []
        return subprocess.run(prefix + [compiler] + list(map(str, args)),
                              cwd=self.work, env=self.env, capture_output=True,
                              timeout=15)

    def compare(self, compiler='gcc', args=()):
        args = ['-E', *args, self.source]
        local = self.run_cc(compiler, args, False)
        local_deps = ((self.work / 'dep.d').read_text().split()
                      if '-MF' in args else None)
        remote = self.run_cc(compiler, args)
        self.assertEqual(remote.returncode, local.returncode, remote.stderr.decode())
        self.assertEqual(remote.stdout, local.stdout, remote.stderr.decode())
        if local.returncode:
            self.assertEqual(remote.stderr, local.stderr)
        self.assertNotIn(b'preprocessing locally', remote.stderr)
        if local_deps is not None:
            self.assertEqual((self.work / 'dep.d').read_text().split(), local_deps)
        return remote

    def test_stdout_files_macros_and_dependencies(self):
        header = self.work / 'value.h'
        header.write_text('#define VALUE 17\n')
        self.source.write_text('#include "value.h"\nint n = VALUE;\nconst char *p = __FILE__;\n')
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            with self.subTest(compiler=compiler):
                self.compare(compiler)
                self.compare(compiler, ['-P', '-dM'])
                self.compare(compiler, [f'-ffile-prefix-map={self.work}=/mapped'])
                self.compare(compiler, ['-MMD', '-MF', 'dep.d', '-MT', 'target'])
                self.assertIn('target:', (self.work / 'dep.d').read_text())
                self.compare(compiler, ['-MMD', '-MF', 'dep.d'])
                args = ['-E', self.source, '-o', 'result.i']
                local = self.run_cc(compiler, args, False)
                expected = (self.work / 'result.i').read_bytes()
                remote = self.run_cc(compiler, args)
                self.assertEqual(remote.returncode, local.returncode, remote.stderr.decode())
                self.assertEqual(remote.stdout, b'')
                self.assertEqual((self.work / 'result.i').read_bytes(), expected)
        log = (self.work / 'client.log').read_text()
        self.assertIn('client finished sending request to server', log)

    def test_changed_generated_deleted_headers_and_symlinks(self):
        for directory in ('first', 'second'):
            (self.work / directory).mkdir()
        later = self.work / 'second' / 'value.h'
        earlier = self.work / 'first' / 'value.h'
        later.write_text('#define VALUE 11\n')
        self.source.write_text('#include <value.h>\nint value = VALUE;\n')
        args = ['-I', 'first', '-I', 'second', '-P']
        self.assertIn(b'11', self.compare(args=args).stdout)
        later.write_text('#define VALUE 12\n')
        self.assertIn(b'12', self.compare(args=args).stdout)
        earlier.write_text('#define VALUE 13\n')
        self.assertIn(b'13', self.compare(args=args).stdout)
        earlier.unlink()
        self.assertIn(b'12', self.compare(args=args).stdout)
        (self.work / 'alias').symlink_to('second')
        self.compare(args=['-I', 'alias'])
        earlier.write_text('#define VALUE 14\n')
        (self.work / 'alias').unlink()
        (self.work / 'alias').symlink_to('first')
        self.assertIn(b'14', self.compare(args=['-I', 'alias', '-P']).stdout)
        self.source.write_text('int changed_source = 15;\n')
        self.assertIn(b'15', self.compare().stdout)

    def test_compile_execute_and_pool_reuse(self):
        self.source.write_text('#include "value.h"\nint main(void) { return VALUE - 19; }\n')
        (self.work / 'value.h').write_text('#define VALUE 19\n')
        before = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        for _ in range(3):
            result = self.run_cc('gcc', ['-c', self.source, '-o', 'hello.o'])
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        after = int(re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))[1])
        self.assertEqual(after - before, 1)
        subprocess.run([self.cc, 'hello.o', '-o', 'hello'], cwd=self.work, check=True)
        subprocess.run([str(self.work / 'hello')], check=True)

    def test_error_preserves_partial_stdout(self):
        self.source.write_text('int before;\n#error deliberate_error\nint after;\n')
        self.compare(args=['-P'])
        args = ['-E', '-P', self.source, '-o', 'failed.i']
        local = self.run_cc('gcc', args, False)
        self.assertFalse((self.work / 'failed.i').exists())
        (self.work / 'failed.i').write_text('old output')
        remote = self.run_cc('gcc', args)
        self.assertEqual(remote.returncode, local.returncode)
        self.assertFalse((self.work / 'failed.i').exists())

    def test_unsupported_inputs_preprocess_locally(self):
        self.env.pop('DISTCC_TESTING_INCLUDE_SERVER')
        self.source.write_text('#define HEADER "optional.h"\n#if __has_include(HEADER)\n#include HEADER\n#endif\nint n;\n')
        # Include-analysis fallback still permits remote compilation, while -E
        # itself runs locally only when fallback is allowed.
        self.env['DISTCC_FALLBACK'] = '1'
        local = self.run_cc('gcc', ['-E', self.source], False)
        remote = self.run_cc('gcc', ['-E', self.source])
        self.assertEqual(remote.returncode, 0, remote.stderr.decode())
        self.assertEqual(remote.stdout, local.stdout)
        self.assertIn('preprocessing locally', (self.work / 'client.log').read_text())

    def test_header_existence_without_including_it(self):
        self.source.write_text('#if __has_include("optional.h")\nint present;\n#else\nint absent;\n#endif\n')
        self.assertIn(b'absent', self.compare(args=['-P']).stdout)
        (self.work / 'optional.h').write_text('/* presence is enough */\n')
        self.assertIn(b'present', self.compare(args=['-P']).stdout)

    def test_remote_cpp_does_not_wait_for_local_cpp_slot(self):
        lease = self.request(6)
        status, _, descriptors = self.receive(lease)
        self.assertEqual(status, 0)
        for fd in descriptors:
            os.close(fd)
        self.compare(args=['-P'])
        lease.close()

    def test_truncated_response_does_not_publish_stdout_twice(self):
        # Forward a real compilation, then drop the end of its result after
        # SOUT. The local retry must be the sole writer of user-visible output.
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(10)
        self.addCleanup(listener.close)
        failures = []

        def proxy():
            try:
                with listener.accept()[0] as client, socket.create_connection(
                        ('127.0.0.1', self.ports[0]), timeout=10) as server:
                    response = bytearray()
                    while True:
                        readable, _, _ = select.select([client, server], [], [], 10)
                        if not readable:
                            raise TimeoutError('compiler proxy stalled')
                        for sock in readable:
                            chunk = sock.recv(65536)
                            if not chunk:
                                if sock is server:
                                    client.sendall(response[:-12])
                                    return
                                raise ConnectionError('client abandoned compile')
                            if sock is server:
                                response.extend(chunk)
                            else:
                                server.sendall(chunk)
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=proxy)
        worker.start()
        self.env.pop('DISTCC_BROKER')
        self.env.update(DISTCC_HOSTS=f'127.0.0.1:{listener.getsockname()[1]},cpp,{self.compression}',
                        DISTCC_FALLBACK='1', DISTCC_MAX_RETRIES='1')
        self.compare(args=['-P'])
        worker.join(timeout=12)
        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])
        self.assertIn('running locally instead', (self.work / 'client.log').read_text())


class Lz4PreprocessTest(PreprocessTest):
    compression = 'lz4'

    def test_mixed_codec_cache(self):
        self.env.pop('DISTCC_BROKER')
        (self.work / 'empty.h').write_bytes(b'')
        self.source.write_text('#include "empty.h"\nint n;\n')
        for codec in ('lz4', 'lzo', 'lz4'):
            self.env['DISTCC_HOSTS'] = self.host(0, ',cpp,' + codec)
            self.compare(args=['-P'])
        for codec in ('lz4', 'lzo'):
            self.assertTrue(list(self.work.rglob('empty.h.' + codec)))


if __name__ == '__main__':
    unittest.main()
