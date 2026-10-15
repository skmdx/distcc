#!/usr/bin/env python3
"""Remote preprocessing with mutable inputs, real compilers and pooled TCP."""
import os
import xxhash
import random
from pathlib import Path
import re
import select
import shlex
import shutil
import socket
import signal
import subprocess
import threading
import time
import unittest

from test_broker import BrokerTestCase


class PreprocessTest(BrokerTestCase):
    compression = 'lzo'
    transport = ''

    def setUp(self):
        super().setUp()
        self.start_broker(self.host(0, ',' + self.compression + self.transport) + ' --localslots_cpp=1', idle=60)
        self.env.update(TMPDIR=str(self.root), DISTCC_CLIENT_TMP=str(self.work),
                        DISTCC_CPP_LOOKUP_LIBRARY=str(self.build / 'cpp_lookup.so'),
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

    def test_local_lookup_cache_then_remote_compile(self):
        search = []
        for i in range(300):
            directory = self.root / ('lookup-' + str(i))
            directory.mkdir()
            search.append('-I' + str(directory))
        (directory / 'answer.h').write_text('#define ANSWER 42\n')
        self.source.write_text('#include "answer.h"\nint answer(void) { return ANSWER; }\n')
        scanner = self.env.pop('INCLUDE_SERVER_PORT')
        try:
            result = self.run_cc('gcc', [*search, '-c', self.source, '-o', 'lookup.o'])
        finally:
            self.env['INCLUDE_SERVER_PORT'] = scanner
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertTrue((self.work / 'lookup.o').exists())
        self.assertRegex(self.get_stats(self.stats_ports[0]), r'dcc_compile_ok [1-9]')

    def test_scanner_wait_does_not_reserve_worker(self):
        pid = int(self.env['INCLUDE_SERVER_PID'])
        os.kill(pid, signal.SIGSTOP)
        client = subprocess.Popen([str(self.build / 'distcc'), 'gcc', '-c',
                                   str(self.source), '-o', 'waiting.o'],
                                  cwd=self.work, env=self.env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        leases = []
        try:
            deadline = time.monotonic() + 5
            while 'send CDIR' not in (self.work / 'client.log').read_text():
                self.assertLess(time.monotonic(), deadline, 'scanner not reached')
                time.sleep(.01)
            # All four real worker slots must remain available while this
            # client's include request is blocked in the stopped scanner.
            for _ in range(4):
                lease = self.request(9)
                leases.append(lease)
                status, _, descriptors = self.receive(lease)
                for fd in descriptors:
                    os.close(fd)
                self.assertEqual(status, 0)
        finally:
            for lease in leases:
                lease.close()
            os.kill(pid, signal.SIGCONT)
            try:
                client.wait(timeout=15)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.wait()
        self.assertEqual(client.returncode, 0)
        self.assertTrue((self.work / 'waiting.o').exists())

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
        self.assertEqual(remote.returncode, local.returncode,
                         remote.stderr.decode() + (self.work / 'client.log').read_text())
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
        self.source.write_text('#include <stddef.h>\n#include "value.h"\nint n = VALUE;\nconst char *p = __FILE__;\n')
        for compiler in ('gcc', shutil.which('clang'), 'clang' if shutil.which('clang') else None):
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
        self.assertIn('pooled TCP job completed' if self.transport == ',tcp' else
                      'client finished sending request to server', log)

    def test_utf8_bom_header_and_macro_diagnostic(self):
        header = self.work / 'bom.h'
        header.write_bytes(b'\xef\xbb\xbf#define NONNULL(x) ((x) != 0)\n'
                           b'static int values[2];\n')
        self.source.write_text('#include "bom.h"\n'
                               'int f(void) { return NONNULL(values); }\n')
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            args = ['-I' + str(self.work), '-O2', '-Werror=address',
                    '-c', self.source, '-o', 'bom.o']
            local = self.run_cc(compiler, args, False)
            self.assertEqual(local.returncode, 0, local.stderr.decode())
            remote = self.run_cc(compiler, args)
            self.assertEqual(remote.returncode, 0, remote.stderr.decode())

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

    def test_compiler_scanner_computed_has_include_and_generation(self):
        self.source.write_text('#define HEADER(name) <name.h>\n'
                               '#if __has_include(HEADER(generated))\n'
                               '#include HEADER(generated)\n'
                               '#else\n#define VALUE 7\n#endif\nint n = VALUE;\n')
        header = self.work / 'generated.h'
        for compiler in ('gcc', shutil.which('clang')):
            if not compiler:
                continue
            with self.subTest(compiler=compiler):
                self.assertIn(b'7', self.compare(compiler, ['-I.', '-P']).stdout)
                header.write_text('#define VALUE 19\n')
                self.assertIn(b'19', self.compare(compiler, ['-I.', '-P']).stdout)
                header.unlink()
                self.assertIn(b'7', self.compare(compiler, ['-I.', '-P']).stdout)

    def test_compiler_scanner_uses_gcc_macros(self):
        (self.work / 'gcc.h').write_text('#define VALUE 41\n')
        (self.work / 'clang.h').write_text('#define VALUE 43\n')
        self.source.write_text('#ifdef __clang__\n#define HEADER "clang.h"\n'
                               '#else\n#define HEADER "gcc.h"\n#endif\n'
                               '#include HEADER\nint n = VALUE;\n')
        for compiler, value in (('gcc', b'41'), ('g++', b'41'),
                                (shutil.which('clang'), b'43')):
            if compiler:
                with self.subTest(compiler=compiler):
                    args = ['-P'] if compiler != 'g++' else ['-x', 'c++', '-P']
                    self.assertIn(value, self.compare(compiler, args).stdout)

    def test_gcc_only_branch_existence_probe_is_transferred(self):
        # GCC -M does not list existence-only probes, and Clang selects the
        # other branch for this GCC builtin. The conservative guard is needed.
        (self.work / 'probe.h').write_text('/* must exist remotely */\n')
        self.source.write_text(
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if __has_include("probe.h")\n#define VALUE 19\n'
            '#else\n#define VALUE 7\n#endif\n'
            '#else\n#define VALUE 42\n#endif\nint value = VALUE;\n')
        dependencies = self.run_cc('gcc', ['-M', self.source], False)
        self.assertNotIn(b'probe.h', dependencies.stdout)
        self.compare('gcc', ['-P'])

    def test_gcc_native_and_optimizer_flags_preserve_header_selection(self):
        (self.work / 'abm.h').write_text('#define VALUE 19\n')
        (self.work / 'plain.h').write_text('#define VALUE 7\n')
        self.source.write_text('#ifdef __ABM__\n#include "abm.h"\n'
                               '#else\n#include "plain.h"\n#endif\nint n = VALUE;\n')
        for flags in (['-mabm', '-fno-tree-pre'],
                      ['-mno-abm', '-ftree-pre'], ['-march=native', '-mtune=native']):
            with self.subTest(flags=flags):
                self.compare('gcc', ['-P', *flags])

    def test_gcc_cxx_system_header_conditions(self):
        self.source.write_text('#include <string>\nstd::string value;\n')
        self.compare('g++', ['-P', '-std=c++17'])

    def test_gcc_only_indirect_probe_is_transferred(self):
        (self.work / 'probe.h').write_text('/* existence only */\n')
        for definition in ('#define HAS __has_include',
                           '#define HAS(x) __has_include(x)',
                           '#define JOIN(a,b) a ## b\n#define HAS JOIN(__has_,include)'):
            with self.subTest(definition=definition):
                self.source.write_text(
                    definition + '\n#if __has_builtin(__builtin_speculation_safe_value)\n'
                    '#if HAS("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
                    '#else\nint other;\n#endif\n')
                self.assertIn(b'present', self.compare('gcc', ['-P']).stdout)

    def test_gcc_command_line_probe_alias_is_transferred(self):
        (self.work / 'probe.h').write_text('/* existence only */\n')
        self.source.write_text(
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if HAS("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
            '#else\nint other;\n#endif\n')
        for definition in ('HAS=__has_include', 'HAS(x)=__has_include(x)'):
            with self.subTest(definition=definition):
                self.assertIn(b'present', self.compare('gcc', ['-P', '-D' + definition]).stdout)

    def test_gcc_pasted_alias_rechecks_macro_namespace_and_header(self):
        header = self.work / 'probe.h'
        header.write_text('/* existence only */\n')
        self.source.write_text(
            '#define JOIN(a,b) a ## b\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if JOIN(H,A)("probe.h")\nint present;\n#else\nint absent;\n#endif\n'
            '#else\nint other;\n#endif\n')
        self.assertIn(b'absent', self.compare('gcc', ['-P', '-DHA(x)=0']).stdout)
        args = ['-P', '-DHA=__has_include']
        self.assertIn(b'present', self.compare('gcc', args).stdout)
        header.unlink()
        self.assertIn(b'absent', self.compare('gcc', args).stdout)
        header.write_text('/* recreated */\n')
        self.assertIn(b'present', self.compare('gcc', args).stdout)

    def test_gcc_computed_next_probe_is_transferred(self):
        first, second = self.work / 'first', self.work / 'second'
        first.mkdir()
        second.mkdir()
        (second / 'optional.h').write_text('/* existence only */\n')
        (first / 'probe.h').write_text(
            '#define NAME(x) <x.h>\n'
            '#if __has_builtin(__builtin_speculation_safe_value)\n'
            '#if __has_include_next(NAME(optional))\nint present;\n'
            '#else\nint absent;\n#endif\n#else\nint other;\n#endif\n')
        self.source.write_text('#include "probe.h"\n')
        self.assertIn(b'present', self.compare('gcc', ['-P', '-Ifirst', '-Isecond']).stdout)

    def test_compile_execute_and_pool_reuse(self):
        self.source.write_text('#include "value.h"\nint main(void) { return VALUE - 19; }\n')
        (self.work / 'value.h').write_text('#define VALUE 19\n')
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        before = int(match[1])
        for _ in range(3):
            result = self.run_cc('gcc', ['-c', self.source, '-o', 'hello.o'])
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        match = re.search(r'dcc_tcp_accept (\d+)', self.get_stats(self.stats_ports[0]))
        assert match is not None, 'daemon did not report its accept counter'
        after = int(match[1])
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
        header = self.work / 'absolute.h'
        header.write_text('int n;\n')
        self.source.write_text('#include "%s"\n' % header)
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

    def test_failed_include_analysis_is_not_cached_as_complete(self):
        (self.work / 'unsupported.h').write_text(
            '#include "%s"\n' % (self.work / 'value.h'))
        (self.work / 'value.h').write_text('#define VALUE 17\n')
        self.source.write_text('#include "unsupported.h"\nint n = VALUE;\n')
        for attempt in range(3):
            with self.subTest(attempt=attempt):
                (self.work / 'client.log').write_text('')
                result = self.run_cc('gcc', ['-E', self.source])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'failed to get includes',
                              (self.work / 'client.log').read_bytes())
        (self.work / 'unsupported.h').write_text('#include "value.h"\n')
        self.assertIn(b'17', self.compare(args=['-P']).stdout)

    def test_has_include_compatibility_definition(self):
        (self.work / 'optional.h').write_text('#define VALUE 17\n')
        self.source.write_text(
            '#ifndef __has_include\n#define __has_include(header) 0\n#endif\n'
            '#if __has_include("optional.h")\n#include "optional.h"\n'
            '#else\n#define VALUE 0\n#endif\nint n = VALUE;\n')
        for compiler in ('gcc', shutil.which('clang')):
            if compiler:
                with self.subTest(compiler=compiler):
                    self.compare(compiler, ['-P'])

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


class TcpPreprocessTest(PreprocessTest):
    compression = 'lz4s'
    transport = ',tcp'


    def cache_counts(self):
        log = (self.work / 'client.log').read_text()
        counts = re.findall(r'input cache: (\d+) of (\d+) entries transferred', log)
        self.assertTrue(counts, 'cached RPC was not reached: ' + log)
        return tuple(map(int, counts[-1]))

    def test_cache_reuses_and_repairs_input_snapshots(self):
        header = ('/*' + random.Random(31).randbytes(16384).hex() + '*/\n#define VALUE 731\n').encode()
        (self.work / 'value.h').write_bytes(header)
        (self.work / 'empty.h').write_bytes(b'')
        self.source.write_text('#include "value.h"\n#include "empty.h"\nint n = VALUE;\n')
        self.compare(args=['-P'])
        self.assertGreater(self.cache_counts()[0], 0)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

        key = xxhash.xxh3_64(header).hexdigest()
        entry = self.root / f'distcc-cache-{os.geteuid()}' / f'{int(key[:2], 16) % 64:02x}' / key
        self.assertEqual(entry.read_bytes(), header)
        entry.chmod(0o600)
        entry.write_bytes(b'corrupt cached input')
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 1)
        self.assertEqual(entry.read_bytes(), header)
        entry.unlink()
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 1)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

        (self.work / 'value.h').write_bytes(header.replace(b'VALUE 731', b'VALUE 732'))
        self.assertIn(b'732', self.compare(args=['-P']).stdout)
        self.assertEqual(self.cache_counts()[0], 1)

    def test_cached_symlink_retarget(self):
        padding = '/*' + random.Random(39).randbytes(16384).hex() + '*/\n'
        for name, value in (('first', 71), ('second', 72)):
            directory = self.work / name
            directory.mkdir()
            (directory / 'nested').mkdir()
            (directory / 'value.h').write_text(padding + f'#define VALUE {value}\n')
        alias = self.work / 'alias'
        alias.symlink_to('first/nested')
        # Resolving '..' before the symlink would choose the wrong header.
        (self.work / 'value.h').write_text('#error incorrect path normalization\n')
        self.source.write_text('#include "alias/../value.h"\nint n = VALUE;\n')
        self.assertIn(b'71', self.compare(args=['-P']).stdout)
        self.cache_counts()
        alias.unlink()
        alias.symlink_to('second/nested')
        self.assertIn(b'72', self.compare(args=['-P']).stdout)
        self.cache_counts()
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

    def test_uncacheable_input_skips_lookup(self):
        self.source.write_text('/*' + random.Random(61).randbytes(3 * 1024 * 1024).hex() + '*/\nint n;\n')
        for _ in range(2):
            self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())

    def test_mixed_uncacheable_and_cached_inputs(self):
        (self.work / 'large.h').write_bytes(b'/*' + b' ' * (5 * 1024 * 1024) + b'*/\n')
        header = '/*' + random.Random(62).randbytes(16384).hex() + '*/\n#define VALUE 733\n'
        (self.work / 'value.h').write_text(header)
        self.source.write_text('#include "large.h"\n#include "value.h"\nint n = VALUE;\n')
        self.compare(args=['-P'])
        self.assertGreater(self.cache_counts()[0], 0)
        self.compare(args=['-P'])
        self.assertEqual(self.cache_counts()[0], 0)

    def test_missing_sidecar_does_not_reuse_cache(self):
        self.source.write_text('/*' + random.Random(32).randbytes(16384).hex() + '*/\nint n;\n')
        self.compare(args=['-P'])
        self.cache_counts()
        for digest in self.work.rglob('*.xxh3'):
            digest.unlink()
        (self.work / 'client.log').write_text('')
        self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())

    def test_disabled_server_cache(self):
        self.source.write_text('/*' + random.Random(33).randbytes(16384).hex() + '*/\nint n;\n')
        try:
            self.restart_server(self.build / 'distccd', DISTCCD_FILE_CACHE='')
            self.compare(args=['-P'])
            first = self.cache_counts()[0]
            self.compare(args=['-P'])
            self.assertGreater(first, 0)
            self.assertEqual(self.cache_counts()[0], first)
        finally:
            self.restart_server(self.build / 'distccd')

    def test_small_inputs_skip_manifest_round_trip(self):
        for _ in range(2):
            self.compare(args=['-P'])
        self.assertNotIn('input cache:', (self.work / 'client.log').read_text())


if __name__ == '__main__':
    unittest.main()
