"""Streaming analysis of one replay's perf state streams and paired TCP traces."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from typing import Any


HEADER = re.compile(r'^(.*?)\s+(-?\d+) \[(\d+)\]\s+(\d+\.\d+):\s*(.*)$')
SWITCH = re.compile(r'sched:sched_switch: (.*):(\d+) \[\d+\] (\S+) ==> (.*):(\d+) \[\d+\]')
WAKE = re.compile(r'sched:sched_wakeup: (.*):(\d+) \[\d+\]')


def decode(directory):
    for name in ('states', 'callers'):
        with (directory/(name+'.txt.zst')).open('wb') as output:
            with subprocess.Popen(['perf', 'script', '-i', str(directory/(name+'.data')),
                                   '--show-mmap-events', '--show-task-events', '--ns'],
                                  stdout=subprocess.PIPE) as perf:
                assert perf.stdout is not None
                result = subprocess.run(['zstd', '-q', '-T1', '-c'], stdin=perf.stdout, stdout=output)
                perf.stdout.close()
                if perf.wait() or result.returncode:
                    raise RuntimeError('perf decoding failed')


def records(path):
    with subprocess.Popen(['zstd', '-dc', str(path)], stdout=subprocess.PIPE, text=True) as stream:
        assert stream.stdout is not None
        current: dict[str, Any] | None = None
        for line in stream.stdout:
            if re.search(r'PERF_RECORD_LOST|LOST \d+', line):
                raise RuntimeError('perf stream contains lost events')
            if line.startswith('\t'):
                if current:
                    current['stack'].append(line.strip())
                continue
            match = HEADER.match(line)
            if match:
                if current:
                    yield current
                current = dict(comm=match[1].strip(), pid=int(match[2]), cpu=int(match[3]),
                               time=float(match[4]), event=match[5], stack=[])
        if current:
            yield current
        if stream.wait():
            raise RuntimeError('compressed perf stream failed')


def validate_sampling(continuous, sampled):
    if not 0 < sampled < continuous:
        raise RuntimeError(f'caller stream not sampled: {sampled}/{continuous}')


def scheduler(directory, low, high):
    service = set()
    callers = collections.Counter()
    caller_samples = 0
    for row in records(directory/'callers.txt.zst'):
        if 'PERF_RECORD_LOST' in row['event']:
            raise RuntimeError('sampled caller events lost')
        if not low <= row['time'] <= high or not SWITCH.match(row['event']):
            continue
        caller_samples += 1
        stack = ' '.join(row['stack'])
        if 'fuse_dev_do_read' in stack:
            service.add(row['pid'])
        for name in ('fuse_lookup_name', 'fuse_chan_send', 'fuse_dev_do_read',
                     'unix_stream_read_generic', 'unix_seqpacket_recvmsg', 'do_sys_poll',
                     'do_epoll_wait', 'do_wait', 'futex_wait', 'io_schedule'):
            if name in stack:
                callers[(row['comm'], name)] += 1
                break
    processes, cpus, states = {}, {}, {}
    totals = collections.defaultdict(collections.Counter)
    pid_totals = collections.defaultdict(collections.Counter)
    mismatches, switches, wakes, cpu_samples = 0, 0, 0, 0
    def role(pid, comm):
        if not pid:
            return 'idle'
        if pid in service:
            return 'fuse-service'
        info = processes.get(pid, {})
        name = info.get('exe', comm)
        if info.get('planner'):
            name += '/task' if info.get('forked') else '/resident'
        return name
    def add(pid, name, kind, begin, end):
        seconds = max(0, min(end, high)-max(begin, low))
        totals[name][kind] += seconds
        pid_totals[pid][kind] += seconds
    for row in records(directory/'states.txt.zst'):
        event, now, cpu = row['event'], row['time'], row['cpu']
        if 'PERF_RECORD_LOST' in event:
            raise RuntimeError('continuous scheduler events lost')
        match = re.search(r'FORK\((\d+):\d+\):\((\d+):\d+\)', event)
        if match and match[1] != match[2]:
            processes[int(match[1])] = dict(processes.get(int(match[2]), {}), forked=True)
        match = re.search(r'COMM exec: (.+):(\d+)/\d+', event)
        if match:
            processes[int(match[2])] = {}
        match = re.search(r'MMAP2 (\d+)/\d+:.*: r-xp (.+)$', event)
        if match:
            info = processes.setdefault(int(match[1]), {})
            name = Path(match[2]).name
            if name == 'gcc_plan.so':
                info['planner'] = True
            elif name in ('distcc', 'distccd', 'cc1', 'cc1plus', 'as', 'python3.14') or name.endswith('-gcc'):
                info['exe'] = name
        match = SWITCH.match(event)
        if match:
            switches += low <= now <= high
            old, new = int(match[2]), int(match[5])
            if cpu in cpus:
                begin, pid, name = cpus[cpu]
                if pid == old:
                    add(pid, name, 'cpu', begin, now)
                elif low < now < high:
                    mismatches += 1
            if new in states:
                kind, begin, name = states.pop(new)
                add(new, name, kind, begin, now)
            cpus[cpu] = now, new, role(new, match[4])
            if old and match[3] not in ('X', 'Z'):
                states[old] = ('runnable' if match[3].startswith('R') else 'blocked',
                               now, role(old, match[1]))
        match = WAKE.match(event)
        if match:
            wakes += low <= now <= high
            pid = int(match[2])
            if pid in states and states[pid][0] == 'blocked':
                _, begin, name = states[pid]
                add(pid, name, 'blocked', begin, now)
                states[pid] = 'runnable', now, name
        if 'cpu-clock/' in event and low <= now <= high:
            cpu_samples += bool(row['stack'])
    for pid, (kind, begin, name) in states.items():
        add(pid, name, kind, begin, high)
    for begin, pid, name in cpus.values():
        add(pid, name, 'cpu', begin, high)
    observed = sum(v['cpu'] for v in totals.values())
    expected = len(cpus)*(high-low)
    if mismatches or abs(observed-expected) > .001 or not switches or not wakes:
        raise RuntimeError(f'incomplete scheduler coverage: mismatches={mismatches}, CPU={observed}/{expected}')
    validate_sampling(switches, caller_samples)
    return dict(interval=[low, high], roles=totals, processes=pid_totals,
                switches=switches, wakeups=wakes, cpu_caller_samples=cpu_samples,
                sampled_switches=caller_samples, sampled_callsites=[dict(comm=k[0], site=k[1], count=v)
                    for k, v in callers.most_common()], service_pids=sorted(service),
                lost=0, continuity_mismatches=0, cpu_seconds=observed, expected_cpu_seconds=expected,
                limitation='Caller sampling identifies sites, not their exact blocked durations; '
                           'new-task time before the first observed switch is not attributed.')


def packets(path, port=None):
    with path.open('rb') as source:
        header = source.read(24)
        scale = {b'\xd4\xc3\xb2\xa1': 1e6, b'\x4d\x3c\xb2\xa1': 1e9}[header[:4]]
        if struct.unpack_from('<I', header, 20)[0] != 1:
            raise ValueError('expected Ethernet pcap')
        while block := source.read(16):
            sec, fraction, size, original = struct.unpack('<IIII', block)
            data = source.read(size)
            if len(data) != original:
                raise ValueError('truncated packet')
            offset, kind = 14, struct.unpack_from('!H', data, 12)[0]
            if kind == 0x8100:
                offset, kind = 18, struct.unpack_from('!H', data, 16)[0]
            if kind != 0x0800 or data[offset+9] != 6:
                continue
            ip = data[offset:]
            tcp = ip[(ip[0]&15)*4:struct.unpack_from('!H', ip, 2)[0]]
            sport, dport, sequence = struct.unpack_from('!HHI', tcp)
            if port and port not in (sport, dport):
                continue
            key = ('.'.join(map(str, ip[12:16])), sport, '.'.join(map(str, ip[16:20])), dport)
            yield sec+fraction/scale, key, sequence, tcp[(tcp[12]>>4)*4:]


def frames(rows):
    streams = collections.defaultdict(list)
    for at, key, seq, data in rows:
        if data:
            streams[key].append((seq, at, data))
    result = []
    for key, stream in streams.items():
        stream.sort()
        position, buffer, bounds = stream[0][0], bytearray(), []
        for seq, at, data in stream:
            if seq > position:
                raise RuntimeError('gap in observed TCP stream')
            data = data[max(0, position-seq):]
            if data:
                buffer.extend(data)
                position += len(data)
                bounds.append((len(buffer), at))
        cursor = boundary = 0
        while cursor+8 <= len(buffer):
            kind, size = struct.unpack_from('!II', buffer, cursor)
            end = cursor+8+size
            if end > len(buffer):
                raise RuntimeError('incomplete RPC frame')
            while bounds[boundary][0] < end:
                boundary += 1
            result.append((bounds[boundary][1], key, kind, size))
            cursor = end
        if cursor != len(buffer):
            raise RuntimeError('incomplete RPC header')
    return sorted(result)


def rpc(rows, worker) -> dict[str, Any]:
    pending, intervals, counts = {}, [], collections.Counter()
    for at, key, kind, size in frames(rows):
        base = kind & ~0x30000
        counts[hex(base)] += 1
        stream = key if key[0] == worker else (key[2], key[3], key[0], key[1])
        if base == 0x54000309:
            if stream in pending:
                raise RuntimeError('overlapping filesystem request on one job connection')
            pending[stream] = at
        if base == 0x5400030a and kind & 0x20000:
            intervals.append((pending.pop(stream), at))
    if pending:
        raise RuntimeError('filesystem request without final response')
    return dict(frames=counts, count=len(intervals), seconds=sum(b-a for a,b in intervals))


def clock_offset(client, worker, address) -> dict[str, Any]:
    def index(rows):
        result = {}
        for at, key, seq, data in rows:
            if data:
                result.setdefault((key, seq, hashlib.sha256(data).digest()), at)
        return result
    a, b = index(client), index(worker)
    lower, upper = [], []
    for key in a.keys() & b.keys():
        (upper if key[0][0] == address else lower).append(a[key]-b[key])
    if not lower or not upper or max(lower) > min(upper):
        raise RuntimeError('no consistent two-way packet clock bound')
    return dict(client_minus_worker_seconds=[max(lower), min(upper)], matched_packets=len(lower)+len(upper))


def summarize(directory, config):
    jobs = json.loads((directory/'jobs.json').read_text())
    commands = sorted(jobs['results'], key=lambda row: row['start'])
    low, high = commands[jobs['jobs']-1]['start'], commands[-1]['start']
    port, address = config['worker']['capture']['port'], config['worker']['address']
    client = list(packets(directory/'client/packets.pcap', port))
    worker = list(packets(directory/'worker/packets.pcap', port))
    offset = clock_offset(client, worker, address)
    shift = sum(offset['client_minus_worker_seconds'])/2
    result = dict(clock=offset, pending_seconds=high-low, jobs=jobs['jobs'], commands=len(commands),
                  wall_seconds=jobs['end']-jobs['start'], outputs_match=sum(c['output_match'] for c in commands))
    for name in ('client', 'worker'):
        anchors = json.loads((directory/name/'clocks.json').read_text())
        delta = sum((a['mono_ns']-a['wall_ns'])/1e9 for a in anchors)/2
        adjust = -shift if name == 'worker' else 0
        result[name] = scheduler(directory/name, low+delta+adjust, high+delta+adjust)
    result['rpc_client_handling'] = rpc(client, address)
    result['rpc_worker_roundtrip'] = rpc(worker, address)
    if result['rpc_client_handling']['frames'] != result['rpc_worker_roundtrip']['frames']:
        raise RuntimeError('packet captures disagree on RPC frames')
    if result['rpc_client_handling']['frames'].get('0x54000305') != config['remote_completions']:
        raise RuntimeError('replay did not complete on the selected worker')
    if result['outputs_match'] != len(commands) or any(c['returncode'] for c in commands):
        raise RuntimeError('output verification failed')
    logs = (directory/'client.log').read_text() + (directory/'broker.log').read_text()
    diagnostics = re.findall(r'\b(?:ERROR|Warning|fallback|timeout)\b', logs, re.I)
    if diagnostics:
        raise RuntimeError(f'distcc diagnostics: {collections.Counter(diagnostics)}')
    result.update(valid=True, diagnostics=0)
    (directory/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decode', type=Path)
    args = parser.parse_args()
    decode(args.decode)
