#!/usr/bin/env python3
"""Real LLD planning, indexed imports, distributed backends and final links."""
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from test_broker import BrokerTestCase
from test_adaptive import stats_response

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distcc_job import JOB_THINLTO, fingerprint, write_metadata


@unittest.skipUnless(all(shutil.which(tool) for tool in ('clang', 'bwrap', 'sandbox')),
                     'Clang, bubblewrap and Gentoo sandbox required')
class PortageSandboxTest(BrokerTestCase):
    def test_local_job_creates_namespace_inside_portage_sandbox(self):
        self.compile_local_job()

    @unittest.skipUnless(os.geteuid() == 0, 'root install phase required')
    def test_root_install_job_under_private_build_user_directory(self):
        import pwd

        build_uid = pwd.getpwnam('portage').pw_uid
        self.assertNotEqual(build_uid, 0)
        self.compile_local_job(build_uid)

    def compile_local_job(self, build_uid=None):
        import tempfile

        with tempfile.TemporaryDirectory(prefix='distcc-sandbox-') as directory:
            work = Path(directory).resolve()
            if build_uid is not None:
                os.chown(work, build_uid, -1)
                work.chmod(0o700)
            self.start_broker(' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2)))
            compiler = shutil.which('clang')
            assert compiler is not None
            source = work / 'library.bc'
            definitions = ''.join(f'__attribute__((noinline)) static int part{i}(int x) '
                                  f'{{ volatile int n=x; return n*{i+1}; }}' for i in range(32))
            body = definitions + 'int private_definition(int x) {return ' + '+'.join(
                f'part{i}(x)' for i in range(32)) + ';}'
            subprocess.run([compiler, '-O2', '-flto=thin', '-x', 'c', '-c', '-', '-o', source],
                           input=body.encode(), check=True)
            subprocess.run([Path(compiler).with_name('ld.lld'), '-shared',
                            '--thinlto-index-only', source, '-o', work/'library.so'], check=True)
            index = str(source) + '.thinlto.bc'
            aliases = [work/f'alias-{i}.bc' for i in range(1100)]
            for alias in aliases:
                os.link(source, alias)
            output = work/'result.o'
            spec = work/'jobs.json'
            spec.write_text(json.dumps(dict(common=dict(args=[compiler, '-c'], inputs=list(map(str, aliases))),
                jobs=[dict(args=['-x', 'ir', str(source), '-fthinlto-index='+index, '-o', str(output)],
                           inputs=[str(source), index], outputs=[str(output)])])))
            trace = work / 'exec.log'
            sandbox_log = work / 'sandbox.log'
            env = dict(self.env, SANDBOX_WRITE=str(self.root)+':'+str(work), SANDBOX_LOG=str(sandbox_log),
                       DISTCC_VERBOSE='1', DISTCC_CLANG_LOCAL='0', TMPDIR=str(work))
            command = ['sandbox', 'sh', '-c', 'ulimit -Sn 1024 && test "$SANDBOX_ON" = 1 && exec "$@"',
                       'sh', self.build/'distcc-thinlto', spec]
            if shutil.which('strace'):
                command = ['strace', '-f', '-s', '256', '-e', 'trace=execve,capset,prctl,seccomp,openat',
                           '-o', trace, *command]
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(result.stderr, r'partitioned .* into (?:[2-9]|\d{2,})')
            self.assertIn('status=0 operation=M', result.stderr)
            symbols = subprocess.check_output(['llvm-nm', output], text=True)
            self.assertRegex(symbols, r' t part\d+')
            self.assertFalse(sandbox_log.exists(), result.stderr)
            if trace.exists():
                events = trace.read_text()
                # The module and its index each get one snapshot, including
                # the 1100 hard-link aliases and all generated partitions.
                copies = re.findall(r'openat\([^ \n]+, "[^"]*/distcc-lto-[^/"]+/input-\d+", O_WRONLY', events)
                self.assertEqual(len(copies), 2)
                self.assertNotRegex(events, r'execve\("[^"]*/(bwrap|ld\.lld|llvm-objcopy)"')
                self.assertRegex(events, r'prctl\(PR_SET_NO_NEW_PRIVS, 1, .*\)\s+= 0')
                self.assertRegex(events, r'capset\(.*\)\s+= 0')
                self.assertRegex(events, r'prctl\(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, .*\)\s+= 0')


@unittest.skipUnless(shutil.which('clang') and shutil.which('bwrap'), 'Clang and bubblewrap required')
class ThinLTOTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon_env = {'DISTCCD_PATH': str(Path.cwd()) + os.pathsep + os.environ['PATH']}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        hosts = ' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2))
        self.start_broker(hosts)
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        self.env.pop('DISTCC_LINK_SYSROOT', None)

    def partition_probe(self, compiler):
        return self.clang_slot(compiler).with_name('h_lto_split')

    def remote_link_worker(self, name='remote-link'):
        # A local worker can grant a small link directly to the caller.
        # These cases require actual remote backend submission.
        address = '127.0.0.2'
        self.restart_server(self.build/'distccd', address=address)
        self.addCleanup(self.restart_server, self.build/'distccd')
        self.start_broker(f'{address}:{self.ports[0]}/4,tcp,stats={self.stats_ports[0]}', name=name)
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])

    def run_command(self, command, **kwargs):
        try:
            return subprocess.run(list(map(str, command)), cwd=self.work, env=self.env,
                                  text=True, capture_output=True, timeout=60, **kwargs)
        except subprocess.TimeoutExpired as error:
            broker = self.work / 'broker.log'
            details = broker.read_text()[-8192:] if broker.exists() else ''
            self.fail(f'command timed out: {command}\n{error.stderr!r}\n{details}')

    @unittest.skipUnless(shutil.which('strace'), 'strace required')
    def test_link_and_index_plans_share_the_serial_worker(self):
        trace = self.trace_local_worker()
        self.env['DISTCC_CLANG_LOCAL'] = '1'
        compiler = shutil.which('clang')
        assert compiler is not None
        # Each L request produces a driver diagnostic and ends before backend execution.
        # The next request must still see its own diagnostics, cwd and result stream.
        src = self.work/'input.o'
        subprocess.run([compiler, '-flto=thin', '-x', 'c', '-c', '-', '-o', src],
                       input=b'int value(void) {return 42;}', check=True)
        subprocess.run([Path(compiler).with_name('ld.lld'), '-shared', '--thinlto-index-only', src,
                        '-o', self.work/'linked.so'], check=True)
        index = str(src) + '.thinlto.bc'
        out = str(self.work/'result.o')
        desc = self.work/'jobs.json'
        desc.write_text(json.dumps(dict(common=dict(args=[compiler, '-c'], inputs=[]),
            jobs=[dict(args=[str(src), '-fthinlto-index='+index, '-o', out],
                       inputs=[str(src), index], outputs=[out])])))
        for i in range(2):
            obj = f'missing-{i}.o'
            linked = self.run_command([self.build/'distcc', compiler, obj, '-o', 'out'])
            self.assertNotEqual(linked.returncode, 0)
            self.assertIn(obj, linked.stderr)
            self.assertEqual(linked.stdout, '')
            plan = self.run_command([self.build/'distcc-thinlto', desc])
            self.assertEqual(plan.returncode, 0, plan.stderr)
        events = trace.read_text()
        self.assertNotIn('/distcc-lto"', events)
        resident = re.search(r'execve\("[^"]*/distcc-clang-\d+",[^\n]* = 0', events)
        self.assertIsNotNone(resident, events)
        assert resident is not None
        plans = events[resident.end():]
        for op in 'LN':
            self.assertEqual(plans.count(f'iov_base="{op}\\0'), 2, plans)
        self.assertNotRegex(plans, r'\b(?:clone3?|vfork|fork)\(')

    def test_missing_planner_keeps_whole_module_backends(self):
        # The launcher reports U for an absent sibling. Remote backends still
        # use the real compiler installed on the daemon.
        launcher = self.work/'distcc-clang'
        shutil.copy2(self.build/'distcc-clang', launcher)
        self.env.update(DISTCC_CLANG_LOCAL='0', DISTCC_CLANG_DRIVER=str(launcher))
        result = self.link('clang')
        self.assertNotIn('distcc-thinlto: partitioned ', result.stderr)

    def start_local_lto(self):
        prefix, cache = str(self.root/'local-lto'), self.root/'input-cache'
        self.restart_server(self.build/'distccd', DISTCC_LOCAL_SOCKET=prefix, DISTCCD_FILE_CACHE=str(cache))
        self.addCleanup(self.restart_server, self.build/'distccd')
        with patch.dict(self.base_env, DISTCC_LOCAL_SOCKET=prefix, DISTCC_VERBOSE='1'):
            self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='local-lto')
        return cache

    def test_local_thinlto_input_descriptor_budget(self):
        self.start_local_lto()
        self.check_backend_cwd(False, extra=80)
        sizes = sum((self.work/'inputs'/str(i)).stat().st_size for i in range(64))
        stats = self.get_stats(self.stats_ports[0])
        self.assertIn('dcc_local_input_jobs 1\n', stats)
        self.assertIn(f'dcc_local_input_bytes {sizes}\n', stats)

    def test_local_partitions_and_different_driver_arguments(self):
        compiler = shutil.which('clang')
        assert compiler is not None
        self.clang_slot(compiler)
        cache = self.start_local_lto()
        jobs, shared = [], set()
        for name, body in [('large', '\n'.join(f'int f{i}(int x) {{ volatile int n=x; return n*{i}+x; }}'
                                               for i in range(160))), ('small', 'int small(void) {return 42;}')]:
            src, out = self.work/(name+'.bc'), self.work/(name+'.o')
            self.run_command([compiler, '-O2', '-flto=thin', '-x', 'c', '-c', '-', '-o', src], input=body, check=True)
            self.run_command([Path(compiler).with_name('ld.lld'), '-shared', '--thinlto-index-only', src,
                              '-o', self.work/(name+'.so')], check=True)
            index = Path(str(src)+'.thinlto.bc')
            args = [str(src), '-fthinlto-index='+str(index), '-o', str(out)]
            if name == 'small':
                args += ['-ffunction-sections']
                shared.add(fingerprint(src)[1].hex())
            shared.add(fingerprint(index)[1].hex())
            jobs.append(dict(args=args, inputs=[str(src), str(index)], outputs=[str(out)]))
        spec = self.work/'jobs.json'
        spec.write_text(json.dumps(dict(common=dict(args=[compiler, '-c'], inputs=[]), jobs=jobs)))
        trace = self.work/'transfer.trace'
        tracer = ['strace', '-f', '-yy', '-e', 'recvmsg', '-o', trace] if shutil.which('strace') else []
        result = self.run_command([*tracer, self.build/'distcc-thinlto', spec])
        self.assertEqual(result.returncode, 0, result.stderr)
        children = re.findall(r'submitting .*\.part-\d+\.bc .* command=cc1', result.stderr)
        self.assertGreater(len(children), 1, result.stderr)
        self.assertRegex(result.stderr, r'submitting .*small\.bc .* command=driver')
        sections = self.run_command(['readelf', '-SW', self.work/'small.o'], check=True)
        self.assertIn('.text.small', sections.stdout)
        self.assertEqual((self.work/'large.o').stat().st_mode & 0o777, 0o600)
        self.assertEqual({p.name for p in cache.glob('*/*') if len(p.name) == 16}, shared)
        self.assertIn(f'dcc_local_input_jobs {len(children)+1}\n', self.get_stats(self.stats_ports[0]))
        if tracer:
            self.assertRegex(trace.read_text(), r'SCM_RIGHTS[^\n]+/__distcc_lto/results/0')

    def link(self, compiler, archive=False, hip=False):
        objects = []
        for i in range(8):
            source = self.work / f'part {i}.c'
            source.write_text(f'int part{i}(int n) {{ int v = 0; '
                              f'for (int j=0;j<n;j++) v += (j * {i+1}) % 101; return v; }}')
            obj = source.with_suffix('.o')
            result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        source = self.work / 'main.c'
        source.write_text('\n'.join(f'int part{i}(int);' for i in range(8)) +
                          '\n#include <stdio.h>\nint main(int argc, char **argv) { '
                          'printf("%d\\n", ' + '+'.join(f'part{i}(argc*23)' for i in range(8)) +
                          '); return 0; }')
        main = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', main])
        self.assertEqual(result.returncode, 0, result.stderr)
        if archive:
            library = self.work / 'library.a'
            result = self.run_command(['llvm-ar', 'rcs', library, *objects])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects = [library]
        args = [compiler, '-flto=thin', '-O2', '-fuse-ld=lld', main, *objects]
        if hip:
            args.extend(['--hip-link', '--offload-arch=gfx900'])
        reference = self.work / 'reference'
        result = self.run_command([*args, '-o', reference])
        self.assertEqual(result.returncode, 0, result.stderr)
        before = [self.get_stats(port) for port in self.stats_ports]
        output = self.work / 'distributed'
        result = self.run_command([self.build / 'distcc', *args, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        partitions = list(map(int, re.findall(r'partitioned .* into (\d+)', result.stderr)))
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'),
                         9 + sum(count - 1 for count in partitions), result.stderr)
        for port, stats in zip(self.stats_ports, before):
            def completed(text):
                return int(next(line.split()[1] for line in text.splitlines()
                                if line.startswith('dcc_compile_ok ')))
            self.assertGreater(completed(self.get_stats(port)), completed(stats))
        for arguments in ([], ['argument'], ['one', 'two']):
            expected = self.run_command([reference, *arguments])
            actual = self.run_command([output, *arguments])
            self.assertEqual((actual.returncode, actual.stdout), (expected.returncode, expected.stdout))

        return result

    def test_objects_and_imports(self):
        self.link(shutil.which('clang'))

    def test_prepared_link_admission_and_archive_inputs(self):
        compiler = shutil.which('clang')
        peer = self.peer(stats_response(cpus=4, cores=4, jobs=4, busy=0, running=0))
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'), name='tiny')
        self.env.update(DISTCC_CLANG_LOCAL='1', DISTCC_VERBOSE='1',
                        DISTCC_LOG=str(self.work/'client.log'), PATH=str(self.build)+':'+os.environ['PATH'])
        def obj(name, code, thin=True):
            path = self.work/(name+'.c')
            path.write_text(code)
            self.run_command([compiler, '-O2', '-fPIC', *(['-flto=thin'] if thin else []),
                              '-c', path, '-o', name+'.o'], check=True)
        obj('native', 'int native(void){return 0;}', False)
        for i in range(3): obj('part'+str(i), f'int part{i}(void){{return {i};}}')
        obj('large', 'const char blob[]="'+('abcdefgh'*20000)+'";')
        self.assertGreater((self.work/'large.o').stat().st_size, 65536)
        for name, member, mode in [('elf', 'native', 'rcs'), ('bits', 'part0', 'rcs'),
                                   ('thinbits', 'part0', 'rcsT'), ('thinelf', 'native', 'rcsT')]:
            self.run_command(['llvm-ar', mode, f'lib{name}.a', member+'.o'], check=True)
        def link(inputs, distributed):
            result = self.run_command([self.build/'distcc', compiler, '-shared', '-nostdlib',
                                       '-fuse-ld=lld', '-flto=thin', *inputs, '-o', 'libout.so'])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual('distcc-thinlto: submitting' in result.stderr, distributed, result.stderr)
            self.assertNotIn('distcc-thinlto: partitioned', result.stderr)
            self.assertTrue((self.work/'libout.so').read_bytes().startswith(b'\x7fELF'))
        # Check idle admission before backend completions publish real host load.
        for inputs, distributed in [
                (['native.o'], False), (['part0.o'], False), (['part0.o','part1.o'], False),
                (['part1.o','-L.', '-Wl,-Bstatic','-lelf','-l:libthinelf.a'], False),
                (['part0.o','part1.o','part2.o'], True), (['large.o'], True),
                (['-Wl,--whole-archive', 'libbits.a'], True),
                (['-Wl,--whole-archive', '-L.', '-lbits'], True),
                (['-Wl,--whole-archive', '-L.', '-l:libthinbits.a'], True)]:
            with self.subTest(inputs=inputs): link(inputs, distributed)
        log = (self.work/'client.log').read_text()
        self.assertIn('operation=B', log)
        self.assertNotRegex(log, r'forking to execute: \S*distcc-link\b')
        self.assertNotIn('--show-hosts', log)
        self.assertNotIn('--local-command', log)
        # Two definitions exercise LLD's own error, with local retries enabled.
        self.env['DISTCC_FALLBACK'] = '1'
        failed = self.run_command([self.build/'distcc', compiler, '-shared', '-nostdlib', '-fuse-ld=lld',
                                   'native.o', 'native.o', '-o', 'bad.so'])
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(failed.stderr.count('duplicate symbol:'), 1, failed.stderr)

    def test_remote_only_tiny_module_runs_one_unsplit_backend(self):
        self.restart_server(self.build/'distccd', address='127.0.0.2')
        self.addCleanup(self.restart_server, self.build/'distccd')
        self.start_broker(f'127.0.0.2:{self.ports[0]}/4,tcp,stats={self.stats_ports[0]}', name='remote')
        self.env.update(DISTCC_CLANG_LOCAL='1', DISTCC_VERBOSE='1', PATH=str(self.build)+':'+os.environ['PATH'])
        source = self.work/'tiny.c'
        source.write_text('int main(void){return 0;}')
        self.run_command(['clang','-O2','-flto=thin','-c',source,'-o','tiny.o'], check=True)
        command = [self.build/'distcc','clang','-flto=thin','-fuse-ld=lld','tiny.o','-o','tiny']
        result = self.run_command(command, check=True)
        self.run_command(['./tiny'], check=True)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 1, result.stderr)
        self.assertNotIn('distcc-thinlto: partitioned', result.stderr)

        # The same backend must also distribute when the pool includes a
        # fast local peer whose only lane is already leased.
        local = self.peer(stats_response(cpus=1, cores=1, jobs=1, busy=0, running=0))
        remote = self.env['DISTCC_HOSTS']
        self.start_broker(self.host(1, f',tcp,stats={local.port},speed=100', slots=1)+' '+remote,
                          name='occupied')
        self.env.update(DISTCC_CLANG_LOCAL='1', DISTCC_VERBOSE='1', PATH=str(self.build)+':'+os.environ['PATH'])
        self.assertEqual(self.receive(self.request(15)), (0, 1, []))
        result = self.run_command(command, check=True)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 1, result.stderr)

    def test_fat_lto_executable_inputs_follow_linker_mode(self):
        peer = self.peer(stats_response(cpus=4, cores=4, jobs=4, busy=0, running=0))
        self.start_broker(self.host(0, f',tcp,stats={peer.port}'))
        log = self.work/'client.log'
        self.env.update(DISTCC_CLANG_LOCAL='1', DISTCC_LOG=str(log), DISTCC_VERBOSE='1',
                        PATH=str(self.build)+':'+os.environ['PATH'])
        for name, source in [('main', 'int main(void){return 0;}'), ('one', 'int one;'),
                             ('two', 'int two;'), ('large', 'const char data[]="'+'abcdefgh'*20000+'";')]:
            (self.work/(name+'.c')).write_text(source)
            self.run_command(['clang', '-flto=thin', '-ffat-lto-objects', '-c', name+'.c',
                              '-o', name+'.o'], check=True)
        for mode, name in [('rcs', 'regular'), ('rcsT', 'thin')]:
            self.run_command(['llvm-ar', mode, 'lib'+name+'.a', 'one.o'], check=True)
        for inputs, distributed in [
                ([], False), (['one.o'], False), (['one.o', 'two.o'], True), (['large.o'], True),
                *[(['-Wl,--whole-archive', *args, '-Wl,--no-whole-archive'], True) for args in
                  (['libregular.a'], ['libthin.a'], ['-L.', '-lregular'], ['-L.', '-lthin'])],
                (['one.o', 'two.o', '-Wl,--no-fat-lto-objects'], False)]:
            with self.subTest(inputs=inputs):
                result = self.run_command([self.build/'distcc', 'clang', '-fuse-ld=lld', '-flto=thin',
                                           '-ffat-lto-objects', 'main.o', *inputs, '-o', 'app'])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual('distcc-thinlto: submitting' in result.stderr, distributed, result.stderr)
                self.run_command(['./app'], check=True)
        self.assertIn('operation=B', log.read_text())

    def test_executable_input_scripts_and_local_admission(self):
        peer = self.peer(stats_response(cpus=4, cores=4, jobs=4, busy=0, running=0))
        log = self.work/'client.log'
        (self.work/'main.c').write_text('int main(void){ return 0; }')
        (self.work/'member.c').write_text('int member(void){ return 1; }')
        libs = self.work/'libs'
        libs.mkdir()
        self.run_command(['clang','-flto=thin','-c','member.c','-o','member.o'], check=True)
        self.run_command(['llvm-ar','rcs',libs/'libbits.a','member.o'], check=True)
        (libs/'libvia.so').write_text('/* script */ GROUP ( AS_NEEDED ( "libbits.a" ) )')
        (libs/'member.o').hardlink_to(self.work/'member.o')
        (libs/'libobject.so').write_text('INPUT ( "member.o" )')
        (libs/'libunknown.so').write_text('EXTERN(main)')
        for compiler in ('clang', 'clang++'):
            # A distributed backend returns live capacity. Start both idle
            # admission checks with the same idle worker statistics.
            self.start_broker(self.host(0, f',tcp,stats={peer.port}'), name=compiler)
            self.env.update(DISTCC_CLANG_LOCAL='1', DISTCC_LOG=str(log), DISTCC_VERBOSE='1',
                            PATH=str(self.build)+':'+os.environ['PATH'])
            self.run_command([compiler,'-x','c++' if compiler == 'clang++' else 'c',
                              '-flto=thin','-c','main.c','-o','main.o'], check=True)
            for library in (None, 'via', 'object', 'unknown'):
                with self.subTest(compiler=compiler, library=library):
                    flags = ['-Llibs', '-l'+library] if library else []
                    result = self.run_command([self.build/'distcc',compiler,'-flto=thin','-fuse-ld=lld',
                                               'main.o','-lm','-lgcc_s',*flags,'-o','app'])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual('distcc-thinlto: submitting' in result.stderr, bool(library), result.stderr)
                    self.run_command(['./app'], check=True)
        self.assertIn('operation=B', log.read_text())
        self.assertNotRegex(log.read_text(), r'forking to execute: \S*distcc-link\b')

    @unittest.skipUnless(shutil.which('meson'), 'Meson required')
    def test_meson_install_preserves_origin_rpath(self):
        import ctypes

        self.remote_link_worker()
        prefix = self.work / 'installed'
        libdir = prefix / 'lib/plugins/long-install-directory'
        (self.work / 'dep.c').write_text('int dep(void) { return 42; }\n')
        (self.work / 'plugin.c').write_text('extern int dep(void); int answer(void) { return dep(); }\n')
        (self.work / 'meson.build').write_text('''\
project('origin-rpath', 'c', default_options: ['b_lto=true', 'b_lto_mode=thin'])
add_project_link_arguments('-fuse-ld=lld', language: 'c')
location = get_option('prefix') / 'lib/plugins/long-install-directory'
dep = shared_library('dep', 'dep.c', install: true, install_dir: location)
shared_library('plugin', 'plugin.c', link_with: dep, install: true,
               install_dir: location, install_rpath: location)
''')
        compiler = shutil.which('clang')
        assert compiler is not None
        # This fixture selects ThinLTO/lld in meson.build. Package-build flags
        # (for example -flto=auto/-fuse-ld=bfd) must not override that contract.
        for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS'):
            self.env.pop(name, None)
        self.env['CC'] = shlex.join([str(self.build / 'distcc'), compiler])
        before = self.get_stats(self.stats_ports[0], '127.0.0.2')
        for args in (['setup', 'build', '--prefix=' + str(prefix)],
                     ['compile', '-C', 'build'], ['install', '-C', 'build', '--no-rebuild']):
            result = self.run_command(['meson', *args])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        built = self.run_command(['readelf', '-d', self.work / 'build/libplugin.so'])
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertIn('[$ORIGIN/', built.stdout)
        self.assertNotIn(r'\$ORIGIN', built.stdout)
        installed = self.run_command(['readelf', '-d', libdir / 'libplugin.so'])
        self.assertEqual(installed.returncode, 0, installed.stderr)
        self.assertIn('[' + str(libdir) + ']', installed.stdout)
        self.assertEqual(ctypes.CDLL(str(libdir / 'libplugin.so')).answer(), 42)
        def completed(stats):
            match = re.search(r'^dcc_compile_ok (\d+)$', stats, re.M)
            assert match is not None, stats
            return int(match[1])
        self.assertGreater(completed(self.get_stats(self.stats_ports[0], '127.0.0.2')), completed(before))

    def test_backend_cwd_survives_private_tmp_mount(self):
        self.check_backend_cwd(False)

    def test_cc1_backend_without_driver_compile_flag(self):
        self.check_backend_cwd(True)

    def test_resident_backend_transfers_multiple_descriptor_batches(self):
        trace = self.trace_backend()
        log = Path(self.logs[0].name)
        offset = log.stat().st_size
        self.check_backend_cwd(False, extra=1100)
        started = re.findall(rb'started reusable sandboxed ThinLTO worker pid=(\d+)', log.read_bytes()[offset:])
        self.assertEqual(len(started), 1)
        if trace.exists():
            self.assertEqual(len(re.findall(r'execve\("/usr/bin/bwrap"', trace.read_text())), 1)
        self.assert_resident_sandbox(int(started[0]), [b'/distcc-clang', b'--lto-worker'])

    @unittest.skipUnless(shutil.which('strace'), 'daemon exec tracing required')
    def test_lld_object_and_partition_jobs_share_one_sandbox(self):
        trace = self.trace_backend()
        self.env.update(DISTCC_VERBOSE='1', DISTCC_FALLBACK='0')
        compiler = shutil.which('clang')
        assert compiler is not None
        for name, flags, source in [
                ('plain', [], 'int plain(void) {return 1;}'),
                ('fat', ['-ffat-lto-objects'], 'int fat(void) {return 2;}'),
                ('split', [], '\n'.join(f'int f{i}(int x) {{return (x*{i+1}) ^ (x/7);}}' for i in range(200)))]:
            with self.subTest(name=name):
                obj = self.work/(name+'.o')
                self.run_command([compiler, '-x', 'c', '-O2', '-fPIC', '-flto=thin', *flags,
                                  '-c', '-', '-o', obj], input=source, check=True)
                result = self.run_command([compiler, '-flto=thin', *flags, '-fuse-ld=lld', '-shared',
                    '-Wl,--thinlto-distributor='+str(self.build/'distcc-thinlto'),
                    '-Wl,--thinlto-remote-compiler='+compiler, obj, '-o', name+'.so'])
                self.assertEqual(result.returncode, 0, result.stderr)
                if name == 'split':
                    self.assertRegex(result.stderr, r'submitting .*\.part-\d+\.bc ')
                else:
                    # LLD can extract the embedded FatLTO module to a sibling .o.
                    self.assertRegex(result.stderr, 'submitting '+re.escape(str(obj))+r'(?:\.\S+\.o)? ')
                self.assertEqual((self.work/(name+'.so')).read_bytes()[:4], b'\x7fELF')
        self.assertEqual(len(re.findall(r'execve\("/usr/bin/bwrap"', trace.read_text())), 1)

    def test_index_import_requires_declared_snapshot(self):
        compiler = shutil.which('clang')
        assert compiler is not None
        compilers = [Path(compiler), Path('/usr/lib/llvm/22/bin/clang')]
        for tool in dict.fromkeys(c.resolve() for c in compilers if c.exists()):
            with self.subTest(compiler=tool):
                directory = self.work/tool.name
                directory.mkdir()
                self.check_index_import(str(tool), directory)

    def check_index_import(self, compiler, directory):
        main = directory/'main.bc'
        other = directory/'other.bc'
        for path, body in [(main, 'extern int leaf(void); int main(void) { return leaf(); }'),
                           (other, 'int leaf(void) { return 0; }')]:
            r = self.run_command([compiler, '-O2', '-flto=thin', '-c', '-x', 'c', '-', '-o', path], input=body)
            self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_command([Path(compiler).with_name('ld.lld'), '--thinlto-index-only',
                              '--thinlto-emit-imports-files', '-e', 'main', main, other])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(str(other), Path(str(main)+'.imports').read_text().splitlines())
        index = Path(str(main)+'.thinlto.bc')
        inputs = directory/'inputs'
        inputs.mkdir()
        for i, path in enumerate([main, index]):
            os.link(path, inputs/str(i))
        output = self.work/'backend.o'
        job = dict(cwd=str(self.work), args=[compiler, '-c', '-x', 'ir', str(main),
                   '-fthinlto-index=' + str(index), '-o', str(output)], outputs=[str(output)])
        spec = self.work/'job'
        write_metadata(JOB_THINLTO, job, [(str(p), *fingerprint(p)) for p in (main, index)], spec)
        r = self.run_command([self.build/'distcc', '--file-job', spec, inputs, '-', output])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('undeclared ThinLTO input: ' + str(other), r.stderr)
        self.assertFalse(output.exists())

    def check_backend_cwd(self, cc1, extra=0):
        inputs = self.work / 'inputs'
        inputs.mkdir()
        source = inputs / '0'
        result = self.run_command(['clang', '-x', 'c', '-flto=thin', '-c', '-o', source, '-'],
                                  input='int main(void) { return 0; }')
        self.assertEqual(result.returncode, 0, result.stderr)
        name, output = str(self.work / 'input.bc'), str(self.work / 'output.o')
        cwd = '/tmp/client-build'
        options = ['-cc1', '-emit-obj'] if cc1 else ['-c']
        job = dict(cwd=cwd, args=[shutil.which('clang'), *options, '-x', 'ir', name, '-o', output],
                   outputs=[output])
        metadata = self.work / 'metadata'
        entries = [(name, *fingerprint(source))]
        for i in range(1, extra+1):
            path = inputs/str(i)
            path.write_text(str(i))
            entries.append((cwd+'/import-'+str(i)+'.bc', *fingerprint(path)))
        write_metadata(JOB_THINLTO, job, entries, metadata)
        result = self.run_command([self.build/'distcc', '--file-job', metadata, inputs, '-', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        executable = self.work / 'linked'
        result = self.run_command(['clang', output, '-o', executable])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([executable]).returncode, 0)

    def test_backend_cost_identity_survives_temporary_paths(self):
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.remote_link_worker('cost-broker')
        compiler = shutil.which('clang')
        source = self.work / 'cost.c'
        source.write_text('#include <stdio.h>\n'
                          '__attribute__((noinline)) int calculate(int n) { volatile int v=n; '
                          'for(int i=0;i<n;i++) v=(v*3+i)%101; return v; }\n'
                          'int main(int argc,char **argv) { printf("%d\\n", calculate(argc*23)); }\n')
        obj = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        args = [self.build / 'distcc', compiler, '-flto=thin', '-fuse-ld=lld', obj]
        log = self.work / 'cost-broker.log'
        identities = []
        outputs = []
        for index, level in enumerate(('-O2', '-O2', '-O0')):
            before = log.stat().st_size
            output = self.work / f'cost-{index}'
            result = self.run_command([*args, level, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-thinlto: compiled on', result.stderr)
            rows = re.findall(r'job cost: key=(\w+) kind=8 bytes=\d+ units=(\d+) predicted=([\d.]+)',
                              log.read_text()[before:])
            self.assertTrue(rows, log.read_text()[before:])
            self.assertTrue(all(int(row[1]) > 0 for row in rows), rows)
            identities.append({row[0] for row in rows})
            if index == 1:
                self.assertTrue(all(float(row[2]) > 0 for row in rows), rows)
            outputs.append(self.run_command([output, 'argument']).stdout)
        self.assertEqual(identities[0], identities[1])
        self.assertFalse(identities[1] & identities[2])
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0], outputs[2])

    def test_distributor_reports_capacity_failure(self):
        env = self.env.copy()
        env.pop('DISTCC_BROKER', None)
        expected = subprocess.run([self.build/'distcc', '--lto-capacity'], env=env,
                                  capture_output=True).returncode
        self.assertNotEqual(expected, 0)
        desc = self.work/'jobs.json'
        desc.write_text(json.dumps(dict(common=dict(args=['clang'], inputs=[]),
            jobs=[dict(args=['-c'], inputs=['input.bc'], outputs=['out.o'])])))
        result = subprocess.run([self.build/'distcc-thinlto', desc], env=env, capture_output=True)
        self.assertEqual(result.returncode, expected)

    def test_capacity_uses_worker_speed_without_increasing_concurrency(self):
        metrics = stats_response(cpus=4, cores=4, jobs=4, capacity=4,
                                 busy=0, queued=0, running=0, load=0)
        peer = self.peer(lambda: metrics)
        self.start_broker(self.host(0, f',tcp,stats={peer.port},speed=1') + ' ' +
                          self.host(1, f',tcp,stats={peer.port},speed=0.25'), name='capacity')
        result = self.run_command([self.build / 'distcc', '--lto-capacity'])
        self.assertEqual(result.returncode, 0, result.stderr)
        capacity = json.loads(result.stdout)
        # Each worker also admits one prepared job while its CPUs are busy.
        self.assertEqual(capacity['jobs'], 10)
        self.assertAlmostEqual(capacity['work_rate'], 5)
        self.assertAlmostEqual(capacity['min_rate'], .25)
        result = self.run_command([self.build / 'distcc', '-j'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(int(result.stdout), capacity['jobs'])
        # A changed CPU ceiling proves that the new snapshot reached the
        # broker. Transient load must not change its full-capacity grain rate.
        metrics = stats_response(cpus=3, cores=3, jobs=3, capacity=3,
                                 busy=0, queued=0, running=16, load=16)
        deadline = time.monotonic() + 3
        while True:
            result = self.run_command([self.build / 'distcc', '--lto-capacity'])
            self.assertEqual(result.returncode, 0, result.stderr)
            updated = json.loads(result.stdout)
            if updated['jobs'] == 8:
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.assertAlmostEqual(updated['work_rate'], 3.75)
        self.assertAlmostEqual(updated['min_rate'], .25)

    def test_dominant_function_skips_unproductive_partitioning(self):
        self.remote_link_worker()
        compiler = shutil.which('clang')
        self.clang_slot(compiler)
        source = self.work / 'dominant.c'
        source.write_text('__attribute__((noinline)) int heavy(int x) { volatile int v=x;\n' +
                          '\n'.join(f'v = (v * 3 + {i}) % 101;' for i in range(128)) +
                          '\nreturn v; }\n'
                          '__attribute__((noinline)) int tiny(int x) { return x+2; }\n'
                          'int main(void) { return heavy(3)==tiny(7); }\n')
        obj = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        native, distributed = self.work / 'native', self.work / 'distributed'
        args = [compiler, '-O2', '-flto=thin', '-fuse-ld=lld', obj]
        reference = self.run_command([*args, '-o', native])
        self.assertEqual(reference.returncode, 0, reference.stderr)
        result = self.run_command([self.build / 'distcc', *args, '-o', distributed])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('distcc-thinlto: partitioned ', result.stderr)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 1, result.stderr)
        self.assertEqual(self.run_command([distributed]).returncode, self.run_command([native]).returncode)

    def test_partitioned_backend_preserves_private_symbols_tls_and_unwinding(self):
        self.check_partitioned_backend()

    @unittest.skipUnless(shutil.which('sandbox'), 'Gentoo sandbox required')
    def test_partitioned_link_inside_portage_sandbox(self):
        self.check_partitioned_backend(portage=True)

    def check_partitioned_backend(self, portage=False):
        """Execute real partitions, then compare the final link's ABI and behavior."""
        compiler = shutil.which('clang++')
        self.clang_slot(compiler)
        heavy = self.work / 'heavy.cc'
        definitions = '\n'.join(
            f'__attribute__((noinline)) static int local_{i}(int x) '
            f'{{ volatile int y = x; return y * {i+1} + shared; }}'
            for i in range(32))
        expression = ' + '.join(f'local_{i}(x)' for i in range(32))
        heavy.write_text('#include <unordered_map>\n'
            'template<int N> struct Registry { static const std::unordered_map<int,int> values; };\n'
            'template<int N> const std::unordered_map<int,int> Registry<N>::values{{1,42+N}};\n'
            'template<int N> int lookup() { return Registry<N>::values.at(1); }\n'
            'asm(".globl heavy\\n");\n'
            'static int shared=2, sequence;\n'
            '__attribute__((constructor(101))) static void first() { sequence=3; }\n'
            '__attribute__((constructor(102))) static void second() { sequence*=7; }\n'
            'static thread_local int epoch=1;\n'
            'struct Scope { ~Scope() { ++shared; } };\n' + definitions + '\n'
            'extern "C" int heavy(int x) { Scope guard; if(x<0) throw x; '
            'return sequence + epoch++ + lookup<0>() + lookup<1>() + lookup<2>() + lookup<3>() '
            '+ lookup<4>() + lookup<5>() + lookup<6>() + lookup<7>() + ' + expression + '; }\n')
        small = self.work / 'small.cc'
        small.write_text('static int shared=11;\n'
            '__attribute__((noinline)) static int local_0(int x) { return x+shared; }\n'
            'extern "C" int small(int x) { return local_0(x); }\n')
        main = self.work / 'main.cc'
        main.write_text('extern "C" int printf(const char*, ...), atoi(const char*);\n'
            'extern "C" int heavy(int), small(int);\n'
            'int main(int argc,char**argv) { int x=argc>1?atoi(argv[1]):3; '
            'try { int a=heavy(x), b=heavy(x+1); printf("%d %d %d\\n",a,b,small(x)); } '
            'catch(int e) { printf("exception %d %d\\n",e,small(x)); } }\n')
        objects = []
        for source in (heavy, small, main):
            output = source.with_suffix('.o')
            result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(output)
        native, distributed = self.work / 'native', self.work / 'partitioned'
        args = [compiler, '-O2', '-flto=thin', '-fuse-ld=lld', *objects]
        reference = self.run_command([*args, '-o', native])
        self.assertEqual(reference.returncode, 0, reference.stderr)
        command = [self.build / 'distcc', *args, '-o', distributed]
        if portage:
            sandbox_log = self.work / 'sandbox.log'
            with patch.dict(self.env, SANDBOX_WRITE=str(self.root),
                            SANDBOX_LOG=str(sandbox_log), PYTHONDONTWRITEBYTECODE='1'):
                result = self.run_command(['sandbox', *command])
            self.assertFalse(sandbox_log.exists(), result.stderr)
        else:
            result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        counts = list(map(int, re.findall(r'partitioned .* into (\d+)', result.stderr)))
        self.assertTrue(any(count > 1 for count in counts), result.stderr)
        self.assertGreater(result.stderr.count('distcc-thinlto: compiled on'), len(objects))
        for argument in ('0', '4', '-7'):
            expected = self.run_command([native, argument])
            self.assertEqual(expected.returncode, 0, expected.stderr)
            actual = self.run_command([distributed, argument])
            self.assertEqual((actual.returncode, actual.stdout), (expected.returncode, expected.stdout))
        symbols = []
        for path in (native, distributed):
            result = self.run_command(['llvm-nm', '-g', '--defined-only', '-j', path])
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols.append(sorted(result.stdout.splitlines()))
        self.assertEqual(*symbols)

    def test_partitions_balance_ir_work_with_mixed_symbol_linkages(self):
        for compiler in (shutil.which('clang'), '/usr/lib/llvm/22/bin/clang'):
            if compiler and Path(compiler).exists():
                with self.subTest(compiler=compiler):
                    self.check_partition_balance(compiler)

    def check_partition_balance(self, compiler):
        probe = self.partition_probe(compiler)
        source = self.work / 'uneven.ll'
        definitions = []
        sizes = [60] * 9 + [3] * 18
        for index, size in enumerate(sizes):
            name = f'function_{index}'
            linkage = 'weak_odr ' if index % 3 != 2 else ''
            group = f' comdat(${name})' if index % 3 == 1 else ''
            if group:
                definitions.append(f'${name} = comdat any')
            body = [f'define {linkage}i32 @{name}(i32 %x) noinline{group} {{']
            for step in range(size):
                previous = '%x' if step == 0 else f'%v{step-1}'
                body.append(f'  %v{step} = add i32 {previous}, {index+1}')
            body.extend([f'  ret i32 %v{size-1}', '}'])
            definitions.append('\n'.join(body))
        definitions.append('@alias = alias i32 (i32), ptr @function_0')
        source.write_text('\n'.join(definitions))
        loads, identities = [], []
        objects = []
        for run in range(2):
            result = self.run_command([probe, source, '3',
                                       self.work / f'balanced-{run}-'])
            self.assertEqual(result.returncode, 0, result.stderr)
            parts = json.loads(result.stdout)['parts']
            loads.append([part['instructions'] for part in parts])
            identities.append([fingerprint(Path(part['path'])) for part in parts])
            if run == 0:
                for index, part in enumerate(parts):
                    output = self.work / f'balanced-{index}.o'
                    result = self.run_command([compiler, '-O2', '-c', part['path'], '-o', output])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    objects.append(output)
        self.assertEqual(*loads)
        self.assertEqual(*identities)
        self.assertEqual(sum(loads[0]), sum(size + 1 for size in sizes))
        self.assertLessEqual(max(loads[0]) - min(loads[0]), max(sizes) + 1)
        main = self.work / 'check.c'
        main.write_text('\n'.join(f'int function_{i}(int);' for i in range(len(sizes))) +
            '\nint alias(int);\nint main(void) { return alias(7) + ' +
            ' + '.join(f'function_{i}(7)' for i in range(len(sizes))) +
            ' != ' + str(7 + sizes[0] + sum(7 + size * (i+1)
                                           for i, size in enumerate(sizes))) + '; }\n')
        output = self.work / 'balanced'
        result = self.run_command([compiler, main, *objects, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_module_assembly_preserves_private_definitions(self):
        compiler = shutil.which('clang')
        probe = self.partition_probe(compiler)
        source = self.work / 'assembly.c'
        source.write_text('asm(".local asm_value\\n.data\\nasm_value: .long 7\\n.text\\n");\n'
                          'extern int asm_value;\n'
                          '__attribute__((noinline)) int first(void) { return asm_value; }\n'
                          '__attribute__((noinline)) int second(void) { return first()+1; }\n'
                          'int main(void) { return second()!=8; }\n')
        bitcode = self.work / 'assembly.bc'
        result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command([probe, bitcode, '3', self.work / 'part'])
        self.assertEqual(result.returncode, 0, result.stderr)
        parts = json.loads(result.stdout)['parts']
        self.assertGreater(len(parts), 1)
        output = self.work / 'assembly'
        result = self.run_command([compiler, *(part['path'] for part in parts), '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_function_assembly_preserves_cross_function_symbols(self):
        compiler = shutil.which('clang')
        probe = self.partition_probe(compiler)
        source = self.work / 'function-assembly.c'
        source.write_text(
            '__attribute__((noinline)) void define(void) {\n'
            '  asm volatile(".local asm_value\\n.pushsection .data\\n"'
            '"asm_value: .long 7\\n.popsection\\n");\n}\n'
            'extern int asm_value;\n'
            '__attribute__((noinline)) int read_value(void) { return asm_value; }\n'
            'int main(void) { define(); return read_value()!=7; }\n')
        bitcode = source.with_suffix('.bc')
        result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command([probe, bitcode, '3', self.work / 'part'])
        self.assertEqual(result.returncode, 0, result.stderr)
        parts = json.loads(result.stdout)['parts']
        self.assertEqual(len(parts), 1)
        output = self.work / 'function-assembly'
        result = self.run_command([compiler, parts[0]['path'], '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)
        obj = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command([self.build / 'distcc', compiler, '-O2', '-flto=thin',
                                   '-fuse-ld=lld', obj, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_partitioned_backend_preserves_retained_section(self):
        self.check_used_section(retain=True)

    def test_instruction_assembly_and_plain_data_section_split(self):
        compiler = shutil.which('clang')
        assert compiler is not None
        probe = self.partition_probe(compiler)
        source = self.work / 'barrier.c'
        source.write_text('volatile int state __attribute__((section(".data"))) = 40;\n'
                          'int value(void) { asm volatile("" ::: "memory");\n'
                          '#if defined(__x86_64__) || defined(__i386__)\n'
                          'int n = 2; asm volatile("0: dec %0; jnz 0b" : "+r"(n) : : "cc");\n'
                          '#endif\nreturn state; }\n'
                          'int main(void) { return value() + 2 == 42 ? 0 : 1; }\n')
        module = source.with_suffix('.bc')
        subprocess.run([compiler, '-emit-llvm', '-c', source, '-o', module], check=True)
        partition = subprocess.check_output([probe, module, '2', self.work/'part'])
        parts = json.loads(partition)['parts']
        self.assertEqual(len(parts), 2)
        output = self.work / 'barrier'
        subprocess.run([compiler, *(p['path'] for p in parts), '-o', output], check=True)
        subprocess.run([output], check=True)

    def test_partitioned_backend_preserves_compiler_used_section(self):
        self.check_used_section(retain=False)

    def test_versioned_import_applies_to_implicit_backend_call(self):
        compiler = 'clang'
        probe = self.partition_probe(compiler)
        src, bc, obj = (self.work / ('import.'+ext) for ext in ('c', 'bc', 'o'))
        src.write_text('asm(".symver memcpy, memcpy@IMPORT_1");\n'
                       'void copy(char*a,char*b,unsigned n){__builtin_memcpy(a,b,n);}\n'
                       'int independent(int x){return x+1;}\n')
        subprocess.run([compiler, '-emit-llvm', '-c', src, '-o', bc], check=True)
        parts = json.loads(subprocess.check_output([probe, bc, '2', self.work/'part']))['parts']
        self.assertEqual(len(parts), 1)
        subprocess.run([compiler, '-c', parts[0]['path'], '-o', obj], check=True)
        self.assertIn('memcpy@IMPORT_1', subprocess.check_output(['llvm-nm', '-u', obj], text=True))

    @unittest.skipUnless(os.uname().machine == 'x86_64', 'x86 ELF assembly')
    def test_versioned_definition_and_section_comdat_survive_partitioned_link(self):
        self.remote_link_worker()
        compiler = 'clang'
        self.clang_slot(compiler)
        module = self.work / 'definitions.ll'
        # A retained version tag and a function belong to the same COMDAT.
        # A versioned exported definition must share its assembler's object.
        functions = []
        for name in ('implementation', 'group_member', 'independent', 'another'):
            comdat = ' comdat($tag)' if name == 'group_member' else ''
            body = '\n'.join('store volatile i32 %x, ptr @state' for _ in range(128))
            assembly = 'call void asm sideeffect "pause", ""()' if name == 'implementation' else ''
            functions.append(f'define dso_local i32 @{name}(i32 %x) noinline{comdat} {{\n'
                             f'{assembly}\n{body}\nret i32 %x\n}}')
        module.write_text('target triple = "x86_64-unknown-linux-gnu"\n'
                          'module asm ".symver implementation,exported@@PART_1"\n'
                          '!llvm.module.flags = !{!0}\n!0 = !{i32 8, !"PIC Level", i32 2}\n'
                          '$tag = comdat any\n@state = internal global i32 0\n'
                          '@tag = linkonce_odr hidden constant {ptr,i64} {ptr @group_member,i64 42},'
                          ' section "version_records", comdat, align 8\n'
                          '@llvm.compiler.used = appending global [1 x ptr] [ptr @tag],'
                          ' section "llvm.metadata"\n@__start_version_records = external constant i8\n'
                          'define ptr @records() { ret ptr @__start_version_records }\n'
                          + '\n'.join(functions))
        main = self.work / 'caller.c'
        main.write_text('int exported(int), independent(int), another(int);\n'
                        'extern const struct {int (*f)(int); long v;} *records(void);\n'
                        'int main(void){return exported(5)!=5 || independent(7)!=7 || another(9)!=9 || '
                        'records()->v!=42 || records()->f(4)!=4;}\n')
        versions = self.work / 'version.map'
        versions.write_text('PART_1 { global: exported; };\n')
        obj, caller, binary = self.work/'definitions.o', self.work/'caller.o', self.work/'check'
        for args in ([compiler, '-flto=thin', '-fPIC', '-c', module, '-o', obj],
                     [compiler, '-fPIC', '-c', main, '-o', caller]):
            subprocess.run(args, check=True)
        result = self.run_command([self.build/'distcc', compiler, '-flto=thin', '-fuse-ld=lld',
                                   '-Wl,--version-script=' + str(versions), obj, caller, '-o', binary])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stderr, r'partitioned .* into (?:[2-9]|\d{2,})\b')
        self.assertEqual(self.run_command([binary]).returncode, 0)

        library = self.work/'libversions.so'
        result = self.run_command([self.build/'distcc', compiler, '-flto=thin', '-shared',
                                   '-fPIC', '-fuse-ld=lld', '-Wl,--version-script=' + str(versions),
                                   obj, '-o', library])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stderr, r'partitioned .* into (?:[2-9]|\d{2,})\b')
        symbols = subprocess.check_output(['llvm-nm', library], text=True)
        self.assertRegex(symbols, r'(?m)\bt \w+\.distcc\.local$')
        subprocess.run([compiler, caller, library, '-o', binary], check=True)
        self.assertEqual(self.run_command([binary]).returncode, 0)

    @unittest.skipUnless(os.uname().machine == 'x86_64', 'requires x86-64 ELF')
    def test_segment_overrides_preserve_operand_ownership(self):
        probe = self.partition_probe('clang')
        for dialect, template, constraint, argument, expected in (
                ('', 'mov %fs:${1:c}, $0', '=r,i', 'i64 0', 2),
                ('', 'mov %gs:0x48, $0', '=r', '', 2),
                ('', 'mov %fs:(%rax), $0', '=r', '', 2),
                ('inteldialect', 'mov $0, qword ptr fs:[0x28]', '=r', '', 2),
                ('', 'mov %fs:ext_sym, $0', '=r', '', 1)):
            with self.subTest(assembly=template):
                module = self.work/'segment.bc'
                ir = ('target triple = "x86_64-unknown-linux-gnu"\n'
                      'define i64 @load_segment() {\n'
                      f'  %v = call i64 asm sideeffect {dialect} "{template}", "{constraint}"({argument})\n'
                      '  ret i64 %v\n}\ndefine i64 @other() { ret i64 1 }\n')
                subprocess.run(['llvm-as', '-o', module], input=ir, check=True, text=True)
                parts = json.loads(subprocess.check_output(
                    [probe, module, '2', self.work/'part']))['parts']
                self.assertEqual(len(parts), expected)
                for part in parts:
                    subprocess.run(['clang', '-c', part['path'], '-o', self.work/'part.o'], check=True)

    def test_partitioned_shared_object_keeps_absolute_alias(self):
        probe = self.partition_probe('clang')
        # ThinLTO summary generation in LLVM 22/23 cannot represent this
        # legal constant alias. Exercise the same post-import splitter on IR.
        module = self.work/'absolute.bc'
        ir = ('@absolute_value = alias i8, inttoptr (i64 42 to ptr)\n'
              'define i32 @independent() { ret i32 2 }\n' +
              ''.join(f'define i32 @f{i}() section "ordered" {{ ret i32 {i} }}\n' for i in range(5)))
        subprocess.run(['llvm-as', '-o', module], input=ir, text=True, check=True)
        parts = json.loads(subprocess.check_output(
            [probe, module, '6', self.work/'part']))['parts']
        self.assertEqual(len(parts), 2)
        objects = []
        for part in parts:
            obj = Path(part['path']).with_suffix('.o')
            subprocess.run(['clang', '-fPIC', '-c', part['path'], '-o', obj], check=True)
            objects.append(obj)
        merged, library = self.work/'merged.o', self.work/'libabsolute.so'
        self.assertRegex(subprocess.check_output(['llvm-nm', objects[0]], text=True),
                         r'(?m)^0*2a A absolute_value$')
        subprocess.run(['ld.lld', '-r', *objects, '-o', merged], check=True)
        subprocess.run(['clang', '-shared', merged, '-o', library], check=True)
        symbols = subprocess.check_output(['llvm-nm', '-D', library], text=True)
        self.assertRegex(symbols, r'(?m)^0*2a A absolute_value$')

    def test_rust_linker_plugin_assembly_partitioning(self):
        rustc = shutil.which('rustc')
        if not rustc or os.uname().machine != 'x86_64':
            self.skipTest('x86-64 rustc required')
        version = subprocess.check_output([rustc, '-vV'], text=True)
        llvm = next(line.split(': ')[1].split('.')[0] for line in version.splitlines()
                    if line.startswith('LLVM version: '))
        compiler = shutil.which('clang-' + llvm)
        if not compiler:
            self.skipTest('Clang matching rustc required')
        probe = self.partition_probe(compiler)
        src, module = self.work/'ops.rs', self.work/'ops.bc'
        main = self.work/'main.c'
        main.write_text('unsigned intel(unsigned), att(unsigned);\n'
                        'int main(void) {return intel(21) != 42 || att(11) != 22;}\n')
        functions = '''
            #![no_std]
            #![allow(named_asm_labels)]
            use core::arch::asm;
            #[no_mangle] pub extern "C" fn intel(mut n:u32)->u32 {
                unsafe {asm!("mov eax, {0:e}", "add {0:e}, eax", inout(reg) n,
                             out("eax") _, options(nomem, nostack))}; n
            }
            #[no_mangle] pub extern "C" fn att(mut n:u32)->u32 {
                unsafe {asm!("mov {0:e}, %eax", "add %eax, {0:e}", inout(reg) n,
                             out("eax") _, options(att_syntax, nomem, nostack))}; n
            }'''
        for extra in ('', 'asm!("call outside")', 'asm!("mov eax, %eax", options(att_syntax))',
                      'asm!(".pushsection extra", ".popsection")', 'asm!("private_label:")'):
            with self.subTest(extra=extra):
                src.write_text(functions + ('\n#[no_mangle] pub unsafe fn other(){' + extra + ';}' if extra else ''))
                subprocess.run([rustc, '--edition=2021', '--crate-type=lib', '-Cpanic=abort',
                                '-Clinker-plugin-lto', '-Ccodegen-units=1', '--emit=obj', src, '-o', module],
                               check=True)
                plan = subprocess.check_output([probe, module, '2', self.work/'part'])
                parts = json.loads(plan)['parts']
                self.assertEqual(len(parts), 1 if extra else 2)
                if not extra:
                    output = self.work/'check-asm'
                    subprocess.run([compiler, main, *(p['path'] for p in parts), '-o', output], check=True)
                    subprocess.run([output], check=True)

    def check_used_section(self, retain):
        compiler = shutil.which('clang')
        assert compiler is not None
        self.clang_slot(compiler)
        source = self.work / 'retained.c'
        body = 'volatile int v=x;\n' + '\n'.join(f'v += {i};' for i in range(128))
        attributes = 'used,retain' if retain else 'used'
        source.write_text(
            '__attribute__((noinline)) int first(int x) { ' + body + '; return x+1; }\n'
            '__attribute__((noinline)) int second(int x) { ' + body + '; return first(x)+1; }\n'
            '__attribute__((noinline)) int third(int x) { ' + body + '; return second(x)+1; }\n'
            '__attribute__((' + attributes + ',section("ENTRIES"))) '
            'static int (*const verb_data_27)(int)=third;\n'
            '__attribute__((' + attributes + ',section("ENTRIES"))) '
            'static int (*const verb_data_28)(int)=second;\n'
            'extern int (*__start_ENTRIES[])(int), (*__stop_ENTRIES[])(int);\n'
            'int main(void) { return __stop_ENTRIES-__start_ENTRIES!=2 || '
            '__start_ENTRIES[0](2)!=5 || __start_ENTRIES[1](2)!=4; }\n')
        obj, output = source.with_suffix('.o'), self.work / 'retained'
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        second = self.work/'second.o'
        subprocess.run([compiler, '-flto=thin', '-x', 'c', '-c', '-', '-o', second],
                       input=b'int additional_module(void) { return 11; }', check=True)
        flags = ['-Wl,--gc-sections'] if retain else []
        trace = self.work / 'executed'
        prefix = ['strace', '-f', '-e', 'execve', '-o', trace] if shutil.which('strace') else []
        result = self.run_command([*prefix, compiler, '-O2', '-flto=thin', '-fuse-ld=lld',
                                   '-Wl,--thinlto-distributor='+str(self.build/'distcc-thinlto'),
                                   '-Wl,--thinlto-remote-compiler='+compiler, '-Wl,-u,additional_module',
                                   *flags, obj, second, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        counts = list(map(int, re.findall(r'partitioned .* into (\d+)', result.stderr)))
        self.assertTrue(counts and min(counts) > 1, result.stderr)
        self.assertIn('status=0 operation=N', result.stderr)
        self.assertIn('status=0 operation=A', result.stderr)
        self.assertIn('status=0 operation=M', result.stderr)
        if prefix:
            executed = trace.read_text()
            self.assertNotIn('"-r", "-o", "merged.o"', executed)
            self.assertNotRegex(executed, r'execve\("[^"]*/(bwrap|llvm-objcopy|distcc-lto)"')
            self.assertNotRegex(executed, r'execve\("[^"]*/llvm-config')
            self.assertNotIn('"--lto-capacity"', executed)
            self.assertNotIn('"--file-jobs"', executed)
            self.assertNotRegex(executed, r'execve\("[^"]*/python(?:3[.\d]*)?"')
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_parallel_inputs_survive_replacement_and_cancellation(self):
        import signal
        compiler = shutil.which('clang')
        bc = self.work/'input.bc'
        result = self.run_command([compiler, '-x', 'c', '-emit-llvm', '-c', '-', '-o', bc],
                                  input='int value(void){return 42;}')
        self.assertEqual(result.returncode, 0, result.stderr)
        original = bc.read_bytes()
        outputs = [self.work/f'out-{i}.o' for i in range(32)]
        jobs = [dict(args=['-x', 'ir', str(bc), '-o', str(out)], inputs=[str(bc)], outputs=[str(out)])
                for out in outputs]
        desc = self.work/'jobs.json'
        desc.write_text(json.dumps(dict(common=dict(args=[compiler, '-c'], inputs=[]), jobs=jobs)))
        temp = self.work/'distributor'
        temp.mkdir()
        for cancel in (True, False):
            bc.write_bytes(original)
            with subprocess.Popen([self.build/'distcc-thinlto', desc], cwd=self.work,
                                  env=dict(self.env, TMPDIR=str(temp)), stderr=subprocess.PIPE, text=True) as proc:
                assert proc.stderr is not None
                for line in proc.stderr:
                    if line.startswith('distcc-thinlto: submitting '):
                        bc.write_bytes(b'replaced after snapshot')
                        if cancel:
                            proc.send_signal(signal.SIGTERM)
                        break
                else:
                    self.fail('backend never submitted')
                _, log = proc.communicate(timeout=30)
            self.assertEqual(proc.returncode, 128 + signal.SIGTERM if cancel else 0, log)
            self.assertFalse(list(temp.glob('distcc-lto-*')))
            self.assertFalse(list(self.work.glob('*.distcc-*')))
        self.assertEqual(log.count('distcc-thinlto: compiled on '), len(outputs))
        self.assertTrue(all(path.read_bytes()[:4] == b'\x7fELF' for path in outputs))

    def test_parallel_local_retry_diagnostics_and_cancellation(self):
        import select
        compiler = self.work / 'clang-98765'
        compiler.write_text("""#!/bin/sh
if test -n "${RETRY_STARTED:-}"; then
    echo $$ > "$RETRY_STARTED"
    exec sleep 40
fi
printf 'first %s\\n' "$2" >&2
sleep .02
printf 'last %s\\n' "$2" >&2
cp "$1" "$2"
""")
        compiler.chmod(0o755)
        source = self.work/'input.o'
        self.run_command(['clang', '-x', 'c', '-c', '-', '-o', source], input='int data;').check_returncode()
        outputs = [str(self.work/f'retry-{i}.o') for i in range(8)]
        jobs = [dict(args=[str(source), out], inputs=[str(source)], outputs=[out]) for out in outputs]
        request = self.work/'jobs.json'
        request.write_text(json.dumps(dict(common=dict(args=[str(compiler)], inputs=[]), jobs=jobs)))
        self.env.update(DISTCC_FALLBACK='1', DISTCC_SKIP_LOCAL_RETRY='0')
        command = [self.build/'distcc-thinlto', request]
        result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        for out in outputs:
            self.assertIn(f'first {out}\nlast {out}\n', result.stderr)
            self.assertEqual(Path(out).read_bytes(), source.read_bytes())

        fifo = self.work/'retry-started'
        os.mkfifo(fifo)
        with os.fdopen(os.open(fifo, os.O_RDWR), 'r') as reader, subprocess.Popen(
                command, env=dict(self.env, RETRY_STARTED=str(fifo)), stderr=subprocess.PIPE) as proc:
            try:
                self.assertTrue(select.select([reader], [], [], 10)[0])
                child = int(reader.readline())
                proc.terminate()
                _, log = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 143, log)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child, 0)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()

    def test_parallel_batches_release_transport_descriptors(self):
        compiler = shutil.which('clang')
        assert compiler is not None
        work, temporary, final = (self.work/name for name in ('inputs', 'lto', 'output'))
        for directory in (work, temporary, final):
            directory.mkdir()
        source = work/'input.o'
        linked = final/'library.so'
        subprocess.run([compiler, '-flto=thin', '-x', 'c', '-c', '-', '-o', source],
                       input=b'int value(void) {return 42;}', check=True)
        subprocess.run([Path(compiler).with_name('ld.lld'), '-shared', '--thinlto-index-only',
                        source, '-o', linked], check=True)
        index = str(source)+'.thinlto.bc'
        (work/'dtlto-inputs').touch()
        jobs = []
        for i in range(16):
            output = str(temporary/f'part-{i}.o')
            jobs.append(dict(inputs=[str(source), index], outputs=[output],
                             args=[str(source), '-fthinlto-index='+index, '-o', output]))
        description = temporary/'jobs.json'
        description.write_text(json.dumps(dict(common=dict(linker_output=str(linked),
            args=[compiler, '-c'], inputs=[]), jobs=jobs)))
        # Sample at the linker's reply boundary, after every transfer thread has exited.
        sender = """import os, socket, sys
s = socket.socket(fileno=int(os.environ['DISTCC_RUST_DTLTO_FD']))
for _ in range(3):
    s.send(os.fsencode(sys.argv[1]))
    assert s.recv(1) == bytes([0])
    print(len(os.listdir('/proc/%d/fd' % os.getppid())))
"""
        with open(os.devnull, 'rb') as fd:
            result = self.run_command([self.build/'distcc-thinlto', '--rust-link', work, linked,
                temporary, compiler, fd.fileno(), sys.executable, '-c', sender, description],
                pass_fds=(fd.fileno(),))
        self.assertEqual(result.returncode, 0, result.stderr)
        counts = list(map(int, result.stdout.splitlines()))
        self.assertEqual(counts, [counts[0]]*3)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on '), 3 * len(jobs))

    def test_ranked_parallel_submission(self):
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_broker(' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}')
                                  for i in range(2)), name='ranked')
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        jobs, sizes, objects = [], [], []
        for index, size in enumerate((8192, 2048, 512)):
            source = self.work / f'part{index}.c'
            source.write_text(f'int value{index}(void) {{ return {index + 1}; }}\n'
                              f'const char pad{index}[] = "' + 'a' * size + '";\n')
            bitcode = source.with_suffix('.bc')
            result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
            self.assertEqual(result.returncode, 0, result.stderr)
            output = source.with_suffix('.o')
            objects.append(output)
            sizes.append(bitcode.stat().st_size)
            jobs.append(dict(args=['-O2', '-c', str(bitcode), '-o', str(output)],
                             inputs=[str(bitcode)], outputs=[str(output)]))
        description = self.work / 'jobs.json'
        description.write_text(json.dumps(dict(common=dict(args=[compiler], inputs=[]), jobs=jobs)))
        command = [self.build / 'distcc-thinlto', description]
        result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        submitted = re.findall(r'submitting (\S+) instructions=\d+ bytes=(\d+)', result.stderr)
        self.assertEqual([int(n) for _, n in submitted], sorted(sizes, reverse=True))
        observed = {key: int(size) for key, size in re.findall(r'job cost: key=(\w+) kind=8 bytes=(\d+)',
                                                               (self.work/'ranked.log').read_text())}
        assigned = re.findall(r'assigning host=.* key=(\w+) ', (self.work/'ranked.log').read_text())
        self.assertEqual([observed[k] for k in assigned], sorted(sizes, reverse=True))
        main = self.work / 'main.c'
        main.write_text('int value0(void), value1(void), value2(void);\n'
                        'int main(void) { return value0()+value1()+value2()!=6; }\n')
        result = self.run_command([compiler, main, *objects, '-o', 'check'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([self.work / 'check']).returncode, 0)
        # An error before enqueueing must release the submission barrier
        # and leave previously published objects intact.
        previous = [path.read_bytes() for path in objects]
        self.env.update(DISTCC_BROKER=str(self.work / 'absent.sock'), DISTCC_RECONNECT_TIMEOUT='0')
        self.assertNotEqual(self.run_command(command).returncode, 0)
        self.assertEqual([path.read_bytes() for path in objects], previous)

    def test_closed_submission_notification_cancels_without_reconnect(self):
        inputs = self.work / 'inputs'
        inputs.mkdir()
        source = inputs / '0'
        source.write_bytes(b'')
        metadata, output = self.work / 'job', self.work / 'result.o'
        job = dict(cwd=self.work, args=['clang', '-c', 'empty.bc'], outputs=[output])
        write_metadata(JOB_THINLTO, job, [(str(self.work / 'empty.bc'), *fingerprint(source))], metadata)
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([str(self.build / 'distcc'), '--file-job', str(metadata),
                str(inputs), str(write_fd), str(output)], env=self.env, capture_output=True,
                pass_fds=(write_fd,), timeout=3)
        finally:
            os.close(write_fd)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_hip_wrapper_distributes_host_thinlto(self):
        self.env['DISTCC_CLANG_LOCAL'] = '1'
        self.link(shutil.which('clang'), hip=True)

    def test_hip_device_links_and_host_backends(self):
        """Resolve a cross-TU GPU call on two targets, then link the host."""
        compiler = shutil.which('clang++')
        devices = []
        if shutil.which('amdgpu-arch'):
            detected = self.run_command(['amdgpu-arch'])
            if detected.returncode == 0:
                devices = detected.stdout.split()
        targets = list(dict.fromkeys(devices)) or ['gfx900', 'gfx1030']
        flags = ['-O2', '-flto=thin', '-fgpu-rdc',
                 *('--offload-arch=' + target for target in targets)]
        sources = (
            '__attribute__((device)) int value(int n) { return n + 17; }',
            '#include <hip/hip_runtime.h>\n'
            '__attribute__((device)) int value(int);\n'
            '__global__ void kernel(int *p) { *p = value(*p); }\n'
            '#include <cstdio>\n'
            'int main(int argc, char **) { if (argc == 1) return 0; '
            'int count = 0; if (hipGetDeviceCount(&count) != hipSuccess || !count) return 10; '
            'for (int i = 0; i < count; ++i) { int *p, n = 5; '
            'if (hipSetDevice(i) != hipSuccess || hipMalloc(&p, sizeof(n)) != hipSuccess) return 11; '
            'if (hipMemcpy(p, &n, sizeof(n), hipMemcpyHostToDevice) != hipSuccess) return 12; '
            'hipLaunchKernelGGL(kernel, dim3(1), dim3(1), 0, 0, p); '
            'if (hipMemcpy(&n, p, sizeof(n), hipMemcpyDeviceToHost) != hipSuccess || n != 22) return 13; '
            'if (hipFree(p) != hipSuccess) return 14; } '
            'std::printf("%d GPU devices checked\\n", count); return 0; }',
        )
        objects = []
        for index, source in enumerate(sources):
            path = self.work / f'device {index}.hip'
            path.write_text(source)
            obj = path.with_suffix('.o')
            result = self.run_command([compiler, *flags, '-c', path, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        args = [compiler, *flags, '--hip-link', '-fuse-ld=lld', *objects, '-lamdhip64']
        for prefix, name in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            output = self.work / name
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-offload-link: compiled on'), len(targets),
                                 result.stderr)
                self.assertGreater(result.stderr.count('distcc-thinlto: compiled on'), 0,
                                   result.stderr)
            self.assertEqual(self.run_command([output]).returncode, 0)
            if devices:
                runtime = self.run_command([output, '--gpu'])
                self.assertEqual(runtime.returncode, 0, runtime.stdout + runtime.stderr)
                print(name + ': ' + runtime.stdout.strip(), flush=True)
            image = self.work / (name + '.hipfb')
            dump = self.run_command(['llvm-objcopy', '--dump-section',
                                     '.hip_fatbin=' + str(image), output, '/dev/null'])
            self.assertEqual(dump.returncode, 0, dump.stderr)
            bundles = self.run_command(['clang-offload-bundler', '-list', '-type=o',
                                        '-input=' + str(image)])
            self.assertEqual(bundles.returncode, 0, bundles.stderr)
            for target in targets:
                self.assertIn(target, bundles.stdout)

    def test_target_prefixed_compiler(self):
        selected = shutil.which('clang')
        assert selected is not None
        clang = Path(selected)
        target = subprocess.check_output([clang, '-dumpmachine'], text=True).strip()
        compiler = clang.parent / (target + '-clang++')
        if not compiler.is_file():
            self.skipTest('target-prefixed Clang is required')
        self.link(compiler)

    def test_static_constructor_order(self):
        objects = []
        for index, name in enumerate(('z_first', 'a_second', 'm_third')):
            source = self.work / (name + '.cpp')
            source.write_text('#include <cstdio>\nnamespace { struct Init { Init() { std::puts("' +
                              str(index) + '"); } }; Init init; }\n')
            obj = source.with_suffix('.o')
            result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        main = self.work / 'main.cpp'
        main.write_text('int main() {}\n')
        result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', main, '-o', 'main.o'])
        self.assertEqual(result.returncode, 0, result.stderr)
        args = ['clang++', '-O2', '-flto=thin', '-fuse-ld=lld', *objects, 'main.o']
        for prefix, output in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 4, result.stderr)
            actual = self.run_command([self.work / output])
            self.assertEqual(actual.stdout, '0\n1\n2\n')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
    def test_cmake_discovers_openmp_with_link_launcher(self):
        (self.work / 'main.c').write_text('#include <omp.h>\n'
            'int main(void) { return omp_get_max_threads() < 1; }\n')
        (self.work / 'main.cpp').write_text((self.work / 'main.c').read_text())
        (self.work / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(openmp_probe C CXX)\n'
            'find_package(OpenMP REQUIRED)\n'
            'add_executable(check_c main.c)\n'
            'target_link_libraries(check_c PRIVATE OpenMP::OpenMP_C)\n'
            'add_executable(check_cxx main.cpp)\n'
            'target_link_libraries(check_cxx PRIVATE OpenMP::OpenMP_CXX)\n')
        for language in ('C', 'CXX'):
            self.env[f'CMAKE_{language}_COMPILER_LAUNCHER'] = str(self.build / 'distcc')
            self.env[f'CMAKE_{language}_LINKER_LAUNCHER'] = str(self.build / 'distcc')
        compiler, cxx = shutil.which('clang'), shutil.which('clang++')
        assert compiler is not None and cxx is not None
        build = self.work / 'build'
        result = self.run_command(['cmake', '-S', self.work, '-B', build, '-G', 'Ninja',
                                  '-DCMAKE_C_COMPILER=' + compiler,
                                  '-DCMAKE_CXX_COMPILER=' + cxx,
                                  '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_command(['cmake', '--build', build])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for program in ('check_c', 'check_cxx'):
            self.assertEqual(self.run_command([build / program]).returncode, 0)

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
    def test_cmake_link_launcher(self):
        self.remote_link_worker()
        for language, suffix, driver in (('C', 'c', 'clang'), ('HIP', 'hip', 'clang++')):
            with self.subTest(language=language):
                source = self.work / language
                source.mkdir()
                (source / ('part.' + suffix)).write_text('int value(void) { return 42; }')
                (source / ('main.' + suffix)).write_text('int value(void); int main(void) { return value()!=42; }')
                (source / 'CMakeLists.txt').write_text(
                    f'cmake_minimum_required(VERSION 3.21)\nproject(distributed_lto {language})\n'
                    f'add_executable(check main.{suffix} part.{suffix})\n'
                    'target_compile_options(check PRIVATE -flto=thin)\n'
                    'target_link_options(check PRIVATE -flto=thin -fuse-ld=lld)\n')
                self.env[f'CMAKE_{language}_LINKER_LAUNCHER'] = 'distcc'
                self.env[f'CMAKE_{language}_COMPILER_LAUNCHER'] = 'distcc'
                compiler = shutil.which(driver)
                assert compiler is not None
                build = source / 'build'
                result = self.run_command(['cmake', '-S', source, '-B', build, '-G', 'Ninja',
                                           f'-DCMAKE_{language}_COMPILER=' + compiler,
                                           *(['-DCMAKE_HIP_ARCHITECTURES=gfx900'] if language == 'HIP' else [])])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = self.run_command(['cmake', '--build', build])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((result.stdout + result.stderr).count('distcc-thinlto: compiled on'), 2)
                self.assertEqual(self.run_command([build / 'check']).returncode, 0)

    def test_absolute_clang_preserves_client_target(self):
        selected = shutil.which('clang')
        assert selected is not None
        compiler = Path(selected).resolve()
        target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
        other = 'x86_64-linux-gnu' if target.startswith('aarch64') else 'aarch64-linux-gnu'
        wrappers = self.work / 'bin'
        wrappers.mkdir()
        wrapper = wrappers / compiler.name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler)) +
                           ' --target=' + other + ' "$@"\n')
        wrapper.chmod(0o755)
        # A secondary loopback address exercises worker execution even when
        # the broker can optimize the primary local address into direct exec.
        address = '127.0.0.2'
        self.restart_server(self.build / 'distccd', address=address,
                            DISTCCD_PATH=str(wrappers) + os.pathsep + os.defpath)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        saved = dict(self.env)
        self.start_broker(f'{address}:{self.ports[0]}/4,tcp,stats={self.stats_ports[0]}', name='target')
        saved['DISTCC_BROKER'] = self.env['DISTCC_BROKER']
        self.env = saved
        source = self.work / 'target.c'
        source.write_text('int main(void) { return 0; }')
        obj = self.work / 'target.o'
        result = self.run_command([self.build / 'distcc', compiler, '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        output = self.work / 'target'
        result = self.run_command([compiler, obj, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_archive_and_selected_compiler(self):
        compiler = '/usr/lib/llvm/22/bin/clang'
        if not Path(compiler).exists():
            self.skipTest('second Clang toolchain not installed')
        self.link(compiler, archive=True)

    def test_failure_propagates_without_local_retry(self):
        source = self.work / 'invalid.o'
        source.write_bytes(b'invalid bitcode')
        index = self.work / 'invalid.thinlto.bc'
        index.write_bytes(b'invalid index')
        output = self.work / 'result.o'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c'], 'inputs': []},
            'jobs': [{'args': ['-x', 'ir', str(source), '-fthinlto-index=' + str(index), '-o', str(output)],
                      'inputs': [str(source), str(index)], 'outputs': [str(output)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertNotIn('retrying backend locally', result.stderr)

    def test_lld_cache_avoids_backend_reexecution(self):
        self.remote_link_worker()
        source = self.work / 'main.c'
        source.write_text('int main(void) { return 0; }')
        obj = source.with_suffix('.o')
        result = self.run_command(['clang', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        command = [self.build / 'distcc', 'clang', '-flto=thin', '-fuse-ld=lld', obj,
                   '-Wl,--thinlto-cache-dir=' + str(self.work / 'cache'), '-o', self.work / 'linked']
        for count in (1, 0):
            result = self.run_command(command)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), count, result.stderr)
            self.assertEqual(self.run_command([self.work / 'linked']).returncode, 0)

    def test_multiple_declared_outputs(self):
        source = self.work / 'input.bc'
        result = self.run_command(['clang', '-x', 'c', '-flto=thin', '-c', '-o', source, '-'],
                                  input='int value(void) { return 42; }')
        self.assertEqual(result.returncode, 0, result.stderr)
        output, trace = self.work / 'output.o', self.work / 'trace.json'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c', '-x', 'ir'], 'inputs': []},
            'jobs': [{'args': [str(source), '-o', str(output), '-ftime-trace=' + str(trace)],
                      'inputs': [str(source)], 'outputs': [str(output), str(trace)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-thinlto: compiled on', result.stderr)
        self.assertEqual(output.read_bytes()[:4], b'\x7fELF')
        self.assertIn('traceEvents', json.loads(trace.read_text()))


if __name__ == '__main__':
    unittest.main()
