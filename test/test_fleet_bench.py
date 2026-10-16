"""Do not promote incomplete or incomparable measurements to speedup evidence."""
import copy
from pathlib import Path
import tempfile
import unittest
import os
import shutil
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
import fleet
import measure
sys.path.pop(0)


class ComparisonTest(unittest.TestCase):
    def setUp(self):
        self.before = dict(id='before', success=True, condition_key='same', seconds={'mean': 10})
        self.after = dict(id='after', success=True, condition_key='same', seconds={'mean': 8})

    def test_complete_comparable_builds(self):
        self.assertEqual(fleet.comparison(self.after, self.before)['change_percent'], -20)

    def test_failed_build_not_compared(self):
        self.after['success'] = False
        self.assertFalse(fleet.comparison(self.after, self.before)['eligible'])

    def test_changed_conditions_not_compared(self):
        self.after['condition_key'] = 'other jobs or compiler'
        self.assertFalse(fleet.comparison(self.after, self.before)['eligible'])

    def test_first_measurement_is_not_a_speedup(self):
        self.assertFalse(fleet.comparison(self.after, None)['eligible'])

    def test_repetitions_require_the_same_conditions(self):
        runs: list[dict] = [dict(conditions={'makeopts': 'auto'}, workers={}, elapsed=10, success=True,
                                logs={'ninja_jobs': [24, 25]}),
                           dict(conditions={'makeopts': 'auto'}, workers={}, elapsed=8, success=True,
                                logs={'ninja_jobs': [24]})]
        summary = fleet.summarize('repeat', runs, {'repetitions': 2})
        self.assertTrue(summary['success'])
        self.assertEqual(summary['seconds']['mean'], 9)
        runs[1]['conditions']['makeopts'] = '-j25'
        self.assertFalse(fleet.summarize('repeat', runs, {'repetitions': 2})['success'])


class StatisticsTest(unittest.TestCase):
    def test_collection_memory_limits_are_not_hidden_by_successful_commands(self):
        before = {'memory.events': 'max 0\noom 0\noom_kill 0\n'}
        self.assertFalse(fleet.memory_limited(before, before))
        for key in ('max', 'oom', 'oom_kill'):
            with self.subTest(key=key):
                after = {'memory.events': before['memory.events'].replace(key + ' 0', key + ' 1')}
                self.assertTrue(fleet.memory_limited(before, after))

    def test_release_artifacts_preserves_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'nested').mkdir()
            data = bytes(range(256)) * 4096
            path = root / 'nested' / 'data'
            path.write_bytes(data)
            (root / 'link').symlink_to(path)
            fleet.release_artifacts(root)
            self.assertEqual(path.read_bytes(), data)
            self.assertTrue((root / 'link').is_symlink())

    def test_corrupt_profile_is_rejected_even_when_perf_exits_successfully(self):
        self.assertFalse(measure.profile_valid('', "failed to process sample\n# Samples: 100 of event 'cpu-clock'\n"))
        self.assertFalse(measure.profile_valid('failed to write perf data, error: bad address', '# Samples: 100\n'))
        self.assertFalse(measure.profile_valid('', '# Samples: 0\n'))
        self.assertTrue(measure.profile_valid('', "# Samples: 100 of event 'cpu-clock'\n"))
    def test_unavailable_io_remains_unknown(self):
        result = fleet.resource_usage({'cpu.stat': 'usage_usec 2000000\n', 'io.stat': None})
        self.assertEqual(result['cpu_seconds'], 2)
        self.assertIsNone(result['read_bytes'])
        self.assertIsNone(result['write_bytes'])

    def test_device_without_counters_remains_unknown(self):
        result = fleet.resource_usage({'cpu.stat': 'usage_usec 2000000\n',
                                       'io.stat': '251:0 \n8:0 rbytes=30 wbytes=60\n'})
        self.assertIsNone(result['read_bytes'])
        self.assertIsNone(result['write_bytes'])

    def test_io_is_aggregated_across_devices_as_a_delta(self):
        before = {'cpu.stat': 'usage_usec 1000000\n', 'io.stat': '1:1 rbytes=10 wbytes=20\n'}
        after = {'cpu.stat': 'usage_usec 3000000\n', 'io.stat': '1:1 rbytes=30 wbytes=60\n2:1 rbytes=7 wbytes=8\n'}
        self.assertEqual(fleet.resource_usage(after, before), dict(cpu_seconds=2, read_bytes=27, write_bytes=48))

    def rows(self, values):
        return [dict(hosts={'worker': dict(dcc_compile_ok=str(n), dcc_busy='2', dcc_queued='0')}) for n in values]

    def test_counter_reset_midway_is_not_hidden_by_later_jobs(self):
        summary = measure.summarize_samples(self.rows([10, 20, 0, 30]), ['worker'])['worker']
        self.assertTrue(summary['counter_reset'])

    def test_missing_statistics_not_zero_activity(self):
        rows = self.rows([10, 20])
        rows.insert(1, dict(hosts={'worker': {'error': 'timeout'}}))
        self.assertEqual(measure.summarize_samples(rows, ['worker'])['worker']['failed_samples'], 1)

    def test_worker_restart_invalidates_measurement(self):
        before = {'worker': dict(boot='boot', service={'MainPID': '10'})}
        after: dict = copy.deepcopy(before)
        after['worker']['service']['MainPID'] = '11'
        with self.assertRaisesRegex(RuntimeError, 'restarted'):
            fleet.worker_changes(before, after)

    def test_feedback_timings_and_build_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'broker.log'
            path.write_text('job cost: key=ab execute=2.000 prepare=0.250 queue=0.125 host=worker\n'
                            'include server gave up analyzing\nninja -v -j8\n')
            result = measure.log_summary([path])
            self.assertEqual(result['phases']['prepare']['total_seconds'], .25)
            self.assertEqual(result['analysis_warnings'], 1)
            self.assertEqual(result['ninja_jobs'], [8])


class ConfigurationTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('distcc'), 'distcc required')
    def test_commented_host_file_reaches_real_parser(self):
        hosts = measure.host_list('# worker capacity\nlocalhost/2 # local\nworker.invalid/4\n')
        parsed = subprocess.check_output(['distcc', '--show-hosts'], text=True,
                                         env=dict(os.environ, DISTCC_HOSTS=hosts))
        self.assertIn('localhost', parsed)
        self.assertIn('worker.invalid', parsed)

    def test_relocated_profile_and_private_internal_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'etc/portage'
            source.mkdir(parents=True)
            (root / 'profiles').mkdir()
            (root / 'profiles/parent').write_text('profile\n')
            (source / 'make.profile').symlink_to('../../profiles')
            (source / 'flags').write_text('flags\n')
            (source / 'local').symlink_to('flags')
            destination = root / 'relocated/portage'
            destination.parent.mkdir()
            measure.copy_configuration(source, destination)
            self.assertEqual((destination / 'make.profile/parent').read_text(), 'profile\n')
            (destination / 'flags').write_text('private\n')
            self.assertEqual((destination / 'local').read_text(), 'private\n')
            self.assertEqual((source / 'flags').read_text(), 'flags\n')


@unittest.skipUnless(os.environ.get('DISTCC_BENCH_PERF'), 'set DISTCC_BENCH_PERF for real perf recording')
class PerfTest(unittest.TestCase):
    def test_memfd_record_report_archive_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            fd = os.memfd_create('distcc-bench-test')
            self.addCleanup(os.close, fd)
            config = {'executable': os.environ['DISTCC_BENCH_PERF']}
            with (out / 'perf-record.log').open('w') as log:
                subprocess.run([config['executable'], 'record', '--buildid-mmap', '--no-buildid-cache',
                    '-e', 'cpu-clock', '-F', '49', '--call-graph', 'dwarf,4096', '-o', f'/proc/self/fd/{fd}',
                    '--', sys.executable, '-c', 'sum(i*i for i in range(10000000))'],
                    pass_fds=(fd,), stdout=log, stderr=subprocess.STDOUT, check=True)
            self.assertTrue(measure.finish_profile(config, out, fd))
            os.lseek(fd, 0, os.SEEK_SET)
            raw = os.read(fd, os.fstat(fd).st_size)
            self.assertEqual(subprocess.check_output(['zstd', '-dc', out / 'perf.data.zst']), raw)
            os.ftruncate(fd, 16)
            os.lseek(fd, 0, os.SEEK_SET)
            os.write(fd, b'not a perf file!\n')
            self.assertFalse(measure.finish_profile(config, out, fd))


if __name__ == '__main__':
    unittest.main()
