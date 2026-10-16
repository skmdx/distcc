#!/usr/bin/env python3
"""Bounded Gentoo build measurement, invoked by fleet.py inside its service."""
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time


def execute(args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def cgroup():
    name = next(s[3:] for s in Path('/proc/self/cgroup').read_text().splitlines()
                if s.startswith('0::/'))
    return Path('/sys/fs/cgroup') / name.lstrip('/')


def metrics(group):
    names = ('cpu.stat', 'io.stat', 'memory.current', 'memory.peak', 'memory.events',
             'memory.max', 'memory.swap.max', 'cpu.pressure', 'memory.pressure', 'io.pressure')
    return {name: (group / name).read_text() if (group / name).exists() else None for name in names}


def snapshot():
    properties = dict(line.split('=', 1) for line in execute([
        'systemctl', 'show', 'distccd.service', '-p', 'ControlGroup', '-p', 'MainPID',
        '-p', 'ActiveState']).splitlines())
    if properties['ActiveState'] != 'active' or not properties['ControlGroup']:
        raise RuntimeError('distccd service is not active')
    data = metrics(Path('/sys/fs/cgroup') / properties['ControlGroup'].lstrip('/'))
    if not data['memory.max'] or not data['memory.swap.max'] or \
            data['memory.max'].strip() in ('max', '0') or data['memory.swap.max'].strip() != '0':
        raise RuntimeError('worker requires a finite cgroup memory ceiling and no swap')
    return dict(service=properties, metrics=data,
                cpu=list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9])),
                boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                compiler=execute(['gcc', '-dumpfullversion']),
                distcc=execute(['portageq', 'best_version', '/', 'sys-devel/distcc']))


def stats(worker):
    try:
        with socket.create_connection((worker['address'], worker.get('port', 3633)), timeout=2) as conn:
            conn.sendall(b'GET / HTTP/1.0\r\n\r\n')
            data = b''
            while b'</distccstats>' not in data:
                chunk = conn.recv(8192)
                if not chunk or len(data) + len(chunk) > 65536:
                    raise ValueError('incomplete or oversized statistics response')
                data += chunk
        row = dict(line.split(None, 1) for line in data.decode().splitlines() if line.startswith('dcc_'))
        for key in ('dcc_compile_ok', 'dcc_busy', 'dcc_queued'):
            int(row[key])
        return row
    except (OSError, ValueError, KeyError) as error:
        return {'error': str(error)}


def host_list(text):
    return ' '.join(line.split('#', 1)[0].strip() for line in text.splitlines()).strip()


def summarize_samples(samples, names):
    counters = ('dcc_compile_ok', 'dcc_compile_error', 'dcc_compile_timeout', 'dcc_cli_disconnect',
                'dcc_tcp_accept', 'dcc_tcp_ready_idle_ns', 'dcc_tcp_parallel_tasks',
                'dcc_tcp_input_prefetch_bytes', 'dcc_tcp_output_overlaps', 'dcc_local_input_bytes')
    result = {}
    for name in names:
        rows = [sample['hosts'][name] for sample in samples if 'error' not in sample['hosts'][name]]
        deltas = {key: int(rows[-1][key]) - int(rows[0][key])
                  for key in counters if rows and key in rows[0] and key in rows[-1]}
        reset = any(int(b[key]) < int(a[key]) for a, b in zip(rows, rows[1:])
                    for key in counters if key in a and key in b)
        result[name] = dict(counters=deltas, counter_reset=reset,
                            failed_samples=len(samples) - len(rows),
                            peak_busy=max((int(r['dcc_busy']) for r in rows), default=None),
                            peak_queued=max((int(r['dcc_queued']) for r in rows), default=None))
    return result


def log_summary(paths):
    counts = dict(analysis_warnings=0, io_timeouts=0, sandbox_denials=0, broker_waits=0)
    timings = []
    jobs = set()
    for path in paths:
        if not path.exists():
            continue
        with path.open(errors='replace') as stream:
            for line in stream:
                counts['analysis_warnings'] += 'include server gave up analyzing' in line
                counts['io_timeouts'] += 'ERROR: IO timeout' in line
                counts['sandbox_denials'] += 'ACCESS DENIED' in line
                counts['broker_waits'] += 'waiting for hosts' in line
                if 'job cost:' in line:
                    fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
                    timings.append({key: float(fields[key]) for key in ('prepare', 'queue', 'execute')})
                if 'ninja ' in line:
                    jobs.update(map(int, re.findall(r'(?:^|\s)-j\s*(\d+)', line)))
    phases = {key: dict(total_seconds=sum(t[key] for t in timings),
                        mean_seconds=sum(t[key] for t in timings) / len(timings))
              for key in ('prepare', 'queue', 'execute')} if timings else {}
    return dict(**counts, feedback_jobs=len(timings), phases=phases, ninja_jobs=sorted(jobs))


def fingerprint(tree):
    digest = hashlib.sha256()
    for path in sorted(tree.rglob('*')):
        if path.is_symlink() or path.is_file():
            digest.update(str(path.relative_to(tree)).encode() + b'\0')
            digest.update(os.readlink(path).encode() if path.is_symlink() else path.read_bytes())
    return digest.hexdigest()


def implementation():
    """Record code visible inside the build's mount namespace, before timing."""
    spec = importlib.util.find_spec('include_server')
    if spec is None or not spec.submodule_search_locations:
        raise RuntimeError('installed include server is missing')
    package = Path(next(iter(spec.submodule_search_locations)))
    paths = []
    for name in ('distcc', 'distccd', 'pump'):
        binary = shutil.which(name)
        if binary is None:
            raise RuntimeError(name + ' is missing')
        paths.append(Path(binary))
    paths += sorted(p for p in package.iterdir() if p.suffix in ('.py', '.so'))
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}


def copy_configuration(source, destination):
    subprocess.run(['cp', '-a', '--reflink=auto', source, destination], check=True)
    # Relative links into repositories must retain their original meaning after
    # relocation; links within the private configuration remain private.
    for path in source.rglob('*'):
        if path.is_symlink():
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(source):
                copy = destination / path.relative_to(source)
                copy.unlink()
                copy.symlink_to(resolved)


def stop_process(proc):
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def profile_valid(record, report):
    fatal = ('failed to write perf data', 'failed to process sample', 'Data corruption detected',
             "Couldn't decompress data", '\nError:')
    return bool(re.search(r'# Samples:\s+[1-9]', report)) and not any(
        text in record or text in report for text in fatal)


def finish_profile(config, out, descriptor):
    source = f'/proc/self/fd/{descriptor}'
    with (out / 'perf-report.txt').open('w') as log:
        result = subprocess.run([config['executable'], 'report', '--stdio', '--no-children',
            '--percent-limit', '1', '--sort', 'comm,dso,symbol', '-i', source],
            env=dict(os.environ, **config.get('environment', {})), pass_fds=(descriptor,),
            stdout=log, stderr=subprocess.STDOUT)
    valid = result.returncode == 0 and profile_valid((out / 'perf-record.log').read_text(errors='replace'),
                                                    (out / 'perf-report.txt').read_text(errors='replace'))
    # perf's in-stream compressor can silently produce corrupt data. Keep raw
    # samples in bounded memory, then compress the checked artifact for storage.
    with (out / 'perf.data.zst').open('wb') as archive:
        os.lseek(descriptor, 0, os.SEEK_SET)
        subprocess.run(['zstd', '-q', '-1', '-T1', '-c'], stdin=descriptor,
                       stdout=archive, check=True)
    subprocess.run(['zstd', '-q', '-t', out / 'perf.data.zst'], check=True)
    return valid


def measure(config, out, buildroot):
    group = cgroup()
    assert (group / 'memory.max').read_text().strip() not in ('max', '0')
    assert (group / 'memory.swap.max').read_text().strip() == '0'
    # Preserve Portage's short paths (pump uses Unix sockets under T), while all
    # temporary writes belong to the workspace directory mounted by fleet.py.
    assert os.path.samefile('/var/tmp', buildroot), 'private build mount is missing'
    buildtree = Path('/var/tmp/portage')
    out.mkdir()
    (out / 'implementation.json').write_text(json.dumps(implementation(), indent=2) + '\n')
    runtime = out / 'runtime'
    runtime.mkdir(mode=0o755)
    account = pwd.getpwnam('portage')
    os.chown(runtime, account.pw_uid, account.pw_gid)
    configroot = out / 'config'
    (configroot / 'etc').mkdir(parents=True)
    copy_configuration(Path('/etc/portage'), configroot / 'etc/portage')
    private = configroot / 'etc/portage'
    conditions: dict = dict(portage_config=fingerprint(private), hosts=Path('/etc/distcc/hosts').read_text(),
                      compiler=execute(['gcc', '-v'], stderr=subprocess.STDOUT),
                      ebuild=hashlib.sha256(Path(config['ebuild']).read_bytes()).hexdigest(),
                      package=config['package'], memory_max=(group / 'memory.max').read_text().strip(),
                      perf=config['perf'], sample_seconds=config['sample_seconds'])
    conditions['collector'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    conditions['repository'] = execute(['git', '-C', str(Path(config['ebuild']).parents[2]), 'rev-parse', 'HEAD'])
    hosts = host_list(conditions['hosts'])
    execute(['distcc', '--show-hosts'], env=dict(os.environ, DISTCC_HOSTS=hosts))
    mapping = private / 'package.env'
    if mapping.is_file():
        original = mapping.read_text()
        mapping.unlink()
        mapping.mkdir()
        (mapping / 'original').write_text(original)
    mapping.mkdir(exist_ok=True)
    (private / 'env').mkdir(exist_ok=True)
    if (mapping / 'zz-distcc-bench').exists() or (private / 'env/distcc-bench').exists():
        raise RuntimeError('reserved benchmark configuration name already exists')
    (mapping / 'zz-distcc-bench').write_text('=' + config['package'] + ' distcc-bench\n')
    socket_path = str(runtime / 'broker.sock')
    (private / 'env/distcc-bench').write_text(
        'FEATURES="${FEATURES} distcc keepwork keeptemp -ccache"\n'
        'export DISTCC_PUMP=1 CCACHE_DISABLE=1 DISTCC_FALLBACK=0 DISTCC_SKIP_LOCAL_RETRY=1 DISTCC_VERBOSE=1\n'
        'export DISTCC_HOSTS=' + shlex.quote(hosts) + '\n'
        'export DISTCC_BROKER=' + shlex.quote(socket_path) + '\n'
        'export DISTCC_LOG=' + shlex.quote(str(runtime / 'client.log')) + '\n'
        'PORTAGE_TMPDIR=/var/tmp\n')
    env = dict(os.environ, PORTAGE_CONFIGROOT=str(configroot),
               PORTAGE_TMPDIR='/var/tmp', CCACHE_DISABLE='1')
    workers = config['workers']
    names = [w['name'] for w in workers]
    samples: list[dict] = []
    done = threading.Event()
    broker = perf = build = None
    perf_fd = os.memfd_create('distcc-bench-perf')
    perf_interrupted = False
    elapsed = None
    error = None
    started = time.monotonic()
    def sample(pool):
        row: dict = dict(elapsed=time.monotonic() - started, hosts=dict(zip(names, pool.map(stats, workers))))
        samples.append(row)
        return row
    def monitor():
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(workers)) as pool, (out / 'stats.jsonl').open('w') as log:
            while not done.is_set():
                log.write(json.dumps(sample(pool)) + '\n')
                log.flush()
                done.wait(config['sample_seconds'])
            log.write(json.dumps(sample(pool)) + '\n')
    sampler = threading.Thread(target=monitor)
    def interrupted(signum, frame):
        raise InterruptedError(f'measurement interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(workers)) as pool:
            initial = sample(pool)
        if any('error' in r or int(r['dcc_busy']) or int(r['dcc_queued']) for r in initial['hosts'].values()):
            raise RuntimeError('workers must be reachable and idle before measurement')
        with (out / 'broker.log').open('w') as log:
            broker = subprocess.Popen(['runuser', '-u', 'portage', '--', 'env', 'DISTCC_VERBOSE=1',
                'DISTCC_HOSTS=' + hosts, 'DISTCC_DIR=' + str(runtime / '.distcc'),
                'TMPDIR=' + str(runtime), '/usr/bin/distccd', '--broker', socket_path], stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while not Path(socket_path).exists():
            if broker.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('measurement broker did not start')
            time.sleep(.02)
        perf_env = dict(os.environ, **config['perf'].get('environment', {}))
        with (out / 'perf-record.log').open('w') as log:
            perf = subprocess.Popen([config['perf']['executable'], 'record', '-e', 'cpu-clock',
                '--buildid-mmap', '--no-buildid-cache',
                '-F', str(config['perf']['frequency']), '--call-graph', 'dwarf,4096', '-m', '1024',
                '--proc-map-timeout', '50', '-a', '-G', str(group.relative_to('/sys/fs/cgroup')),
                '-o', f'/proc/self/fd/{perf_fd}'], env=perf_env, pass_fds=(perf_fd,),
                stdout=log, stderr=subprocess.STDOUT)
        # Failure to open the perf event must precede the costly build.
        time.sleep(.2)
        if perf.poll() is not None:
            raise RuntimeError('perf could not start; see perf-record.log')
        sampler.start()
        build_started = time.monotonic()
        with (out / 'emerge.log').open('w') as log:
            build = subprocess.Popen(['emerge', '--oneshot', '--nodeps', '--usepkg=n',
                                      '=' + config['package']], env=env, stdout=log, stderr=subprocess.STDOUT)
            rc = build.wait()
        elapsed = time.monotonic() - build_started
        if rc:
            raise RuntimeError(f'build exited {rc}; see emerge.log')
    except Exception as failure:
        error = str(failure)
    finally:
        stop_process(build)
        done.set()
        if sampler.is_alive():
            sampler.join()
        if perf and perf.poll() is None:
            perf_interrupted = True
            perf.send_signal(signal.SIGINT)
            try:
                perf.wait(timeout=30)
            except subprocess.TimeoutExpired:
                error = error or 'perf did not finish after the build'
                stop_process(perf)
        stop_process(broker)
    perf_status = None if perf is None else perf.returncode
    perf_ok = perf_status == 0 or (perf_interrupted and perf_status == -signal.SIGINT)
    if not perf_ok:
        error = error or f'perf exited {perf_status}'
    if perf_ok:
        try:
            if not finish_profile(config['perf'], out, perf_fd):
                error = error or 'perf data failed validation; see perf-record.log and perf-report.txt'
        except (OSError, subprocess.CalledProcessError) as failure:
            error = error or 'perf archive failed: ' + str(failure)
    os.close(perf_fd)
    buildlogs = list(buildtree.glob('*/*/temp/build.log'))
    for index, source in enumerate(buildlogs):
        shutil.copyfile(source, out / f'build-{index}.log')
    summary = log_summary([out / 'emerge.log', runtime / 'client.log', out / 'broker.log', *buildlogs])
    objects = set()
    for index, source in enumerate(buildtree.glob('*/*/work/*/.ninja_log')):
        shutil.copyfile(source, out / f'ninja-{index}.log')
        for line in source.read_text().splitlines()[1:]:
            fields = line.split('\t')
            if len(fields) >= 4 and fields[3].endswith('.o'):
                objects.add((index, fields[3]))
    hosts = summarize_samples(samples, names)
    measured = metrics(group)
    memory_events = measured['memory.events']
    if memory_events is None:
        raise RuntimeError('cgroup memory counters disappeared')
    events = dict(line.split() for line in memory_events.splitlines())
    if len(objects) != config['expected_objects']:
        error = error or f"expected {config['expected_objects']} compiled objects, got {len(objects)}"
    if any(summary[k] for k in ('analysis_warnings', 'io_timeouts', 'sandbox_denials')):
        error = error or 'build diagnostics failed validation'
    if not summary['feedback_jobs']:
        error = error or 'no measured broker feedback; distributed path not established'
    if any(r['failed_samples'] or r['counter_reset'] for r in hosts.values()):
        error = error or 'statistics missing or counters reset'
    if any(int(events[k]) for k in ('max', 'oom', 'oom_kill')):
        error = error or 'cgroup memory limit reached; measurement incomplete'
    # Automatic parallelism is an observed scheduler outcome. Explicit job
    # settings are already covered by the Portage configuration fingerprint.
    result = dict(success=error is None, error=error, elapsed=elapsed, compiled_objects=len(objects),
                  conditions=conditions, hosts=hosts, logs=summary, cgroup=measured,
                  perf_returncode=perf_status, returncode=None if build is None else build.returncode)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    # Keep evidence; build trees and private Portage state are not results.
    for path in (buildtree, configroot, runtime / '.distcc'):
        if path.exists():
            shutil.rmtree(path)
    print(json.dumps(result), flush=True)
    return int(error is not None)


if __name__ == '__main__':
    if sys.argv[1:] == ['--snapshot']:
        print(json.dumps(snapshot()))
    else:
        raise SystemExit(measure(json.loads(Path(sys.argv[1]).read_text()), Path(sys.argv[2]), Path(sys.argv[3])))
