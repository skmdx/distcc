"""Contract tests for missing-event rejection and matched RPC analysis."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from analyze import clock_offset, frames, rpc, scheduler, records, validate_sampling
from capture import attributes
from unittest.mock import patch


class ReplayTests(unittest.TestCase):
    def test_clock_bound_uses_same_packets(self):
        key = ('client', 1, 'worker', 2)
        reverse = ('worker', 2, 'client', 1)
        client = [(10.1, key, 1, b'q'), (10.4, reverse, 1, b'r')]
        worker = [(10.0, key, 1, b'q'), (10.2, reverse, 1, b'r')]
        bound = clock_offset(client, worker, 'worker')['client_minus_worker_seconds']
        self.assertAlmostEqual(bound[0], .1)
        self.assertAlmostEqual(bound[1], .2)

    def test_inconsistent_clock_bound_rejected(self):
        with self.assertRaises(RuntimeError):
            clock_offset([], [], 'worker')

    def test_attributes_alone_do_not_prove_sampling(self):
        with self.assertRaises(RuntimeError):
            validate_sampling(100, 100)
        validate_sampling(100, 5)

    def test_lost_notice_without_event_header_rejected(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            path = Path(directory)/'events.zst'
            path.write_bytes(subprocess.check_output(['zstd','-q','-c'], input=b'LOST 12 events\n'))
            with self.assertRaises(RuntimeError):
                list(records(path))

    def test_retransmission_and_fragmented_frame(self):
        key = ('worker', 1, 'client', 2)
        frame = struct.pack('!II', 0x54000309, 3)+b'abc'
        rows = [(1., key, 20, frame[:5]), (1.1, key, 20, frame[:5]),
                (1.2, key, 25, frame[5:])]
        self.assertEqual(frames(rows), [(1.2, key, 0x54000309, 3)])

    def test_missing_packet_rejected(self):
        with self.assertRaises(RuntimeError):
            frames([(1., ('a',1,'b',2), 1, b'1234'), (2., ('a',1,'b',2), 9, b'1234')])

    def test_missing_response_rejected(self):
        with self.assertRaises(RuntimeError):
            rpc([(1., ('worker',1,'client',2), 1, struct.pack('!II',0x54000309,0))], 'worker')

    def test_scheduler_period_and_callers_are_separate(self):
        cpu = 'cpu-clock/freq=99,call-graph=fp/: freq: 1, sample_type: IP|CALLCHAIN\n'
        events = ''.join(f'{name}: {{ sample_period, sample_freq }}: 1, sample_type: TID|TIME|CPU|RAW\n'
                         for name in ('sched:sched_switch','sched:sched_wakeup'))
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            path = Path(directory)/'states.data'
            with patch('capture.subprocess.check_output', return_value=cpu+events):
                attributes(path)
            with patch('capture.subprocess.check_output', return_value=cpu+events.replace('1, sample_type','1, freq: 1, sample_type')):
                with self.assertRaises(RuntimeError):
                    attributes(path)

    def test_scheduler_missing_transitions_rejected(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            path = Path(directory)
            continuous = ('idle 0 [000] 1.000: sched:sched_switch: idle:0 [0] R ==> cc1:2 [0]\n'
                          'cc1 3 [000] 2.000: sched:sched_switch: cc1:3 [0] S ==> idle:0 [0]\n')
            for name, text in (('states', continuous), ('callers', '')):
                (path/(name+'.txt.zst')).write_bytes(subprocess.check_output(['zstd','-q','-c'], input=text.encode()))
            with self.assertRaises(RuntimeError):
                scheduler(path, 1., 3.)

    def test_full_profile_decoder_accepts_event_callgraph_terms(self):
        text = '7/7 1.0: 1000000 cpu-clock/freq=49,call-graph=fp/:\n  100 foo (compiler)\n'
        result = subprocess.check_output(['python3', str(Path(__file__).parents[1]/'profile_roles.py'), '-'], input=text, text=True)
        self.assertEqual(json.loads(result)['cpu_seconds'], .001)


if __name__ == '__main__':
    unittest.main()
