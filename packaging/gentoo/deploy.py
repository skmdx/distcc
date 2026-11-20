#!/usr/bin/env python3
"""Deploy the latest published overlay revision to the configured Gentoo fleet."""
import concurrent.futures
import fcntl
import http.client
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit
import xxhash


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
              'distcc-link', 'distcc-multi', 'distcc-clang',
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


def build_cache_environment(target, context):
    """Enable an installed build-host ccache without replacing Portage policy."""
    settings = json.loads(run(['env', *context, 'python3', '-c', '''import json, shutil, subprocess
if shutil.which('ccache'):
    keys = ['FEATURES', 'PORTAGE_TMPDIR']
    values = subprocess.check_output(['portageq', 'envvar', *keys], text=True).splitlines()
    print(json.dumps(dict(zip(keys, values, strict=True))))
else:
    print('null')
'''], target))
    if settings is None:
        return []
    features = settings['FEATURES'].split()
    if 'ccache' not in features:
        features.append('ccache')
    return ['FEATURES=' + ' '.join(features),
            'CCACHE_BASEDIR=' + settings['PORTAGE_TMPDIR'], 'CCACHE_NOHASHDIR=1']


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
            env = env + build_cache_environment(None, env)
            args = ['env', *env, 'emerge', '--buildpkgonly',
                    '--oneshot', '--nodeps', '--usepkg=n', '--quiet-build=y', '=sys-devel/' + version]
            builder = dict(target, build_limits=cross.get('limits', []))
            bounded(args, builder, 'build', log, cross=True)
            remote_packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
            transfer([str(packages) + '/'], remote_packages, target)
        else:
            packages = target['temporary'] + '/distcc-deploy-binpkgs/' + version
            env = ['PKGDIR=' + packages]
            env = env + build_cache_environment(target, env)
            args = ['env', *env, 'emerge', '--buildpkgonly', '--oneshot', '--nodeps',
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
            args[2] = 'VALIDATE_DISTCC_HOSTS=' + ' '.join(with_stats(h) for h in hosts.split())
            bounded(args, target, 'check', log)
    run(['sudo', '-n', 'rm', '--', *[remote+'/'+name for name in VALIDATION_FILES]], target)
    run(['sudo', '-n', 'rmdir', remote], target)
    print(f"{target['name']}: C/C++ and Rust remote compilation and execution passed", flush=True)


def llvm_abi(version):
    match = re.match(r'^(\d+\.\d+)', version)
    if not match:
        raise ValueError('missing LLVM ABI in ' + version)
    return match[1]


def rust_cgu_requirement(client, compiler):
    abi = llvm_abi(compiler['identity']['llvm'])
    library = next((lib['path'] for lib in compiler['runtime']
                    if lib['name'].partition('+')[0] == 'libLLVM.so.' + abi), None)
    if not library:
        return None
    target = run(['sh', '-ec', '''
directory=$(dirname "$(realpath -e -- "$1")")/distcc
test -f "$directory/distcc-rust-cgu.so" && test -x "$directory/distcc-rust-cgu" || exit 0
"$directory/distcc-rust-cgu" --target-name "$2" "$3" || { rc=$?; test "$rc" = 121 || exit "$rc"; }
''', 'sh', library, abi, compiler['host']], client)
    return (abi, target) if target else None


def with_stats(hostspec):
    options = hostspec.split(',')[1:]
    if 'tcp' in options and not any(option.startswith('stats=') for option in options):
        return hostspec + ',stats=3633'
    return hostspec


def rust_compiler_key(slot):
    identity = slot['identity']
    report = '\n'.join((identity['version'], identity['commit'], llvm_abi(identity['llvm']), slot['host']))
    return xxhash.xxh3_64_hexdigest(report.encode())


def rust_worker_capabilities(hostspec):
    hostspec = with_stats(hostspec)
    endpoint, *options = hostspec.split(',')
    address = urlsplit('//' + endpoint).hostname
    assert address and 'tcp' in options, hostspec
    port = next(int(s[6:]) for s in options if s.startswith('stats='))
    connection = http.client.HTTPConnection(address, port, timeout=10)
    try:
        connection.request('GET', '/')
        response = connection.getresponse()
        if response.status != 200:
            raise RuntimeError(f'{address}: worker statistics returned {response.status}')
        fields = dict(line.split(' ', 1) for line in response.read().decode().splitlines()
                      if line.startswith('dcc_'))
    finally:
        connection.close()
    backends = set()
    for record in fields['dcc_llvm_cgu'].split(';'):
        abi, *names = record.split()
        backends.update((abi, name) for name in names)
    compilers = fields['dcc_rust_compilers'].split()
    if compilers == ['-']:
        compilers = []
    if not all(re.fullmatch(r'[0-9a-f]{16}', key) for key in compilers):
        raise ValueError('invalid worker Rust compiler identities')
    return hostspec, set(compilers), backends


def validate_rust_cluster(targets, work, inventory, workers=None):
    def path(target):
        return target['temporary'] + '/distcc-rust-cluster-check'

    try:
        parallel(targets, lambda target: transfer(
            [str(ROOT / 'packaging/gentoo'/name) for name in VALIDATION_FILES], path(target), target))
        # The same rustc bytes on different clients can target different native ISAs.
        clients = {}
        for client, compilers in inventory:
            slots = {slot['digest']: slot for slot in compilers}
            clients[client['name']] = [(slot, rust_cgu_requirement(client, slot)) for slot in slots.values()]
        builds = sum(map(len, clients.values()))
        if not builds:
            raise RuntimeError('no executable Rust compiler for cluster validation')
        offers = [(worker['name'], *rust_worker_capabilities(worker['server_hosts']))
                  for worker in (targets if workers is None else workers)]

        def validate_slots(client):
            start = targets.index(client)
            for step in range(len(offers)):
                worker, hosts, compilers, backends = offers[(start + step) % len(offers)]
                for compiler, requirement in clients[client['name']]:
                    expected = ('remote' if rust_compiler_key(compiler) in compilers else
                                'cgu' if requirement in backends else 'local')
                    name = client['name'] + '-' + compiler['digest'][:12] + '-to-' + worker
                    with (work / ('rust-' + name + '.log')).open('w') as log:
                        bounded(['env', 'TMPDIR=' + client['temporary'],
                                 'VALIDATE_DISTCC_HOSTS=' + hosts,
                                 'VALIDATE_DISTCC_RUSTC=' + compiler['root'] + '/bin/rustc',
                                 'VALIDATE_DISTCC_RUST_ROUTE=' + expected,
                                 'python3', '-B', path(client) + '/validate-installed.py', '--rust-only'],
                                client, 'rust-' + worker, log)
                    print(f'Rust {name}: {expected} compilation and execution passed', flush=True)
        parallel(targets, validate_slots)
    finally:
        parallel(targets, lambda target: run(['sudo', '-n', 'rm', '-rf', '--', path(target)], target))
    return builds * len(offers)


def rust_slot(target, action, *arguments, privileged=False):
    command = (['sudo', '-n'] if privileged else []) + ['python3', '-', action, *arguments]
    return json.loads(run(command, target, input=(ROOT / 'packaging/gentoo/rust-slots.py').read_text()))


def rust_copies(targets, inventories):
    """Plan one compatible copy per missing compiler identity on each worker."""
    copies = {}
    for target, inventory in zip(targets, inventories):
        cpu = inventory['cpu']
        available = {rust_compiler_key(slot) for slot in inventory['slots']}
        for source, origin in zip(targets, inventories):
            required = origin['cpu']['required']
            if (source == target or not required or origin['cpu']['machine'] != cpu['machine'] or
                    not set(required).issubset(cpu['available'])):
                continue
            for slot in origin['slots']:
                identity = rust_compiler_key(slot)
                if identity in available:
                    continue
                available.add(identity)
                copies.setdefault(slot['digest'], (source, slot, []))[2].append(target)
    return copies


def distribute_rust_compilers(targets, work):
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(targets) or 1) as pool:
        inventories = list(pool.map(lambda t: rust_slot(t, 'inventory', t['temporary']), targets))
    if not any(inventory['slots'] for inventory in inventories):
        raise RuntimeError('no executable Rust compiler in deployment inventory')
    keep = {t['name']: [] for t in targets}
    for digest, (source, info, peers) in rust_copies(targets, inventories).items():
        for peer in peers:
            keep[peer['name']].append(digest)
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(peers)) as pool:
            ready = pool.map(lambda t: rust_slot(t, 'ready', digest, t['temporary']), peers)
            peers = [peer for peer, present in zip(peers, ready) if not present]
        if not peers:
            continue
        destination = '/opt/rust-distcc-' + digest
        stage = work / ('rust-' + digest)
        stage.mkdir()
        origin = info['root'] + '/'
        if source.get('ssh'):
            origin = source['ssh'] + ':' + origin
        # Preserve relative runtime-library links. The compiler and its driver
        # come from the administrator's installed toolchain, never a job upload.
        run(['rsync', '-rlpt', '--exclude=/share', origin, str(stage) + '/'])
        runtime = stage / 'distcc-runtime'
        runtime.mkdir()
        origins = [(source['ssh'] + ':' if source.get('ssh') else '') + lib['path']
                   for lib in info['runtime']]
        run(['rsync', '-Lpt', *origins, str(runtime) + '/'])
        for library in info['runtime']:
            if Path(library['path']).name != library['name']:
                (runtime / Path(library['path']).name).rename(runtime / library['name'])
        def install_peer(target):
            rust_slot(target, 'prepare', digest, privileged=True)
            remote = (target['ssh'] + ':' if target.get('ssh') else '') + destination + '/'
            prefix = ['rsync', '--rsync-path=sudo -n rsync'] if target.get('ssh') else ['sudo', '-n', 'rsync']
            run([*prefix, '-rlpt', '--checksum', '--delete', '--exclude=/.distcc-complete',
                 str(stage) + '/', remote])
            if not rust_slot(target, 'verify', digest, target['temporary']):
                keep[target['name']].remove(digest)
                print(f"Rust {target['name']}: {digest} failed compile/execute; retiring copy", flush=True)
                return
            run(['sudo', '-n', 'sync', '-f', destination], target)
            rust_slot(target, 'publish', digest, privileged=True)
        parallel(peers, install_peer)
        shutil.rmtree(stage)
    parallel(targets, lambda t: rust_slot(t, 'prune', *keep[t['name']], privileged=True))
    (work / 'rust-inventory.json').write_text(json.dumps(inventories, indent=2) + '\n')
    return [(target, inventory['slots']) for target, inventory in zip(targets, inventories)]


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
            for target in targets:
                names = paused[target['name']]
                if names:
                    resume[target['name']] = sorted(set(resume.get(target['name'], []) + names))
                    resume_path.write_text(json.dumps(resume))
                    run(['sudo', '-n', 'systemctl', 'stop', *names], target)
            rust_inventory = distribute_rust_compilers(targets, work)
            def deploy_target(target):
                install(target, version, work)
                configure(target, work)
                validate(target, work)
            rollout(targets, lambda target: prepare(target, version, ebuild, work, settings['cross']),
                    deploy_target)
            rust_pairs = validate_rust_cluster(targets, work, rust_inventory)
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
                                                        'rust_pairs': rust_pairs,
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
