"""Replay saved compiler invocations through the configured distcc frontend."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def digest(path):
    with open(path, 'rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def replay(directory):
    commands = json.loads((directory / 'commands.json').read_text())
    config = json.loads((directory / 'config.json').read_text())['client']
    resolved = {name: shutil.which(name) for name in
                {'distcc', 'pump', *(c['argv'][0] for c in commands)}}
    for name, expected in config['resolved'].items():
        if resolved.get(name) != expected:
            raise RuntimeError(f'PATH resolves {name} to {resolved.get(name)}, expected {expected}')
    identity = {path: digest(path) for path in config['binaries']}
    if identity != config['binaries']:
        raise RuntimeError('client executable differs from the selected observation target')
    (directory / 'identity.json').write_text(json.dumps(
        dict(resolved=resolved, binaries=identity, path=os.environ['PATH']), indent=2))
    jobs = int(subprocess.check_output(['distcc', '-j'], text=True))
    cpu_before = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
    start = time.time()
    monotonic = time.monotonic()

    def run(item):
        index, command = item
        output = command['output']
        destination = str(directory / f'output-{index}.o')
        argv = [s.replace(output, destination) if s in (output, output+'.dt') else s
                for s in command['argv']]
        began = time.time()
        with (directory / f'job-{index}.log').open('w') as log:
            result = subprocess.run(argv, cwd=command['cwd'], stdout=log, stderr=log)
        return dict(job=index, ready=start, start=began, end=time.time(),
                    returncode=result.returncode,
                    output_match=result.returncode == 0 and digest(destination) == digest(output))

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        results = list(executor.map(run, enumerate(commands)))
    end = time.time()
    cpu_after = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
    cpu_seconds = [(b-a)/os.sysconf('SC_CLK_TCK') for a,b in zip(cpu_before,cpu_after)]
    result = dict(jobs=jobs, start=start, ready_monotonic=monotonic,
                  end=end, results=results, host_cpu=dict(seconds=cpu_seconds,
                    idle_cores=cpu_seconds[3]/(end-start),
                    scope='Whole replay, all host tasks; tick-resolution counters.'))
    (directory / 'jobs.json').write_text(json.dumps(result, indent=2)+'\n')
    if any(r['returncode'] or not r['output_match'] for r in results):
        raise RuntimeError('replay failed or output differs; see jobs.json and job logs')
    print(json.dumps(dict(commands=len(results), jobs=jobs, wall_seconds=end-start)))


if __name__ == '__main__':
    replay(Path(sys.argv[1]).resolve())
