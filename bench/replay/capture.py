"""Host-side observer: exact scheduler states, sampled callers, and TCP packets."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time


def clock():
    return dict(wall_ns=time.time_ns(), mono_ns=time.monotonic_ns())


def perf_arguments(output, *, scheduler=False, call_graph='fp', cgroup=None):
    # Do not apply -F or a global callgraph to scheduler tracepoints. Their
    # continuous state stream must have period=1 and no per-switch stack dump.
    events = ['-e', f'cpu-clock/freq={99 if scheduler else 49},call-graph={call_graph}/']
    if scheduler:
        events += ['-e', 'sched:sched_switch', '-e', 'sched:sched_wakeup']
    args = ['perf', 'record', '--clockid', 'mono', '-c', '1', *events,
            '--no-bpf-event', '--buildid-mmap', '--no-buildid-cache',
            '-m', '1024', '--proc-map-timeout', '50', '-a']
    if cgroup:
        args += ['-G', cgroup]
    return [*args, '-o', str(output)]


def capture(output, command, call_graph, cgroup=None, scheduler=False, pass_fds=()):
    subprocess.run([*perf_arguments(output, scheduler=scheduler, call_graph=call_graph,
                                   cgroup=cgroup), '--', *command],
                   pass_fds=pass_fds, check=True)


def attributes(path, sampled=False):
    text = subprocess.check_output(['perf', 'evlist', '-v', '-i', str(path)], text=True)
    path.with_suffix('.events.txt').write_text(text)
    events = {}
    for name in ('sched:sched_switch',) if sampled else ('sched:sched_switch', 'sched:sched_wakeup'):
        line = next(line for line in text.splitlines() if line.startswith(name+':'))
        if ('freq: 1' in line) != sampled or f'sample_freq }}: {99 if sampled else 1},' not in line:
            raise RuntimeError(f'invalid scheduler period: {line}')
        if any(field not in line for field in ('TID', 'TIME', 'CPU', 'RAW')):
            raise RuntimeError(f'incomplete scheduler fields: {line}')
        if ('CALLCHAIN' in line) != sampled:
            raise RuntimeError(f'invalid scheduler callchain policy: {line}')
        events[name] = line
    if not sampled:
        cpu = next(line for line in text.splitlines() if line.startswith('cpu-clock/'))
        if 'freq: 1' not in cpu or 'CALLCHAIN' not in cpu:
            raise RuntimeError('CPU samples must contain callers and use frequency sampling')
    return events


class Observer:
    def __init__(self, directory, config):
        self.directory, self.config = directory, config
        self.processes, self.files = [], []
        self.packet = None

    def record(self, args, name):
        log = (self.directory / (name+'.log')).open('w')
        self.files.append(log)
        child = subprocess.Popen([*args, '--', sys.executable, '-u', '-c',
                                  'import sys;print("ready",flush=True);sys.stdin.read()'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
        self.processes.append(child)
        assert child.stdout is not None
        if child.stdout.readline().strip() != 'ready':
            raise RuntimeError(f'{name} did not reach its recording command')

    def start(self):
        d, c = self.directory, self.config
        d.mkdir(parents=True, exist_ok=True)
        self.before = clock()
        self.record(perf_arguments(d/'states.data', scheduler=True), 'states')
        # Caller samples are a separate stream; they never supply durations
        # or interpolate missing transitions in the exact state stream.
        self.record(['perf', 'record', '--clockid', 'mono', '-a', '-F', '99',
                     '-e', 'sched:sched_switch', '--call-graph', 'fp',
                     '--no-bpf-event', '--no-buildid-cache', '-m', '256',
                     '-o', str(d/'callers.data')], 'callers')
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
        failures = []
        for child in self.processes:
            if child.poll() is None:
                child.stdin.close()
            if child.wait(timeout=15):
                failures.append('perf failed')
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
        attributes(d/'states.data')
        attributes(d/'callers.data', sampled=True)
        # A header alone is readiness, not successful packet observation.
        if (d/'packets.pcap').stat().st_size <= 24:
            raise RuntimeError('observer captured no actual packets')
        text = (d/'packets.log').read_text()
        dropped = (re.findall(r'(\d+) packets dropped by kernel', text)
                   if Path(self.config['packet_tool']).name == 'tcpdump' else
                   re.findall(r'Packets received/dropped on interface .*?: \d+/(\d+)', text))
        if not dropped or any(int(n) for n in dropped):
            raise RuntimeError('packet capture dropped events')
        for path in d.iterdir():
            path.chmod(0o644)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('role', choices=('client', 'worker'))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    observer = Observer(args.directory, json.loads(args.config.read_text())[args.role]['capture'])
    try:
        observer.start()
        print('ready', flush=True)
        sys.stdin.read()
    finally:
        observer.stop()
