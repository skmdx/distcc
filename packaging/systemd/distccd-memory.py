#!/usr/bin/env python3
"""Set a finite service ceiling from kernel accounting before worker startup."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def snapshot(group, root=Path('/sys/fs/cgroup'), meminfo=Path('/proc/meminfo')):
    memory = {key.rstrip(':'): int(value) * 1024
              for key, value, *_ in (line.split() for line in meminfo.read_text().splitlines())}
    current = int((group / 'memory.current').read_text())
    stat = dict(line.split() for line in (group / 'memory.stat').read_text().splitlines())
    # Anonymous and shmem pages are not in MemAvailable. Do not add file cache
    # again, or the limit could spend the same reclaimable pages twice.
    resident = min(current, int(stat['anon']) + int(stat['shmem']))
    host_budget = min(memory['MemTotal'], memory['MemAvailable'] + resident)
    ancestors = []
    budget = host_budget
    parent = group.parent
    while parent != root:
        maximum = (parent / 'memory.max').read_text().strip()
        if maximum != 'max':
            used = int((parent / 'memory.current').read_text())
            remaining = max(0, int(maximum) - max(0, used - current))
            ancestors.append({'maximum': int(maximum), 'current': used,
                              'budget': remaining})
            budget = min(budget, remaining)
        parent = parent.parent
    page = os.sysconf('SC_PAGESIZE')
    return {'mem_available': memory['MemAvailable'], 'resident': resident,
            'current': current, 'host_budget': host_budget,
            'ancestors': ancestors, 'limit': budget // page * page}


def constrain(measurement, previous=None):
    limit = measurement['limit']
    if previous is not None:
        # A restart, including recovery after OOM, must not enlarge the hard
        # bound. /run is cleared at boot; live admission follows free memory.
        limit = min(limit, previous)
    if limit <= measurement['current']:
        raise RuntimeError('calculated memory limit has no headroom; refusing to lower it onto live workers')
    return dict(measurement, limit=limit, previous=previous)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('unit')
    parser.add_argument('--apply', action='store_true', help='apply the computed limit; default prints only')
    args = parser.parse_args()
    if '/' in args.unit or not args.unit.endswith('.service') or args.unit.startswith('-'):
        parser.error('expected a systemd service name')
    root = Path('/sys/fs/cgroup')
    control = subprocess.check_output(['systemctl', 'show', '--value', '-p', 'ControlGroup',
                                       args.unit], text=True).strip()
    group = (root / control.lstrip('/')).resolve()
    if not control.startswith('/') or group == root or root not in group.parents:
        raise RuntimeError('service must have a cgroup v2 with memory accounting')
    state = Path('/run/distccd-memory') / args.unit
    previous = int(state.read_text()) if state.exists() else None
    result = constrain(snapshot(group, root), previous)
    if args.apply:
        state.parent.mkdir(mode=0o755, exist_ok=True)
        # Commit the ceiling before applying it so a failed start cannot retry
        # with a larger budget. Only this root-owned helper writes the state.
        temporary = state.with_suffix('.new')
        temporary.write_text(str(result['limit']) + '\n')
        temporary.replace(state)
        subprocess.run(['systemctl', 'set-property', '--runtime', args.unit,
                        'MemoryMax=' + str(result['limit']), 'MemorySwapMax=0'], check=True)
        actual = int((group / 'memory.max').read_text())
        if actual != result['limit'] or (group / 'memory.swap.max').read_text().strip() != '0':
            raise RuntimeError('systemd did not apply the calculated memory limits')
    print(json.dumps(result, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
