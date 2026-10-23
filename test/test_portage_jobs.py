"""Portage phase inheritance and explicit parallelism overrides."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


class PortageJobsTest(unittest.TestCase):
    def test_portage_saved_phase_environment(self):
        helpers = list(Path('/usr/lib/portage').glob('*/save-ebuild-env.sh'))
        if not helpers:
            self.skipTest('Portage environment serializer is not installed')
        self.assertEqual(self.run_hook('''
PORTAGE_BIN_PATH="''' + str(helpers[-1].parent) + '''"
EAPI=8
MAKEOPTS='--output-sync=line'
source "$JOBS_HOOK"
source "$PORTAGE_BIN_PATH/eapi.sh"
source "$PORTAGE_BIN_PATH/phase-functions.sh"
source "$PORTAGE_BIN_PATH/save-ebuild-env.sh"
set -o pipefail
saved=$(__save_ebuild_env | __filter_readonly_variables) || exit 1
unset ${!DISTCC_AUTO_@} ${!distcc_auto_@}
eval "$saved" || exit 1
workers=6
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['--output-sync=line -j6 -j6 6'])

    def run_hook(self, script):
        hook = Path(__file__).resolve().parents[1] / 'packaging/gentoo/jobs.bashrc'
        env = {key: value for key, value in os.environ.items()
               if key not in ('CC', 'CXX') and
               not key.startswith(('DISTCC_', 'CCACHE_', 'MAKE', 'NINJA', 'CMAKE_'))}
        env['JOBS_HOOK'] = str(hook)
        result = subprocess.run(['bash', '-c', '''
has() { local word=$1; shift; [[ " $* " == *" $word "* ]]; }
timeout() { shift; "$@"; }
distcc() { echo "$workers"; return "${failed:-0}"; }
nproc() { echo 3; }
FEATURES=distcc EBUILD_PHASE=compile workers=12
source "$JOBS_HOOK"
''' + script], env=env, capture_output=True, text=True, check=True)
        self.assertEqual(result.stderr, '')
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

    def test_non_parallel_make_options_keep_automatic_jobs_across_phases(self):
        self.assertEqual(self.run_hook('''
MAKEOPTS='--output-sync=line --keep-going'
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS"
workers=6
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS"
'''), ['--output-sync=line --keep-going -j12 -j12',
       '--output-sync=line --keep-going -j6 -j6'])

    def test_explicit_make_jobs_survive_other_options(self):
        for option in ('-j1', '-j 2', '--jobs=3', '--jobs 4', '-j', '--jobs'):
            with self.subTest(option=option):
                self.assertEqual(self.run_hook('''
MAKEOPTS='--output-sync=line ''' + option + ''''
source "$JOBS_HOOK"
echo "$MAKEOPTS"
'''), ['--output-sync=line ' + option])

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

    def test_absolute_compilers_do_not_claim_distributed_capacity(self):
        for compiler in ('CC', 'CXX'):
            with self.subTest(compiler=compiler):
                self.assertEqual(self.run_hook(compiler + '''=/usr/bin/gcc
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['-j3 -j3 3'])

    def test_explicit_distcc_path_and_symlink_keep_distributed_capacity(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'distcc'
            target.touch()
            link = Path(directory) / 'compiler'
            link.symlink_to(target)
            for compiler in (target, link):
                with self.subTest(compiler=compiler):
                    self.assertEqual(self.run_hook('CC="' + str(compiler) + ''' gcc"
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), ['-j12 -j12 12'])

    def test_ccache_masquerade_uses_effective_compiler_route(self):
        ccache = shutil.which('ccache')
        if not ccache:
            self.skipTest('ccache is not installed')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache, remote, local = (root / name for name in ('cache', 'remote', 'local'))
            for path in (cache, remote, local):
                path.mkdir()
            (cache / 'cc').symlink_to(ccache)
            for path, target in ((remote, 'distcc'), (local, 'real-cc')):
                (path / target).write_text('#!/bin/sh\nexit 0\n')
                (path / target).chmod(0o755)
                (path / 'cc').symlink_to(target)
            config = root / 'ccache.conf'
            config.touch()
            cases = (
                ('', '', 12),
                ('path = ' + str(local), '', 3),
                ('compiler = ' + str(local / 'real-cc'), '', 3),
                ('compiler = ' + str(remote / 'distcc'), '', 12),
                ('path = ' + str(local) + '\nprefix_command = ' + str(remote / 'distcc'), '', 12),
                ('', 'export CCACHE_PATH=' + shlex.quote(str(local)), 3),
                ('', 'export CCACHE_COMPILER=' + shlex.quote(str(local / 'real-cc')), 3),
                ('path = ' + str(local), 'export CCACHE_PREFIX=' + shlex.quote(str(remote / 'distcc')), 12),
            )
            for settings, environment, jobs in cases:
                with self.subTest(settings=settings, environment=environment):
                    config.write_text(settings + '\n')
                    self.assertEqual(self.run_hook('''
export CCACHE_CONFIGPATH=''' + shlex.quote(str(config)) + '''
export PATH=''' + shlex.quote(f'{cache}:{remote}:{local}:') + '''"$PATH"
CC=''' + shlex.quote(str(cache / 'cc')) + '''
''' + environment + '''
source "$JOBS_HOOK"
echo "$MAKEOPTS $NINJAOPTS $CMAKE_BUILD_PARALLEL_LEVEL"
'''), [f'-j{jobs} -j{jobs} {jobs}'])


if __name__ == '__main__':
    unittest.main()
