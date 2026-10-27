#!/usr/bin/env python3
"""Real LLD planning, indexed imports, distributed backends and final links."""
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from test_broker import BrokerTestCase
from test_adaptive import stats_response

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distcc_job import fingerprint, write_metadata
from distcc_lto import InputSnapshot, llvm_tools, publish_archive, work_key


@unittest.skipUnless(all(shutil.which(tool) for tool in ('clang', 'bwrap', 'sandbox')),
                     'Clang, bubblewrap and Gentoo sandbox required')
class PortageSandboxTest(unittest.TestCase):
    def test_local_job_creates_namespace_inside_portage_sandbox(self):
        import tempfile

        with tempfile.TemporaryDirectory(prefix='distcc-sandbox-') as directory:
            work = Path(directory).resolve()
            root = work / 'root'
            source, output = work / 'input.c', work / 'output.o'
            staged_source = root / str(source).lstrip('/')
            staged_source.parent.mkdir(parents=True)
            staged_source.write_text('int answer(void) { return 42; }\n')
            job = dict(cwd=str(work), inputs=[str(source)], outputs=[str(output)],
                       args=[shutil.which('clang'), '-c', '-x', 'c', str(source), '-o', str(output)])
            manifest = work / 'job.json'
            manifest.write_text(json.dumps(job))
            env = dict(os.environ, SANDBOX_WRITE=str(work), SANDBOX_LOG=str(work / 'sandbox.log'),
                       PYTHONDONTWRITEBYTECODE='1',
                       PYTHONPATH=str(Path(__file__).resolve().parents[1]))
            script = ('import json, os, sys; from distcc_lto import run_job; '
                      'before = os.environ["SANDBOX_WRITE"]; '
                      'assert os.environ["SANDBOX_ON"] == "1"; '
                      'result = run_job(sys.argv[1], json.load(open(sys.argv[2])), 4); '
                      'assert os.environ["SANDBOX_WRITE"] == before; '
                      'assert os.environ["SANDBOX_ON"] == "1"; sys.exit(result)')
            result = subprocess.run(['sandbox', sys.executable, '-c', script, str(root), str(manifest)],
                                    env=env, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater((root / str(output).lstrip('/')).stat().st_size, 0)
            self.assertFalse((work / 'sandbox.log').exists(), result.stderr)


@unittest.skipUnless(shutil.which('clang') and shutil.which('bwrap'), 'Clang and bubblewrap required')
class ThinLTOTest(BrokerTestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon_env = {'DISTCCD_PATH': str(Path.cwd()) + os.pathsep + os.environ['PATH']}
        super().setUpClass()

    def setUp(self):
        super().setUp()
        hosts = ' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}') for i in range(2))
        self.start_broker(hosts)
        self.env.update(DISTCC_VERBOSE='1', PATH=str(self.build) + os.pathsep + os.environ['PATH'])
        self.env.pop('DISTCC_LINK_SYSROOT', None)

    def run_command(self, command, **kwargs):
        try:
            return subprocess.run(list(map(str, command)), cwd=self.work, env=self.env,
                                  text=True, capture_output=True, timeout=60, **kwargs)
        except subprocess.TimeoutExpired as error:
            broker = self.work / 'broker.log'
            details = broker.read_text()[-8192:] if broker.exists() else ''
            self.fail(f'command timed out: {command}\n{error.stderr!r}\n{details}')

    def test_snapshot_identity_is_lazy_shared_and_immutable(self):
        source = self.work / 'input.bc'
        source.write_bytes(b'original input')
        expected = fingerprint(source)
        with patch('distcc_lto.fingerprint', wraps=fingerprint) as hashed:
            item = InputSnapshot(source, self.work / 'snapshot')
            self.assertEqual(item.size, source.stat().st_size)
            hashed.assert_not_called()
            source.write_bytes(b'replaced input')
            self.assertEqual(item.identity, expected)
            files = {str(source): item}
            args = ['clang', '-c', str(source)]
            job = dict(cwd=str(self.work), args=args,
                       inputs=[str(source)], outputs=[])
            key = work_key(job, files)
            args.append('-DOTHER=1')
            self.assertNotEqual(work_key(job, files), key)
            self.assertEqual(item.path.read_bytes(), b'original input')
            hashed.assert_called_once_with(item.path)

    def link(self, compiler, archive=False, hip=False):
        objects = []
        for i in range(8):
            source = self.work / f'part {i}.c'
            source.write_text(f'int part{i}(int n) {{ int v = 0; '
                              f'for (int j=0;j<n;j++) v += (j * {i+1}) % 101; return v; }}')
            obj = source.with_suffix('.o')
            result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        source = self.work / 'main.c'
        source.write_text('\n'.join(f'int part{i}(int);' for i in range(8)) +
                          '\n#include <stdio.h>\nint main(int argc, char **argv) { '
                          'printf("%d\\n", ' + '+'.join(f'part{i}(argc*23)' for i in range(8)) +
                          '); return 0; }')
        main = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', main])
        self.assertEqual(result.returncode, 0, result.stderr)
        if archive:
            library = self.work / 'library.a'
            result = self.run_command(['llvm-ar', 'rcs', library, *objects])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects = [library]
        args = [compiler, '-flto=thin', '-O2', '-fuse-ld=lld', main, *objects]
        if hip:
            args.extend(['--hip-link', '--offload-arch=gfx900'])
        reference = self.work / 'reference'
        result = self.run_command([*args, '-o', reference])
        self.assertEqual(result.returncode, 0, result.stderr)
        before = [self.get_stats(port) for port in self.stats_ports]
        output = self.work / 'distributed'
        result = self.run_command([self.build / 'distcc', *args, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        partitions = list(map(int, re.findall(r'partitioned .* into (\d+)', result.stderr)))
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'),
                         9 + sum(count - 1 for count in partitions), result.stderr)
        for port, stats in zip(self.stats_ports, before):
            def completed(text):
                return int(next(line.split()[1] for line in text.splitlines()
                                if line.startswith('dcc_compile_ok ')))
            self.assertGreater(completed(self.get_stats(port)), completed(stats))
        for arguments in ([], ['argument'], ['one', 'two']):
            expected = self.run_command([reference, *arguments])
            actual = self.run_command([output, *arguments])
            self.assertEqual((actual.returncode, actual.stdout), (expected.returncode, expected.stdout))

    def test_objects_and_imports(self):
        self.link(shutil.which('clang'))

    def test_backend_cwd_survives_private_tmp_mount(self):
        inputs = self.work / 'inputs'
        inputs.mkdir()
        source = inputs / '0'
        result = self.run_command(['clang', '-x', 'c', '-flto=thin', '-c', '-o', source, '-'],
                                  input='int main(void) { return 0; }')
        self.assertEqual(result.returncode, 0, result.stderr)
        name, output = str(self.work / 'input.bc'), str(self.work / 'output.o')
        cwd = '/tmp/client-build'
        manifest = inputs / '1'
        manifest.write_text(json.dumps(dict(cwd=cwd,
            args=[shutil.which('clang'), '-c', '-x', 'ir', name, '-o', output],
            inputs=[name], outputs=[output])))
        metadata, archive = self.work / 'metadata', self.work / 'result.tar'
        write_metadata(4, cwd, 'distcc-thinlto',
                       [(name, *fingerprint(source)),
                        ('/__distcc_lto/job.json', *fingerprint(manifest))], metadata)
        result = self.run_command([self.build / 'distcc', '--file-job', metadata, inputs, archive])
        self.assertEqual(result.returncode, 0, result.stderr)
        publish_archive(archive, [output])
        executable = self.work / 'linked'
        result = self.run_command(['clang', output, '-o', executable])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([executable]).returncode, 0)

    def test_backend_cost_identity_survives_temporary_paths(self):
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_broker(self.env['DISTCC_HOSTS'], name='cost-broker')
        self.env['PATH'] = str(self.build) + os.pathsep + os.environ['PATH']
        self.env.pop('DISTCC_LINK_SYSROOT', None)
        compiler = shutil.which('clang')
        source = self.work / 'cost.c'
        source.write_text('#include <stdio.h>\n'
                          '__attribute__((noinline)) int calculate(int n) { volatile int v=n; '
                          'for(int i=0;i<n;i++) v=(v*3+i)%101; return v; }\n'
                          'int main(int argc,char **argv) { printf("%d\\n", calculate(argc*23)); }\n')
        obj = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        args = [self.build / 'distcc', compiler, '-flto=thin', '-fuse-ld=lld', obj]
        log = self.work / 'cost-broker.log'
        identities = []
        outputs = []
        for index, level in enumerate(('-O2', '-O2', '-O0')):
            before = log.stat().st_size
            output = self.work / f'cost-{index}'
            result = self.run_command([*args, level, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('distcc-thinlto: compiled on', result.stderr)
            rows = re.findall(r'job cost: key=(\w+) kind=8 bytes=\d+ units=(\d+) predicted=([\d.]+)',
                              log.read_text()[before:])
            self.assertTrue(rows, log.read_text()[before:])
            self.assertTrue(all(int(row[1]) > 0 for row in rows), rows)
            identities.append({row[0] for row in rows})
            if index == 1:
                self.assertTrue(all(float(row[2]) > 0 for row in rows), rows)
            outputs.append(self.run_command([output, 'argument']).stdout)
        self.assertEqual(identities[0], identities[1])
        self.assertFalse(identities[1] & identities[2])
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0], outputs[2])

    def test_capacity_uses_worker_speed_without_increasing_concurrency(self):
        metrics = stats_response(cpus=4, cores=4, jobs=4, capacity=4,
                                 busy=0, queued=0, running=0, load=0)
        peer = self.peer(lambda: metrics)
        self.start_broker(self.host(0, f',tcp,stats={peer.port},speed=1') + ' ' +
                          self.host(1, f',tcp,stats={peer.port},speed=0.25'), name='capacity')
        result = self.run_command([self.build / 'distcc', '--lto-capacity'])
        self.assertEqual(result.returncode, 0, result.stderr)
        capacity = json.loads(result.stdout)
        self.assertEqual(capacity['jobs'], 8)
        self.assertAlmostEqual(capacity['work_rate'], 5)
        self.assertAlmostEqual(capacity['min_rate'], .25)
        result = self.run_command([self.build / 'distcc', '-j'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(int(result.stdout), capacity['jobs'])
        # A changed admission ceiling proves that the new snapshot reached the
        # broker. Transient load must not change its full-capacity grain rate.
        metrics = stats_response(cpus=4, cores=4, jobs=4, capacity=3,
                                 busy=0, queued=0, running=16, load=16)
        deadline = time.monotonic() + 3
        while True:
            result = self.run_command([self.build / 'distcc', '--lto-capacity'])
            self.assertEqual(result.returncode, 0, result.stderr)
            updated = json.loads(result.stdout)
            if updated['jobs'] == 6:
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.assertAlmostEqual(updated['work_rate'], 3.75)
        self.assertAlmostEqual(updated['min_rate'], .25)

    def test_dominant_function_skips_unproductive_partitioning(self):
        compiler = shutil.which('clang')
        if not llvm_tools(compiler):
            self.skipTest('matching LLVM partition helpers are not installed')
        source = self.work / 'dominant.c'
        source.write_text('__attribute__((noinline)) int heavy(int x) { volatile int v=x;\n' +
                          '\n'.join(f'v = (v * 3 + {i}) % 101;' for i in range(128)) +
                          '\nreturn v; }\n'
                          '__attribute__((noinline)) int tiny(int x) { return x+2; }\n'
                          'int main(void) { return heavy(3)==tiny(7); }\n')
        obj = source.with_suffix('.o')
        result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        native, distributed = self.work / 'native', self.work / 'distributed'
        args = [compiler, '-O2', '-flto=thin', '-fuse-ld=lld', obj]
        reference = self.run_command([*args, '-o', native])
        self.assertEqual(reference.returncode, 0, reference.stderr)
        result = self.run_command([self.build / 'distcc', *args, '-o', distributed])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('distcc-thinlto: partitioned ', result.stderr)
        self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 1, result.stderr)
        self.assertEqual(self.run_command([distributed]).returncode, self.run_command([native]).returncode)

    def test_partitioned_backend_preserves_private_symbols_tls_and_unwinding(self):
        self.check_partitioned_backend()

    @unittest.skipUnless(shutil.which('sandbox'), 'Gentoo sandbox required')
    def test_partitioned_link_inside_portage_sandbox(self):
        self.check_partitioned_backend(portage=True)

    def check_partitioned_backend(self, portage=False):
        """Execute real partitions, then compare the final link's ABI and behavior."""
        compiler = shutil.which('clang++')
        if not llvm_tools(compiler):
            self.skipTest('matching LLVM partition helpers are not installed')
        heavy = self.work / 'heavy.cc'
        definitions = '\n'.join(
            f'__attribute__((noinline)) static int local_{i}(int x) '
            f'{{ volatile int y = x; return y * {i+1} + shared; }}'
            for i in range(32))
        expression = ' + '.join(f'local_{i}(x)' for i in range(32))
        heavy.write_text('asm(".globl heavy\\n");\n'
            'static int shared=2, sequence;\n'
            '__attribute__((constructor(101))) static void first() { sequence=3; }\n'
            '__attribute__((constructor(102))) static void second() { sequence*=7; }\n'
            'static thread_local int epoch=1;\n'
            'struct Scope { ~Scope() { ++shared; } };\n' + definitions + '\n'
            'extern "C" int heavy(int x) { Scope guard; if(x<0) throw x; '
            'return sequence + epoch++ + ' + expression + '; }\n')
        small = self.work / 'small.cc'
        small.write_text('static int shared=11;\n'
            '__attribute__((noinline)) static int local_0(int x) { return x+shared; }\n'
            'extern "C" int small(int x) { return local_0(x); }\n')
        main = self.work / 'main.cc'
        main.write_text('extern "C" int printf(const char*, ...), atoi(const char*);\n'
            'extern "C" int heavy(int), small(int);\n'
            'int main(int argc,char**argv) { int x=argc>1?atoi(argv[1]):3; '
            'try { int a=heavy(x), b=heavy(x+1); printf("%d %d %d\\n",a,b,small(x)); } '
            'catch(int e) { printf("exception %d %d\\n",e,small(x)); } }\n')
        objects = []
        for source in (heavy, small, main):
            output = source.with_suffix('.o')
            result = self.run_command([compiler, '-O2', '-flto=thin', '-c', source, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(output)
        native, distributed = self.work / 'native', self.work / 'partitioned'
        args = [compiler, '-O2', '-flto=thin', '-fuse-ld=lld', *objects]
        reference = self.run_command([*args, '-o', native])
        self.assertEqual(reference.returncode, 0, reference.stderr)
        command = [self.build / 'distcc', *args, '-o', distributed]
        if portage:
            sandbox_log = self.work / 'sandbox.log'
            with patch.dict(self.env, SANDBOX_WRITE=str(self.root),
                            SANDBOX_LOG=str(sandbox_log), PYTHONDONTWRITEBYTECODE='1'):
                result = self.run_command(['sandbox', *command])
            self.assertFalse(sandbox_log.exists(), result.stderr)
        else:
            result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        counts = list(map(int, re.findall(r'partitioned .* into (\d+)', result.stderr)))
        self.assertTrue(any(count > 1 for count in counts), result.stderr)
        self.assertGreater(result.stderr.count('distcc-thinlto: compiled on'), len(objects))
        for argument in ('0', '4', '-7'):
            expected = self.run_command([native, argument])
            actual = self.run_command([distributed, argument])
            self.assertEqual((actual.returncode, actual.stdout), (expected.returncode, expected.stdout))
        symbols = []
        for path in (native, distributed):
            result = self.run_command(['llvm-nm', '-g', '--defined-only', '-j', path])
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols.append(sorted(result.stdout.splitlines()))
        self.assertEqual(*symbols)

    def test_partitions_balance_ir_work_with_mixed_symbol_linkages(self):
        for compiler in (shutil.which('clang'), '/usr/lib/llvm/22/bin/clang'):
            if compiler and Path(compiler).exists():
                with self.subTest(compiler=compiler):
                    self.check_partition_balance(compiler)

    def check_partition_balance(self, compiler):
        tools = llvm_tools(compiler)
        if not tools:
            self.skipTest('matching LLVM partition helpers are not installed')
        source = self.work / 'uneven.ll'
        definitions = []
        sizes = [60] * 9 + [3] * 18
        for index, size in enumerate(sizes):
            name = f'function_{index}'
            linkage = 'weak_odr ' if index % 3 != 2 else ''
            group = f' comdat(${name})' if index % 3 == 1 else ''
            if group:
                definitions.append(f'${name} = comdat any')
            body = [f'define {linkage}i32 @{name}(i32 %x) noinline{group} {{']
            for step in range(size):
                previous = '%x' if step == 0 else f'%v{step-1}'
                body.append(f'  %v{step} = add i32 {previous}, {index+1}')
            body.extend([f'  ret i32 %v{size-1}', '}'])
            definitions.append('\n'.join(body))
        definitions.append('@alias = alias i32 (i32), ptr @function_0')
        source.write_text('\n'.join(definitions))
        loads, identities = [], []
        objects = []
        for run in range(2):
            result = self.run_command([tools['plan'], 'split', source, '3',
                                       self.work / f'balanced-{run}-'])
            self.assertEqual(result.returncode, 0, result.stderr)
            parts = json.loads(result.stdout)['parts']
            loads.append([part['instructions'] for part in parts])
            identities.append([fingerprint(Path(part['path'])) for part in parts])
            if run == 0:
                for index, part in enumerate(parts):
                    output = self.work / f'balanced-{index}.o'
                    result = self.run_command([compiler, '-O2', '-c', part['path'], '-o', output])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    objects.append(output)
        self.assertEqual(*loads)
        self.assertEqual(*identities)
        self.assertEqual(sum(loads[0]), sum(size + 1 for size in sizes))
        self.assertLessEqual(max(loads[0]) - min(loads[0]), max(sizes) + 1)
        main = self.work / 'check.c'
        main.write_text('\n'.join(f'int function_{i}(int);' for i in range(len(sizes))) +
            '\nint alias(int);\nint main(void) { return alias(7) + ' +
            ' + '.join(f'function_{i}(7)' for i in range(len(sizes))) +
            ' != ' + str(7 + sizes[0] + sum(7 + size * (i+1)
                                           for i, size in enumerate(sizes))) + '; }\n')
        output = self.work / 'balanced'
        result = self.run_command([compiler, main, *objects, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_module_assembly_preserves_private_definitions(self):
        compiler = shutil.which('clang')
        tools = llvm_tools(compiler)
        if not tools:
            self.skipTest('matching LLVM partition helpers are not installed')
        source = self.work / 'assembly.c'
        source.write_text('asm(".local asm_value\\n.data\\nasm_value: .long 7\\n.text\\n");\n'
                          'extern int asm_value;\n'
                          '__attribute__((noinline)) int first(void) { return asm_value; }\n'
                          '__attribute__((noinline)) int second(void) { return first()+1; }\n'
                          'int main(void) { return second()!=8; }\n')
        bitcode = self.work / 'assembly.bc'
        result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command([tools['plan'], 'split', bitcode, '3', self.work / 'part'])
        self.assertEqual(result.returncode, 0, result.stderr)
        parts = json.loads(result.stdout)['parts']
        self.assertEqual(len(parts), 1)
        output = self.work / 'assembly'
        result = self.run_command([compiler, parts[0]['path'], '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_ranked_submission_survives_slow_process_start(self):
        """Use real workers while delaying the largest job before broker IPC."""
        self.base_env = dict(self.base_env, DISTCC_VERBOSE='1')
        self.start_broker(' '.join(self.host(i, f',tcp,stats={self.stats_ports[i]}')
                                  for i in range(2)), name='ranked')
        planner = self.work / 'planner'
        planner.mkdir()
        for name in ('distcc-thinlto', 'distcc_lto.py', 'distcc_job.py'):
            shutil.copy2(self.build / name, planner / name)
        proxy = planner / 'distcc'
        proxy.write_text('#!/usr/bin/env python3\nimport os, sys, time\n'
                         'if len(sys.argv) > 2 and sys.argv[2].endswith("/0/job"):\n'
                         '    time.sleep(0.2)\n'
                         'os.execv(' + repr(str(self.build / 'distcc')) + ', sys.argv)\n')
        proxy.chmod(0o755)
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        jobs, sizes, objects = [], [], []
        for index, size in enumerate((8192, 2048, 512)):
            source = self.work / f'part{index}.c'
            source.write_text(f'int value{index}(void) {{ return {index + 1}; }}\n'
                              f'const char pad{index}[] = "' + 'a' * size + '";\n')
            bitcode = source.with_suffix('.bc')
            result = self.run_command([compiler, '-emit-llvm', '-c', source, '-o', bitcode])
            self.assertEqual(result.returncode, 0, result.stderr)
            output = source.with_suffix('.o')
            objects.append(output)
            sizes.append(bitcode.stat().st_size)
            jobs.append(dict(args=['-O2', '-c', str(bitcode), '-o', str(output)],
                             inputs=[str(bitcode)], outputs=[str(output)]))
        description = self.work / 'jobs.json'
        description.write_text(json.dumps(dict(common=dict(args=[compiler], inputs=[]), jobs=jobs)))
        command = [planner / 'distcc-thinlto', description]
        result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        files = {name: InputSnapshot(name, self.work / f'snapshot-{i}')
                 for i, name in enumerate(name for job in jobs for name in job['inputs'])}
        keys = [work_key(dict(cwd=str(self.work), args=[compiler, *job['args']],
                             inputs=job['inputs'], outputs=job['outputs']), files) for job in jobs]
        assigned = [int(key, 16) for key in re.findall(r'assigning host=.* key=(\w+) ',
                                                      (self.work / 'ranked.log').read_text())]
        self.assertEqual(assigned, [key for _, key in sorted(zip(sizes, keys), reverse=True)])
        main = self.work / 'main.c'
        main.write_text('int value0(void), value1(void), value2(void);\n'
                        'int main(void) { return value0()+value1()+value2()!=6; }\n')
        result = self.run_command([compiler, main, *objects, '-o', 'check'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([self.work / 'check']).returncode, 0)
        # A child failing before enqueueing must release the startup barrier
        # and leave previously published objects intact.
        previous = [path.read_bytes() for path in objects]
        self.env.update(DISTCC_BROKER=str(self.work / 'absent.sock'), DISTCC_RECONNECT_TIMEOUT='0')
        self.assertNotEqual(self.run_command(command).returncode, 0)
        self.assertEqual([path.read_bytes() for path in objects], previous)

    def test_closed_submission_notification_cancels_without_reconnect(self):
        inputs = self.work / 'inputs'
        inputs.mkdir()
        source = inputs / '0'
        source.write_bytes(b'')
        metadata, output = self.work / 'job', self.work / 'result.tar'
        write_metadata(4, str(self.work), 'distcc-thinlto',
                       [(str(self.work / 'empty.bc'), *fingerprint(source))], metadata)
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([str(self.build / 'distcc'), '--file-job', str(metadata),
                str(inputs), str(output), str(write_fd)], env=self.env, capture_output=True,
                pass_fds=(write_fd,), timeout=3)
        finally:
            os.close(write_fd)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_hip_wrapper_distributes_host_thinlto(self):
        self.link(shutil.which('clang'), hip=True)

    def test_hip_device_links_and_host_backends(self):
        """Resolve a cross-TU GPU call on two targets, then link the host."""
        compiler = shutil.which('clang++')
        devices = []
        if shutil.which('amdgpu-arch'):
            detected = self.run_command(['amdgpu-arch'])
            if detected.returncode == 0:
                devices = detected.stdout.split()
        targets = list(dict.fromkeys(devices)) or ['gfx900', 'gfx1030']
        flags = ['-O2', '-flto=thin', '-fgpu-rdc',
                 *('--offload-arch=' + target for target in targets)]
        sources = (
            '__attribute__((device)) int value(int n) { return n + 17; }',
            '#include <hip/hip_runtime.h>\n'
            '__attribute__((device)) int value(int);\n'
            '__global__ void kernel(int *p) { *p = value(*p); }\n'
            '#include <cstdio>\n'
            'int main(int argc, char **) { if (argc == 1) return 0; '
            'int count = 0; if (hipGetDeviceCount(&count) != hipSuccess || !count) return 10; '
            'for (int i = 0; i < count; ++i) { int *p, n = 5; '
            'if (hipSetDevice(i) != hipSuccess || hipMalloc(&p, sizeof(n)) != hipSuccess) return 11; '
            'if (hipMemcpy(p, &n, sizeof(n), hipMemcpyHostToDevice) != hipSuccess) return 12; '
            'hipLaunchKernelGGL(kernel, dim3(1), dim3(1), 0, 0, p); '
            'if (hipMemcpy(&n, p, sizeof(n), hipMemcpyDeviceToHost) != hipSuccess || n != 22) return 13; '
            'if (hipFree(p) != hipSuccess) return 14; } '
            'std::printf("%d GPU devices checked\\n", count); return 0; }',
        )
        objects = []
        for index, source in enumerate(sources):
            path = self.work / f'device {index}.hip'
            path.write_text(source)
            obj = path.with_suffix('.o')
            result = self.run_command([compiler, *flags, '-c', path, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        args = [compiler, *flags, '--hip-link', '-fuse-ld=lld', *objects, '-lamdhip64']
        for prefix, name in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            output = self.work / name
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-offload-link: compiled on'), len(targets),
                                 result.stderr)
                self.assertGreater(result.stderr.count('distcc-thinlto: compiled on'), 0,
                                   result.stderr)
            self.assertEqual(self.run_command([output]).returncode, 0)
            if devices:
                runtime = self.run_command([output, '--gpu'])
                self.assertEqual(runtime.returncode, 0, runtime.stdout + runtime.stderr)
                print(name + ': ' + runtime.stdout.strip(), flush=True)
            image = self.work / (name + '.hipfb')
            dump = self.run_command(['llvm-objcopy', '--dump-section',
                                     '.hip_fatbin=' + str(image), output, '/dev/null'])
            self.assertEqual(dump.returncode, 0, dump.stderr)
            bundles = self.run_command(['clang-offload-bundler', '-list', '-type=o',
                                        '-input=' + str(image)])
            self.assertEqual(bundles.returncode, 0, bundles.stderr)
            for target in targets:
                self.assertIn(target, bundles.stdout)

    def test_target_prefixed_compiler(self):
        selected = shutil.which('clang')
        assert selected is not None
        clang = Path(selected)
        target = subprocess.check_output([clang, '-dumpmachine'], text=True).strip()
        compiler = clang.parent / (target + '-clang++')
        if not compiler.is_file():
            self.skipTest('target-prefixed Clang is required')
        self.link(compiler)

    def test_static_constructor_order(self):
        objects = []
        for index, name in enumerate(('z_first', 'a_second', 'm_third')):
            source = self.work / (name + '.cpp')
            source.write_text('#include <cstdio>\nnamespace { struct Init { Init() { std::puts("' +
                              str(index) + '"); } }; Init init; }\n')
            obj = source.with_suffix('.o')
            result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', source, '-o', obj])
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(obj)
        main = self.work / 'main.cpp'
        main.write_text('int main() {}\n')
        result = self.run_command(['clang++', '-O2', '-flto=thin', '-c', main, '-o', 'main.o'])
        self.assertEqual(result.returncode, 0, result.stderr)
        args = ['clang++', '-O2', '-flto=thin', '-fuse-ld=lld', *objects, 'main.o']
        for prefix, output in (([], 'reference'), ([self.build / 'distcc'], 'distributed')):
            result = self.run_command([*prefix, *args, '-o', output])
            self.assertEqual(result.returncode, 0, result.stderr)
            if prefix:
                self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), 4, result.stderr)
            actual = self.run_command([self.work / output])
            self.assertEqual(actual.stdout, '0\n1\n2\n')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
    def test_cmake_link_launcher(self):
        for language, suffix, driver in (('C', 'c', 'clang'), ('HIP', 'hip', 'clang++')):
            with self.subTest(language=language):
                source = self.work / language
                source.mkdir()
                (source / ('part.' + suffix)).write_text('int value(void) { return 42; }')
                (source / ('main.' + suffix)).write_text('int value(void); int main(void) { return value()!=42; }')
                (source / 'CMakeLists.txt').write_text(
                    f'cmake_minimum_required(VERSION 3.21)\nproject(distributed_lto {language})\n'
                    f'add_executable(check main.{suffix} part.{suffix})\n'
                    'target_compile_options(check PRIVATE -flto=thin)\n'
                    'target_link_options(check PRIVATE -flto=thin -fuse-ld=lld)\n')
                self.env[f'CMAKE_{language}_LINKER_LAUNCHER'] = 'distcc'
                self.env[f'CMAKE_{language}_COMPILER_LAUNCHER'] = 'distcc'
                compiler = shutil.which(driver)
                assert compiler is not None
                build = source / 'build'
                result = self.run_command(['cmake', '-S', source, '-B', build, '-G', 'Ninja',
                                           f'-DCMAKE_{language}_COMPILER=' + compiler,
                                           *(['-DCMAKE_HIP_ARCHITECTURES=gfx900'] if language == 'HIP' else [])])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = self.run_command(['cmake', '--build', build])
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((result.stdout + result.stderr).count('distcc-thinlto: compiled on'), 2)
                self.assertEqual(self.run_command([build / 'check']).returncode, 0)

    def test_absolute_clang_preserves_client_target(self):
        selected = shutil.which('clang')
        assert selected is not None
        compiler = Path(selected).resolve()
        target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
        other = 'x86_64-linux-gnu' if target.startswith('aarch64') else 'aarch64-linux-gnu'
        wrappers = self.work / 'bin'
        wrappers.mkdir()
        wrapper = wrappers / compiler.name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler)) +
                           ' --target=' + other + ' "$@"\n')
        wrapper.chmod(0o755)
        self.restart_server(self.build / 'distccd', DISTCCD_PATH=str(wrappers) + os.pathsep + os.defpath)
        self.addCleanup(self.restart_server, self.build / 'distccd')
        saved = dict(self.env)
        self.start_broker(self.host(0, f',tcp,stats={self.stats_ports[0]}'), name='target')
        saved['DISTCC_BROKER'] = self.env['DISTCC_BROKER']
        self.env = saved
        source = self.work / 'target.c'
        source.write_text('int main(void) { return 0; }')
        obj = self.work / 'target.o'
        result = self.run_command([self.build / 'distcc', compiler, '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pooled TCP job completed', result.stderr)
        output = self.work / 'target'
        result = self.run_command([compiler, obj, '-o', output])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_command([output]).returncode, 0)

    def test_archive_and_selected_compiler(self):
        compiler = '/usr/lib/llvm/22/bin/clang'
        if not Path(compiler).exists():
            self.skipTest('second Clang toolchain not installed')
        self.link(compiler, archive=True)

    def test_failure_propagates_without_local_retry(self):
        source = self.work / 'invalid.o'
        source.write_bytes(b'invalid bitcode')
        index = self.work / 'invalid.thinlto.bc'
        index.write_bytes(b'invalid index')
        output = self.work / 'result.o'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c'], 'inputs': []},
            'jobs': [{'args': ['-x', 'ir', str(source), '-fthinlto-index=' + str(index), '-o', str(output)],
                      'inputs': [str(source), str(index)], 'outputs': [str(output)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertNotIn('retrying backend locally', result.stderr)

    def test_lld_cache_avoids_backend_reexecution(self):
        source = self.work / 'main.c'
        source.write_text('int main(void) { return 0; }')
        obj = source.with_suffix('.o')
        result = self.run_command(['clang', '-flto=thin', '-c', source, '-o', obj])
        self.assertEqual(result.returncode, 0, result.stderr)
        command = [self.build / 'distcc', 'clang', '-flto=thin', '-fuse-ld=lld', obj,
                   '-Wl,--thinlto-cache-dir=' + str(self.work / 'cache'), '-o', self.work / 'linked']
        for count in (1, 0):
            result = self.run_command(command)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count('distcc-thinlto: compiled on'), count, result.stderr)
            self.assertEqual(self.run_command([self.work / 'linked']).returncode, 0)

    def test_multiple_declared_outputs(self):
        source = self.work / 'input.bc'
        result = self.run_command(['clang', '-x', 'c', '-flto=thin', '-c', '-o', source, '-'],
                                  input='int value(void) { return 42; }')
        self.assertEqual(result.returncode, 0, result.stderr)
        output, trace = self.work / 'output.o', self.work / 'trace.json'
        description = self.work / 'job.json'
        description.write_text(json.dumps({
            'common': {'args': [shutil.which('clang'), '-c', '-x', 'ir'], 'inputs': []},
            'jobs': [{'args': [str(source), '-o', str(output), '-ftime-trace=' + str(trace)],
                      'inputs': [str(source)], 'outputs': [str(output), str(trace)]}]}))
        result = self.run_command([self.build / 'distcc-thinlto', description])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('distcc-thinlto: compiled on', result.stderr)
        self.assertEqual(output.read_bytes()[:4], b'\x7fELF')
        self.assertIn('traceEvents', json.loads(trace.read_text()))


if __name__ == '__main__':
    unittest.main()
