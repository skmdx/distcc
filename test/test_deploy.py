"""Release preconditions must fail before deployment can modify a worker."""
import importlib.util
import contextlib
import io
import json
import re
import runpy
from pathlib import Path
import tempfile
import unittest
import shutil
import os
import subprocess
import sys
import threading
from unittest import mock

spec = importlib.util.spec_from_file_location('deploy', Path(__file__).resolve().parents[1] /
                                            'packaging/gentoo/deploy.py')
assert spec is not None and spec.loader is not None
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)
rust_slots = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'packaging/gentoo/rust-slots.py'))

class RustDistributionTest(unittest.TestCase):
    def test_inventory_collects_slots_once_and_never_redistributes_copies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            slots = [root/name for name in ('native', 'rust-bin-release', 'rust-distcc-copy')]
            (root/'loader').write_bytes(b'loader')
            (root/'libdependency.so').write_bytes(b'dependency')
            for slot in slots:
                (slot/'bin').mkdir(parents=True)
                compiler = slot/'bin/rustc'
                compiler.write_text('#!/bin/sh\nprintf "rustc test\\ncommit-hash: commit\\nhost: arch\\nLLVM version: llvm\\n"\n')
                compiler.chmod(0o700)
            def paths(pattern):
                if pattern.startswith('/usr/lib'):
                    return [str(slots[0]/'bin/rustc'), str(slots[0]/'bin/rustc-version')]
                return [str(slots[1]/'bin/rustc'), str(slots[0]/'bin/rustc')]
            os.link(slots[0]/'bin/rustc', slots[0]/'bin/rustc-version')
            check_output = subprocess.check_output
            def output_for(command, **options):
                if command[0] == 'ldd':
                    alias = '/lib/interpreter => ' if 'native' in command[1] else ''
                    return ('\tlibdependency.so => ' + str(root/'libdependency.so') + ' (0x1)\n\t' +
                            alias + str(root/'loader') + ' (0x2)\n')
                return check_output(command, **options)
            with mock.patch('glob.glob', side_effect=paths), \
                 mock.patch('shutil.which', return_value=str(slots[2]/'bin/rustc')), \
                 mock.patch('subprocess.check_output', side_effect=output_for), \
                 mock.patch.dict(rust_slots['inventory'].__globals__, compile_probe=lambda cmd, tmp: True):
                inventory = rust_slots['inventory'](directory)['slots']
            self.assertEqual([info['root'] for info in inventory],
                             [str(slots[0]), str(slots[1])])
            self.assertEqual(inventory[0]['identity']['commit'], 'commit')
            self.assertEqual([item['name'] for item in inventory[0]['runtime']],
                             ['libdependency.so', 'ld.so'])
            self.assertEqual(inventory[0]['runtime'], inventory[1]['runtime'])

    def test_copy_plan_checks_all_cores_and_skips_installed_builds(self):
        cpu = rust_slots['cpu_features']
        wide = cpu('processor: 0\nflags: sse sse4a\n\nprocessor: 1\nflags: sse')
        narrow = cpu('processor: 0\nflags: sse')
        targets = [dict(name=str(i)) for i in range(4)]
        slot = dict(host='triple', identity=dict(version='rustc', commit='a', llvm='23.1'), digest='a'*64)
        systems = [dict(cpu=wide, slots=[slot]), dict(cpu=narrow, slots=[]),
                   dict(cpu=cpu('processor: 0\nflags: sse4a sse'), slots=[]),
                   dict(cpu=cpu('processor: 0\nflags: sse4a sse\n\nprocessor: 1'), slots=[])]
        jobs = deploy.rust_copies(targets, systems)
        self.assertEqual(jobs[slot['digest']][2], [targets[2]])
        systems[2]['slots'].append(dict(slot, digest='b'*64))
        self.assertEqual(deploy.rust_copies(targets, systems), {})

    def test_distribution_checks_before_copy_and_publishes_after_verification(self):
        targets = [dict(name=name, temporary='work') for name in ('source', 'compatible', 'incompatible')]
        barrier = threading.Barrier(3, timeout=5)
        digest = 'a'*64
        slot = dict(host='triple', identity=dict(version='v', commit='h', llvm='23.1'),
                    digest=digest, root='/rust', runtime=[dict(name='ld.so', path='/lib/ld.so')])
        for ready, works in ((True, True), (False, True), (False, False)):
            calls = []
            def remote(target, action, *args, privileged=False):
                name = target['name']
                if action == 'inventory':
                    barrier.wait()
                    features = ['sse4a'] if name != 'incompatible' else ['sse2']
                    return dict(cpu=dict(machine='x86', required=features, available=features),
                                slots=[slot] if name == 'source' else [])
                calls.append((name, action, args))
                if action == 'ready':
                    return ready
                if action == 'verify':
                    return works
            with tempfile.TemporaryDirectory() as directory, mock.patch.object(deploy, 'rust_slot', remote), \
                 mock.patch.object(deploy, 'run') as run:
                deploy.distribute_rust_compilers(targets, Path(directory))
                checks = [] if ready else ['prepare', 'verify'] + (['publish'] if works else [])
                self.assertEqual([act for name, act, _ in calls if name == 'compatible'],
                                 ['ready', *checks, 'prune'])
                self.assertIn(('compatible', 'prune', (digest,) if works else ()), calls)
                self.assertEqual([c[1:] for c in calls if c[0]=='incompatible'], [('prune', ())])
                copies = [c.args[0] for c in run.call_args_list if '--delete' in c.args[0]]
                self.assertEqual(len(copies), int(not ready))
                if copies:
                    self.assertIn('--checksum', copies[0])
                    self.assertIn('--exclude=/.distcc-complete', copies[0])

    def test_validation_checks_each_client_against_actual_worker_offers(self):
        targets = [dict(name=name, temporary='work', server_hosts=name) for name in ('a', 'b', 'c')]
        identity = dict(version='rustc vendor', commit='hash', llvm='23.1.5')
        slot = dict(root='/rust', digest='a'*64, host='same-triple', identity=identity)
        requirement = ('23.1', 'x86-64')
        def key(**changes):
            return deploy.rust_compiler_key(dict(slot, identity=identity | changes))
        self.assertEqual(key(llvm='23.1.5'), key(llvm='23.1.6'))
        self.assertEqual(key(llvm='23.1.5+libcxx'), key())
        self.assertNotEqual(key(llvm='23.2.0'), key())
        self.assertNotEqual(key(version='rustc other vendor'), key())
        offers = {'a': ({key(llvm='23.1.6')}, {requirement}),
                  'b': ({'1234567890abcdef'}, {requirement}), 'c': (set(), set())}
        inventory = [(targets[0], [slot]), (targets[1], [slot]), (targets[2], [])]
        routes = []
        def check(args, client, unit, log):
            env = dict(arg.split('=', 1) for arg in args if arg.startswith('VALIDATE_'))
            routes.append((client['name'], env['VALIDATE_DISTCC_HOSTS'], env['VALIDATE_DISTCC_RUST_ROUTE']))
        with tempfile.TemporaryDirectory() as work, mock.patch.object(deploy, 'transfer'), \
             mock.patch.object(deploy, 'run'), mock.patch.object(deploy, 'bounded', check), \
             mock.patch.object(deploy, 'rust_cgu_requirement', return_value=requirement), \
             mock.patch.object(deploy, 'rust_worker_capabilities', side_effect=lambda h: (h, *offers[h])):
            self.assertEqual(deploy.validate_rust_cluster(targets, Path(work), inventory), 6)
        self.assertEqual([w for c,w,_ in routes if c=='a'], ['a','b','c'])
        self.assertEqual([w for _,w,_ in routes], ['a','a','b','b','c','c'])
        self.assertCountEqual(routes, [(client, worker, route) for client in ('a', 'b')
                              for worker, route in zip(('a', 'b', 'c'), ('remote', 'cgu', 'local'))])

    def test_retirement_requires_owned_directory_and_matching_marker(self):
        manage = rust_slots['manage']
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            roots = [base/('rust-distcc-' + c*64) for c in 'abcdef']
            for root in roots[:-1]:
                root.mkdir()
            for root, state in zip(roots, ('', 'isa-checked ', 'installing ')):
                (root/'.distcc-complete').write_text(state + root.name[-64:] + '\n')
            (roots[3]/'.distcc-complete').write_text('unrelated')
            (roots[4]/'.distcc-complete').symlink_to(roots[0]/'.distcc-complete')
            roots[5].symlink_to(roots[1], target_is_directory=True)
            manage(base, 'prune', 'b'*64)
            self.assertEqual(set(base.iterdir()), {roots[1], *roots[3:]})
            for root in roots[3:]:
                with self.assertRaises(RuntimeError):
                    manage(base, 'prepare', root.name[-64:])
            with self.assertRaises(ValueError):
                manage(base, 'prune', '../other')

    @unittest.skipUnless(shutil.which('rustc'), 'requires an installed Rust slot')
    def test_managed_slot_compiles_and_executes_before_advertisement(self):
        manage = rust_slots['manage']
        executable = shutil.which('rustc')
        assert executable is not None
        compiler = Path(executable).resolve()
        sysroot = Path(subprocess.check_output([compiler, '--print=sysroot'], text=True).strip())
        loaded = subprocess.check_output(['ldd', str(compiler)], text=True)
        loader = re.search(r'^\s*(/\S+)(?: => (/\S+))? \(', loaded, re.M)
        assert loader is not None, loaded
        with tempfile.TemporaryDirectory() as work:
            base, digest = Path(work), '1'*64
            root = base/('rust-distcc-' + digest)
            manage(base, 'prepare', digest)
            (root/'bin').symlink_to(sysroot/'bin')
            (root/'lib').symlink_to(sysroot/'lib')
            runtime = root/'distcc-runtime'
            runtime.mkdir()
            (runtime/'ld.so').symlink_to(loader[2] or loader[1])
            self.assertFalse(manage(base, 'ready', digest, work))
            self.assertTrue(manage(base, 'verify', digest, work))
            manage(base, 'publish', digest)
            self.assertTrue(manage(base, 'ready', digest, work))
            (runtime/'ld.so').unlink()
            self.assertFalse(manage(base, 'ready', digest, work))


class TimeoutEvidenceTest(unittest.TestCase):
    def test_timeout_records_live_waiter_and_preserves_failure(self):
        log = io.StringIO()
        command = [sys.executable, '-c',
                   'import os,time; print(os.getpid(),flush=True); time.sleep(60)']
        with contextlib.redirect_stderr(log), self.assertRaises(subprocess.TimeoutExpired) as caught:
            deploy.run_observed(command, env=os.environ, timeout=1, observe=(os.getpid(),))
        assert caught.exception.stdout is not None
        pid = int(caught.exception.stdout.strip())
        records = json.loads(log.getvalue().split(': ', 1)[1])
        self.assertIn(str(os.getpid()), records)
        child = records[str(pid)]
        self.assertIn('State:', child['status'])
        self.assertIn('S (sleeping)', child['status'])
        self.assertTrue(child['wchan'])
        self.assertIn('0', child['fds'])
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_success_and_nonzero_exit_keep_output_without_diagnostics(self):
        for code in (0, 7):
            with self.subTest(code=code), contextlib.redirect_stderr(io.StringIO()) as log:
                result = deploy.run_observed([sys.executable, '-c',
                    f'import sys; print("output"); print("error",file=sys.stderr); sys.exit({code})'],
                    env=os.environ, timeout=5, text=True)
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (code, 'output\n', 'error\n'))
                self.assertFalse(log.getvalue())

    def test_process_exit_during_observation_is_recorded(self):
        process = subprocess.Popen([sys.executable, '-c', 'pass'])
        process.wait(timeout=5)
        record = deploy.validation_snapshot([process.pid])[process.pid]
        self.assertIn('error', record['status'])

    def test_binary_stdin_cwd_and_checked_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            result = deploy.run_observed([sys.executable, '-c',
                'import os,sys; assert os.getcwd()==os.environ["EXPECTED_CWD"]; '
                'sys.stdout.buffer.write(sys.stdin.buffer.read())'],
                env=dict(os.environ, EXPECTED_CWD=directory), cwd=directory,
                input=b'\0\xffpayload\n', timeout=5, check=True)
            self.assertEqual(result.stdout, b'\0\xffpayload\n')
        with self.assertRaises(subprocess.CalledProcessError) as caught:
            deploy.run_observed([sys.executable, '-c', 'import sys; sys.exit(9)'],
                                 env=os.environ, timeout=5, check=True)
        self.assertEqual(caught.exception.returncode, 9)


class TransportTest(unittest.TestCase):
    def test_selected_compiler_follows_path_without_version_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            for selection in ('previous', 'updated'):
                directory = Path(temporary) / selection
                directory.mkdir()
                compiler = directory / 'selected-compiler'
                compiler.symlink_to(sys.executable)
                with mock.patch.dict(os.environ, PATH=str(directory) + os.pathsep + os.environ['PATH']):
                    self.assertEqual(deploy.resolve_compilers({'compiler_paths': ['selected-compiler']}),
                                     [str(compiler)])
            self.assertEqual(deploy.resolve_compilers({'compiler_paths': [str(compiler)]}),
                             [str(compiler)])

    def test_package_mirror_removes_retired_ebuild_without_touching_siblings(self):
        if not shutil.which('sudo') or subprocess.run(
                ['sudo', '-n', 'true'], capture_output=True).returncode:
            self.skipTest('passwordless sudo required')
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            source, destination = root / 'source', root / 'destination'
            source.mkdir()
            destination.mkdir()
            (source / 'current.ebuild').write_text('current')
            (destination / 'retired.ebuild').write_text('retired')
            sibling = root / 'other-package'
            sibling.write_text('preserve')
            deploy.transfer([str(source) + '/'], str(destination), mirror=True)
            self.assertEqual(sorted(p.name for p in destination.iterdir()), ['current.ebuild'])
            self.assertEqual(sibling.read_text(), 'preserve')
            (destination / 'keep.distfile').write_text('cached')
            deploy.transfer([str(source / 'current.ebuild')], str(destination))
            self.assertEqual((destination / 'keep.distfile').read_text(), 'cached')

    @unittest.skipUnless(shutil.which('zsh'), 'zsh required')
    def test_remote_arguments_do_not_undergo_login_shell_expansion(self):
        values = ['=category/package-1', 'a b; $(false)', "a'b"]
        cmd = deploy.command(['python3', '-c', 'import sys; print(repr(sys.argv[1:]))', *values],
                             {'ssh': 'unused'})
        self.assertEqual(deploy.run(['zsh', '-c', cmd[-1]]), repr(values))


class RolloutTest(unittest.TestCase):
    def test_failed_canary_does_not_upgrade_other_targets(self):
        visited = []
        def action(target):
            visited.append(target)
            raise RuntimeError('installed validation failed')
        with self.assertRaisesRegex(RuntimeError, 'installed validation failed'):
            deploy.rollout([{'name': 'canary'}, {'name': 'worker'}], lambda target: None, action)
        self.assertEqual(visited, [{'name': 'canary'}])

    def test_other_targets_start_after_canary_succeeds(self):
        visited = []
        def action(target):
            if target['name'] != 'canary':
                self.assertIn('canary', visited)
            visited.append(target['name'])
        deploy.rollout([{'name': name} for name in ('canary', 'first', 'second')], lambda target: None, action)
        self.assertCountEqual(visited, ['canary', 'first', 'second'])
        deploy.rollout([{'name': 'canary'}], lambda target: None, action)

    def test_all_packages_are_built_before_any_worker_is_upgraded(self):
        built = set()
        targets = [{'name': name} for name in ('canary', 'first', 'second')]
        def prepare(target):
            built.add(target['name'])
        def activate(target):
            self.assertEqual(built, {'canary', 'first', 'second'})
        deploy.rollout(targets, prepare, activate)

    def test_build_failure_leaves_every_installed_worker_unchanged(self):
        upgraded = []
        def prepare(target):
            if target['name'] == 'worker':
                raise RuntimeError('package build failed')
        with self.assertRaisesRegex(RuntimeError, 'package build failed'):
            deploy.rollout([{'name': 'canary'}, {'name': 'worker'}], prepare,
                           lambda target: upgraded.append(target['name']))
        self.assertEqual(upgraded, [])

    def test_native_package_preparation_does_not_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            target = {'name': 'worker', 'temporary': temporary, 'repository': str(work/'repo')}
            with mock.patch.object(deploy, 'run', return_value='sys-devel/distcc-old'), \
                 mock.patch.object(deploy, 'build_cache_environment', return_value=[]) as cache, \
                 mock.patch.object(deploy, 'transfer'), mock.patch.object(deploy, 'bounded') as bounded:
                deploy.prepare(target, 'distcc-new', work/'distcc-new.ebuild', work, {})
                args, selected, stage, _ = bounded.call_args.args
                self.assertIn('--buildpkgonly', args)
                self.assertIn('--usepkg=n', args)
                self.assertIn('PKGDIR='+temporary+'/distcc-deploy-binpkgs/distcc-new', args)
                self.assertEqual((selected, stage), (target, 'build'))
                cache.assert_called_once_with(target, ['PKGDIR='+temporary+'/distcc-deploy-binpkgs/distcc-new'])
            with mock.patch.object(deploy, 'run', side_effect=['sys-devel/distcc-old', 'sys-devel/distcc-new']), \
                 mock.patch.object(deploy, 'bounded') as bounded:
                deploy.install(target, 'distcc-new', work)
                args, selected, stage, _ = bounded.call_args.args
                self.assertIn('--usepkgonly', args)
                self.assertEqual((selected, stage), (target, 'install'))

    def test_ccache_preserves_features_and_does_not_choose_a_cache_directory(self):
        target = {'name': 'builder'}
        context = ['ROOT=/cross', 'PORTAGE_CONFIGROOT=/cross']
        for features in ('distcc sandbox userpriv', 'distcc ccache sandbox'):
            with mock.patch.object(deploy, 'run', return_value=json.dumps({
                    'FEATURES': features, 'PORTAGE_TMPDIR': '/build space'})) as run:
                env = deploy.build_cache_environment(target, context)
            self.assertEqual(env, ['FEATURES=' + features + ('' if 'ccache' in features else ' ccache'),
                                   'CCACHE_BASEDIR=/build space', 'CCACHE_NOHASHDIR=1'])
            self.assertEqual(run.call_args.args[1], target)
            self.assertEqual(run.call_args.args[0][:3], ['env', *context])
            self.assertFalse(any(value.startswith('CCACHE_DIR=') for value in env))
        with mock.patch.object(deploy, 'run', return_value='null'):
            self.assertEqual(deploy.build_cache_environment(target, context), [])

    def test_cross_package_probes_cache_on_build_host_in_cross_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            target = {'name': 'worker', 'binary': True, 'temporary': '/target', 'repository': '/repo'}
            cross = {'root': '/cross', 'limits': ['CPUWeight=10']}
            with mock.patch.object(deploy, 'run', return_value='sys-devel/distcc-old'), \
                 mock.patch.object(deploy, 'transfer'), \
                 mock.patch.object(deploy, 'build_cache_environment', return_value=['FEATURES=distcc ccache']) as cache, \
                 mock.patch.object(deploy, 'bounded') as bounded:
                deploy.prepare(target, 'distcc-new', work/'distcc-new.ebuild', work, cross)
            cache.assert_called_once_with(None, ['ROOT=/cross', 'SYSROOT=/cross',
                'PORTAGE_CONFIGROOT=/cross', 'PKGDIR='+str(work/'binpkgs'/'worker')])
            self.assertIn('FEATURES=distcc ccache', bounded.call_args.args[0])
            self.assertTrue(bounded.call_args.kwargs['cross'])

    @unittest.skipUnless(shutil.which('ccache') and shutil.which('gcc') and Path('src/config.h').is_file(),
                         'ccache, GCC and a configured build tree required')
    def test_ccache_reuses_lto_object_across_release_source_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            env = dict(os.environ, CCACHE_DIR=str(root/'cache'),
                       CCACHE_BASEDIR=str(root), CCACHE_NOHASHDIR='1')
            objects = []
            for revision in ('previous', 'next'):
                source = root/('distcc-'+revision)/'work'/('distcc-'+revision)
                (source/'src').mkdir(parents=True)
                shutil.copyfile('src/config.h', source/'config.h')
                for header in (deploy.ROOT/'src').glob('*.h'):
                    shutil.copyfile(header, source/'src'/header.name)
                shutil.copyfile(deploy.ROOT/'src'/'argv_file.c', source/'src'/'argv_file.c')
                output = source/'argv.o'
                subprocess.run(['ccache', 'gcc', '-O3', '-pipe', '-march=native', '-mtune=native',
                    '-flto=auto', '-D_GNU_SOURCE', '-DHAVE_CONFIG_H', '-I'+str(source),
                    '-I'+str(source/'src'), '-c', str(source/'src'/'argv_file.c'), '-o', str(output)],
                    cwd=source, env=env, check=True, capture_output=True, timeout=30)
                objects.append(output.read_bytes())
            self.assertEqual(objects[0], objects[1])
            self.assertIn(b'.gnu.lto_', objects[0])
            stats = dict(line.split() for line in subprocess.check_output(
                ['ccache', '--print-stats'], env=env, text=True).splitlines())
            self.assertEqual(int(stats['cache_miss']), 1)
            self.assertEqual(int(stats['direct_cache_hit']) + int(stats['preprocessed_cache_hit']), 1)


@unittest.skipUnless(shutil.which('systemd-run') and shutil.which('sudo'), 'systemd and sudo required')
class BuildLifecycleTest(unittest.TestCase):
    def setUp(self):
        if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode:
            self.skipTest('passwordless sudo required')

    def test_build_does_not_impose_memory_or_swap_limits(self):
        probe = ('from pathlib import Path; '
                 'p=Path("/proc/self/cgroup").read_text().strip().split("::",1)[1]; '
                 'g=Path("/sys/fs/cgroup")/p.lstrip("/"); '
                 'assert (g/"memory.max").read_text().strip()=="max"; '
                 'assert (g/"memory.swap.max").read_text().strip()=="max"')
        with tempfile.TemporaryFile(mode='w+') as log:
            deploy.bounded(['python3', '-c', probe],
                           {'name': f'limits-test-{os.getpid()}'}, 'build', log)

    def test_persistent_build_can_be_stopped_and_resumed(self):
        unit = f'distcc-deploy-persistent-test-{os.getpid()}.service'
        path = '/run/systemd/system/' + unit
        deploy.run(['sudo', '-n', 'tee', path], input=(
            '[Service]\nType=exec\nExecStart=/bin/sleep 60\n'
            'MemoryMax=32M\nMemorySwapMax=0\nIOAccounting=yes\n'))
        def cleanup():
            deploy.run(['sudo', '-n', 'systemctl', 'stop', unit])
            deploy.run(['sudo', '-n', 'rm', '--', path])
            deploy.run(['sudo', '-n', 'systemctl', 'daemon-reload'])
        self.addCleanup(cleanup)
        deploy.run(['sudo', '-n', 'systemctl', 'daemon-reload'])
        deploy.run(['sudo', '-n', 'systemctl', 'start', unit])
        self.assertEqual(deploy.builds_to_pause({'quiesce_units': [unit]}), [unit])
        deploy.run(['sudo', '-n', 'systemctl', 'stop', unit])
        deploy.run(['sudo', '-n', 'systemctl', 'start', unit])
        self.assertEqual(deploy.run(['systemctl', 'is-active', unit]), 'active')
        print(deploy.run(['systemctl', 'show', unit, '-p', 'CPUUsageNSec',
                          '-p', 'IOReadBytes', '-p', 'IOWriteBytes']))

    def test_transient_build_is_rejected_before_it_is_stopped(self):
        unit = f'distcc-deploy-transient-test-{os.getpid()}.service'
        deploy.run(['sudo', '-n', 'systemd-run', '--unit=' + unit,
                    '-p', 'MemoryMax=32M', '-p', 'MemorySwapMax=0',
                    '-p', 'IOAccounting=yes', '/bin/sleep', '60'])
        self.addCleanup(deploy.run, ['sudo', '-n', 'systemctl', 'stop', unit])
        with self.assertRaisesRegex(RuntimeError, 'Cannot resume transient build unit'):
            deploy.builds_to_pause({'quiesce_units': [unit]})
        self.assertEqual(deploy.run(['systemctl', 'is-active', unit]), 'active')
        print(deploy.run(['systemctl', 'show', unit, '-p', 'CPUUsageNSec',
                          '-p', 'IOReadBytes', '-p', 'IOWriteBytes']))


@unittest.skipUnless(importlib.util.find_spec('portage'), 'Gentoo Portage required')
class ReleaseTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'source'
        self.overlay = self.root / 'overlay'
        for tree in (self.source, self.overlay):
            tree.mkdir()
            deploy.run(['git', 'init', '-q', tree])
        (self.source / 'src').mkdir()
        (self.source / 'src/input.c').write_text('int input;\n')
        self.commit(self.source)
        self.revision = deploy.run(['git', '-C', self.source, 'rev-parse', 'HEAD'])
        self.package = self.overlay / 'sys-devel/distcc'
        self.package.mkdir(parents=True)
        self.ebuild = self.package / 'distcc-9.0-r1.ebuild'
        self.ebuild.write_text(f'DISTCC_COMMIT="{self.revision}"\n')
        self.commit(self.overlay)
        old = deploy.ROOT
        setattr(deploy, 'ROOT', self.source)
        self.addCleanup(setattr, deploy, 'ROOT', old)

    def commit(self, tree):
        deploy.run(['git', '-C', tree, 'add', '.'])
        deploy.run(['git', '-C', tree, '-c', 'user.name=Test', '-c',
                    'user.email=test@example.invalid', 'commit', '-qm', 'Fixture'])

    def test_committed_release(self):
        self.assertEqual(deploy.release(self.overlay), ('distcc-9.0-r1', self.ebuild))

    def test_unrelated_editor_file_does_not_block_release(self):
        (self.source / '.deploy.swp').write_bytes(b'editor state')
        self.assertEqual(deploy.release(self.overlay), ('distcc-9.0-r1', self.ebuild))

    def test_untracked_implementation_is_not_omitted(self):
        (self.source / 'src/new_input.c').write_text('int added;\n')
        with self.assertRaisesRegex(RuntimeError, 'uncommitted'):
            deploy.release(self.overlay)

    def test_dirty_source(self):
        (self.source / 'src/input.c').write_text('int changed;\n')
        with self.assertRaisesRegex(RuntimeError, 'uncommitted'):
            deploy.release(self.overlay)

    def test_dirty_overlay(self):
        self.ebuild.write_text(self.ebuild.read_text() + '# pending\n')
        with self.assertRaisesRegex(RuntimeError, 'uncommitted'):
            deploy.release(self.overlay)

    def test_unreleased_implementation(self):
        (self.source / 'src/input.c').write_text('int changed;\n')
        self.commit(self.source)
        with self.assertRaises(RuntimeError):
            deploy.release(self.overlay)


if __name__ == '__main__':
    unittest.main()
