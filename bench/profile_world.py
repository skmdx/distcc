"""Record UNIT STACK OUTPUT, --report, or --roles."""
import os
from pathlib import Path
import subprocess
import sys
from replay.capture import capture

with os.fdopen(os.memfd_create('distcc-world-profile', os.MFD_CLOEXEC), 'w+b') as data:
    fd = data.fileno()
    if len(sys.argv) > 1 and sys.argv[1] in ('--report', '--roles'):
        subprocess.run(['zstd', '-q', '-d', '-c', sys.argv[2]], stdout=data, check=True)
        data.flush()
        if sys.argv[1] == '--roles':
            data.seek(0)
            with subprocess.Popen(['perf', 'script', '-i', f'/proc/self/fd/{fd}',
                    '--max-stack', '128', '--show-mmap-events', '--show-task-events',
                    '-F', 'pid,tid,time,period,event,ip,sym,dso'],
                    pass_fds=(fd,), stdout=subprocess.PIPE) as events:
                assert events.stdout is not None
                try:
                    with Path(sys.argv[3]).open('w') as output:
                        subprocess.run([sys.executable, str(Path(__file__).with_name('profile_roles.py')), '-'],
                                       stdin=events.stdout, stdout=output, check=True)
                finally:
                    events.stdout.close()
                if events.wait():
                    raise RuntimeError('perf event decoding failed')
            raise SystemExit(0)
        for label, options in (
                ('domains', ['--sort', 'comm,dso', '--no-children']),
                ('functions', ['--sort', 'comm,symbol', '--children']),
                ('callers', ['--sort', 'comm,symbol', '--children',
                             '--call-graph', 'graph,0.5,callee',
                             '--symbol-filter', '__arm64_sys_']),
                ('environment', ['--sort', 'comm,symbol', '--no-children',
                                 '--call-graph', 'graph,0.5,caller',
                                 '--symbol-filter', 'getenv'])):
            data.seek(0)
            with Path(sys.argv[3] + '-perf-' + label + '.txt').open('w') as report:
                subprocess.run(['perf', 'report', '--stdio', '--percent-limit', '0.1',
                                '-i', f'/proc/self/fd/{fd}', *options], pass_fds=(fd,),
                               stdout=report, check=True)
        raise SystemExit(0)
    stack_bytes = sys.argv[2] if len(sys.argv) > 2 else '4096'
    call_graph = 'fp' if stack_bytes == 'fp' else 'dwarf,' + stack_bytes
    capture(f'/proc/self/fd/{fd}', ['sleep', '30'], call_graph,
            cgroup='system.slice/' + sys.argv[1], pass_fds=(fd,))
    data.seek(0)
    with Path(sys.argv[3]).open('wb') as output:
        subprocess.run(['zstd', '-q', '-T1', '-c'], stdin=data, stdout=output, check=True)
