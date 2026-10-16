#!/usr/bin/env python3
"""Run and compare complete builds using the private deployment inventory."""
import concurrent.futures
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import measure as measurement

spec = importlib.util.spec_from_file_location('deploy', Path(__file__).resolve().parents[1] / 'packaging/gentoo/deploy.py')
assert spec is not None and spec.loader is not None
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


def snapshots(workers, targets, script):
    def capture(worker):
        return json.loads(deploy.run(['sudo', '-n', 'python3', script, '--snapshot'], targets[worker['name']]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(workers)) as pool:
        return dict(zip([w['name'] for w in workers], pool.map(capture, workers)))


def resource_usage(after, before=None):
    def values(row):
        if row is None:
            return {'cpu_seconds': 0, 'read_bytes': 0, 'write_bytes': 0}
        cpu = dict(line.split() for line in row['cpu.stat'].splitlines()) if row['cpu.stat'] is not None else {}
        io = [dict(field.split('=') for field in line.split()[1:])
              for line in row['io.stat'].splitlines()] if row['io.stat'] is not None else None
        return dict(cpu_seconds=int(cpu['usage_usec']) / 1e6 if 'usage_usec' in cpu else None,
                    read_bytes=sum(int(r['rbytes']) for r in io)
                    if io is not None and all('rbytes' in r for r in io) else None,
                    write_bytes=sum(int(r['wbytes']) for r in io)
                    if io is not None and all('wbytes' in r for r in io) else None)
    first, last = values(before), values(after)
    result = {}
    for key, value in last.items():
        initial = first[key]
        result[key] = value - initial if value is not None and initial is not None else None
    return result


def memory_limited(before, after):
    events = [dict((k, int(v)) for k, v in (line.split() for line in row['memory.events'].splitlines()))
              for row in (before, after)]
    return any(events[1][key] != events[0][key] for key in ('max', 'oom', 'oom_kill'))


def release_artifacts(directory):
    """Keep completed repetitions' received logs out of the collector's cache budget."""
    for path in directory.rglob('*'):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as stream:
                os.fsync(stream.fileno())
                os.posix_fadvise(stream.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)


def worker_changes(before, after):
    changes = {}
    for name, first in before.items():
        last = after[name]
        if first['boot'] != last['boot'] or first['service']['MainPID'] != last['service']['MainPID']:
            raise RuntimeError(f'{name}: worker restarted during measurement')
        if memory_limited(first['metrics'], last['metrics']):
            raise RuntimeError(f'{name}: memory limit reached; measurement incomplete')
        delta = [b - a for a, b in zip(first['cpu'], last['cpu'])]
        changes[name] = {'cpu_busy_percent': 100 * (sum(delta) - delta[3] - delta[4]) / sum(delta)
                         if sum(delta) else None, 'resources': resource_usage(last['metrics'], first['metrics']),
                         'before': first, 'after': last}
    return changes


def comparison(current, previous):
    """Only full successful runs with identical workload conditions are comparable."""
    if not current['success'] or not previous or not previous['success']:
        return {'eligible': False, 'reason': 'no successful previous measurement'}
    if current['condition_key'] != previous['condition_key']:
        return {'eligible': False, 'reason': 'measurement conditions differ'}
    old = previous['seconds']['mean']
    new = current['seconds']['mean']
    return dict(eligible=True, previous=previous['id'], before_seconds=old, after_seconds=new,
                change_percent=100 * (new - old) / old,
                note='Repeated timings describe this workload; they are not a general speedup guarantee.')


def summarize(run_id, runs, config):
    conditions = []
    for run in runs:
        workers = {name: {'memory_max': row['before']['metrics']['memory.max'],
                          'compiler': row['before']['compiler']}
                   for name, row in run['workers'].items()}
        conditions.append(dict(build=run['conditions'], workers=workers))
    stable = all(c == conditions[0] for c in conditions)
    seconds = [run['elapsed'] for run in runs]
    return dict(id=run_id, success=stable and len(runs) == config['repetitions'] and
                all(run['success'] for run in runs),
                condition_key=hashlib.sha256(json.dumps(conditions[0], sort_keys=True).encode()).hexdigest(),
                seconds=dict(mean=sum(seconds) / len(seconds), minimum=min(seconds), maximum=max(seconds)),
                conditions_stable=stable, runs=runs)


def report(result):
    lines = [f"Measurement: {result['id']}", f"Success: {result['success']}"]
    if 'seconds' in result:
        times = result['seconds']
        lines.append(f"Complete build: mean {times['mean']:.3f} s ({times['minimum']:.3f}–{times['maximum']:.3f} s)")
        for index, run in enumerate(result['runs'], 1):
            lines.append(f"Run {index}: {run['elapsed']:.3f} s, {run['compiled_objects']} objects")
            lines.append('  Client build/profile cgroup: ' + json.dumps(resource_usage(run['cgroup'])))
            for name, host in run['hosts'].items():
                cpu = run['workers'][name]['cpu_busy_percent']
                lines.append(f"  {name}: {host['counters']['dcc_compile_ok']} jobs, peak {host['peak_busy']} busy, "
                             f"{host['peak_queued']} queued, CPU {cpu:.1f}%" if cpu is not None else f"  {name}: CPU unavailable")
            lines.append('  Broker feedback seconds (sum across parallel jobs): ' +
                         ', '.join(f"{name}={value['total_seconds']:.3f}" for name, value in run['logs']['phases'].items()))
        comp = result['comparison']
        lines.append(f"Previous mean: {comp['before_seconds']:.3f} s; change {comp['change_percent']:+.2f}%"
                     if comp['eligible'] else 'Comparison unavailable: ' + comp['reason'])
    if result.get('error'):
        lines.append('Error: ' + result['error'])
    lines.append('perf, IPC/RPC counters, raw statistics and cgroup CPU/memory/I/O are saved with each run.')
    return '\n'.join(lines) + '\n'


def main():
    if len(sys.argv) != 1:
        raise SystemExit('distcc-bench takes no arguments; configure ~/.config/distcc/bench.json')
    config_dir = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'distcc'
    config = json.loads((config_dir / 'bench.json').read_text())
    inventory = json.loads((config_dir / 'deploy.json').read_text())
    targets = {t['name']: t for t in inventory['targets']}
    client = targets[config['client']]
    workers = config['workers']
    limits = config['limits']
    if config['repetitions'] < 2 or config['sample_seconds'] <= 0:
        raise ValueError('at least two complete builds and a positive sample interval are required')
    if 'MemorySwapMax=0' not in limits or not any(p.startswith('MemoryMax=') and
            p.split('=', 1)[1] not in ('0', 'infinity') for p in limits):
        raise ValueError('a finite memory limit with swap disabled is required')
    unit = 'distcc-bench.service'
    # Serialize against both deployment and other benchmark invocations.
    with (config_dir / 'deploy.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (config_dir / 'deploy.resume.json').exists():
            raise RuntimeError('finish the interrupted deployment before benchmarking')
        run_id = time.strftime('distcc-bench-%Y%m%dT%H%M%S')
        out = Path(config['results']) / run_id
        out.mkdir(parents=True)
        remote = client['temporary'] + '/' + run_id
        script = remote + '/measure.py'
        settings = out / 'settings.json'
        settings.write_text(json.dumps(config, indent=2))
        measure = Path(__file__).with_name('measure.py')
        resume_path = config_dir / 'bench.resume.json'
        resume = json.loads(resume_path.read_text()) if resume_path.exists() else {}
        runs = []
        result: dict = dict(id=run_id, success=False)
        controller_group = measurement.cgroup()
        controller_before = measurement.metrics(controller_group)
        if controller_before['memory.events'] is None:
            raise RuntimeError('result collector memory accounting is unavailable')
        owns_unit = False
        def interrupted(signum, frame):
            raise InterruptedError(f'benchmark interrupted by signal {signum}')
        signal.signal(signal.SIGTERM, interrupted)
        try:
            for worker in workers:
                deploy.transfer([measure], remote, targets[worker['name']])
            deploy.transfer([settings], remote, client)
            # Establish the real measurement and worker limits before stopping builds.
            snapshots(workers, targets, script)
            if deploy.run(['systemctl', 'show', unit, '-p', 'ActiveState', '--value'], client) in ('active', 'activating'):
                raise RuntimeError('another measurement is still running on the client')
            for name, target in targets.items():
                for pattern in target.get('quiesce_units', []):
                    active = deploy.run(['systemctl', 'list-units', '--type=service', '--state=active',
                                         '--plain', '--no-legend', pattern], target)
                    units = [line.split()[0] for line in active.splitlines()]
                    if units:
                        resume[name] = sorted(set(resume.get(name, []) + units))
                        resume_path.write_text(json.dumps(resume))
                        deploy.run(['sudo', '-n', 'systemctl', 'stop', *units], target)
            for index in range(1, config['repetitions'] + 1):
                print(f'Build {index}/{config["repetitions"]}; results: {out}', flush=True)
                before = snapshots(workers, targets, script)
                (out / f'run-{index}-workers-before.json').write_text(json.dumps(before, indent=2) + '\n')
                buildroot = remote + f'/build-{index}'
                deploy.run(['sudo', '-n', 'mkdir', '-p', buildroot], client)
                cmd = ['sudo', '-n', 'systemd-run', '--wait', '--pipe', '--collect', '--unit=' + unit]
                for prop in [*limits, 'IOAccounting=yes', 'RuntimeMaxSec=2700',
                             *client.get('mounts', []), 'BindPaths=' + buildroot + ':/var/tmp']:
                    cmd += ['-p', prop]
                dest = remote + f'/run-{index}'
                owns_unit = True
                with (out / f'run-{index}-unit.log').open('w') as log:
                    completed = subprocess.run(deploy.command([*cmd, 'python3', script,
                        remote + '/settings.json', dest, buildroot], client), stdout=log, stderr=subprocess.STDOUT)
                after = snapshots(workers, targets, script)
                (out / f'run-{index}-workers-after.json').write_text(json.dumps(after, indent=2) + '\n')
                # Copy evidence even for failed/OOM builds; never retry with looser limits.
                source = f'{client["ssh"]}:{dest}/' if client.get('ssh') else dest + '/'
                rsync = ['rsync', '-rt', '--rsync-path=sudo -n rsync'] if client.get('ssh') else ['sudo', '-n', 'rsync', '-rt']
                deploy.run([*rsync, '--exclude=/build/', '--exclude=/config/',
                            '--exclude=/runtime/.distcc/', source, str(out / f'run-{index}') + '/'])
                release_artifacts(out / f'run-{index}')
                if memory_limited(controller_before, measurement.metrics(controller_group)):
                    raise RuntimeError('result collection reached its memory limit; measurement incomplete')
                path = out / f'run-{index}/result.json'
                if not path.exists():
                    raise RuntimeError('measurement did not finish; see unit log and saved artifacts')
                run = json.loads(path.read_text())
                run['workers'] = worker_changes(before, after)
                path.write_text(json.dumps(run, indent=2) + '\n')
                runs.append(run)
                if completed.returncode or not run['success']:
                    raise RuntimeError(run.get('error') or 'measurement unit failed')
            result = summarize(run_id, runs, config)
            previous_path = Path(config['results']) / 'latest.json'
            previous = json.loads(previous_path.read_text()) if previous_path.exists() else None
            result['comparison'] = comparison(result, previous)
            if not result['success']:
                raise RuntimeError('build conditions changed between repetitions')
        except (Exception, KeyboardInterrupt) as error:
            result.update(success=False, error=str(error))
        finally:
            result['controller'] = dict(before=controller_before,
                                        after=measurement.metrics(controller_group))
            if memory_limited(controller_before, result['controller']['after']):
                result.update(success=False, error='result collection reached its memory limit; measurement incomplete')
            # systemd owns all descendants, including after SSH interruption.
            try:
                if owns_unit:
                    deploy.run(['sudo', '-n', 'systemctl', 'stop', unit], client)
            except RuntimeError as error:
                if 'not loaded' not in str(error):
                    result.update(success=False, error=str(error))
            try:
                for name, units in list(resume.items()):
                    deploy.run(['sudo', '-n', 'systemctl', 'start', *units], targets[name])
                    del resume[name]
                    resume_path.write_text(json.dumps(resume))
                resume_path.unlink(missing_ok=True)
            except RuntimeError as error:
                result.update(success=False, error='build resume failed: ' + str(error))
            if result['success']:
                try:
                    for name in {w['name'] for w in workers} | {config['client']}:
                        deploy.run(['sudo', '-n', 'rm', '-rf', '--', remote], targets[name])
                except RuntimeError as error:
                    result.update(success=False, error='temporary cleanup failed: ' + str(error))
                if result['success']:
                    (Path(config['results']) / 'latest.json').write_text(json.dumps(result, indent=2) + '\n')
            (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            (out / 'summary.txt').write_text(report(result))
            print(report(result), end='', flush=True)
            print(f'Results: {out}', flush=True)
        return int(not result['success'])


if __name__ == '__main__':
    raise SystemExit(main())
