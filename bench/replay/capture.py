"""Host-side observer: task exit aggregates, sampled callers, and TCP packets."""
import argparse
import json
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import time


def clock():
    return dict(wall_ns=time.time_ns(), mono_ns=time.monotonic_ns())


def perf_arguments(output, *, call_graph='fp', cgroup=None, callers=False):
    events = ['-e', 'cpu-clock']
    if callers:
        events += ['-e', 'context-switches']
    args = ['perf', 'record', '--clockid', 'mono', '-F', '49', '--call-graph', call_graph, *events,
            '--no-bpf-event', '--buildid-mmap', '--no-buildid-cache',
            '-m', '1024', '--proc-map-timeout', '50', '-a']
    if cgroup:
        args += ['-G', cgroup]
    return [*args, '-o', str(output)]


def capture(output, command, call_graph, cgroup=None, pass_fds=()):
    subprocess.run([*perf_arguments(output, call_graph=call_graph,
                                   cgroup=cgroup), '--', *command],
                   pass_fds=pass_fds, check=True)


def attributes(path):
    text = subprocess.check_output(['perf', 'evlist', '-v', '-i', str(path)], text=True)
    path.with_suffix('.events.txt').write_text(text)
    events = {}
    for name in ('cpu-clock', 'context-switches'):
        line = next(line for line in text.splitlines() if line.startswith(name+':'))
        if 'freq: 1' not in line or 'sample_freq }: 49,' not in line:
            raise RuntimeError(f'invalid sampling frequency: {line}')
        if any(field not in line for field in ('TID', 'TIME', 'CPU', 'CALLCHAIN')):
            raise RuntimeError(f'incomplete sample fields: {line}')
        events[name] = line
    if 'sched:sched_' in text:
        raise RuntimeError('continuous scheduler tracing is not part of this observer')
    return events


class Observer:
    def __init__(self, directory, config, role):
        self.directory, self.host, self.config, self.role = directory, config, config['capture'], role
        self.processes, self.files = [], []
        self.packet = None
        self.delay_previous = None

    def record(self, args, name, perf=True):
        log = (self.directory / (name+'.log')).open('w')
        self.files.append(log)
        command = [*args, '--', sys.executable, '-u', '-c',
                   'import sys;print("ready",flush=True);sys.stdin.read()'] if perf else args
        child = subprocess.Popen(command,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
        self.processes.append(child)
        assert child.stdout is not None
        if child.stdout.readline().strip() != 'ready':
            raise RuntimeError(f'{name} did not reach its recording command')

    def start(self):
        d, c = self.directory, self.config
        d.mkdir(parents=True, exist_ok=True)
        self.before = clock()
        self.cpu_before = time.process_time()
        self.usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
        if self.role == 'worker':
            group = subprocess.check_output(['systemctl', 'show', self.host['service'],
                                              '-p', 'ControlGroup', '--value'], text=True).strip()
            seeds = [int(x) for x in (Path('/sys/fs/cgroup')/group.lstrip('/')/'cgroup.procs').read_text().split()]
            (d/'target-pids.json').write_text(json.dumps(seeds))
            setting = Path('/proc/sys/kernel/task_delayacct')
            self.delay_previous = setting.read_text()
            setting.write_text('1\n')
            self.record([str(Path(__file__).with_name('taskstats')), str(d/'tasks.jsonl'),
                         Path('/sys/devices/system/cpu/online').read_text().strip()], 'taskstats', perf=False)
            self.record(perf_arguments(d/'samples.data', callers=True), 'samples')
        packet = (d/'packets.pcap').open('wb')
        log = (d/'packets.log').open('w')
        self.files += [packet, log]
        filt = f"host {c['peer']} and tcp and (port {c['port']} or port {c['stats_port']})"
        tool = c['packet_tool']
        if Path(tool).name == 'dumpcap':
            args = [tool, '-i', c['interface'], '-F', 'pcap', '-s', '0', '-w', '-', '-f', filt]
        elif Path(tool).name == 'tcpdump':
            # IPv4's maximum datagram plus Ethernet/VLAN headers. In immediate
            # mode the default 262144-byte snapshot wastes RX-ring slots even
            # for small packets; a 16-MiB ring retains a burst while scheduled out.
            args = [tool, '-i', c['interface'], '-U', '--immediate-mode', '--time-stamp-precision=nano',
                    '-s', '65553', '-B', '16384', '-w', '-', filt]
        else:
            raise ValueError('packet_tool must be dumpcap or tcpdump')
        # Open the output before the capture tool drops privileges. Verify
        # its real header, not the preliminary "Capturing on" log message.
        self.packet = subprocess.Popen(args, stdout=packet, stderr=log)
        deadline = time.monotonic()+5
        while (d/'packets.pcap').stat().st_size < 24:
            if self.packet.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('packet observer failed before writing a pcap header')
            time.sleep(.02)
        with (d/'packets.pcap').open('rb') as source:
            if source.read(4) not in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):
                raise RuntimeError('capture is not little-endian pcap')

    def stop(self):
        try:
            self.finish()
        finally:
            if self.delay_previous is not None:
                Path('/proc/sys/kernel/task_delayacct').write_text(self.delay_previous)

    def finish(self):
        failures = []
        for child in self.processes:
            if child.poll() is None:
                child.stdin.close()
            if child.wait(timeout=15):
                failures.append('accounting/sample recorder failed')
            child.stdout.close()
        if self.packet:
            self.packet.send_signal(signal.SIGINT)
            if self.packet.wait(timeout=10):
                failures.append('packet capture failed')
        for output in self.files:
            output.close()
        d = self.directory
        (d/'clocks.json').write_text(json.dumps([self.before, clock()]))
        if failures:
            raise RuntimeError('; '.join(failures))
        if self.role == 'worker':
            attributes(d/'samples.data')
            if not (d/'tasks.jsonl').stat().st_size:
                raise RuntimeError('no exit accounting records received')
        # A header alone is readiness, not successful packet observation.
        if (d/'packets.pcap').stat().st_size <= 24:
            raise RuntimeError('observer captured no actual packets')
        text = (d/'packets.log').read_text()
        dropped = (re.findall(r'(\d+) packets dropped by kernel', text)
                   if Path(self.config['packet_tool']).name == 'tcpdump' else
                   re.findall(r'Packets received/dropped on interface .*?: \d+/(\d+)', text))
        if not dropped or any(int(n) for n in dropped):
            raise RuntimeError('packet capture dropped events')
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        (d/'observer-cost.json').write_text(json.dumps(dict(
            observer_cpu_seconds=time.process_time()-self.cpu_before,
            children_cpu_seconds=(usage.ru_utime+usage.ru_stime-
                                  self.usage_before.ru_utime-self.usage_before.ru_stime),
            children_read_bytes=(usage.ru_inblock-self.usage_before.ru_inblock)*512,
            children_write_bytes=(usage.ru_oublock-self.usage_before.ru_oublock)*512,
            output_bytes={p.name:p.stat().st_size for p in d.iterdir() if p.suffix in ('.data','.jsonl','.pcap')},
            scope='Whole observer lifetime; excludes hook costs charged to measured tasks.'), indent=2))
        for path in d.iterdir():
            path.chmod(0o644)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('role', choices=('client', 'worker'))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    observer = Observer(args.directory, json.loads(args.config.read_text())[args.role], args.role)
    try:
        observer.start()
        print('ready', flush=True)
        sys.stdin.read()
    finally:
        observer.stop()
