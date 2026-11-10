"""Task-accounting, sampled-caller and paired-TCP analysis for saved replay."""
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


def decode(directory):
    for name in ('samples',):
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


def task_accounting(directory, low, high, sample_low, sample_high) -> dict[str, Any]:
    rows = [json.loads(line) for line in (directory/'tasks.jsonl').read_text().splitlines()]
    by_pid = {row['pid']: row for row in rows}
    if len(by_pid) != len(rows):
        raise RuntimeError('duplicate task exit IDs; cannot resolve process ancestry')
    seeds = set(json.loads((directory/'target-pids.json').read_text()))
    selected = set()
    for pid in by_pid:
        ancestors = set()
        current = pid
        while current in by_pid and current not in ancestors and current not in seeds:
            ancestors.add(current)
            current = by_pid[current]['ppid']
        if current in seeds:
            selected.add(pid)
    service, callers, events = set(), collections.Counter(), collections.Counter()
    for row in records(directory/'samples.txt.zst'):
        if row['pid'] not in selected or not sample_low <= row['time'] <= sample_high:
            continue
        match = re.search(r'\b(cpu-clock|context-switches):', row['event'])
        if not match:
            continue
        event = match[1]
        events[event] += 1
        stack = ' '.join(row['stack'])
        if 'fuse_dev_do_read' in stack or 'libfuse' in stack:
            service.add(row['pid'])
        for name in ('fuse_lookup_name', 'fuse_chan_send', 'fuse_dev_do_read',
                     'unix_stream_read_generic', 'unix_seqpacket_recvmsg', 'do_sys_poll',
                     'do_epoll_wait', 'do_wait', 'futex_wait', 'io_schedule'):
            if name in stack:
                callers[(event, row['comm'], name)] += 1
                break
    all_roles, interior = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    interior_pids = []
    for pid in selected:
        row = by_pid[pid]
        role = 'fuse-service-sampled' if pid in service else row['comm']
        elapsed = row['elapsed_us']/1e6
        values = dict(tasks=1, cpu=row['cpu_ns']/1e9, scheduled=row['scheduled_ns']/1e9,
                      runnable=row['runnable_ns']/1e9,
                      non_cpu_residual=elapsed-(row['scheduled_ns']+row['runnable_ns'])/1e9,
                      switches=row['voluntary_switches']+row['involuntary_switches'],
                      read_bytes=row['read_bytes'], write_bytes=row['write_bytes'])
        all_roles[role].update(values)
        # Linux computes btime as floor(exit wall time) - floor(elapsed).
        # The true start differs by <1 second. Select whole lifetimes that are
        # certainly inside the pending-work window, never prorate aggregates.
        if row['birth_seconds']-1 >= low and row['birth_seconds']+1+elapsed <= high:
            interior[role].update(values)
            interior_pids.append(pid)
    switches = sum(by_pid[p]['voluntary_switches']+by_pid[p]['involuntary_switches'] for p in selected)
    validate_sampling(switches, events['context-switches'])
    if not events['cpu-clock'] or not any(by_pid[p]['comm'] in ('cc1', 'cc1plus') for p in selected):
        raise RuntimeError('worker compiler path not reached by task accounting and samples')
    return dict(scope='Completed descendant task lifetimes; persistent worker tasks are excluded.',
                pending_window_wall=[low, high], roles=all_roles, interior_roles=interior,
                interior_pids=interior_pids, target_tasks=len(selected), host_exit_records=len(rows),
                sample_counts=events, accounted_switches=switches, service_pids=sorted(service),
                sampled_callsites=[dict(event=k[0], comm=k[1], site=k[2], count=v)
                                   for k,v in callers.most_common()],
                limitations=['Non-CPU residual includes sleeping/stopped time and accounting boundary error; '
                             'it is not an exact per-callsite blocked duration.',
                             'Only FUSE-service PIDs positively identified by sampled stacks are labelled.',
                             'Interior totals use whole lifetimes and conservative one-second birth bounds. '
                             'Boundary tasks are not interpolated; an empty interior means unavailable.'])


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
    anchors = json.loads((directory/'worker/clocks.json').read_text())
    delta = sum((a['mono_ns']-a['wall_ns'])/1e9 for a in anchors)/2
    bound_low, bound_high = offset['client_minus_worker_seconds']
    result['worker'] = task_accounting(directory/'worker', low-bound_low, high-bound_high,
                                       low+delta-shift, high+delta-shift)
    result['client_host_cpu'] = jobs['host_cpu']
    result['observer_cost'] = {name: json.loads((directory/name/'observer-cost.json').read_text())
                               for name in ('client', 'worker')}
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
