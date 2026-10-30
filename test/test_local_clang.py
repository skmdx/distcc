"""Real compiler equivalence and lifecycle through the broker's worker cache."""
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
import unittest

from test_broker import BrokerTestCase


class LocalClangTest(BrokerTestCase):
    def start_local(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang unavailable')
        self.compiler = compiler
        self.broker = self.start_broker()
        self.env.update(DISTCC_HOSTS='localhost/4', DISTCC_CLANG_LOCAL='1', DISTCC_VERBOSE='1',
                        DISTCC_LOG=str(self.work/'client.log'),
                        PATH=str(self.build)+os.pathsep+self.env['PATH'])

    def command(self, arguments, reused=True, **options):
        return subprocess.run(([str(self.build/'distcc')] if reused else [])+
                              [self.compiler, *arguments], env=self.env, cwd=self.work,
                              capture_output=True, timeout=30, **options)

    def test_real_driver_outputs_and_errors(self):
        self.start_local()
        (self.work/'source.c').write_text('#include <stdio.h>\nint main(void) { puts("local"); return 0; }\n')
        (self.work/'source.cc').write_text('#include <vector>\nint main() { std::vector<int> v{1,2}; return v[1]!=2; }\n')
        (self.work/'options.rsp').write_text('-O2 -c source.c -o out -MMD -MF out.d')
        cases = [
            ['-O2', '-c', 'source.c', '-o', 'out', '-MMD', '-MF', 'out.d'],
            ['-O2', '-flto=thin', '-fuse-ld=lld', 'source.c', '-o', 'out'],
            ['-O2', '-fuse-ld=bfd', 'source.c', '-o', 'out'],
            ['--driver-mode=g++', 'source.cc', '-o', 'out'],
            ['-m32', 'source.c', '-o', 'out'],
            ['-E', '-dM', 'source.c'], ['@options.rsp'], ['--version'],
            ['-cc1','-help'],
            ['-c', 'source.c', '/wd4189', '-o', 'out'],
            ['-c', 'source.c', '-fno-such-distcc-option', '-o', 'out'],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                outputs = []
                for reused in (False, True):
                    for name in ('out', 'out.d'): (self.work/name).unlink(missing_ok=True)
                    result = self.command(arguments, reused)
                    artifacts = {name:(self.work/name).read_bytes() for name in ('out','out.d')
                                 if (self.work/name).exists()}
                    outputs.append((result.returncode,result.stdout,artifacts))
                    if not result.returncode and '-c' not in arguments and arguments[0] != '@options.rsp' and 'out' in artifacts:
                        subprocess.run([str(self.work/'out')],check=True,capture_output=True)
                self.assertEqual(outputs[0],outputs[1],result.stderr.decode())
                if result.returncode:
                    self.assertIn(b'error:',result.stderr)
        log = (self.work/'client.log').read_text()
        # Compile-only commands can use distcc's separate prepared-input path;
        # driver queries, preprocessing and link commands take the local path.
        self.assertGreaterEqual(log.count('reused local compiler libraries'),6)
        self.assertEqual(log.count('started local compiler libraries'),1)

    def test_parallel_commands_and_cancellation(self):
        self.start_local()
        # Input blocks inside the actual cc1, not a stand-in compiler.
        process = subprocess.Popen([str(self.build/'distcc'),self.compiler,'-E','-x','c','-'],
                                   cwd=self.work,env=self.env,stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def stop():
            if process.poll() is None: process.kill()
            process.communicate(timeout=5)
        self.addCleanup(stop)
        deadline = time.monotonic()+10
        children = []
        helper = 0
        while not children:
            log = (self.work/'client.log').read_text() if (self.work/'client.log').exists() else ''
            match = re.search(r'started local compiler libraries pid=(\d+)',log)
            if match:
                helper = int(match[1])
                children = Path(f'/proc/{helper}/task/{helper}/children').read_text().split()
            self.assertLess(time.monotonic(),deadline,log)
            time.sleep(.01)
        def preprocess(index):
            result = self.command(['-E','-P','-x','c','-',f'-DVALUE={index}'], input=b'VALUE\n')
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),str(index).encode())
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(preprocess,range(12)))
        process.kill(); process.communicate(timeout=5)
        deadline = time.monotonic()+5
        while Path(f'/proc/{helper}/task/{helper}/children').read_text().strip():
            self.assertLess(time.monotonic(),deadline,'cancelled compiler remains alive')
            time.sleep(.01)
        # Cache replaces a dead helper, while the following real command works.
        os.kill(helper,signal.SIGKILL)
        deadline = time.monotonic()+5
        while Path(f'/proc/{helper}/stat').exists():
            if Path(f'/proc/{helper}/stat').read_text().rsplit(')',1)[1].split()[0] == 'Z': break
            self.assertLess(time.monotonic(),deadline)
            time.sleep(.01)
        result = self.command(['-E','-P','-x','c','-'],input=b'17\n')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.strip(),b'17')

    def test_driver_aliases_share_libraries_and_preserve_language(self):
        self.start_local()
        cxx = shutil.which('clang++')
        if not cxx or not os.path.samefile(self.compiler, cxx):
            self.skipTest('C and C++ driver aliases of one executable required')
        (self.work/'input.c').write_text('')
        for compiler, is_cxx in ((self.compiler, False), (cxx, True)):
            self.compiler = compiler
            arguments = ['-E', '-dM', 'input.c']
            native = self.command(arguments, False, input=b'')
            reused = self.command(arguments, input=b'')
            self.assertEqual(native.returncode, 0, native.stderr)
            self.assertEqual((native.returncode, native.stdout),
                             (reused.returncode, reused.stdout), reused.stderr)
            self.assertEqual(b'#define __cplusplus ' in reused.stdout, is_cxx)
        log = (self.work/'client.log').read_text()
        self.assertEqual(log.count('started local compiler libraries'), 1)
        self.assertEqual(log.count('reused local compiler libraries'), 2)

    def test_available_frontend_releases(self):
        self.start_local()
        (self.work/'source.c').write_text('int value;\n')
        (self.work/'source.S').write_text('.text\n')
        for directory in self.build.glob('_include_server/lib.*/include_server/clang[0-9]*'):
            compiler = shutil.which(directory.name.replace('clang','clang-'))
            if not compiler: continue
            self.compiler = compiler
            for args in (['-E','-dM','source.c'], ['-c','source.S','-o','out'],
                         ['-c','source.c','-fno-such-distcc-option']):
                with self.subTest(compiler=compiler,args=args):
                    native = self.command(args,False)
                    reused = self.command(args)
                    self.assertEqual((native.returncode,native.stdout),(reused.returncode,reused.stdout))


if __name__ == '__main__':
    unittest.main()
