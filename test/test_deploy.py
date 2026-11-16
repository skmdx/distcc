"""Release preconditions must fail before deployment can modify a worker."""
import importlib.util
import contextlib
import io
import json
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
            def run(args, target=None, **kwargs):
                self.assertEqual(args[:2], ['python3', '-c'])
                def paths(pattern):
                    return [str(slots[0]/'bin/rustc')] if pattern.startswith('/usr/lib') else [
                        str(slots[1]/'bin/rustc'), str(slots[0]/'bin/rustc')]
                output = io.StringIO()
                check_output = subprocess.check_output
                def output_for(command, **options):
                    if command[0] == 'ldd':
                        alias = '/lib/interpreter => ' if 'native' in command[1] else ''
                        return '\tlibdependency.so => ' + str(root/'libdependency.so') + ' (0x1)\n\t' + alias + str(root/'loader') + ' (0x2)\n'
                    return check_output(command, **options)
                with mock.patch('glob.glob', side_effect=paths), \
                     mock.patch('shutil.which', return_value=str(slots[2]/'bin/rustc')), \
                     mock.patch('subprocess.check_output', side_effect=output_for), \
                     contextlib.redirect_stdout(output):
                    exec(args[2], {})
                return output.getvalue()
            with mock.patch.object(deploy, 'run', side_effect=run):
                inventory = deploy.distribute_rust_compilers([{'name': 'worker'}], root)
            self.assertEqual([info['root'] for info in inventory[0][1]],
                             [str(slots[0]), str(slots[1])])
            self.assertEqual(inventory[0][1][0]['identity']['commit'], 'commit')
            self.assertEqual([item['name'] for item in inventory[0][1][0]['runtime']],
                             ['libdependency.so', 'ld.so'])
            self.assertEqual(inventory[0][1][0]['runtime'], inventory[0][1][1]['runtime'])

    def test_parallel_inventory_skips_installed_and_only_stages_missing(self):
        targets = [{'name': 'first'}, {'name': 'second'}]
        for state in ('complete', 'unmarked', 'wrong-marker', 'broken'):
            installed = state == 'complete'
            with self.subTest(state=state), tempfile.TemporaryDirectory() as directory:
                barrier = threading.Barrier(2, timeout=5)
                transfers = []
                def run(args, target=None, **kwargs):
                    if args[:2] == ['python3', '-c']:
                        assert target is not None
                        if len(args) == 3:
                            barrier.wait()  # Serial inventory would time out.
                            name = target['name']
                            return json.dumps([dict(root='/toolchain/' + name, host='test-arch', digest=name,
                                                    runtime=[dict(name='libLLVM.so', path='/backend/libLLVM.so')]),
                                               dict(root='/toolchain/common', host='test-arch', digest='common', runtime=[])])
                        # Exercise the actual presence/-vV probe locally.
                        root = Path(directory)/target['name']
                        compiler = root/'bin/rustc'
                        compiler.parent.mkdir(parents=True, exist_ok=True)
                        compiler.write_text('#!/bin/sh\nexit ' + ('1' if state == 'broken' else '0') + '\n')
                        compiler.chmod(0o700)
                        (root/'distcc-runtime').mkdir(exist_ok=True)
                        shutil.copy2(compiler, root/'distcc-runtime/ld.so')
                        if state != 'unmarked':
                            (root/'.distcc-complete').write_text('wrong' if state == 'wrong-marker' else root.name)
                        return subprocess.check_output([*args[:3], str(root)], text=True).strip()
                    transfers.append((args, target))
                    if args[:2] == ['rsync', '-rlpt']:
                        # The source transfer creates the toolchain root.
                        (Path(args[-1])/'lib').mkdir()
                    if args[:4] == ['sudo', '-n', 'python3', '-c']:
                        root = Path(directory)/target['name']
                        subprocess.run([sys.executable, '-c', args[4], str(root)], check=True)
                        self.assertEqual((root/'.distcc-complete').read_text().strip(), root.name)
                    return ''
                with mock.patch.object(deploy, 'run', side_effect=run):
                    deploy.distribute_rust_compilers(targets, Path(directory))
                if installed:
                    self.assertEqual(transfers, [])
                else:
                    self.assertEqual(sum(args[0] == 'rsync' for args, _ in transfers), 4)
                    self.assertEqual(sum(args[:2] == ['rsync', '-Lpt'] for args, _ in transfers), 2)
                    self.assertEqual(sum(args[:3] == ['sudo', '-n', 'rsync'] for args, _ in transfers), 2)
                    self.assertEqual(sum(args[-1] == '-vV' for args, _ in transfers), 2)
                    for args, _ in transfers:
                        if args[:3] == ['sudo', '-n', 'rsync']:
                            self.assertIn('--checksum', args)
                            self.assertIn('--delete', args)
                    for target in targets:
                        commands = [args for args, peer in transfers if peer == target]
                        self.assertEqual(commands[-2][:4], ['sudo', '-n', 'sync', '-f'])
                        self.assertEqual(commands[-1][:4], ['sudo', '-n', 'python3', '-c'])
                self.assertFalse([path for path in Path(directory).glob('rust-*') if path.is_dir()])


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
