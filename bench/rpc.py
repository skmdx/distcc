#!/usr/bin/env python3
"""Compare RPC transports on an actual build command with local fallback off.

Run inside a bounded cgroup. Servers must support the candidate protocols.
Commands are argv lists; {distcc} expands to the selected client executable.
Remove globs are relative to --work and are applied before each measured run.
The optional server cgroup paths allow CPU and IO accounting across the LAN.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


VARIANTS = {'baseline': 'lz4', 'tcp': 'lz4', 'stream': 'lz4s', 'http2': 'h2'}


def median(values):
    values = sorted(values)
    middle = len(values) // 2
    return (values[middle] + values[~middle]) / 2


def process_usage(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    cpu = sum(map(int, fields[11:15])) / os.sysconf('SC_CLK_TCK')
    status = Path(f'/proc/{pid}/status').read_text().splitlines()
    peak = next(int(line.split()[1]) * 1024 for line in status if line.startswith('VmHWM:'))
    return cpu, peak


def network_usage(pid):
    """TCP_INFO counters from sockets actually owned by this broker."""
    text = subprocess.check_output(['ss', '-Hntip'], text=True)
    connections = {}
    key = None
    for line in text.splitlines():
        if line and not line[0].isspace():
            fields = line.split()
            key = tuple(fields[3:5]) if f'pid={pid},' in line else None
        elif key:
            fields = dict(re.findall(r'(bytes_sent|bytes_received):(\d+)', line))
            connections[key] = {name: int(fields.get(name, 0)) for name in ('bytes_sent', 'bytes_received')}
    return connections


def server_usage(spec):
    host, path = spec.split(':', 1)
    script = '''import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
cpu = dict(line.split() for line in (p/'cpu.stat').read_text().splitlines())
io = [dict(field.split('=') for field in line.split()[1:]) for line in (p/'io.stat').read_text().splitlines()]
print(json.dumps(dict(cpu_seconds=int(cpu['usage_usec'])/1e6,
    peak_bytes=int((p/'memory.peak').read_text()),
    read_bytes=sum(int(row['rbytes']) for row in io),
    write_bytes=sum(int(row['wbytes']) for row in io))))'''
    command = shlex.join(['python3', '-c', script, path])
    return json.loads(subprocess.check_output(['ssh', '-o', 'BatchMode=yes', host, command], text=True))


def servers_usage(specs):
    with ThreadPoolExecutor(max_workers=max(1, len(specs))) as executor:
        return list(executor.map(server_usage, specs))


def run_command(command, env, work, log):
    start = time.monotonic()
    proc = subprocess.Popen(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
    _, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = os.waitstatus_to_exitcode(status)
    wall = time.monotonic() - start
    if proc.returncode:
        raise RuntimeError(f'command failed ({proc.returncode}); see {log.name}')
    return dict(wall_seconds=wall, client_cpu_seconds=usage.ru_utime + usage.ru_stime,
                client_peak_bytes=usage.ru_maxrss * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--hosts', required=True, help='TCP host entries, with slots/stats/speed but no codec')
    parser.add_argument('--baseline-hosts', help='separate baseline daemon endpoints; defaults to --hosts')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--variants', nargs='+', choices=VARIANTS, default=list(VARIANTS))
    parser.add_argument('--remove-glob', action='append', default=[])
    parser.add_argument('--verify', type=shlex.split, help='quoted argv command to verify each build')
    parser.add_argument('--pump', action='store_true')
    parser.add_argument('--server', action='append', default=[], metavar='SSH_HOST:CGROUP_PATH')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    args.command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not args.command or args.repeats < 1:
        parser.error('a command and positive repeat count are required')
    args.work = args.work.resolve()
    args.candidate = args.candidate.resolve()
    args.baseline = args.baseline.resolve()
    args.output = args.output.resolve()
    root = args.output.parent / (args.output.stem + '-runtime')
    root.mkdir()
    base_env = dict(os.environ, DISTCC_FALLBACK='0', DISTCC_MAX_RETRIES='1',
                    DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_LOG='', DISTCC_VERBOSE='0',
                    TMPDIR=str(root), DISTCC_CLIENT_TMP=str(root),
                    DISTCC_TESTING_INCLUDE_SERVER='1')
    instances = {}
    samples = []
    try:
        for name in args.variants:
            directory = root / name
            directory.mkdir()
            binary = args.baseline if name == 'baseline' else args.candidate
            hosts = args.baseline_hosts if name == 'baseline' and args.baseline_hosts else args.hosts
            address = directory / 'broker.sock'
            env = dict(base_env, DISTCC_HOSTS=' '.join(
                entry if entry.startswith('--') else entry + ',' + VARIANTS[name]
                for entry in hosts.split()), DISTCC_DIR=str(directory),
                DISTCC_BROKER=str(address), DISTCC_BROKER_IDLE='600',
                PATH=str(binary) + os.pathsep + base_env.get('PATH', os.defpath))
            log = (directory / 'build.log').open('w+')
            broker = subprocess.Popen([str(binary/'distccd'), '--broker', str(address)],
                                      env=env, stdout=log, stderr=log)
            instances[name] = dict(env=env, log=log, broker=broker, binary=binary, pump=False)
            deadline = time.monotonic() + 5
            while not address.exists():
                if broker.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('broker failed to start')
                time.sleep(.01)
            if args.pump:
                startup = subprocess.run([str(args.candidate/'pump'), '--startup'], env=env,
                                         cwd=args.work, check=True, capture_output=True, text=True)
                for line in startup.stdout.splitlines():
                    words = shlex.split(line)
                    if len(words) == 2 and words[0] == 'export':
                        key, value = words[1].split('=', 1)
                        env[key] = value
                if 'INCLUDE_SERVER_PORT' not in env:
                    raise RuntimeError('pump did not start')
                instances[name]['pump'] = True

        def execute(name, measured, repeat):
            instance = instances[name]
            env, log, broker = (instance[key] for key in ('env', 'log', 'broker'))
            for pattern in args.remove_glob:
                for file in args.work.glob(pattern):
                    if not file.is_file() or not file.resolve().is_relative_to(args.work):
                        raise RuntimeError('remove globs must select regular files inside --work')
                    file.unlink()
            command = [part.replace('{distcc}', str(instance['binary']/'distcc')) for part in args.command]
            before = servers_usage(args.server) if measured else []
            network_before = network_usage(broker.pid) if measured else {}
            broker_cpu, _ = process_usage(broker.pid)
            include_pid = int(env.get('INCLUDE_SERVER_PID', '0'))
            include_cpu = process_usage(include_pid)[0] if include_pid else 0
            result = run_command(command, env, args.work, log)
            after_cpu, peak = process_usage(broker.pid)
            include_after, include_peak = process_usage(include_pid) if include_pid else (0, 0)
            if measured:
                after = servers_usage(args.server)
                network_after = network_usage(broker.pid)
                if not network_after:
                    raise RuntimeError('no broker TCP connection: the command did not reach the RPC path')
                # A socket disappearing makes its final counters unavailable.
                # Do not report an undercount as measured transfer volume.
                network = None
                if network_before.keys() <= network_after.keys():
                    network = {field: sum(stats[field] - network_before.get(key, {}).get(field, 0)
                                          for key, stats in network_after.items())
                               for field in ('bytes_sent', 'bytes_received')}
                result.update(variant=name, repeat=repeat, broker_cpu_seconds=after_cpu-broker_cpu,
                              include_cpu_seconds=include_after-include_cpu, include_peak_bytes=include_peak,
                              total_client_cpu_seconds=result['client_cpu_seconds'] + after_cpu-broker_cpu + include_after-include_cpu,
                              broker_peak_bytes=peak, tcp_connections=len(network_after), network=network, servers=[
                    dict(cpu_seconds=b['cpu_seconds']-a['cpu_seconds'],
                         read_bytes=b['read_bytes']-a['read_bytes'],
                         write_bytes=b['write_bytes']-a['write_bytes'],
                         lifetime_peak_bytes=b['peak_bytes']) for a, b in zip(before, after)])
                samples.append(result)
                args.output.write_text(json.dumps(dict(samples=samples), indent=2) + '\n')
                print(json.dumps(result), flush=True)
            if args.verify:
                subprocess.run(args.verify, cwd=args.work, env=env, stdout=log, stderr=log, check=True)

        # Warm immediately before every measurement: the existing TCP pool
        # expires after ten seconds, so interleaving alone is not a warm test.
        for repeat in range(args.repeats):
            order = args.variants[repeat % len(args.variants):] + args.variants[:repeat % len(args.variants)]
            if repeat % 2:
                order = list(reversed(order))
            for name in order:
                execute(name, False, repeat)
                execute(name, True, repeat)
        summary = {name: {field: median(s[field] for s in samples if s['variant'] == name)
                         for field in ('wall_seconds', 'client_cpu_seconds', 'broker_cpu_seconds',
                                       'include_cpu_seconds', 'total_client_cpu_seconds',
                                       'client_peak_bytes', 'broker_peak_bytes', 'include_peak_bytes')}
                   for name in args.variants}
        args.output.write_text(json.dumps(dict(samples=samples, medians=summary,
            repeats=args.repeats, pump=args.pump,
            memory_note='broker and server peaks are lifetime upper bounds; command wait4 peaks can include the inherited launcher peak'), indent=2) + '\n')
        print(json.dumps(dict(medians=summary)), flush=True)
    finally:
        for instance in instances.values():
            if instance['pump']:
                subprocess.run([str(args.candidate/'pump'), '--shutdown'], env=instance['env'],
                               cwd=args.work, stdout=instance['log'], stderr=instance['log'])
            broker = instance['broker']
            if broker.poll() is None:
                broker.terminate()
            try:
                broker.wait(timeout=5)
            except subprocess.TimeoutExpired:
                broker.kill()
                broker.wait()
            instance['log'].close()


if __name__ == '__main__':
    main()
