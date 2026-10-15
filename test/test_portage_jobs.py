"""Portage phase inheritance and explicit parallelism overrides."""
import os
from pathlib import Path
import subprocess
import unittest


class PortageJobsTest(unittest.TestCase):
    def run_hook(self, script):
        hook = Path(__file__).resolve().parents[1] / 'packaging/gentoo/jobs.bashrc'
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('DISTCC_', 'MAKE', 'NINJA', 'CMAKE_'))}
        env['JOBS_HOOK'] = str(hook)
        result = subprocess.run(['bash', '-c', '''
has() { local word=$1; shift; [[ " $* " == *" $word "* ]]; }
timeout() { shift; "$@"; }
distcc() { echo "$workers"; return "${failed:-0}"; }
nproc() { echo 3; }
FEATURES=distcc EBUILD_PHASE=compile workers=12
source "$JOBS_HOOK"
''' + script], env=env, capture_output=True, text=True, check=True)
        return result.stdout.strip().splitlines()

    def test_refresh_and_explicit_package_override(self):
        self.assertEqual(self.run_hook('''
echo "$MAKEOPTS $NINJAOPTS $NINJAFLAGS $CMAKE_BUILD_PARALLEL_LEVEL"
workers=6
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $NINJAFLAGS $CMAKE_BUILD_PARALLEL_LEVEL"
MAKEOPTS='-j2' NINJAOPTS='-j1' CMAKE_BUILD_PARALLEL_LEVEL=1
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $NINJAFLAGS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['-j12 -j12 -j12 12', '-j6 -j6 -j6 6', '-j2 -j1 -j2 1'])

    def test_disabled_restricted_or_unreachable(self):
        for settings in ('FEATURES=', 'RESTRICT=distcc', 'failed=1'):
            with self.subTest(settings=settings):
                self.assertEqual(self.run_hook(settings + '''
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['-j3 -j3 3'])

    def test_exhausted_pool_remains_bounded(self):
        self.assertEqual(self.run_hook('''
workers=0
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['-j1 -j1 1'])


if __name__ == '__main__':
    unittest.main()
