#!/usr/bin/env python3
"""Replay one saved workload, with an unobserved control and a paired observation."""
import argparse
import concurrent.futures
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from typing import Any

from analyze import decode, packets, records, summarize, validate_sampling

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from measure import stats


class Host:
    """The two execution locations; shell quoting and file transfer live here."""
    def __init__(self, config, directory):
        self.config, self.directory = config, str(directory)

    def command(self, args, root=False):
        args = [*(['sudo', '-n'] if root else []), *map(str, args)]
        return ['ssh', self.config['ssh'], shlex.join(args)] if self.config.get('ssh') else args

    def run(self, args, root=False, **kwargs):
        return subprocess.run(self.command(args, root), check=True, **kwargs)

    def text(self, args, root=False):
        return self.run(args, root, stdout=subprocess.PIPE, text=True).stdout

    def put(self, source, destination):
        if self.config.get('ssh'):
            subprocess.run(['scp', '-q', str(source), self.config['ssh']+':'+str(destination)], check=True)
        else:
            shutil.copy2(source, destination)

    def fetch(self, source, destination):
        if self.config.get('ssh'):
            # Broker locks and perf outputs can be mode 0600. Read owned run
            # artifacts as root instead of changing their recorded permissions.
            subprocess.run(['rsync', '-a', '--rsync-path=sudo -n rsync',
                            self.config['ssh']+':'+str(source)+'/', str(destination)+'/'], check=True)

    def cpu(self):
        code = "import json,time,pathlib;print(json.dumps({'time':time.monotonic(),'cpu':pathlib.Path('/proc/stat').read_text().splitlines()[0]}))"
        return json.loads(self.text(['python3', '-c', code]))


def worker_stats(config):
    data = stats(dict(address=config['address'], port=config['capture']['stats_port']))
    if 'error' in data:
        raise RuntimeError(data['error'])
    return data


def check_worker(host, output):
    config = host.config
    data = worker_stats(config)
    if int(data['dcc_busy']) or int(data['dcc_queued']):
        raise RuntimeError('worker is already running jobs; do not replace its input cache')
    pid = host.text(['systemctl', 'show', config['service'], '-p', 'MainPID', '--value']).strip()
    paths = [f'/proc/{pid}/exe', *config['binaries']]
    hashes = dict((line.split()[1], line.split()[0]) for line in
                  host.text(['sha256sum', *paths], root=True).splitlines())
    for path, expected in config['binaries'].items():
        if hashes[path] != expected:
            raise RuntimeError(f'worker binary changed: {path}')
    if hashes[f'/proc/{pid}/exe'] != config['binaries']['/usr/bin/distccd']:
        raise RuntimeError('service is not running the selected installed binary')
    listeners = host.text(['ss', '-ltnp', f"sport = :{config['capture']['port']}"], root=True)
    if f'pid={pid},' not in listeners or config['address'] not in listeners:
        raise RuntimeError('observer destination is not the selected worker listener')
    output.write_text(json.dumps(dict(stats=data, hashes=hashes, listeners=listeners), indent=2))


@contextlib.contextmanager
def caches(hosts, seeds):
    backed_up = []
    try:
        for name, host in hosts.items():
            path = Path(host.config['cache'])
            host.run(['tar', '--zstd', '-cf', host.directory+'/original-cache.tar.zst',
                      '-C', str(path.parent), path.name], root=True)
            host.put(seeds[name], host.directory+'/seed.tar.zst')
            backed_up.append(host)
        yield
    finally:
        for host in reversed(backed_up):
            restore_cache(host, 'original-cache.tar.zst')


def restore_cache(host, archive='seed.tar.zst'):
    path = Path(host.config['cache'])
    # The dedicated fixture owns these idle caches for the paired runs. Do
    # not stop distccd: systemd may remove its RuntimeDirectory on stop.
    host.run(['rm', '-r', '--', str(path)], root=True)
    host.run(['tar', '--zstd', '-xf', host.directory+'/'+archive, '-C', str(path.parent)], root=True)
    host.run(['tar', '--zstd', '-df', host.directory+'/'+archive, '-C', str(path.parent)], root=True)


@contextlib.contextmanager
def observers(hosts, phase, output):
    processes, logs = [], []
    try:
        for name, host in hosts.items():
            host.run(['mkdir', '-p', host.directory+'/'+phase+'/'+name])
            log = (output/(name+'-observer.log')).open('w')
            logs.append(log)
            process = subprocess.Popen(host.command(['python3', host.directory+'/code/capture.py',
                host.directory+'/config.json', name, host.directory+'/'+phase+'/'+name], root=True),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            processes.append(process)
            assert process.stdout is not None
            if process.stdout.readline().strip() != 'ready':
                raise RuntimeError(f'{name} observer did not start; see {log.name}')
        yield
    finally:
        failed = []
        for process in processes:
            if process.poll() is None:
                process.stdin.close()
            if process.wait(timeout=30):
                failed.append(process.args)
            process.stdout.close()
        for log in logs:
            log.close()
        if failed:
            raise RuntimeError(f'observer failed: {failed}')


def fetch_observation(hosts, phase, output):
    for name, host in hosts.items():
        if host.config.get('ssh'):
            if name == 'worker':
                host.run(['python3', host.directory+'/code/analyze.py', '--decode',
                          host.directory+'/'+phase+'/'+name], root=True)
            host.fetch(host.directory+'/'+phase+'/'+name, output/name)
        elif name == 'worker':
            decode(output/name)


def smoke(hosts, work):
    directory = work/'smoke'
    directory.mkdir()
    # The two smoke tasks share one CPU to exercise OS runnable accounting.
    # Only these disposable tasks are pinned; the replay's CPU policy is intact.
    code = '''import os,socket,time,json
os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
a,b=socket.socketpair();p=os.fork()
if not p:
 a.close()
 for _ in range(256): b.recv(1);b.sendall(b'x')
 t=time.monotonic()+.08
 while time.monotonic()<t: pass
 os._exit(0)
b.close()
for _ in range(256): a.sendall(b'x');a.recv(1)
t=time.monotonic()+.08
while time.monotonic()<t: pass
os.waitpid(p,0)
print(json.dumps([os.getpid(),p]))
'''
    with observers(hosts, 'smoke', directory):
        smoke_pids = json.loads(hosts['worker'].text(['python3', '-c', code]))
        c = hosts['worker'].config
        request = ("import socket;s=socket.create_connection("+repr((c['address'],c['capture']['stats_port']))+
                   ");s.sendall(b'GET / HTTP/1.0\\r\\n\\r\\n');s.recv(65536);s.close()")
        hosts['client'].run(['python3', '-c', request])
    fetch_observation(hosts, 'smoke', directory)
    tasks = {r['pid']:r for r in (json.loads(line) for line in
             (directory/'worker/tasks.jsonl').read_text().splitlines())}
    if any(pid not in tasks or not tasks[pid]['scheduled_ns'] or not tasks[pid]['runnable_ns']
           for pid in smoke_pids):
        raise RuntimeError('smoke did not reach CPU/runnable exit accounting')
    sampled = list(records(directory/'worker/samples.txt.zst'))
    cpu = sum('cpu-clock:' in r['event'] and bool(r['stack']) for r in sampled if r['pid'] in smoke_pids)
    callers = sum('context-switches:' in r['event'] and bool(r['stack']) for r in sampled if r['pid'] in smoke_pids)
    switches = sum(tasks[pid]['voluntary_switches']+tasks[pid]['involuntary_switches'] for pid in smoke_pids)
    validate_sampling(switches, callers)
    if not cpu:
        raise RuntimeError('smoke did not sample target CPU callers')
    for name in hosts:
        path = directory/name
        if not list(packets(path/'packets.pcap')):
            raise RuntimeError('smoke captured no real packets')
    (directory/'valid.json').write_text(json.dumps(dict(valid=True, pids=smoke_pids,
        taskstats=[tasks[pid] for pid in smoke_pids], cpu_samples=cpu,
        caller_samples=callers, accounted_switches=switches), indent=2))


def execute(hosts, config, phase, work, observed) -> dict[str, Any]:
    output = work/phase
    output.mkdir()
    check_worker(hosts['worker'], output/'worker-before.json')
    for host in hosts.values():
        restore_cache(host)
        host.run(['mkdir', '-p', host.directory+'/'+phase])
    client = hosts['client']
    for name in ('commands.json', 'config.json'):
        client.run(['cp', client.directory+'/'+name, client.directory+'/'+phase+'/'+name])
    before = {name: host.cpu() for name, host in hosts.items()}
    with observers(hosts, phase, output) if observed else contextlib.nullcontext():
        with (output/'replay.log').open('w') as log:
            client.run(['bash', client.directory+'/code/probe.sh', client.directory+'/'+phase,
                        client.config['environment'], client.config['frontend'], client.config['hosts']],
                       root=True, stdout=log, stderr=log)
    after = {name: host.cpu() for name, host in hosts.items()}
    client.fetch(client.directory+'/'+phase, output)
    end = worker_stats(hosts['worker'].config)
    begin = json.loads((output/'worker-before.json').read_text())['stats']
    if int(end['dcc_compile_ok'])-int(begin['dcc_compile_ok']) != config['remote_completions']:
        raise RuntimeError('worker completion count differs from the selected fixture')
    (output/'resources.json').write_text(json.dumps(dict(before=before, after=after,
                                                        worker_before=begin, worker_after=end), indent=2))
    if observed:
        fetch_observation(hosts, phase, output)
        return summarize(output, config)
    jobs = json.loads((output/'jobs.json').read_text())
    return dict(wall_seconds=jobs['end']-jobs['start'], commands=len(jobs['results']),
                jobs=jobs['jobs'], outputs_match=sum(j['output_match'] for j in jobs['results']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--workdir', type=Path, required=True, help='new empty execution directory')
    args = parser.parse_args()
    work, fixture = args.workdir.resolve(), args.fixture.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise RuntimeError('workdir must be empty; completed runs are immutable evidence')
    config = json.loads(fixture.read_text())
    inputs = {name: (fixture.parent/config[name]).resolve()
              for name in ('commands', 'client_cache', 'worker_cache')}
    provenance = {name: dict(path=str(path), sha256=hashlib.file_digest(path.open('rb'), 'sha256').hexdigest())
                  for name, path in inputs.items()}
    (work/'inputs.json').write_text(json.dumps(provenance, indent=2))
    hosts = {name: Host(config[name], config[name]['scratch_root']+'/'+work.name
                       if config[name].get('ssh') else work)
             for name in ('client', 'worker')}
    source = Path(__file__).resolve().parent
    for host in hosts.values():
        uid, gid = host.text(['id', '-u']).strip(), host.text(['id', '-g']).strip()
        host.run(['install', '-d', '-m', '0755', '-o', uid, '-g', gid,
                  host.directory, host.directory+'/code'], root=True)
        for path in source.iterdir():
            if path.suffix not in ('.py', '.c', '.sh'):
                continue
            host.put(path, host.directory+'/code/'+path.name)
        host.put(fixture, host.directory+'/config.json')
        host.put(inputs['commands'], host.directory+'/commands.json')
    try:
        # Build the thin UAPI/libnl adapter on the worker, using its normal C
        # compiler/flags. No scheduler algorithm or private ABI is duplicated.
        compile_code = '''import os,shlex,subprocess,sys
flags=shlex.split(os.environ.get('CPPFLAGS','')+' '+os.environ.get('CFLAGS',''))
libs=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libnl-genl-3.0'],text=True))
subprocess.run(shlex.split(os.environ.get('CC','cc'))+flags+[sys.argv[1]+'/taskstats.c','-o',sys.argv[1]+'/taskstats']+libs,check=True)
'''
        hosts['worker'].run(['python3', '-c', compile_code, hosts['worker'].directory+'/code'])
        check_worker(hosts['worker'], work/'target.json')
        smoke(hosts, work)
        print('Smoke passed: target exit accounting, sparse caller samples and paired packets.', flush=True)
        with caches(hosts, dict(client=inputs['client_cache'], worker=inputs['worker_cache'])):
            control = execute(hosts, config, 'control', work, False)
            print('Unobserved replay complete.', flush=True)
            observed = execute(hosts, config, 'observed', work, True)
        if control['jobs'] != observed['jobs'] or control['outputs_match'] != observed['outputs_match']:
            raise RuntimeError('control and observation did not exercise the same replay contract')
        result = dict(valid=True, control=control,
                      validity_scope='Capture completeness and matched replay, not negligible observation overhead.',
                      observed={key: observed[key] for key in ('wall_seconds', 'commands', 'outputs_match', 'jobs')},
                      observation_wall_delta_seconds=observed['wall_seconds']-control['wall_seconds'],
                      observation_wall_ratio=observed['wall_seconds']/control['wall_seconds'],
                      limitation='One paired diagnostic, not a repeated estimate or full-build performance claim. '
                                 'Saved input caches are restored; Linux page-cache residency is not controlled.')
        (work/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, indent=2))
    finally:
        # Keep failed observations locally, never label a partially captured
        # run valid. Remote scratch is disposable only after copying its data.
        for host in hosts.values():
            if host.config.get('ssh'):
                host.fetch(host.directory, work/'remote')
                host.run(['rm', '-r', '--', host.directory], root=True)


if __name__ == '__main__':
    main()
