#!/usr/bin/env python3
"""Deploy the latest published overlay revision to the configured Gentoo fleet."""
import concurrent.futures
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def command(args, target=None):
    if target and target.get('ssh'):
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                target['ssh'], 'exec /bin/sh -c ' + shlex.quote(shlex.join(map(str, args)))]
    return list(map(str, args))


def run(args, target=None, *, log=None, input=None):
    result = subprocess.run(command(args, target), input=input, text=True,
                            stdout=log or subprocess.PIPE,
                            stderr=subprocess.STDOUT if log else subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}' +
                           (f'; see {log.name}' if log else ': ' + result.stderr.strip()))
    return result.stdout.strip() if result.stdout is not None else ''


def release(overlay):
    from portage.versions import best
    directory = Path(overlay) / 'sys-devel/distcc'
    version = best([p.stem for p in directory.glob('distcc-*.ebuild')])
    if not version:
        raise RuntimeError('overlay has no distcc release')
    ebuild = directory / (version + '.ebuild')
    match = re.search(r'^DISTCC_COMMIT="([0-9a-f]{40})"$', ebuild.read_text(), re.M)
    if not match:
        raise RuntimeError('release must pin a full source commit')
    # Do not silently deploy an older implementation over pending changes.
    for tree in (ROOT, Path(overlay)):
        if run(['git', '-C', tree, 'status', '--porcelain', '--',
                '.' if tree == ROOT else 'sys-devel/distcc']):
            raise RuntimeError(f'uncommitted deployment inputs: {tree}')
    run(['git', '-C', ROOT, 'diff', '--exit-code', match[1], '--', 'src', 'include_server',
         'configure.ac', 'Makefile.in', 'pump.in', 'distcc-link', 'distcc-clang',
         'packaging/systemd', 'packaging/gentoo/*.bashrc'])
    return version, ebuild


def transfer(paths, destination, target=None):
    run(['sudo', '-n', 'mkdir', '-p', destination], target)
    dest = f"{target['ssh']}:{destination}/" if target and target.get('ssh') else destination + '/'
    args = ['rsync', '-rt', '--rsync-path=sudo -n rsync']
    if not target or not target.get('ssh'):
        args = ['sudo', '-n', 'rsync', '-rt']
    run([*args, *paths, dest])


def parallel(targets, action):
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(targets)) as pool:
        pending = {pool.submit(action, target): target['name'] for target in targets}
        for future in concurrent.futures.as_completed(pending):
            try:
                future.result()
            except Exception as error:
                message = pending[future] + ': ' + str(error)
                print(message, file=sys.stderr, flush=True)
                errors.append(message)
    if errors:
        raise RuntimeError('\n'.join(errors))


def distribute_sources(settings, version, ebuild, targets, work):
    import portage
    transfer([str(ebuild.parent) + '/'], settings['local_repository'] + '/sys-devel/distcc')
    fetcher = dict(targets[0], check_limits=targets[0]['build_limits'])
    with (work / 'fetch.log').open('w') as log:
        bounded(['ebuild', str(ebuild), 'fetch'], fetcher, 'fetch', log, cross=True)
    directory = Path(run(['portageq', 'envvar', 'DISTDIR']))
    # Portage initializes these public attributes lazily at module access.
    database = getattr(portage, 'db')[getattr(portage, 'root')]['porttree'].dbapi
    sources = [str(directory / name) for name in database.getFetchMap('sys-devel/' + version)]
    def distribute(target):
        if not target.get('binary'):
            directory = target.get('distfiles') or run(['portageq', 'envvar', 'DISTDIR'], target)
            transfer(sources, directory, target)
    parallel(targets, distribute)
    if any(t.get('binary') for t in targets):
        root = settings['cross']['root']
        directory = run(['env', 'ROOT=' + root, 'SYSROOT=' + root, 'PORTAGE_CONFIGROOT=' + root,
                         'portageq', 'envvar', 'DISTDIR'])
        transfer(sources, directory)


def bounded(args, target, stage, log, *, cross=False):
    limits = target['build_limits'] if stage == 'build' else target['check_limits']
    unit = 'distcc-deploy-' + stage + '-' + target['name']
    # Deterministic unit names reject duplicate runs whose SSH connection was lost.
    properties = [*limits, 'IOAccounting=yes', 'RuntimeMaxSec=2700' if stage == 'build' else 'RuntimeMaxSec=240']
    if stage == 'build' and not cross:
        properties += target.get('mounts', [])
    cmd = ['sudo', '-n', 'systemd-run', '--wait', '--pipe', '--collect', '--unit=' + unit]
    for prop in properties:
        cmd += ['-p', prop]
    if any(p.startswith('MemoryMax=') for p in limits):
        guard = ('from pathlib import Path; import os,sys; '
                 'p=next(s[3:] for s in Path("/proc/self/cgroup").read_text().splitlines() if s.startswith("0::/")); '
                 'g=Path("/sys/fs/cgroup")/p.lstrip("/"); '
                 'assert (g/"memory.max").read_text().strip() not in ("max","0"); '
                 'assert (g/"memory.swap.max").read_text().strip()=="0"; '
                 'os.execvp(sys.argv[1],sys.argv[1:])')
        args = ['python3', '-c', guard, *args]
    run([*cmd, *args], target if not cross else None, log=log)


def install(target, version, ebuild, work, cross):
    name = target['name']
    print(f'{name}: building/installing {version}', flush=True)
    with (work / (name + '-install.log')).open('w') as log:
        transfer([str(ebuild.parent) + '/'], target['repository'] + '/sys-devel/distcc', target)
        if run(['portageq', 'best_version', '/', 'sys-devel/distcc'], target) == 'sys-devel/' + version:
            print(f'{name}: release already installed', flush=True)
            return
        if target.get('binary'):
            packages = work / 'binpkgs'
            packages.mkdir()
            env = ['ROOT=' + cross['root'], 'SYSROOT=' + cross['root'],
                   'PORTAGE_CONFIGROOT=' + cross['root'], 'PKGDIR=' + str(packages)]
            args = ['env', *env, 'DISTCC_PUMP=0',
                    'MAKEOPTS=-j' + str(cross['jobs']), *emerge_command('distcc buildpkg -ccache'), '--buildpkgonly',
                    '--oneshot', '--nodeps', '--usepkg=n', '--quiet-build=y', '=sys-devel/' + version]
            builder = dict(target, build_limits=cross['limits'])
            bounded(args, builder, 'build', log, cross=True)
            remote_packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
            transfer([str(packages) + '/'], remote_packages, target)
            args = ['env', 'PKGDIR=' + remote_packages, 'emerge', '--oneshot', '--nodeps',
                    '--usepkgonly', '--quiet-build=y', '=sys-devel/' + version]
            bounded(args, target, 'install', log)
        else:
            args = ['env', 'DISTCC_PUMP=0',
                    'MAKEOPTS=-j' + str(target['jobs']), *emerge_command('distcc'), '--oneshot', '--nodeps',
                    '--usepkg=n', '--quiet-build=y', '=sys-devel/' + version]
            bounded(args, target, 'build', log)
    installed = run(['portageq', 'best_version', '/', 'sys-devel/distcc'], target)
    if installed != 'sys-devel/' + version:
        raise RuntimeError(f'{name}: installed version differs: {installed}')
    print(f'{name}: installed', flush=True)


def emerge_command(features):
    # Query inside the build's namespace and cross-root environment. Preserve
    # the target's FEATURES instead of replacing sandbox and packaging policy.
    return ['python3', '-c',
            'import os,subprocess,sys; '
            'env=dict(os.environ); '
            'env["FEATURES"]=subprocess.check_output(["portageq","envvar","FEATURES"],text=True).strip()+" "+sys.argv[1]; '
            'os.execvpe("emerge",["emerge",*sys.argv[2:]],env)', features]


def configure(target, work):
    script = '''from pathlib import Path
import os
import subprocess
if COMPILERS:
    for compiler in COMPILERS:
        if not Path(compiler).is_absolute() or '\\n' in compiler or not os.access(compiler, os.X_OK):
            raise RuntimeError('Configured compiler is not executable: ' + compiler)
    commands = Path('/etc/distcc/compilers')
    existing = commands.read_text().splitlines() if commands.exists() else []
    commands.write_text('\\n'.join(dict.fromkeys(existing + COMPILERS)) + '\\n')
p=Path('/etc/systemd/system/distccd.service.d')
p.mkdir(parents=True,exist_ok=True)
if AUTO:
    # Apply the finite bound before removing the old setting on a live service.
    # Stopped services get their cgroup and bound through ExecStartPre instead.
    if subprocess.run(['systemctl','is-active','--quiet','distccd.service']).returncode == 0:
        subprocess.run(['python3','/usr/lib/distcc/distccd-memory.py','--apply','distccd.service'],check=True)
    old=p/'adaptive.conf'
    if old.exists():
        old.write_text(''.join(s for s in old.read_text().splitlines(keepends=True) if not s.startswith('MemoryMax=')))
    (p/'auto-memory.conf').write_text(Path('/usr/share/distcc/distccd-memory.conf').read_text())
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','restart','distccd.service'],check=True)
brokers=subprocess.check_output(['systemctl','list-units','--type=service','--state=active',
    '--plain','--no-legend','distcc-broker@*.service'],text=True)
for line in brokers.splitlines():
    subprocess.run(['systemctl','restart',line.split()[0]],check=True)
'''
    with (work / (target['name'] + '-services.log')).open('w') as log:
        run(['sudo', '-n', 'python3', '-'], target, log=log,
            input='AUTO=' + repr(target.get('auto_memory', False)) + '\nCOMPILERS=' +
                  repr(target.get('compiler_paths', [])) + '\n' + script)


def validate(target, work):
    remote = target['temporary'] + '/distcc-deploy-check'
    transfer([str(ROOT / 'packaging/gentoo/validate-installed.py')], remote, target)
    args = ['env', 'TMPDIR=' + target['temporary'],
            'VALIDATE_DISTCC_HOSTS=' + target['validation_hosts'],
            'VALIDATE_DISTCC_COMPILERS=' + json.dumps(target.get('compiler_paths', [])),
            'python3', remote + '/validate-installed.py']
    if target.get('assembly_comparison'):
        args += ['--assembly']
    with (work / (target['name'] + '-validation.log')).open('w') as log:
        for hosts in dict.fromkeys([target['server_hosts'], target['validation_hosts']]):
            args[2] = 'VALIDATE_DISTCC_HOSTS=' + hosts
            bounded(args, target, 'check', log)
    run(['sudo', '-n', 'rm', '-f', remote + '/validate-installed.py'], target)
    run(['sudo', '-n', 'rmdir', remote], target)
    print(f"{target['name']}: C/C++ remote compilation and execution passed", flush=True)


def main():
    if len(sys.argv) != 1:
        raise SystemExit('deploy takes no arguments; configure ~/.config/distcc/deploy.json')
    config = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'distcc/deploy.json'
    settings = json.loads(config.read_text())
    with config.with_suffix('.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        version, ebuild = release(settings['overlay'])
        targets = settings['targets']
        if not targets or len({t['name'] for t in targets}) != len(targets):
            raise RuntimeError('target names must be unique')
        for target in targets:
            if not re.fullmatch(r'[a-z0-9_-]+', target['name']):
                raise RuntimeError('target names must be safe systemd unit suffixes')
            if not target.get('server_hosts'):
                raise RuntimeError('server_hosts must select the target worker for inbound validation')
            limits = settings['cross']['limits'] if target.get('binary') else target['build_limits']
            if 'MemorySwapMax=0' not in limits or not any(
                    p.startswith('MemoryMax=') and p.split('=', 1)[1] not in ('0', 'infinity') for p in limits):
                raise RuntimeError('every source build needs a finite memory limit with swap disabled')
        work = Path(tempfile.mkdtemp(prefix='distcc-deploy-', dir=settings['temporary']))
        print(f'{version}: logs in {work}', flush=True)
        resume_path = config.with_suffix('.resume.json')
        resume = json.loads(resume_path.read_text()) if resume_path.exists() else {}
        try:
            # Check all access before modifying any installed host.
            for target in targets:
                run(['sudo', '-n', 'true'], target)
            distribute_sources(settings, version, ebuild, targets, work)
            for target in targets:
                for pattern in target.get('quiesce_units', []):
                    units = run(['systemctl', 'list-units', '--type=service', '--state=active',
                                 '--plain', '--no-legend', pattern], target)
                    names = [line.split()[0] for line in units.splitlines()]
                    if names:
                        resume[target['name']] = sorted(set(resume.get(target['name'], []) + names))
                        resume_path.write_text(json.dumps(resume))
                        run(['sudo', '-n', 'systemctl', 'stop', *names], target)
            parallel(targets, lambda target: install(target, version, ebuild, work, settings['cross']))
            for target in targets:
                configure(target, work)
            parallel(targets, lambda target: validate(target, work))
            for target in targets:
                if target['name'] in resume:
                    run(['sudo', '-n', 'systemctl', 'start', *resume[target['name']]], target)
            resume_path.unlink(missing_ok=True)
            for target in targets:
                if target.get('binary'):
                    run(['sudo', '-n', 'rm', '-rf', target['temporary'] + '/distcc-deploy-binpkgs/' + version], target)
            # Cross builds create root-owned package subdirectories. Failure to
            # remove them must not be hidden before moving logs across devices.
            run(['sudo', '-n', 'rm', '-rf', '--', work / 'binpkgs'])
            (work / 'result.json').write_text(json.dumps({'version': version, 'success': True,
                                                        'targets': [t['name'] for t in targets]}))
            results = Path(settings['results'])
            results.mkdir(parents=True, exist_ok=True)
            shutil.move(str(work), results / work.name)
            print(f'{version}: deployment and validation completed; interrupted builds resumed. Logs: {results / work.name}', flush=True)
        except BaseException:
            print(f'Deployment incomplete. Logs: {work}. Rerun ./deploy after correcting the failure.', file=sys.stderr)
            raise


if __name__ == '__main__':
    main()
