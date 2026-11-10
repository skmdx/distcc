"""Attribute a saved perf script event stream using executable maps and forks."""
import collections
import json
from pathlib import Path
import re
import sys

processes = {}
totals = collections.Counter()
symbols = collections.defaultdict(collections.Counter)
callers = collections.defaultdict(collections.Counter)
samples = collections.Counter()
planner_images = collections.Counter()
loader_samples = collections.defaultdict(collections.Counter)
pending = None
for line in (sys.stdin if sys.argv[1] == '-' else Path(sys.argv[1]).open()):
    if not line.strip() or 'PERF_RECORD_' in line:
        pending = None
    if 'PERF_RECORD_EXIT(' in line:
        # A sample can lack an IP at task exit. Its period is still counted,
        # but the following task event is not a stack frame for that sample.
        # Keep the identity for kernel teardown samples after EXIT; FORK or
        # exec replaces it if the PID is reused.
        pending = None
    elif 'PERF_RECORD_FORK(' in line:
        match = re.search(r'FORK\((-?\d+):\d+\):\((-?\d+):\d+\)', line)
        if match and match[1] != match[2]:
            parent = processes.get(int(match[2]), {})
            processes[int(match[1])] = dict(parent, birth='resident-fork' if parent.get('planner') else 'fork')
    elif 'PERF_RECORD_COMM exec:' in line:
        match = re.search(r'COMM exec: (.+):(-?\d+)/\d+', line)
        if match:
            processes[int(match[2])] = {'comm': match[1], 'birth': 'exec'}
    elif 'PERF_RECORD_MMAP2' in line:
        match = re.search(r'MMAP2 (\d+)/\d+:.*: r-xp (.+)$', line)
        if match:
            process = processes.setdefault(int(match[1]), {})
            name = Path(match[2].strip()).name
            if name == 'gcc_plan.so':
                process['planner'] = True
                planner_images[(process.get('exe', 'unknown'), process.get('birth', 'initial-map'))] += 1
            elif name in ('distcc', 'distccd', 'cc1', 'cc1plus', 'gmake', 'as',
                          'python3.14', 'bash', 'gawk', 'm4', 'sed', 'rm',
                          'mkdir', 'bison', 'mv') or name.endswith(('-gcc', '-g++')):
                process['exe'] = name
    elif 'PERF_RECORD_COMM:' in line:
        match = re.search(r'COMM: (.+):(-?\d+)/\d+', line)
        if match:
            processes.setdefault(int(match[2]), {})['comm'] = match[1]
    elif re.search(r'cpu-clock(?:/[^/]+/)?:', line):
        match = re.match(r'\s*(-?\d+)/-?\d+\s+[\d.]+:\s+(\d+) cpu-clock(?:/[^/]+/)?:', line)
        if not match:
            raise ValueError(line)
        process = processes.get(int(match[1]), {})
        role = process.get('exe', 'unknown')
        if role in ('cc1', 'cc1plus'):
            role += '-planner' if process.get('planner') else '-compiler'
        elif role == 'distcc':
            role += '-wrapper' if process.get('comm') == 'distcc' else '-client'
        period = int(match[2])
        totals[role] += period
        samples[role] += 1
        pending = role, period, set(), process.get('birth', 'initial-map')
    elif pending and line.strip():
        match = re.match(r'\s*[0-9a-f]+\s+(.+) \((.+)\)\s*$', line)
        if not match:
            raise ValueError(line)
        role, period, seen, birth = pending
        symbol, dso = match.groups()
        if not seen:
            symbols[role][symbol + ' (' + dso + ')'] += period
            if Path(dso).name.startswith('ld-linux-'):
                loader_samples[role][birth] += period
        if symbol not in seen and symbol.startswith(('dcc_', '__arm64_sys_')):
            callers[role][symbol] += period
        seen.add(symbol)

total = sum(totals.values())
print(json.dumps({'cpu_seconds': total/1e9,
    'planner_images': [{'executable': exe, 'birth': birth, 'mappings': count}
                       for (exe, birth), count in planner_images.items()], 'roles': {
    role: {'cpu_seconds': value/1e9, 'percent': 100*value/total,
           'samples': samples[role],
           'loader_leaf_seconds_by_birth': {birth: value/1e9 for birth, value in loader_samples[role].items()},
           'hotspots': {name: time/1e9 for name, time in symbols[role].most_common(12)},
           'callers': {name: time/1e9 for name, time in callers[role].most_common()}}
    for role, value in totals.most_common()}}, indent=2))
