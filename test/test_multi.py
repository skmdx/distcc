"""Compound GCC commands through real broker/worker compilation and linking."""
import os
from pathlib import Path
import subprocess
import unittest

from test_broker import BrokerTestCase


class MultiSourceTest(BrokerTestCase):
    def setUp(self):
        super().setUp()
        options = ',tcp,cpp,lz4' if self._testMethodName.startswith('test_pump') else ',tcp'
        self.start_broker(hosts=' '.join(self.host(i, options) for i in range(len(self.ports))))
        library = os.environ.get('DISTCC_GCC_SCAN_LIBRARY')
        if not library:
            from include_server import clang_scan
            target = subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
            version = subprocess.check_output(['gcc', '-dumpfullversion'], text=True).strip()
            library = str(Path(clang_scan.gcc_scan_dir) / target / version / 'distcc_gcc_scan.so')
        if not Path(library).is_file():
            self.skipTest('The selected GCC dependency module must be built first')
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
                   self.first.name, self.last.name, '-o', str(self.output)]
        if pump:
            command.insert(0, str(self.build / 'pump'))
        return subprocess.run(command, env=self.env, cwd=self.root,
                              text=True, capture_output=True, timeout=90)

    def completed(self):
        return sum(int(line.split()[1]) for port in self.stats_ports
                   for line in self.get_stats(port).splitlines()
                   if line.startswith('dcc_compile_ok '))

    def test_compiles_sources_remotely_then_links(self):
        before = self.completed()
        result = self.run_compound('-O2', '-Wl,--as-needed')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)
        after = self.completed()
        self.assertEqual(after - before, 2, result.stderr)

    def test_dependency_publication_matches_native_driver(self):
        dependency = self.root / 'compound.d'
        options = ['-MMD', '-MF', str(dependency), '-MQ', 'target with spaces']
        command = ['g++', *options, self.first.name, self.last.name, '-o', str(self.output)]
        subprocess.run(command, cwd=self.root, check=True)
        expected = dependency.read_bytes()
        dependency.unlink()
        result = self.run_compound(*options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(dependency.read_bytes(), expected)

    def test_pump_compound_command(self):
        self.env['TMPDIR'] = str(self.root)
        self.env['DISTCC_TESTING_INCLUDE_SERVER'] = '1'
        before = self.completed()
        result = self.run_compound('-O2', pump=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('gave up analyzing', result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)
        self.assertEqual(self.completed() - before, 2)

    def test_pump_system_header_aliases(self):
        system = self.root / 'system'
        system.mkdir()
        (system / 'x.h').write_text('#define ANSWER 42\n')
        (system / 'longer-header-name.h').symlink_to('x.h')
        self.first.write_text('#include <longer-header-name.h>\nint value(){return ANSWER;}\n')
        self.env.update(TMPDIR=str(self.root), DISTCC_TESTING_INCLUDE_SERVER='1')
        before = self.completed()
        result = self.run_compound('-isystem', str(system), pump=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)
        self.assertEqual(self.completed() - before, 2)

    def test_pump_absolute_source_parent_components(self):
        child = self.root / 'child'
        child.mkdir(exist_ok=True)
        source = str(child / '..' / self.first.name)
        self.first.write_text('const char *source_name = __FILE__;\n')
        self.last.write_text('#include <cstring>\nextern const char *source_name;\n'
                             'int main(){return std::strcmp(source_name, "' + source + '");}\n')
        self.env.update(TMPDIR=str(self.root), DISTCC_TESTING_INCLUDE_SERVER='1')
        obj = self.root / 'parent.o'
        result = subprocess.run([self.build / 'pump', self.build / 'distcc', 'g++',
                                 '-c', source, '-o', obj], cwd=self.root, env=self.env,
                                text=True, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('gave up analyzing', result.stderr)
        subprocess.run(['g++', str(self.last), str(obj), '-o', self.output], check=True)
        self.assertEqual(subprocess.call([self.output]), 0)

    def test_pump_implicit_header_is_injected_once(self):
        system = self.root / 'implicit-system'
        system.mkdir()
        (system / 'stdc-predef.h').write_text('#include "child.h"\n#define ANSWER 42\n')
        # Deliberately unguarded: injecting this transitive input again changes
        # the program, whereas replaying the root preinclude is correct.
        (system / 'child.h').write_text('struct implicit_type {};\n')
        self.first.write_text('int value(){return ANSWER;}\n')
        self.env.update(TMPDIR=str(self.root), DISTCC_TESTING_INCLUDE_SERVER='1')
        before = self.completed()
        result = self.run_compound('-isystem', str(system), pump=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('gave up analyzing', result.stderr)
        self.assertEqual(subprocess.call([self.output]), 0)
        self.assertEqual(self.completed() - before, 2)

    def test_default_dependency_names_and_option_operands(self):
        include = self.work / 'include.cpp'
        include.mkdir()
        dependency = self.output.with_suffix('.d')
        command = ['g++', '-MD', '-I', str(include), self.first.name, self.last.name,
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
