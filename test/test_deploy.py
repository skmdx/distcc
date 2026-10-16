"""Release preconditions must fail before deployment can modify a worker."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('deploy', Path(__file__).resolve().parents[1] /
                                            'packaging/gentoo/deploy.py')
assert spec is not None and spec.loader is not None
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


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
