"""Kernel-derived service ceilings, without host-size or percentage guesses."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('service_memory', Path(__file__).resolve().parents[1] /
                                            'packaging/systemd/distccd-memory.py')
assert spec is not None and spec.loader is not None
memory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(memory)
M = 1024**2


class ServiceMemoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.group = self.root / 'outer.slice' / 'inner.slice' / 'worker.service'
        self.group.mkdir(parents=True)
        for parent in (self.group.parent, self.group.parent.parent):
            (parent / 'memory.max').write_text('max')
        (self.group / 'memory.current').write_text(str(200 * M))
        (self.group / 'memory.stat').write_text(f'anon {40*M}\nshmem {10*M}\ninactive_file {100*M}\n')
        self.meminfo = self.root / 'meminfo'
        self.meminfo.write_text('MemTotal: 1048576 kB\nMemAvailable: 614400 kB\n')

    def sample(self):
        return memory.snapshot(self.group, self.root, self.meminfo)

    def test_file_cache_is_not_counted_twice(self):
        self.assertEqual(self.sample()['limit'], 650 * M)

    def test_ancestor_accounts_for_siblings_and_own_charge(self):
        parent = self.group.parent.parent
        (parent / 'memory.max').write_text(str(800*M))
        (parent / 'memory.current').write_text(str(700*M))
        self.assertEqual(self.sample()['limit'], 300*M)

    def test_host_budget_remains_binding(self):
        parent = self.group.parent
        (parent / 'memory.max').write_text(str(1024*M))
        (parent / 'memory.current').write_text(str(300*M))
        self.assertEqual(self.sample()['limit'], 650*M)

    def test_released_memory_is_available_at_next_application(self):
        self.assertEqual(self.sample()['limit'], 650*M)
        self.meminfo.write_text('MemTotal: 1048576 kB\nMemAvailable: 409600 kB\n')
        self.assertEqual(self.sample()['limit'], 450*M)
        self.meminfo.write_text('MemTotal: 1048576 kB\nMemAvailable: 819200 kB\n')
        self.assertEqual(self.sample()['limit'], 850*M)

    def test_pressure_must_not_force_a_live_oom(self):
        self.meminfo.write_text('MemTotal: 1048576 kB\nMemAvailable: 153600 kB\n')
        with self.assertRaises(RuntimeError):
            self.sample()

    def test_exhausted_parent_fails_closed(self):
        parent = self.group.parent
        (parent / 'memory.max').write_text(str(100*M))
        (parent / 'memory.current').write_text(str(400*M))
        with self.assertRaises(RuntimeError):
            self.sample()

    def test_missing_accounting_fails_closed(self):
        self.meminfo.write_text('MemTotal: 1048576 kB\n')
        with self.assertRaises(KeyError):
            self.sample()

    def test_rounds_down_to_pages(self):
        (self.group / 'memory.stat').write_text('anon 1\nshmem 1\n')
        page = os.sysconf('SC_PAGESIZE')
        self.assertEqual(self.sample()['limit'], (600*M + 2) // page * page)


if __name__ == '__main__':
    unittest.main()
