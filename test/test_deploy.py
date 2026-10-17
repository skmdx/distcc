"""Release preconditions must fail before deployment can modify a worker."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import shutil
import os
import subprocess

spec = importlib.util.spec_from_file_location('deploy', Path(__file__).resolve().parents[1] /
                                            'packaging/gentoo/deploy.py')
assert spec is not None and spec.loader is not None
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class TransportTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('zsh'), 'zsh required')
    def test_remote_arguments_do_not_undergo_login_shell_expansion(self):
        values = ['=category/package-1', 'a b; $(false)', "a'b"]
        cmd = deploy.command(['python3', '-c', 'import sys; print(repr(sys.argv[1:]))', *values],
                             {'ssh': 'unused'})
        self.assertEqual(deploy.run(['zsh', '-c', cmd[-1]]), repr(values))


@unittest.skipUnless(shutil.which('systemd-run') and shutil.which('sudo'), 'systemd and sudo required')
class BuildLifecycleTest(unittest.TestCase):
    def setUp(self):
        if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode:
            self.skipTest('passwordless sudo required')

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
