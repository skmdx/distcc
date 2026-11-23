"""Real compiler equivalence and lifecycle through the broker's worker cache."""
import concurrent.futures
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import signal
import subprocess
import sys
import time
import unittest

from test_broker import BrokerTestCase
from test_adaptive import stats_response


class ExplicitClangLauncherTest(BrokerTestCase):
    worker_address = '127.0.0.2'

    def test_private_helper_with_masquerade_first_on_path(self):
        compiler = shutil.which('clang++')
        if compiler is None:
            self.skipTest('Clang++ unavailable')
        client = self.build/'distcc'
        wrappers = self.work/'wrappers'
        wrappers.mkdir()
        for name in ('clang', 'clang++', 'distcc-clang'):
            (wrappers/name).symlink_to(client)
        search = str(wrappers) + os.pathsep + self.base_env['PATH']
        self.restart_server(self.build/'distccd', PATH=search)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'))
        env = dict(self.env, PATH=search, DISTCC_CLANG_LOCAL='1', DISTCC_FRONTEND_REMOTE='1',
                   DISTCC_FALLBACK='0', DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_VERBOSE='1')
        probe = subprocess.run([client, compiler, '-dumpversion'], env=env, capture_output=True,
                               check=True, timeout=30)
        self.assertIn(b'started local compiler libraries', probe.stderr)
        cases = [('cpp', [], b'operation=P'),
                 ('hip', ['--cuda-host-only', '-nogpuinc', '-nogpulib', '-x', 'hip',
                          '--offload-arch=gfx1032', '--offload-arch=gfx90c'], b'prepared AMDGPU offload')]
        for suffix, flags, preparation in cases:
            with self.subTest(language=suffix):
                source = self.work/f'input.{suffix}'
                source.write_text('int main() { return 0; }\n')
                obj = self.work/f'{suffix}.o'
                compiled = subprocess.run([client, compiler, *flags, '-c', source, '-o', obj],
                                          env=env, capture_output=True, check=True, timeout=30)
                self.assertIn(preparation, compiled.stderr)
                self.assertIn(b'pooled TCP job completed', compiled.stderr)
                program = self.work/suffix
                subprocess.run([client, compiler, '-fuse-ld=lld', obj, '-o', program], env=env,
                               check=True, capture_output=True, timeout=30)
                subprocess.run([program], check=True, timeout=5)


class LocalClangTest(BrokerTestCase):
    def start_local(self, hosts=None):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang unavailable')
        self.compiler = compiler
        self.broker = self.start_broker(hosts=hosts)
        self.env.update(DISTCC_HOSTS='localhost/4', DISTCC_CLANG_LOCAL='1', DISTCC_VERBOSE='1',
                        DISTCC_CLANG_CODEGEN='1',
                        DISTCC_LOG=str(self.work/'client.log'),
                        PATH=str(self.build)+os.pathsep+self.env['PATH'])

    def command(self, arguments, reused=True, **options):
        return subprocess.run(([str(self.build/'distcc')] if reused else [])+
                              [self.compiler, *arguments], env=self.env, cwd=self.work,
                              capture_output=True, timeout=30, **options)

    def test_resident_worker_releases_inherited_caller_pipe(self):
        self.start_local()
        reader, writer = os.pipe()
        try:
            result = self.command(['-dumpversion'], pass_fds=(writer,))
            self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            os.close(writer)
        try:
            self.assertTrue(select.select([reader], [], [], 2)[0],
                            'A completed command must not keep its caller waiting for EOF')
            self.assertEqual(os.read(reader, 1), b'')
            again = self.command(['-dumpversion'])
            self.assertEqual((again.returncode, again.stdout), (0, result.stdout))
            self.assertEqual((self.work/'client.log').read_text().count(
                'started local compiler libraries'), 1)
        finally:
            os.close(reader)

    @unittest.skipUnless(shutil.which('strace'), 'strace required')
    def test_prepared_local_cc1_reuses_frontend(self):
        log = self.work/'client.log'
        def capacity():
            busy = int(not log.exists() or 'operation=P' not in log.read_text())
            return stats_response(cpus=1, cores=1, jobs=1, capacity=1, busy=busy,
                                  running=busy, queued=0, preparing=0, returning=0)
        peer = self.peer(capacity)
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_local(self.host(0, f',tcp,stats={peer.port}', slots=1))
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_FRONTEND_ROOTS=str(self.work))
        resident = self.trace_local_worker()
        self.command(['-dumpversion'], check=True)  # Warm the compiler before tracing the client.
        (self.work/'value.h').write_text('#define VALUE 42\n')
        self.source.write_text('#include "value.h"\nint main(void) { return VALUE != 42; }\n')
        trace = self.work/'client.exec'
        def traced(*args):
            return subprocess.run(['strace', '-f', '-qq', '-e', 'execve', '-o', trace,
                                   self.build/'distcc', self.compiler, *args], cwd=self.work,
                                  env=self.env, capture_output=True, timeout=20)
        before = log.stat().st_size
        result = traced('-O2', '-fprofile-instr-generate', '-MD', '-MF', 'object.d',
                        '-c', self.source, '-o', 'object.o')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('prepared=1', (self.work/'broker.log').read_text())
        for op in 'IPC':
            self.assertIn('operation='+op, log.read_text()[before:])
        self.assertIn(b'distcc codegen plan:', result.stderr)
        self.assertNotIn('"-cc1"', trace.read_text())
        self.assertNotRegex(resident.read_text(), r'execve\([^\n]*"-cc1"')
        self.assertIn('value.h', (self.work/'object.d').read_text())
        self.command(['-fprofile-instr-generate', 'object.o', '-o', 'check'], reused=False, check=True)
        subprocess.run([self.work/'check'], check=True, cwd=self.work)

        (self.work/'empty.s').touch()
        before = log.stat().st_size
        result = traced('-cc1as', '-triple', subprocess.check_output(
            [self.compiler, '-dumpmachine'], text=True).strip(), '-filetype', 'obj',
            '-o', 'asm.o', 'empty.s')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('operation=C', log.read_text()[before:])
        self.assertIn(f'execve("{self.compiler}",', trace.read_text())

    def test_request_environment_is_independent_of_worker_image(self):
        hosts = self.host(0, ',tcp')
        self.start_local(hosts)
        self.env.pop('LD_BIND_NOW', None)
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_HOSTS=hosts,
                        DISTCC_FRONTEND_ROOTS=str(self.work))
        # Observe libc lookups inside the real helper. Snapshot each request
        # once, including variables the current driver might never consult.
        library = self.work/'inspect.so'
        subprocess.run([self.compiler, '-shared', '-fPIC', '-x', 'c', '-', '-o', library],
                       input=b'''
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
char *getenv(const char *key) {
    const char *path = secure_getenv("CUSTOM_REQUEST");
    int fd = path ? open(path, O_CREAT|O_EXCL|O_WRONLY, 0600) : -1;
    if (fd >= 0) {
        dprintf(fd, "%s\\n%s\\n%s\\n%s\\n", secure_getenv("CPATH"),
                secure_getenv("SOURCE_DATE_EPOCH"), secure_getenv("TMPDIR"), secure_getenv("PWD"));
        close(fd);
    }
    return secure_getenv(key);
}
''', check=True)
        helper = self.work/'inspect-clang'
        helper.write_text('#!/bin/sh\nexport LD_PRELOAD=' + shlex.quote(str(library)) +
                          '\nexec ' + shlex.quote(str(self.build/'distcc-clang')) + ' "$@"\n')
        helper.chmod(0o755)
        self.env['DISTCC_CLANG_DRIVER'] = helper.name
        self.env['PATH'] = str(self.work) + os.pathsep + self.env['PATH']
        self.source.write_text('#include <value.h>\nconst char stamp[] = __DATE__;\n'
                               'int main(void) { return VALUE; }\n')
        for i in (1, 2):
            directory = self.work/f'env-{i}'
            directory.mkdir()
            (directory/'value.h').write_text(f'#define VALUE {i}\n')
            (directory/'cwd').symlink_to(self.work)
            self.env.update(PWD=str(directory/'cwd'), SHLVL=str(i), CFLAGS=f'-O{i}',
                            CPATH=str(directory), SOURCE_DATE_EPOCH=str(86400*i),
                            TMPDIR=str(directory), LIBRARY_PATH=str(directory))
            # C runs the native preprocessor. P plans a remote frontend with
            # the driver's current header search; L prints its current -L path.
            args = ['-E', '-P', str(self.source)]
            self.env['CUSTOM_REQUEST'] = str(directory/'C')
            self.assertEqual(self.command(args, check=True).stdout,
                             self.command(args, False, check=True).stdout)
            self.env['CUSTOM_REQUEST'] = str(directory/'P')
            self.command(['-c', str(self.source), '-o', str(self.work/'env.o')], check=True)
            self.env['CUSTOM_REQUEST'] = str(directory/'L')
            link = self.command(['env.o', '-v', '-fuse-ld=lld', '-o', 'env'])
            self.assertEqual(link.returncode, 0, link.stderr)
            self.assertIn(('-L'+str(directory)).encode(), link.stderr)
            self.assertEqual(subprocess.run([self.work/'env']).returncode, i)
            for operation in 'CPL':
                self.assertEqual((directory/operation).read_text().splitlines(),
                                 [self.env[k] for k in ('CPATH', 'SOURCE_DATE_EPOCH', 'TMPDIR', 'PWD')])
        log = self.work/'client.log'
        self.assertEqual(log.read_text().count('started local compiler libraries'), 1)
        for op in 'CPL': self.assertIn('operation='+op, log.read_text())
        self.env['LD_BIND_NOW'] = '1'
        self.command(['-dumpversion'], check=True)
        self.assertEqual(log.read_text().count('started local compiler libraries'), 2)

    def test_worker_startup_uses_transport_deadline_and_reaps_timeout(self):
        self.start_local()
        helpers = self.work/'slow-start'
        helpers.mkdir()
        helper = helpers/'distcc-clang'
        pidfile = self.work/'startup.pid'
        helper.write_text('#!/bin/sh\necho $$ > ' + shlex.quote(str(pidfile)) +
                          '\nsleep 6\nexec ' + shlex.quote(str(self.build/'distcc-clang')) +
                          ' "$@"\n')
        helper.chmod(0o755)
        self.env['DISTCC_CLANG_DRIVER'] = str(helper)
        original = self.command(['-dumpversion'], reused=False)
        result = self.command(['-dumpversion'])
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (original.returncode, original.stdout, original.stderr))
        self.assertIn('started local compiler libraries', (self.work/'client.log').read_text())

        # Force a cold start with a different helper. The timeout itself
        # belongs to the requesting client.
        second = helpers/'second-clang'
        second.symlink_to(helper)
        self.env['DISTCC_CLANG_DRIVER'] = str(second)
        self.env['DISTCC_IO_TIMEOUT'] = '1'
        result = self.command(['-dumpversion'])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(Path('/proc', pidfile.read_text().strip()).exists())
        self.assertIn('IO timeout', (self.work/'client.log').read_text())


    def test_wrapper_name_containing_distcc_is_a_compiler(self):
        self.start_local()
        wrappers = self.work/'wrappers'
        drivers = self.work/'distcc-toolchain'
        wrappers.mkdir()
        drivers.mkdir()
        name = 'distcc-test-driver'
        (wrappers/name).symlink_to(self.build/'distcc')
        driver = drivers/name
        driver.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        driver.chmod(0o755)
        env = dict(self.env, PATH=os.pathsep.join((str(wrappers), str(drivers), self.env['PATH'])))
        result = subprocess.run([wrappers/name, '--version'], env=env, check=True,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout, '--version\n')

        # Compiler discovery must skip the masquerade but accept a real
        # compiler in a directory whose name happens to contain "distcc".
        name = 'clang-99997'
        (wrappers/name).symlink_to(self.build/'distcc')
        (drivers/name).symlink_to(self.compiler)
        result = subprocess.run([self.build/'distcc', name, '-dumpversion'], env=env,
                                capture_output=True, check=True, timeout=10)
        self.assertEqual(result.stdout, subprocess.check_output([self.compiler, '-dumpversion']))
        self.assertIn('started local compiler libraries', (self.work/'client.log').read_text())

    def use_unpackaged_clang(self):
        driver = self.work/'clang-99999'
        driver.write_text('#!/bin/sh\nif [ "$1" = -dumpversion ]; then echo 99999; exit 0; fi\n'
                          'exec ' + shlex.quote(self.compiler) + ' "$@"\n')
        driver.chmod(0o755)
        self.compiler = str(driver)

    def test_clang_tools_run_with_their_original_arguments(self):
        self.start_local()
        self.env.update(DISTCC_FALLBACK='0', DISTCC_FRONTEND_REMOTE='1')
        for name in ('hipify-clang', 'distcc-clang', 'clang-linker-wrapper', 'clang-offload-bundler',
                     'clang-scan-deps', 'clang-tidy', 'clang-format', 'clang-cl', 'clang-23.'):
            with self.subTest(tool=name):
                tool = self.work/name
                tool.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
                tool.chmod(0o755)
                self.compiler = str(tool)
                args = ['input.a', '-o', 'out']
                result = self.command(args, text=True)
                self.assertEqual((result.returncode, result.stdout.splitlines()), (0, args))
        self.assertNotIn('--native-worker', (self.work/'client.log').read_text())

    def test_missing_driver_uses_native_exec_diagnostic(self):
        self.saturate_frontend()
        # clang is present in PATH. An explicit bad path must still fail.
        for driver in ('clang-99998', self.work/'absent/clang', self.source/'clang'):
            for args in (['-dumpversion'], ['-c', str(self.source), '-o', 'absent.o']):
                with self.subTest(driver=driver, args=args):
                    self.compiler = str(driver)
                    result = self.command(args)
                    self.assertEqual(result.returncode, 110, result.stderr)
        log = (self.work/'client.log').read_text()
        self.assertIn('failed to exec', log)
        self.assertNotIn('--native-worker', log)

    def test_versioned_clang_cpp_preserves_preprocessor_mode(self):
        self.start_local()
        alias = self.work/'x86_64-pc-linux-gnu-clang-cpp-23.1'
        alias.symlink_to(self.compiler)
        self.compiler = str(alias)
        args = ['-P', str(self.source)]
        expected = self.command(args, False, check=True)
        result = self.command(args, check=True)
        self.assertIn(b'int main', result.stdout)
        self.assertEqual(result.stdout, expected.stdout)
        self.assertIn('operation=C', (self.work/'client.log').read_text())

    def test_unreachable_broker_runs_local_driver_but_fails_planning(self):
        self.start_local()
        self.env.update(DISTCC_BROKER=str(self.work/'missing.sock'), DISTCC_RECONNECT_TIMEOUT='0',
                        DISTCC_FALLBACK='0', DISTCC_FRONTEND_REMOTE='1')
        result = self.command(['-dumpversion'])
        self.assertEqual((result.returncode, result.stdout), (0, self.command(['-dumpversion'], False).stdout))
        self.command(['-c', str(self.source), '-o', 'in.o'], False, check=True)
        result = self.command(['in.o', '-o', 'linked'])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command(['-fprofile-instr-generate', '-c', str(self.source), '-o', 'profile.o'])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work/'profile.o').exists())

    def saturate_frontend(self):
        capacity = self.peer(stats_response(cpus=1, cores=1, jobs=1))
        self.start_local(self.host(0, f',tcp,stats={capacity.port}', slots=1))
        self.env.update(DISTCC_FRONTEND_REMOTE='1', DISTCC_FALLBACK='0')
        self.assertEqual(self.receive(self.request(15)), (0, 1, []))
        self.assertEqual(self.receive(self.request(15)), (0, 0, []))

    def test_missing_frontend_is_cached_across_local_and_prepared_requests(self):
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.saturate_frontend()
        self.use_unpackaged_clang()
        helper = self.work/'distcc-clang'
        helper.write_text('#!/bin/sh\necho "$1" >> startups\nexec ' +
                          shlex.quote(str(self.build/'distcc-clang')) + ' "$@"\n')
        helper.chmod(0o755)
        self.env['DISTCC_CLANG_DRIVER'] = str(helper)
        for i, extra in enumerate(([], [], ['-fprofile-instr-generate'])):
            obj = self.work/f'output-{i}.o'
            result = self.command(['-c', str(self.source), '-o', str(obj), *extra])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(obj.stat().st_size, 0)
        self.assertEqual((self.work/'startups').read_text().splitlines(), ['--native-worker'])
        self.assertNotIn('input preparation command=12 ', (self.work/'broker.log').read_text())

    def test_broken_frontend_fails_with_fallback_disabled(self):
        self.saturate_frontend()
        helper = self.work/'distcc-clang'
        helper.write_text('#!/bin/sh\nexit 1\n')
        helper.chmod(0o755)
        self.env['DISTCC_CLANG_DRIVER'] = str(helper)
        result = self.command(['-c', str(self.source), '-o', 'broken.o'])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work/'broken.o').exists())
        self.assertIn('fallbacks are disabled', (self.work/'client.log').read_text())

    def test_auxiliary_input_planning_without_a_broker_uses_the_compiler_major(self):
        self.start_local()
        del self.env['DISTCC_BROKER']
        for supported in (True, False):
            with self.subTest(supported=supported):
                if not supported:
                    self.use_unpackaged_clang()
                log = self.work/'client.log'
                log.unlink(missing_ok=True)
                obj = self.work/'profile.o'
                result = self.command(['-fprofile-instr-generate', '-c', str(self.source), '-o', str(obj)])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual('operation=I' in log.read_text(), supported)
                self.assertGreater(obj.stat().st_size, 0)
                obj.unlink()

    def test_missing_native_module_or_dependency_is_not_an_unsupported_major(self):
        self.saturate_frontend()
        version = self.command(['-dumpversion'], reused=False, check=True, text=True).stdout
        launcher = self.work/'distcc-clang'
        shutil.copy2(self.build/'distcc-clang', launcher)
        slot = self.work/('distcc-clang-' + version.split('.')[0])
        slot.write_text('invalid executable')
        slot.chmod(0o755)
        self.env['DISTCC_CLANG_DRIVER'] = str(launcher)
        result = self.command(['-c', str(self.source), '-o', 'broken.o'])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work/'broken.o').exists())

    def test_driver_config_without_remote_frontend_is_local(self):
        self.saturate_frontend()
        self.env['DISTCC_FRONTEND_REMOTE'] = '0'
        config = self.work/'settings.cfg'
        config.write_text('-DDRIVER_CONFIG=1\n')
        self.source.write_text('#ifndef DRIVER_CONFIG\n#error config missing\n#endif\nint value;\n')
        for args in (['--config', str(config)], [f'--config={config}']):
            result = self.command([*args, '-c', str(self.source), '-o', 'configured.o'])
            self.assertEqual(result.returncode, 0, result.stderr)
            (self.work/'configured.o').unlink()
        self.assertNotIn('operation=P', (self.work/'client.log').read_text())

    def test_link_without_resident_library_uses_driver(self):
        self.start_local()
        self.env.update(DISTCC_VERBOSE='0', DISTCC_FALLBACK='0')
        self.use_unpackaged_clang()
        (self.work/'source.c').write_text('int main(void) { return 0; }\n')
        original = self.command(['-c', 'source.c', '-o', 'source.o'], reused=False)
        self.assertEqual(original.returncode, 0, original.stderr)
        for extra in ([], ['-Wl,--distcc-invalid-link-option']):
            with self.subTest(extra=extra):
                args = ['-fuse-ld=lld', 'source.o', '-o', 'linked', *extra]
                original = self.command(args, reused=False)
                expected = (self.work/'linked').read_bytes() if not original.returncode else None
                (self.work/'linked').unlink(missing_ok=True)
                result = self.command(args)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (original.returncode, original.stdout, original.stderr))
                if expected is not None:
                    self.assertEqual((self.work/'linked').read_bytes(), expected)
                    subprocess.run([str(self.work/'linked')], check=True)
                else:
                    self.assertFalse((self.work/'linked').exists())

    def test_bfd_link_plan_preserves_output_and_diagnostics(self):
        self.start_local()
        (self.work/'source.c').write_text('int main(void) { return 0; }\n')
        self.assertEqual(self.command(['-c', 'source.c', '-o', 'source.o'], reused=False).returncode, 0)
        for flags in ([], ['-v'], ['-Wno-unused-command-line-argument']):
            with self.subTest(flags=flags):
                args = ['-fuse-ld=bfd', *flags, 'source.o', '-o', 'linked']
                original = self.command(args, reused=False)
                self.assertEqual(original.returncode, 0, original.stderr)
                output = (self.work/'linked').read_bytes()
                result = self.command(args)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (original.returncode, original.stdout, original.stderr))
                self.assertEqual((self.work/'linked').read_bytes(), output)
        log = (self.work/'client.log').read_text()
        self.assertIn('operation=L', log)
        self.assertNotRegex(log, r'forking to execute: (?:\S*/)?distcc-link\b')

    def test_link_plan_driver_error_is_not_success_or_local_retry(self):
        self.start_local()
        for args in (['absent.o', '-o', 'linked'],
                     ['absent.o', '-fnot-a-clang-option', '-o', 'linked']):
            with self.subTest(args=args):
                original = self.command(args, reused=False)
                result = self.command(args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((result.returncode, result.stderr),
                                 (original.returncode, original.stderr))
                self.assertFalse((self.work/'linked').exists())

    def test_link_plan_response_file_and_explicit_linker_path(self):
        self.start_local()
        linker = shutil.which('ld.bfd')
        if not linker:
            self.skipTest('BFD required')
        (self.work/'source.c').write_text('int main(void) { return 0; }\n')
        self.assertEqual(self.command(['-c', 'source.c', '-o', 'source.o'], reused=False).returncode, 0)
        (self.work/'link.rsp').write_text('-fuse-ld=' + linker + ' source.o -o linked\n')
        original = self.command(['@link.rsp'], reused=False)
        self.assertEqual(original.returncode, 0, original.stderr)
        output = (self.work/'linked').read_bytes()
        result = self.command(['@link.rsp'])
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (original.returncode, original.stdout, original.stderr))
        self.assertEqual((self.work/'linked').read_bytes(), output)

    def test_link_plan_config_diagnostics_are_not_duplicated(self):
        self.start_local()
        (self.work/'source.c').write_text('int main(void) { return 0; }\n')
        self.assertEqual(self.command(['-c', 'source.c', '-o', 'source.o'], reused=False).returncode, 0)
        for option in ('-ccc-print-bindings', '-ccc-print-phases'):
            with self.subTest(option=option):
                config = self.work/'driver.cfg'
                config.write_text(option+'\n')
                args = ['--config='+str(config), 'source.o', '-o', 'linked']
                original = self.command(args, reused=False)
                result = self.command(args)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (original.returncode, original.stdout, original.stderr))
                self.assertFalse((self.work/'linked').exists())

    def test_absolute_masquerade_matches_path_directory_identity(self):
        self.start_local()
        wrappers = self.work / 'wrappers'
        wrappers.mkdir()
        compiler = wrappers / Path(self.compiler).name
        compiler.symlink_to(self.build / 'distcc')
        alias = self.work / 'alias'
        alias.symlink_to(wrappers, target_is_directory=True)
        source = self.work / 'main.c'
        source.write_text('int main(void) { return 0; }\n')
        for directory in (str(wrappers) + '/', str(wrappers) + '/.', str(alias), '.'):
            with self.subTest(directory=directory):
                env = dict(self.env, PATH=directory + os.pathsep +
                           str(Path(self.compiler).parent) + os.pathsep + self.env['PATH'])
                obj, exe = self.work / 'main.o', self.work / 'main'
                for arguments in (['-c', str(source), '-o', str(obj)],
                                  [str(obj), '-o', str(exe)]):
                    result = subprocess.run([str(compiler), *arguments], cwd=wrappers,
                                            env=env, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(exe)], check=True, timeout=5)

    def test_ccache_compiler_query_completes_with_resident_worker(self):
        self.start_local()
        ccache = shutil.which('ccache')
        if not ccache:
            self.skipTest('ccache unavailable')
        wrappers = self.work/'wrappers'
        wrappers.mkdir()
        name = Path(self.compiler).name
        (wrappers/name).symlink_to(self.build/'distcc')
        # Portage disables ccache in src_test; this test exercises it explicitly.
        self.env.pop('CCACHE_DISABLE', None)
        self.env.update(CCACHE_DIR=str(self.work/'ccache'),
                        CCACHE_CONFIGPATH=str(self.work/'ccache.conf'),
                        CCACHE_COMPILERCHECK='%compiler% -dumpversion',
                        PATH=str(wrappers)+os.pathsep+str(Path(self.compiler).parent)+
                             os.pathsep+self.env['PATH'])
        (self.work/'source.c').write_text('int answer(void) { return 42; }\n')
        result = subprocess.run([ccache, name, '-c', 'source.c', '-o', 'source.o'],
                                env=self.env, cwd=self.work, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.work/'source.o').is_file())
        self.assertIn('reused local compiler libraries', (self.work/'client.log').read_text())

    def test_profile_preprocessing_preserves_output_and_ccache_identity(self):
        # The broker needs a local TCP worker to grant direct execution of
        # ccache's compile command, just as on an installed worker/client.
        self.start_local(self.host(0, f',tcp,stats={self.stats_ports[0]}'))
        profdata, ccache = shutil.which('llvm-profdata'), shutil.which('ccache')
        if not profdata or not ccache:
            self.skipTest('llvm-profdata and ccache required')
        (self.work/'training.c').write_text('int main(void) { return 0; }\n')
        training = self.command(['-fprofile-generate', 'training.c', '-o', 'training'], False)
        self.assertEqual(training.returncode, 0, training.stderr)
        subprocess.run([str(self.work/'training')], cwd=self.work, check=True,
                       env=dict(self.env, LLVM_PROFILE_FILE=str(self.work/'training.profraw')))
        subprocess.run([profdata, 'merge', 'training.profraw', '-o', 'training.profdata'],
                       cwd=self.work, check=True, capture_output=True)
        profile = '-fprofile-use='+str(self.work/'training.profdata')
        sources = {'first.c': 'int first(void) { return 1; }\n',
                   'second.c': 'int second(void) { return 2; }\n'}
        for name, content in sources.items():
            (self.work/name).write_text(content)
            for flags in ([profile, '-E'], ['-E', profile]):
                expected = self.command([*flags, '-o', 'result.i', name], False)
                self.assertEqual(expected.returncode, 0, expected.stderr)
                data = (self.work/'result.i').read_bytes()
                (self.work/'result.i').unlink()
                result = self.command([*flags, '-o', 'result.i', name])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((self.work/'result.i').is_file(), result.stderr)
                self.assertEqual((self.work/'result.i').read_bytes(), data)
                self.assertEqual((self.work/name).read_text(), content)
        wrappers = self.work/'wrappers'
        wrappers.mkdir()
        compiler = Path(self.compiler).name
        (wrappers/compiler).symlink_to(self.build/'distcc')
        self.env.pop('CCACHE_DISABLE', None)
        self.env.update(CCACHE_DIR=str(self.work/'ccache'),
                        CCACHE_CONFIGPATH=str(self.work/'ccache.conf'),
                        CCACHE_LOGFILE=str(self.work/'ccache.log'),
                        DISTCC_VERBOSE='0',
                        PATH=str(wrappers)+os.pathsep+str(Path(self.compiler).parent)+
                             os.pathsep+self.env['PATH'])
        for repetition in range(2):
            for name in sources:
                args = [profile, '-c', name, '-o', 'result.o', '-MMD', '-MF', 'result.d']
                expected = self.command(args, False)
                self.assertEqual(expected.returncode, 0, expected.stderr)
                artifacts = [(self.work/f).read_bytes() for f in ('result.o', 'result.d')]
                try:
                    result = subprocess.run([ccache, compiler, *args], cwd=self.work,
                                            env=self.env, capture_output=True, timeout=30)
                except subprocess.TimeoutExpired as error:
                    error.add_note((self.work/'client.log').read_text()[-2400:])
                    error.add_note((self.work/'ccache.log').read_text()[-1800:])
                    raise
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([(self.work/f).read_bytes() for f in ('result.o', 'result.d')],
                                 artifacts)
                self.assertEqual(result.stderr, expected.stderr)
        self.assertEqual((self.work/'ccache.log').read_text().count('Result: direct_cache_hit'), 2)

    def test_driver_alias_preserves_target_and_explicit_overrides(self):
        self.start_local()
        target = subprocess.check_output([self.compiler, '-dumpmachine'], text=True).strip()
        if not target.startswith(('x86_64', 'i686')):
            self.skipTest('x86 compiler required')
        compiler = self.compiler
        (self.work/'source.S').write_text('.text\n.globl probe\nprobe:\nret\n')
        cases = [('i686', [], 1), ('i686', ['--target=x86_64-pc-linux-gnu'], 2),
                 ('i686', ['-m64'], 2), ('x86_64', ['-m32'], 1)]
        for target, flags, elf_class in cases:
            with self.subTest(target=target, flags=flags):
                alias = self.work/(target + '-pc-linux-gnu-clang')
                if not alias.exists():
                    alias.symlink_to(compiler)
                self.compiler = str(alias)
                artifacts = []
                for reused in (False, True):
                    result = self.command(['-march=native', '-mtune=native', *flags,
                                           '-c', 'source.S', '-o', 'out'], reused)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    artifacts.append((self.work/'out').read_bytes())
                    (self.work/'out').unlink()
                self.assertEqual(artifacts[0][:5], b'\x7fELF' + bytes([elf_class]))
                self.assertEqual(artifacts[1], artifacts[0])
        self.assertIn('reused local compiler libraries', (self.work/'client.log').read_text())

    def test_driver_alias_selects_cxx_mode(self):
        self.start_local()
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('Clang C++ driver unavailable')
        self.compiler = compiler
        (self.work/'source.cc').write_text('#include <vector>\n'
            'int main() { std::vector<int> values{42}; return values.at(0) != 42; }\n')
        for reused in (False, True):
            result = self.command(['source.cc', '-o', 'out'], reused)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([self.work/'out'], check=True, capture_output=True)
            (self.work/'out').unlink()
        self.assertIn('reused local compiler libraries', (self.work/'client.log').read_text())

    def test_verbose_link_preserves_implicit_library_diagnostics(self):
        self.start_local()
        saved = dict(self.env)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='verbose-link')
        saved.update({key: self.env[key] for key in ('DISTCC_HOSTS', 'DISTCC_BROKER')})
        self.env = saved
        self.env.pop('DISTCC_VERBOSE', None)
        (self.work/'openmp.c').write_text('#include <omp.h>\n'
            'int main(void) { return omp_get_max_threads() < 1; }\n')
        compiled = self.command(['-fopenmp', '-c', 'openmp.c', '-o', 'openmp.o'])
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        expected_link_line = None
        for reused in (False, True):
            result = self.command(['-v', '-fopenmp', '-fuse-ld=lld', 'openmp.o', '-o', 'out'], reused)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'clang version', result.stderr)
            self.assertIn(b'-lomp', result.stderr)
            self.assertIn(b'ld.lld', result.stderr)
            link_line, = [line for line in result.stderr.splitlines()
                          if b'ld.lld' in line and b'openmp.o' in line]
            # Libtool filters for an unquoted -L and then splits this line on
            # whitespace to find implicit objects and runtime libraries.
            self.assertIn(b' -L', link_line)
            self.assertIn(b'-lc', link_line.split())
            if expected_link_line is None: expected_link_line = link_line
            self.assertEqual(link_line, expected_link_line)
            subprocess.run([self.work/'out'], check=True, capture_output=True)
        (self.work/'out').unlink()
        result = self.command(['-###', '-fopenmp', '-fuse-ld=lld', 'openmp.o', '-o', 'out'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'-lomp', result.stderr)
        self.assertFalse((self.work/'out').exists())

    def test_large_command_preserves_driver_arguments(self):
        self.start_local()
        source = self.work/'source with spaces.c'
        source.write_text('#ifndef LAST_ARGUMENT\n#error lost argument\n#endif\n'
                          'int main(void) { return LAST_ARGUMENT != 123; }\n')
        result = self.command(['-fno-inline']*4096 + ['-DLAST_ARGUMENT=123',
                              str(source), '-o', str(self.work/'program')])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('reused local compiler libraries', (self.work/'client.log').read_text())
        subprocess.run([str(self.work/'program')], check=True)

    def test_command_file_preserves_filesystem_bytes(self):
        self.start_local()
        source = self.work/os.fsdecode(b'source-\xff.c')
        source.write_text('int main(void) { return 0; }\n')
        result = self.command([str(source), '-o', str(self.work/'program')])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'reused local compiler libraries', (self.work/'client.log').read_bytes())
        subprocess.run([str(self.work/'program')], check=True)

    def test_real_driver_outputs_and_errors(self):
        self.start_local()
        (self.work/'source.c').write_text('#include <stdio.h>\nint main(void) { puts("local"); return 0; }\n')
        (self.work/'source.cc').write_text('#include <vector>\nint main() { std::vector<int> v{1,2}; return v[1]!=2; }\n')
        (self.work/'warning.c').write_text('#warning diagnostic must be finalized\nint value;\n')
        (self.work/'error.c').write_text('#error diagnostic must be finalized\n')
        (self.work/'options.rsp').write_text('-O2 -c source.c -o out -MMD -MF out.d')
        cases = [
            ['-O2', '-c', 'source.c', '-o', 'out', '-MMD', '-MF', 'out.d'],
            ['-O2', '-flto=thin', '-fuse-ld=lld', 'source.c', '-o', 'out'],
            ['-O2', '-fuse-ld=bfd', 'source.c', '-o', 'out'],
            ['--driver-mode=g++', 'source.cc', '-o', 'out'],
            ['-m32', 'source.c', '-o', 'out'],
            ['-E', '-dM', 'source.c'], ['@options.rsp'], ['--version'],
            ['-cc1','-help'],
            ['-c', 'source.c', '/wd4189', '-o', 'out'],
            ['-c', 'source.c', '-fno-such-distcc-option', '-o', 'out'],
            ['-c', 'source.c', '-Werror=unknown-warning-option',
             '-Wdistcc-unknown-warning', '-o', 'out'],
            ['-c', 'warning.c', '-Werror', '-o', 'out'],
            ['-c', 'warning.c', '-o', 'out', '-serialize-diagnostics', 'out.dia'],
            ['-c', 'error.c', '-o', 'out', '-serialize-diagnostics', 'out.dia'],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                outputs = []
                for reused in (False, True):
                    for name in ('out', 'out.d', 'out.dia'): (self.work/name).unlink(missing_ok=True)
                    result = self.command(arguments, reused)
                    artifacts = {name:(self.work/name).read_bytes() for name in ('out','out.d','out.dia')
                                 if (self.work/name).exists()}
                    outputs.append((result.returncode,result.stdout,artifacts))
                    if not result.returncode and '-c' not in arguments and arguments[0] != '@options.rsp' and 'out' in artifacts:
                        subprocess.run([str(self.work/'out')],check=True,capture_output=True)
                self.assertEqual(outputs[0],outputs[1],result.stderr.decode())
                if result.returncode:
                    self.assertIn(b'error:',result.stderr)
        log = (self.work/'client.log').read_text()
        # Compile-only commands can use distcc's separate prepared-input path;
        # driver queries, preprocessing and link commands take the local path.
        self.assertGreaterEqual(log.count('reused local compiler libraries'),6)
        self.assertEqual(log.count('started local compiler libraries'),1)

    def test_source_and_link_retains_object_and_archive_inputs(self):
        self.start_local()
        (self.work/'main.c').write_text(
            'extern int version(void), extra(void);\n'
            'int main(void) { return version()+extra()!=42; }\n')
        (self.work/'version.c').write_text('int version(void) { return 40; }\n')
        (self.work/'extra.c').write_text('int extra(void) { return 2; }\n')
        for lto in ([], ['-flto=thin']):
            for source in ('version', 'extra'):
                result = self.command(['-O2', *lto, '-c', source+'.c',
                                       '-o', source+'.o'], False)
                self.assertEqual(result.returncode, 0, result.stderr)
            archive = self.work/'extra.a'
            archive.unlink(missing_ok=True)
            subprocess.run(['llvm-ar', 'rcs', str(archive), 'extra.o'],
                           cwd=self.work, env=self.env, check=True)
            for extra, profile in [('extra.o', ['-fprofile-update=atomic',
                                                 '-fprofile-generate=profiles']),
                                   ('extra.a', [])]:
                with self.subTest(lto=lto, extra=extra):
                    arguments = ['-O2', *lto, *profile, '-fuse-ld=lld',
                                 'main.c', 'version.o', extra, '-o', 'program']
                    for reused in (False, True):
                        (self.work/'program').unlink(missing_ok=True)
                        result = self.command(arguments, reused)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        subprocess.run([str(self.work/'program')], cwd=self.work,
                                       check=True, capture_output=True)
            # An archive really is unused during compilation without a link.
            result = self.command(['-O2', *lto, '-c', 'main.c', str(archive),
                                   '-o', 'compiled.o'])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((self.work/'compiled.o').exists())

    def test_resident_linker_reports_success_and_failure(self):
        self.start_local()
        self.env['DISTCC_VERBOSE'] = '0'
        (self.work/'main.c').write_text('int main(void) { return 0; }\n')
        cases = [
            ['-Wl,--version'], ['-Wl,--help'],
            ['main.c', '-Wl,--no-such-distcc-option', '-o', 'program'],
            ['main.c', '-o', 'program'],
            ['-Wl,--version'],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                native = self.command(['-fuse-ld=lld', *arguments], False)
                result = self.command(['-fuse-ld=lld', *arguments])
                self.assertEqual(result.returncode, native.returncode, result.stderr)
                self.assertEqual(result.stdout, native.stdout)
                if '--no-such-distcc-option' in ' '.join(arguments):
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b'unknown argument', result.stderr)
                elif 'main.c' in arguments:
                    subprocess.run([str(self.work/'program')], check=True)

    def test_parallel_commands_and_cancellation(self):
        self.start_local()
        # Input blocks inside the actual cc1, not a stand-in compiler.
        process = subprocess.Popen([str(self.build/'distcc'),self.compiler,'-E','-x','c','-'],
                                   cwd=self.work,env=self.env,stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def stop():
            if process.poll() is None: process.kill()
            process.communicate(timeout=5)
        self.addCleanup(stop)
        deadline = time.monotonic()+10
        children = []
        helper = 0
        while not children:
            log = (self.work/'client.log').read_text() if (self.work/'client.log').exists() else ''
            match = re.search(r'started local compiler libraries pid=(\d+)',log)
            if match:
                helper = int(match[1])
                children = Path(f'/proc/{helper}/task/{helper}/children').read_text().split()
            self.assertLess(time.monotonic(),deadline,log)
            time.sleep(.01)
        def preprocess(index):
            result = self.command(['-E','-P','-x','c','-',f'-DVALUE={index}'], input=b'VALUE\n')
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),str(index).encode())
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(preprocess,range(12)))
        process.kill(); process.communicate(timeout=5)
        deadline = time.monotonic()+5
        while Path(f'/proc/{helper}/task/{helper}/children').read_text().strip():
            self.assertLess(time.monotonic(),deadline,'cancelled compiler remains alive')
            time.sleep(.01)
        # Cache replaces a dead helper, while the following real command works.
        os.kill(helper,signal.SIGKILL)
        deadline = time.monotonic()+5
        while Path(f'/proc/{helper}/stat').exists():
            if Path(f'/proc/{helper}/stat').read_text().rsplit(')',1)[1].split()[0] == 'Z': break
            self.assertLess(time.monotonic(),deadline)
            time.sleep(.01)
        result = self.command(['-E','-P','-x','c','-'],input=b'17\n')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.strip(),b'17')

    def test_driver_aliases_share_libraries_and_preserve_language(self):
        self.start_local()
        cxx = shutil.which('clang++')
        if not cxx or not os.path.samefile(self.compiler, cxx):
            self.skipTest('C and C++ driver aliases of one executable required')
        (self.work/'input.c').write_text('')
        for compiler, is_cxx in ((self.compiler, False), (cxx, True)):
            self.compiler = compiler
            arguments = ['-E', '-dM', 'input.c']
            native = self.command(arguments, False, input=b'')
            reused = self.command(arguments, input=b'')
            self.assertEqual(native.returncode, 0, native.stderr)
            self.assertEqual((native.returncode, native.stdout),
                             (reused.returncode, reused.stdout), reused.stderr)
            self.assertEqual(b'#define __cplusplus ' in reused.stdout, is_cxx)
        log = (self.work/'client.log').read_text()
        self.assertEqual(log.count('started local compiler libraries'), 1)
        self.assertEqual(log.count('reused local compiler libraries'), 2)

    def test_codegen_uses_spare_leases_and_preserves_result(self):
        self.check_codegen(False)

    def test_codegen_preserves_warning_option_errors(self):
        self.start_local()
        saved = dict(self.env)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='diagnostics')
        saved.update({key: self.env[key] for key in ('DISTCC_HOSTS', 'DISTCC_BROKER')})
        self.env = saved
        (self.work/'source.c').write_text('int value(void) { return 42; }\n')
        valid = self.command(['-c', 'source.c', '-o', 'source.o'])
        self.assertEqual(valid.returncode, 0, valid.stderr)
        self.assertIn(b'distcc codegen plan:', valid.stderr)
        for flags in (['-Werror=unknown-warning-option', '-Wdistcc-unknown-warning'],
                      ['-Werror', '-Wdistcc-unknown-warning']):
            with self.subTest(flags=flags):
                results = []
                for reused in (False, True):
                    (self.work/'source.o').unlink(missing_ok=True)
                    result = self.command(['-c', 'source.c', *flags, '-o', 'source.o'], reused)
                    results.append((result.returncode, (self.work/'source.o').exists()))
                    self.assertIn(b'error: unknown warning option', result.stderr)
                self.assertEqual(results[0], (1, False))
                self.assertEqual(results[1], results[0])

    def test_codegen_shared_library_preserves_local_calls(self):
        self.check_codegen(False, shared_flags=['-fPIC', '-fno-semantic-interposition'])

    def test_codegen_shared_library_preserves_interposition(self):
        self.check_codegen(False, shared_flags=['-fPIC', '-fsemantic-interposition'])

    @unittest.skipUnless(os.uname().machine in ('x86_64', 'i686'), 'x86 target required')
    def test_codegen_32bit_shared_library_preserves_local_calls(self):
        self.check_codegen(False, shared_flags=['-m32', '-fPIC', '-fno-semantic-interposition'])

    def test_codegen_selects_the_fastest_predicted_partition_count(self):
        self.start_local()
        header = self.build/'src/clang/clang_codegen.h'
        (self.work/'plan.cc').write_text(f'#include "{header}"\n' + '''
int main() {
    // Both 4 and 5 parts cost .45s; prefer the smaller resource footprint.
    // Sixteen parts still beat serial (0.8625s), but are not the best choice.
    if (dcc_codegen_parallelism(1., .05, 16, true) != 4) return 1;
    if (dcc_codegen_parallelism(1., .05, 3, true) != 3) return 2;
    if (dcc_codegen_parallelism(1., .25, 16, true) != 1) return 3;
    if (dcc_codegen_parallelism(0., 0., 16, false) != 1) return 4;
    if (dcc_codegen_parallelism(1., 0., 16, false) != 2) return 5;
    if (dcc_codegen_parallelism(1., 0., 1, false) != 1) return 6;
    if (dcc_codegen_parallelism(1., 0., 16, true) != 16) return 7;
    return 0;
}
''')
        result = self.command(['--driver-mode=g++', '-std=c++17', 'plan.cc', '-o', 'plan'])
        self.assertEqual(result.returncode, 0, result.stderr)
        checked = subprocess.run([str(self.work/'plan')], cwd=self.work)
        self.assertEqual(checked.returncode, 0)

    def test_native_cpu_feedback_is_per_job(self):
        self.start_local()
        self.base_env['DISTCC_VERBOSE'] = '1'
        saved = dict(self.env)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='usage')
        saved.update({key: self.env[key] for key in ('DISTCC_HOSTS', 'DISTCC_BROKER')})
        self.env = saved
        # Compile different amounts of real work in the same resident helper.
        # Its accumulated CPU counter must not be charged to the second job.
        (self.work/'large.c').write_text('\n'.join(
            f'unsigned f{i}(unsigned x) {{ for (unsigned j=0;j<100;j++) x=x*33+j+{i}; return x; }}'
            for i in range(800)))
        (self.work/'small.c').write_text('int answer(void) { return 42; }\n')
        for name in ('large', 'small'):
            result = self.command(['-O2', '-c', name+'.c', '-o', name+'.o'])
            self.assertEqual(result.returncode, 0, result.stderr)
        deadline = time.monotonic()+3
        while True:
            cpu = [float(value) for value in re.findall(r'job cost: .* cpu=([0-9.]+)',
                                                       (self.work/'usage.log').read_text())]
            if len(cpu) == 2: break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        self.assertGreater(cpu[1], 0)
        self.assertLess(cpu[1], cpu[0])
        self.assertEqual((self.work/'client.log').read_text().count('started local compiler libraries'), 1)

    def test_codegen_finishes_when_no_extra_cpu_is_available(self):
        self.check_codegen(True)

    def test_codegen_without_diagnostics_preserves_result(self):
        self.check_codegen(True, verbose=False)

    def test_disabled_verbose_preserves_compiler_diagnostics(self):
        self.start_local()
        (self.work/'conftest.c').write_text(
            'int value;\nint read_value(void) { return value; }\n')
        (self.work/'warning.c').write_text('#warning keep this diagnostic\n')
        (self.work/'error.c').write_text('#error keep this error\n')
        # Libtool rejects PIC and -c -o support if successful probes emit
        # unexpected stderr. Exercise both prepared input and resident driver
        # jobs, including environment changes on the same compiler worker.
        for verbose in ('1', '0', '', None):
            if verbose is None:
                self.env.pop('DISTCC_VERBOSE', None)
            else:
                self.env['DISTCC_VERBOSE'] = verbose
            for mode in ([], ['-m32']):
                for action in (['-c', '-o', 'conftest.o'],
                               ['-shared', '-fuse-ld=lld', '-o', 'conftest.so']):
                    with self.subTest(verbose=verbose, mode=mode, action=action):
                        arguments = [*mode, '-O2', '-fPIC', '-DPIC', 'conftest.c', *action]
                        native = self.command(arguments, False)
                        if mode and native.returncode:
                            self.skipTest('native compiler lacks 32-bit support: ' + native.stderr.decode())
                        self.assertEqual(native.returncode, 0, native.stderr)
                        self.assertEqual(native.stderr, b'')
                        (self.work/action[-1]).unlink()
                        result = self.command(arguments)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertGreater((self.work/action[-1]).stat().st_size, 0)
                        if verbose == '1' and action[0] == '-c':
                            self.assertIn(b'distcc compiler phases:', result.stderr)
                            self.assertIn(b'distcc codegen plan:', result.stderr)
                        elif verbose != '1':
                            self.assertEqual(result.stderr, native.stderr)
            for source, diagnostic, success in (('warning.c', b'warning:', True),
                                                 ('error.c', b'error:', False)):
                with self.subTest(verbose=verbose, source=source):
                    result = self.command(['-c', source, '-o', 'diagnostic.o'])
                    self.assertEqual(result.returncode == 0, success, result.stderr)
                    self.assertIn(diagnostic, result.stderr)

    def test_codegen_cpp_preserves_exceptions_and_static_initialization(self):
        self.check_codegen(False, True)

    def test_codegen_with_alternate_installed_llvm(self):
        compiler = shutil.which('clang-22')
        if not compiler: self.skipTest('alternate LLVM release unavailable')
        self.check_codegen(False, compiler=compiler)

    def test_codegen_does_not_publish_failed_assembly(self):
        self.start_local()
        (self.work/'broken.c').write_text('void f(void) { __asm__("not_a_distcc_instruction"); }\n')
        for reused in (False, True):
            result = self.command(['-c', 'broken.c', '-o', 'broken.o'], reused)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'error:', result.stderr)
            self.assertFalse((self.work/'broken.o').exists())

    def test_codegen_partition_failure_releases_leases(self):
        self.check_codegen(False)
        source = self.work/'codegen.c'
        valid = source.read_text()
        # Keep the same substantial partitionable module as the successful
        # invocation, but make every backend fail in its assembler.
        source.write_text(valid.replace('for (unsigned j=0;',
                          '__asm__("not_a_distcc_instruction"); ' * 512 + 'for (unsigned j=0;'))
        (self.work/'codegen.o').unlink()
        failed = self.command(['-O3', '-c', 'codegen.c', '-o', 'codegen.o'])
        self.assertNotEqual(failed.returncode, 0, failed.stderr)
        self.assertIn(b'cannot emit LLVM partition', failed.stderr)
        self.assertFalse((self.work/'codegen.o').exists())
        self.assertFalse(list(self.work.glob('distcc-codegen*')))
        source.write_text(valid)
        result = self.command(['-O3', '-c', 'codegen.c', '-o', 'codegen.o'])
        self.assertEqual(result.returncode, 0, result.stderr)
        # Profitability learning may now prefer serial execution. Check the
        # actual lease pool, independently of that advisory choice.
        leases = [self.request(15) for _ in range(4)]
        for lease in leases: self.assertEqual(self.receive(lease), (0, 1, []))

    def check_codegen(self, occupied, cpp=False, compiler=None, verbose=True, shared_flags=None):
        self.start_local()
        if compiler: self.compiler = compiler
        if cpp:
            cxx = shutil.which('clang++')
            if not cxx: self.skipTest('C++ driver required')
            self.compiler = cxx
        saved = dict(self.env)
        # These tests exercise compiler isolation and spare-lease ownership,
        # not host-load estimation. A desktop's unrelated CPU load must not
        # remove the lanes required to reach the parallel backend under test.
        capacity = self.peer(stats_response(cpus=4, jobs=4, cores=4))
        self.start_broker(self.host(0, f',tcp,stats={capacity.port}'), name='codegen')
        saved.update({key: self.env[key] for key in ('DISTCC_HOSTS', 'DISTCC_BROKER')})
        self.env = dict(saved, DISTCC_CLANG_CODEGEN='1')
        if not verbose:
            self.env.pop('DISTCC_VERBOSE', None)
        if occupied:
            for _ in range(3):
                self.assertEqual(self.receive(self.request(15)), (0, 1, []))
        source = self.work/'codegen.c'
        source.write_text('#include <stdio.h>\nstatic unsigned value = 7;\n'
                          '_Thread_local unsigned thread_value = 3;\n' +
                          '\n'.join(f'__attribute__((noinline)) unsigned f{i}(unsigned n) {{ '
                                    f'for (unsigned j=0;j<100;j++) n=(n*33+{i}+value)^thread_value; return n; }}'
                                    for i in range(8)) +
                          '\nint main(void) { for (unsigned i=0;i<20;i++) printf("%u\\n", ' +
                          '+'.join(f'f{i}(i)' for i in range(8)) + '); return 0; }\n')
        if cpp:
            source.write_text('#include <vector>\nstatic std::vector<unsigned> values{4, 8, 12};\n' +
                              source.read_text().replace('_Thread_local', 'thread_local')
                              .replace('return n; }', 'try { if (n&1) throw n; return n+values[0]; } '
                                       'catch (unsigned v) { return v+values[1]; } }'))
        flags = shared_flags or []
        if shared_flags:
            source.write_text(source.read_text().replace('int main(void)', 'int entry(void)'))
            (self.work/'main.c').write_text('unsigned f1(unsigned n) { return n + 9001; }\n'
                                          'int entry(void); int main(void) { return entry(); }\n')
        # The first serial job supplies measured codegen cost to the shared
        # compiler parent. No guessed throughput or fixed size cutoff is used.
        warm = self.command([*flags, '-O3', '-c', 'codegen.c', '-o', 'codegen.o'])
        self.assertEqual(warm.returncode, 0, warm.stderr)
        expected = None
        for reused in (False, True):
            result = self.command([*flags, '-O3', '-g', '-ffunction-sections', '-fdata-sections',
                                   '-MMD', '-MF', 'codegen.d', '-c', 'codegen.c', '-o', 'codegen.o'], reused)
            self.assertEqual(result.returncode, 0, result.stderr)
            link_input = ['codegen.o']
            if shared_flags:
                linked = subprocess.run([self.compiler, *flags, '-fuse-ld=lld', '-shared',
                                         '-Wl,-z,text', 'codegen.o', '-o', 'libcodegen.so'],
                                        cwd=self.work, capture_output=True)
                self.assertEqual(linked.returncode, 0, linked.stderr)
                link_input = ['main.c', '-L.', '-lcodegen', '-Wl,-rpath,$ORIGIN']
            subprocess.run([self.compiler, *flags, *link_input, '-o', 'codegen'], cwd=self.work,
                           capture_output=True, check=True)
            actual = (subprocess.check_output([str(self.work/'codegen')]),
                      (self.work/'codegen.d').read_bytes())
            if shared_flags:
                symbols = subprocess.check_output(['nm', '-D', '--defined-only', '--format=posix',
                                                   'libcodegen.so'], cwd=self.work, text=True)
                actual += (sorted(tuple(line.split()[:2]) for line in symbols.splitlines()),)
            if expected is None: expected = actual
            else: self.assertEqual(actual, expected)
        self.assertEqual(b'distcc codegen:' in result.stderr, not occupied,
                         result.stderr.decode() + (self.work/'client.log').read_text())
        if not occupied:
            self.assertIn(b'distcc codegen: 2 parts', result.stderr)
        traces = [json.loads(line.removeprefix(b'distcc compiler phases: '))
                  for line in result.stderr.splitlines()
                  if line.startswith(b'distcc compiler phases: ')]
        if not verbose:
            self.assertFalse(traces)
            return
        self.assertEqual(len(traces), 1, result.stderr.decode())
        trace = traces[0]
        self.assertTrue(trace['source'].endswith('codegen.c'))
        self.assertGreater(trace['unfinalized_instructions'], 0)
        self.assertGreater(trace['optimized_instructions'], 0)
        self.assertEqual([phase['name'] for phase in trace['phases']],
                         ['frontend', 'finalize_optimize', 'ir_cleanup', 'codegen'])
        self.assertEqual(sum(phase['wall_ns'] for phase in trace['phases']),
                         trace['end_ns'] - trace['start_ns'])
        for phase in trace['phases']:
            self.assertGreater(phase['wall_ns'], 0)
            self.assertGreater(phase['cpu_ns'], 0)

    def test_available_frontend_releases(self):
        self.start_local()
        (self.work/'source.c').write_text('int value;\n')
        (self.work/'source.S').write_text('.text\n')
        for directory in self.build.glob('distcc-clang-[0-9]*'):
            compiler = shutil.which(directory.name.removeprefix('distcc-'))
            if not compiler: continue
            self.compiler = compiler
            for args in (['-E','-dM','source.c'], ['-c','source.S','-o','out'],
                         ['-c','source.c','-fno-such-distcc-option']):
                with self.subTest(compiler=compiler,args=args):
                    native = self.command(args,False)
                    reused = self.command(args)
                    self.assertEqual((native.returncode,native.stdout),(reused.returncode,reused.stdout))


if __name__ == '__main__':
    unittest.main()
