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
VALIDATION_FILES = ['validate-installed.py', 'deploy.py']


def validation_snapshot(pids):
    """Read only the supplied processes and their descendants, without ptrace."""
    pending = list(pids)
    records = {}
    while pending:
        pid = pending.pop()
        if pid in records:
            continue
        process = Path('/proc') / str(pid)
        record = records[pid] = {}
        for name in ('comm', 'status', 'wchan', 'syscall', 'stack'):
            try:
                record[name] = (process/name).read_text().strip()
            except OSError as error:
                record[name] = {'error': str(error)}
        try:
            record['fds'] = {fd.name: os.readlink(fd) for fd in (process/'fd').iterdir()}
        except OSError as error:
            record['fds_error'] = str(error)
        try:
            children = set()
            for task in (process/'task').iterdir():
                children.update(map(int, (task/'children').read_text().split()))
            record['children'] = sorted(children)
            pending.extend(children)
        except OSError as error:
            record['children_error'] = str(error)
    return records


def run_observed(args, *, env, timeout, observe=(), input=None, cwd=None,
                 text: bool = False, check=False) -> subprocess.CompletedProcess:
    """Keep live process evidence without changing validation failure semantics."""
    with subprocess.Popen(args, env=env, cwd=cwd, text=text,
                          stdin=subprocess.PIPE if input is not None else None,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        try:
            stdout, stderr = process.communicate(input=input, timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                print('Validation timeout live processes: ' +
                      json.dumps(validation_snapshot([process.pid, *observe])), file=sys.stderr, flush=True)
            finally:
                process.kill()
                process.communicate()
            raise
        except BaseException:
            process.kill()
            process.wait()
            raise
        result = subprocess.CompletedProcess(args, process.returncode, stdout, stderr)
        if check:
            result.check_returncode()
        return result


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
    inputs = ['src', 'include_server', 'configure.ac', 'Makefile.in', 'pump.in',
              'distcc-link', 'distcc-multi', 'distcc-clang', 'distcc-rust',
              'distcc-thinlto', 'distcc-offload', 'distcc-offload-link',
              'distcc_lto.py', 'distcc_job.py', 'distcc_python.py',
              'packaging/systemd', 'packaging/gentoo/*.bashrc']
    # Do not silently deploy an older implementation over pending changes.
    for tree, paths in ((ROOT, inputs), (Path(overlay), ['sys-devel/distcc'])):
        if run(['git', '-C', tree, 'status', '--porcelain', '--', *paths]):
            raise RuntimeError(f'uncommitted deployment inputs: {tree}')
    run(['git', '-C', ROOT, 'diff', '--exit-code', match[1], '--', *inputs])
    return version, ebuild


def transfer(paths, destination, target=None, *, mirror=False):
    run(['sudo', '-n', 'mkdir', '-p', destination], target)
    dest = f"{target['ssh']}:{destination}/" if target and target.get('ssh') else destination + '/'
    args = ['rsync', '-rt', '--rsync-path=sudo -n rsync']
    if not target or not target.get('ssh'):
        args = ['sudo', '-n', 'rsync', '-rt']
    if mirror:
        args.append('--delete-delay')
    run([*args, *paths, dest])


def parallel(targets, action):
    if not targets:
        return
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


def rollout(targets, prepare, action):
    # Build every package while the installed client/server protocol is still
    # consistent. Rolling activation must never supply workers to old clients
    # that are building the remaining upgrade packages.
    parallel(targets, prepare)
    # The first target must pass installed-path validation before any other
    # target is upgraded. A failed canary leaves the remaining workers intact.
    action(targets[0])
    parallel(targets[1:], action)


def builds_to_pause(target):
    names = set()
    for pattern in target.get('quiesce_units', []):
        active = run(['systemctl', 'list-units', '--type=service', '--state=active',
                      '--plain', '--no-legend', pattern], target)
        names.update(line.split()[0] for line in active.splitlines())
    for name in sorted(names):
        if run(['systemctl', 'show', name, '-p', 'Transient', '--value'], target) == 'yes':
            raise RuntimeError('Cannot resume transient build unit after stopping: ' + name +
                               '; provide a persistent service definition first')
    return sorted(names)


def distribute_sources(settings, version, ebuild, targets, work):
    import portage
    transfer([str(ebuild.parent) + '/'], settings['local_repository'] + '/sys-devel/distcc', mirror=True)
    fetcher = dict(targets[0], check_limits=targets[0].get('build_limits', []))
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
    limits = target.get('build_limits' if stage == 'build' else 'check_limits', [])
    unit = 'distcc-deploy-' + stage + '-' + target['name']
    # Deterministic unit names reject duplicate runs whose SSH connection was lost.
    properties = [*limits, 'IOAccounting=yes', 'RuntimeMaxSec=2700' if stage == 'build' else 'RuntimeMaxSec=240']
    if stage == 'build' and not cross:
        properties += target.get('mounts', [])
    cmd = ['sudo', '-n', 'systemd-run', '--wait', '--pipe', '--collect', '--unit=' + unit]
    for prop in properties:
        cmd += ['-p', prop]
    run([*cmd, *args], target if not cross else None, log=log)


def prepare(target, version, ebuild, work, cross):
    name = target['name']
    print(f'{name}: preparing {version}', flush=True)
    with (work / (name + '-build.log')).open('w') as log:
        transfer([str(ebuild.parent) + '/'], target['repository'] + '/sys-devel/distcc', target, mirror=True)
        if run(['portageq', 'best_version', '/', 'sys-devel/distcc'], target) == 'sys-devel/' + version:
            print(f'{name}: release already installed', flush=True)
            return
        if target.get('binary'):
            packages = work / 'binpkgs' / name
            packages.mkdir(parents=True)
            env = ['ROOT=' + cross['root'], 'SYSROOT=' + cross['root'],
                   'PORTAGE_CONFIGROOT=' + cross['root'], 'PKGDIR=' + str(packages)]
            args = ['env', *env, 'emerge', '--buildpkgonly',
                    '--oneshot', '--nodeps', '--usepkg=n', '--quiet-build=y', '=sys-devel/' + version]
            builder = dict(target, build_limits=cross.get('limits', []))
            bounded(args, builder, 'build', log, cross=True)
            remote_packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
            transfer([str(packages) + '/'], remote_packages, target)
        else:
            packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
            args = ['env', 'PKGDIR=' + packages, 'emerge', '--buildpkgonly', '--oneshot', '--nodeps',
                    '--usepkg=n', '--quiet-build=y', '=sys-devel/' + version]
            bounded(args, target, 'build', log)
    print(f'{name}: package ready', flush=True)


def install(target, version, work):
    name = target['name']
    if run(['portageq', 'best_version', '/', 'sys-devel/distcc'], target) == 'sys-devel/' + version:
        return
    print(f'{name}: installing {version}', flush=True)
    packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
    with (work / (name + '-install.log')).open('w') as log:
        args = ['env', 'PKGDIR=' + packages, 'emerge', '--oneshot', '--nodeps',
                '--usepkgonly', '--quiet-build=y', '=sys-devel/' + version]
        bounded(args, target, 'install', log)
    installed = run(['portageq', 'best_version', '/', 'sys-devel/distcc'], target)
    if installed != 'sys-devel/' + version:
        raise RuntimeError(f'{name}: installed version differs: {installed}')
    print(f'{name}: installed', flush=True)



def resolve_compilers(target):
    # Resolve before sudo changes PATH; preserve names such as clang++ whose
    # driver mode depends on argv[0]. Explicit toolchain paths remain supported.
    return json.loads(run(['python3', '-c', '''import json, shutil, sys
paths = []
for name in sys.argv[1:]:
    path = shutil.which(name)
    if not path:
        raise SystemExit('Configured compiler is not executable: ' + name)
    paths.append(path)
print(json.dumps(paths))
''', *target.get('compiler_paths', [])], target))


def configure(target, work):
    target['compiler_paths'] = resolve_compilers(target)
    script = '''from pathlib import Path
import os
import subprocess
if COMPILERS:
    for compiler in COMPILERS:
        if not Path(compiler).is_absolute() or '\\n' in compiler or not os.access(compiler, os.X_OK):
            raise RuntimeError('Configured compiler is not executable: ' + compiler)
    commands = Path('/etc/distcc/compilers')
    existing = commands.read_text().splitlines() if commands.exists() else []
    names = {Path(compiler).name for compiler in COMPILERS}
    # Retire vanished tools and replace earlier bindings of the selected names.
    existing = [compiler for compiler in existing
                if Path(compiler).name not in names and os.access(compiler, os.X_OK)]
    commands.write_text('\\n'.join(dict.fromkeys(existing + COMPILERS)) + '\\n')
p=Path('/etc/systemd/system/distccd.service.d')
p.mkdir(parents=True,exist_ok=True)
# Restarts replace workers, not the boot-scoped immutable input cache. A real
# stop still releases the runtime directory and its RAM through systemd.
(p/'cache-lifetime.conf').write_text('[Service]\\nRuntimeDirectoryPreserve=restart\\n')
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
    transfer([str(ROOT / 'packaging/gentoo'/name) for name in VALIDATION_FILES], remote, target)
    args = ['env', 'TMPDIR=' + target['temporary'],
            'VALIDATE_DISTCC_HOSTS=' + target['validation_hosts'],
            'VALIDATE_DISTCC_COMPILERS=' + json.dumps(target.get('compiler_paths', [])),
            'python3', '-B', remote + '/validate-installed.py']
    with (work / (target['name'] + '-validation.log')).open('w') as log:
        for hosts in dict.fromkeys([target['server_hosts'], target['validation_hosts']]):
            args[2] = 'VALIDATE_DISTCC_HOSTS=' + hosts
            bounded(args, target, 'check', log)
    run(['sudo', '-n', 'rm', '--', *[remote+'/'+name for name in VALIDATION_FILES]], target)
    run(['sudo', '-n', 'rmdir', remote], target)
    print(f"{target['name']}: C/C++ and Rust remote compilation and execution passed", flush=True)


def validate_rust_cluster(targets, work):
    def path(target):
        return target['temporary'] + '/distcc-rust-cluster-check'

    try:
        parallel(targets, lambda target: transfer(
            [str(ROOT / 'packaging/gentoo'/name) for name in VALIDATION_FILES], path(target), target))
        # Exercise every client against each worker concurrently. Self tests
        # alone miss cross-architecture, access-control and input-budget errors.
        for worker in targets:
            def validate_client(client):
                name = client['name'] + '-to-' + worker['name']
                with (work / ('rust-' + name + '.log')).open('w') as log:
                    bounded(['env', 'TMPDIR=' + client['temporary'],
                             'VALIDATE_DISTCC_HOSTS=' + worker['server_hosts'],
                             'python3', '-B', path(client) + '/validate-installed.py', '--rust-only'],
                            client, 'rust-' + worker['name'], log)
                print(f'Rust {name}: distributed compilation and execution passed', flush=True)
            parallel(targets, validate_client)
    finally:
        parallel(targets, lambda target: run(['sudo', '-n', 'rm', '-rf', '--', path(target)], target))


def distribute_rust_compilers(targets, work):
    """Keep selected native toolchains available to peers without changing PATH."""
    def inspect(target):
        info = json.loads(run(['python3', '-c', '''import hashlib, json, pathlib, shutil, subprocess
compiler = pathlib.Path(shutil.which('rustc')).resolve()
root = compiler.parent.parent
version = subprocess.check_output([compiler, '-vV'], text=True)
host = dict(line.split(': ', 1) for line in version.splitlines()[1:] if ': ' in line)['host']
digest = hashlib.sha256()
files = [compiler, *sorted((root / 'lib').glob('librustc_driver*')),
         *sorted((root / 'lib/rustlib' / host / 'codegen-backends').glob('*'))]
for path in files:
    if path.is_file():
        digest.update(path.name.encode() + b'\\0')
        with path.open('rb') as stream:
            digest.update(hashlib.file_digest(stream, 'sha256').digest())
print(json.dumps(dict(root=str(root), host=host, digest=digest.hexdigest())))
'''], target))
        return target, info
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(targets) or 1) as pool:
        inventory = list(pool.map(inspect, targets))
    for identity in dict.fromkeys(info['digest'] for _, info in inventory):
        source, info = next(item for item in inventory if item[1]['digest'] == identity)
        peers = [target for target, other in inventory
                 if other['host'] == info['host'] and other['digest'] != identity]
        if not peers:
            continue
        destination = '/opt/rust-distcc-' + identity
        def needs_install(target):
            return not json.loads(run(['python3', '-c', '''import json, pathlib, subprocess, sys
compiler = pathlib.Path(sys.argv[1]) / 'bin/rustc'
try:
    ready = compiler.is_file() and subprocess.run([compiler, '-vV'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
except OSError:
    ready = False
print(json.dumps(ready))
''', destination], target))
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(peers)) as pool:
            peers = [target for target, missing in zip(peers, pool.map(needs_install, peers)) if missing]
        if not peers:
            continue
        stage = work / ('rust-' + identity)
        stage.mkdir()
        origin = info['root'] + '/'
        if source.get('ssh'):
            origin = source['ssh'] + ':' + origin
        # Preserve relative runtime-library links. The compiler and its driver
        # come from the administrator's installed toolchain, never a job upload.
        run(['rsync', '-rlpt', '--exclude=/share', origin, str(stage) + '/'])
        def install_peer(target):
            run(['sudo', '-n', 'mkdir', '-p', destination], target)
            remote = (target['ssh'] + ':' if target.get('ssh') else '') + destination + '/'
            prefix = ['rsync', '--rsync-path=sudo -n rsync'] if target.get('ssh') else ['sudo', '-n', 'rsync']
            run([*prefix, '-rlpt', str(stage) + '/', remote])
            run([destination + '/bin/rustc', '-vV'], target)
        parallel(peers, install_peer)
        shutil.rmtree(stage)
    print('Rust: selected native compilers available on matching worker architectures', flush=True)


def validate_thinlto_cluster(targets, work):
    hosts = ' '.join(target['server_hosts'] for target in targets)
    def validate_client(client):
        directory = client['temporary'] + '/distcc-thinlto-cluster-check'
        script = directory + '/validate-installed.py'
        try:
            transfer([str(ROOT / 'packaging/gentoo'/name) for name in VALIDATION_FILES], directory, client)
            with (work / ('thinlto-' + client['name'] + '.log')).open('w') as log:
                bounded(['env', 'TMPDIR=' + client['temporary'], 'VALIDATE_DISTCC_HOSTS=' + hosts,
                         'python3', '-B', script, '--thinlto-only'], client, 'thinlto', log)
            print(f"ThinLTO {client['name']}: multiple workers and executable results passed", flush=True)
        finally:
            run(['sudo', '-n', 'rm', '-f', '--', *[directory+'/'+name for name in VALIDATION_FILES]], client)
            run(['sudo', '-n', 'rmdir', '--', directory], client)
    parallel(targets, validate_client)


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
        work = Path(tempfile.mkdtemp(prefix='distcc-deploy-', dir=settings['temporary']))
        print(f'{version}: logs in {work}', flush=True)
        resume_path = config.with_suffix('.resume.json')
        resume = json.loads(resume_path.read_text()) if resume_path.exists() else {}
        try:
            # Check all access before modifying any installed host.
            for target in targets:
                run(['sudo', '-n', 'true'], target)
            paused = {target['name']: builds_to_pause(target) for target in targets}
            distribute_sources(settings, version, ebuild, targets, work)
            distribute_rust_compilers(targets, work)
            for target in targets:
                names = paused[target['name']]
                if names:
                    resume[target['name']] = sorted(set(resume.get(target['name'], []) + names))
                    resume_path.write_text(json.dumps(resume))
                    run(['sudo', '-n', 'systemctl', 'stop', *names], target)
            def deploy_target(target):
                install(target, version, work)
                configure(target, work)
                validate(target, work)
            rollout(targets, lambda target: prepare(target, version, ebuild, work, settings['cross']),
                    deploy_target)
            validate_rust_cluster(targets, work)
            validate_thinlto_cluster(targets, work)
            for target in targets:
                if target['name'] in resume:
                    run(['sudo', '-n', 'systemctl', 'start', *resume[target['name']]], target)
            resume_path.unlink(missing_ok=True)
            for target in targets:
                run(['sudo', '-n', 'sh', '-c',
                     'if test -d "$1"; then rm -r -- "$1"; fi', 'sh',
                     target['temporary'] + '/distcc-deploy-binpkgs/' + version], target)
            # Cross builds create root-owned package subdirectories. Failure to
            # remove them must not be hidden before moving logs across devices.
            if (work / 'binpkgs').exists():
                run(['sudo', '-n', 'rm', '-r', '--', work / 'binpkgs'])
            (work / 'result.json').write_text(json.dumps({'version': version, 'success': True,
                                                        'rust_pairs': len(targets) ** 2,
                                                        'thinlto_clients': len(targets),
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
