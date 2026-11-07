"""Compound GCC commands through real broker/worker compilation and linking."""
import os
from pathlib import Path
import subprocess
import unittest

from test_broker import BrokerTestCase


class MultiSourceTest(BrokerTestCase):
    def setUp(self):
        super().setUp()
        self.start_broker(hosts=' '.join(self.host(i, ',tcp') for i in range(len(self.ports))))
        library = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if not library:
            directory = subprocess.check_output(['gcc', '-print-file-name=plugin'], text=True).strip()
            library = str(Path(directory) / 'distcc_gcc_scan.so')
        self.env = dict(self.env,
                        DISTCC_GCC_SCAN_LIBRARY=library,
                        PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        self.first = self.root / 'first.cpp'
        self.last = self.root / 'last.cpp'
        self.first.write_text('int value() {return 42;}\n')
        self.last.write_text('int value(); int main() {return value() != 42;}\n')
        self.output = self.root / 'compound'

    def run_compound(self, *options, pump=False):
        command = [str(self.build / 'distcc'), 'g++', '-pipe', *options,
                   str(self.first), str(self.last), '-o', str(self.output)]
        if pump:
            command.insert(0, str(self.build / 'pump'))
        return subprocess.run(command, env=self.env, cwd=self.root,
                              text=True, capture_output=True, timeout=90)

    def test_compiles_sources_remotely_then_links(self):
        def completed():
            return sum(int(line.split()[1]) for port in self.stats_ports
                       for line in self.get_stats(port).splitlines()
                       if line.startswith('dcc_compile_ok '))
        before = completed()
        result = self.run_compound('-O2', '-Wl,--as-needed')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)
        after = completed()
        self.assertEqual(after - before, 2, result.stderr)

    def test_dependency_publication_matches_native_driver(self):
        dependency = self.root / 'compound.d'
        options = ['-MMD', '-MF', str(dependency), '-MQ', 'target with spaces']
        command = ['g++', *options, str(self.first), str(self.last), '-o', str(self.output)]
        subprocess.run(command, cwd=self.root, check=True)
        expected = dependency.read_bytes()
        dependency.unlink()
        result = self.run_compound(*options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(dependency.read_bytes(), expected)

    def test_pump_compound_command(self):
        self.env['TMPDIR'] = str(self.root)
        result = self.run_compound('-O2', pump=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('gave up analyzing', result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)

    def test_default_dependency_names_and_option_operands(self):
        include = self.work / 'include.cpp'
        include.mkdir()
        dependency = self.output.with_suffix('.d')
        command = ['g++', '-MD', '-I', str(include), str(self.first), str(self.last),
                   '-o', str(self.output)]
        subprocess.run(command, cwd=self.root, check=True)
        expected = dependency.read_bytes()
        dependency.unlink()
        result = self.run_compound('-MD', '-I', str(include))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(dependency.read_bytes(), expected)

    def test_response_file_and_language_switches(self):
        self.first.write_text('int value(void) {return 42;}\n')
        self.last.write_text('extern "C" int value(void); int main(){return value()!=42;}\n')
        response = self.work / 'arguments'
        response.write_text('-x c "' + str(self.first) + '" -x c++ "' + str(self.last) +
                            '" -o "' + str(self.output) + '"')
        result = subprocess.run([self.build / 'distcc', 'g++', '@' + str(response)],
                                env=self.env, cwd=self.root, text=True,
                                capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)

    def test_compiler_error_does_not_replace_existing_executable(self):
        self.first.write_text('#error expected compound failure\n')
        self.output.write_bytes(b'existing executable')
        result = self.run_compound()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected compound failure', result.stderr)
        self.assertEqual(self.output.read_bytes(), b'existing executable')


if __name__ == '__main__':
    unittest.main()
