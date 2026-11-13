"""Real GCC planning, broker publication and filesystem invalidation contracts."""
import concurrent.futures
import ctypes
import array
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import unittest

from test_broker import BrokerTestCase
from test_tcp_jobs import GccFrontendTest


class PlanMemoTest(BrokerTestCase):
    worker_address = '127.0.0.1'
    start_frontend = GccFrontendTest.start_frontend
    compile = GccFrontendTest.compile

    def prepare(self):
        self.start_frontend()
        self.env['DISTCC_LOG'] = str(self.work/'client.log')
        self.sources = self.work/'sources'
        self.outputs = self.work/'outputs'
        self.sources.mkdir(); self.outputs.mkdir()
        for name in ('a.c','b.c','c.c','a.S'):
            (self.sources/name).write_text('int value = VALUE;\n' if name.endswith('.c') else '.text\n')

    def compile_one(self, name='a.c', flags=(), expect=None):
        source = self.sources/name
        output = self.outputs/(name+'.o')
        dep = self.outputs/(name+'.d')
        output.parent.mkdir(parents=True,exist_ok=True)
        log = Path(self.env['DISTCC_LOG'])
        offset = log.stat().st_size if log.exists() else 0
        command = [self.cc,'-c',str(source),'-DVALUE=3','-MD','-MF',str(dep),'-o',str(output),*flags]
        self.compile(command)
        trace = log.read_text()[offset:]
        if expect: self.assertTrue('GCC plan memo '+expect in trace,
                                  '\n'.join(line for line in trace.splitlines() if 'GCC ' in line))
        actual = output.read_bytes(),dep.read_bytes()
        native = subprocess.run(command,cwd=self.work,env=self.env,capture_output=True,text=True)
        self.assertEqual(native.returncode,0,native.stderr)
        self.assertEqual(actual,(output.read_bytes(),dep.read_bytes()))
        return trace

    def test_same_parent_hits_and_preserves_outputs(self):
        self.prepare()
        self.compile_one(expect='publish')
        trace = self.compile_one('b.c',expect='hit')
        self.assertNotIn('reused GCC driver',trace)
        self.assertNotIn('reused GCC planner',trace)

    def test_missing_include_creation_deletion_and_symlink_replacement(self):
        self.prepare()
        inc = self.work/'later'
        options=['-I'+str(inc)]
        self.compile_one(flags=options,expect='publish')
        self.compile_one('b.c',flags=options,expect='hit')
        inc.mkdir()
        self.compile_one('c.c',flags=options,expect='publish')
        self.compile_one('b.c',flags=options,expect='hit')
        inc.rmdir()
        self.compile_one(flags=options,expect='publish')
        first,second=self.work/'first',self.work/'second'
        first.mkdir();second.mkdir();inc.symlink_to(first)
        self.compile_one(flags=options,expect='publish')
        inc.unlink();inc.symlink_to(second)
        self.compile_one('b.c',flags=options,expect='publish')

    def test_macro_parent_extension_and_prefix_are_separate(self):
        self.prepare()
        self.compile_one(expect='publish')
        self.compile_one('b.c',flags=['-DOTHER=1'],expect='publish')
        self.compile_one('b.c',flags=['-UOTHER'],expect='publish')
        self.compile_one('a.S',expect='publish')
        other=self.sources/'other';other.mkdir();(other/'b.c').write_text('int value=VALUE;\n')
        self.compile_one('other/b.c',expect='publish')
        prefix=self.work/'prefix';prefix.mkdir()
        self.compile_one(flags=['-B'+str(prefix)+'/'],expect='publish')
        (prefix/'include').mkdir()
        self.compile_one('b.c',flags=['-B'+str(prefix)+'/'],expect='publish')

    def test_custom_specs_and_invalid_options_do_not_publish(self):
        self.prepare()
        one=self.work/'spec-one';two=self.work/'spec-two';one.mkdir();two.mkdir()
        specs=self.work/'custom.specs';specs.write_text('*cpp:\n-I'+str(one)+'\n')
        first=self.plan(flags=['-specs='+str(specs)])
        self.assertNotIn('GCC plan memo ',first.stderr)
        specs.write_text('*cpp:\n-I'+str(two)+'\n')
        second=self.plan('b.c',flags=['-specs='+str(specs)])
        self.assertNotIn('GCC plan memo ',second.stderr)
        self.assertNotEqual(first.stdout,second.stdout)
        log=Path(self.env['DISTCC_LOG']);offset=log.stat().st_size if log.exists() else 0
        bad=subprocess.run([str(self.build/'distcc'),self.cc,'-c',str(self.sources/'a.c'),
                            '-f-distcc-invalid-option'],env=self.env,cwd=self.work,capture_output=True)
        self.assertNotEqual(bad.returncode,0)
        self.assertNotIn('GCC plan memo publish',log.read_text()[offset:])

    def test_implicit_specs_appearance_and_cpp_live_plan(self):
        self.prepare()
        (self.sources/'a.cpp').write_text('template<class T> T value(T x) { return x; }\n')
        first=self.plan('a.cpp');hit=self.plan('a.cpp')
        self.assertIn('memo hit',hit.stderr)
        self.assertEqual(first.stdout,self.plan('a.cpp',live=True).stdout)
        prefix=self.work/'toolchain';prefix.mkdir()
        flags=['-B'+str(prefix)+'/']
        self.assertIn('memo publish',self.plan(flags=flags).stderr)
        specs=prefix/'specs'
        specs.write_text('*cpp:\n-I'+str(self.sources)+'\n')
        changed=self.plan(flags=flags)
        self.assertIn('memo uncacheable',changed.stderr)
        self.assertEqual(changed.stdout,self.plan(flags=flags,live=True).stdout)
        specs.write_text('*cpp:\n-I'+str(self.work)+'\n')
        changed_again=self.plan(flags=flags)
        self.assertIn('memo uncacheable',changed_again.stderr)
        self.assertNotEqual(changed.stdout,changed_again.stdout)

    def plan(self,name='a.c',flags=(),env=None,live=False):
        source=str(self.sources/name)
        result=subprocess.run([str(self.build/'h_compile'),'plan','-' if live else source,self.cc,'-c',source,
                               '-o',str(self.outputs/(name+'.o')),*flags],cwd=self.work,
                              env=env or self.env,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr.decode())
        result.stderr=result.stderr.decode()
        return result

    def test_live_plan_equality_and_concurrent_cold_requests(self):
        self.prepare()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            plans=list(pool.map(lambda _:self.plan(),range(8)))
        self.assertEqual(sum('memo publish' in p.stderr for p in plans),1)
        self.assertEqual(sum('memo hit' in p.stderr for p in plans),7)
        live=self.plan(live=True)
        self.assertTrue(all(p.stdout==live.stdout for p in plans))
        for flags in (['-Irelative'],['-include','absent.h'],['-imacros','absent.h']):
            first=self.plan(flags=flags);hit=self.plan('b.c',flags=flags)
            self.assertIn('memo hit',hit.stderr)
            self.assertEqual(first.stdout,hit.stdout)
            self.assertEqual(hit.stdout,self.plan('b.c',flags=flags,live=True).stdout)

    def test_key_bypass_boundaries(self):
        self.prepare()
        source=str(self.sources/'a.c')
        def key(flags,output='object.o'):
            p=subprocess.run([str(self.build/'h_compile'),'plan_key',source,self.cc,
                              '-c',source,'-o',output,*flags],env=self.env,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            return p.stdout.strip()
        original=key([])
        self.assertNotEqual(original,'bypass')
        self.assertEqual(original,key([],output='other.o'))
        for flags in (['@args'],['-specs=custom'],['-wrapper','x'],['-E'],['-v'],['-###'],
                      ['-'],[str(self.sources/'b.c')],['-DVALUE='+'0'*65536]):
            with self.subTest(flags=flags[0][:40]):self.assertEqual(key(flags),'bypass')

    def test_permission_and_oversized_journal_bypass(self):
        self.prepare()
        directory=self.work/'permissions';directory.mkdir()
        flags=['-I'+str(directory)]
        self.assertIn('memo publish',self.plan(flags=flags).stderr)
        directory.chmod(0o700)
        self.assertIn('memo publish',self.plan('b.c',flags=flags).stderr)
        many=['-I'+str(self.work/('missing-'+str(i))) for i in range(200)]
        first=self.plan(flags=many);second=self.plan('b.c',flags=many)
        self.assertIn('memo uncacheable',first.stderr)
        self.assertIn('memo uncacheable',second.stderr)
        self.assertEqual(first.stdout,second.stdout)

    def test_environment_search_and_directory_metadata_are_validated(self):
        self.prepare()
        directory=self.work/'environment';directory.mkdir()
        env=dict(self.env,CPATH=str(directory))
        first=self.plan(env=env)
        self.assertIn('memo publish',first.stderr)
        self.assertIn('memo hit',self.plan('b.c',env=env).stderr)
        (directory/'new.h').write_text('')
        self.assertIn('memo hit',self.plan('b.c',env=env).stderr)
        self.assertEqual(first.stdout,self.plan(env=env,live=True).stdout)
        self.assertNotEqual(first.stdout,self.plan().stdout)

    def test_parallel_outputs_in_include_directory_do_not_invalidate(self):
        self.prepare()
        self.outputs=self.sources
        flags=['-I'+str(self.sources),'-I.']
        self.compile_one(flags=flags,expect='publish')
        self.compile_one('b.c',flags=flags,expect='hit')
        self.assertIn('memo publish',self.plan('c.c',flags=flags).stderr)
        def output(i):
            for suffix in ('.o','.dt'):
                (self.sources/(str(i)+suffix)).write_bytes(b'object')
                (self.work/(str(i)+suffix)).write_bytes(b'dependency')
            return self.plan('c.c',flags=flags)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            plans=list(pool.map(output,range(32)))
        self.assertTrue(all('memo hit' in p.stderr for p in plans),
                        '\n'.join(p.stderr for p in plans if 'memo hit' not in p.stderr))
        live=self.plan('c.c',flags=flags,live=True).stdout
        self.assertTrue(all(p.stdout==live for p in plans))

    def test_broker_capacity_stale_compare_exchange_and_owner_exit(self):
        self.start_broker()
        def memo(key,command=4):
            peer=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            peer.settimeout(2);peer.connect(self.env['DISTCC_BROKER'])
            peer.sendall(struct.pack('=II',3,command)+key+b'\0')
            self.addCleanup(peer.close)
            return peer
        def publish(key,value):
            owner=memo(key);self.assertEqual(owner.recv(1),b'\0')
            owner.sendall(struct.pack('=II',3,5)+value+b'\0')
            self.assertEqual(owner.recv(1),b'');owner.close()
        # 1,400 realistic-size payloads and worker handles share one byte-bounded
        # broker tree. This must not regress to the former 256-entry ceiling.
        large=b'v'*32768
        for i in range(1400):publish(('plan-'+str(i)).encode(),large)
        for i in range(1400):
            peer=memo(('plan-'+str(i)).encode());self.assertEqual(peer.recv(65536),large+b'\0');peer.close()
        class Hash(ctypes.Structure):_fields_=[('low',ctypes.c_uint64),('high',ctypes.c_uint64)]
        library=ctypes.CDLL('libxxhash.so.0');library.XXH3_128bits.restype=Hash
        value=library.XXH3_128bits(large,ctypes.c_size_t(len(large)))
        payload=b'plan-0\n'+f'{value.high:016x}{value.low:016x}'.encode()
        owner=memo(payload,18);self.assertEqual(owner.recv(1),b'\0')
        waiter=memo(payload,18);owner.close()
        self.assertEqual(waiter.recv(1),b'\0')
        waiter.sendall(struct.pack('=II',3,5)+b'new\0');self.assertEqual(waiter.recv(1),b'')
        # A late invalidator must not discard the already replaced value.
        self.assertEqual(memo(payload,18).recv(100),b'new\0')
        for i in range(257):
            left,right=socket.socketpair();self.addCleanup(right.close)
            owner=memo(('worker-'+str(i)).encode(),16);self.assertEqual(owner.recv(1),b'\0')
            owner.sendmsg([struct.pack('=II',3,17)],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,
                          array.array('i',[left.fileno()]))])
            left.close();self.assertEqual(owner.recv(1),b'');owner.close()
        self.assertEqual(memo(b'worker-0',16).recv(1),b'\0')
        latest=memo(b'worker-256',16)
        reply,ancillary,flags,_=latest.recvmsg(16,socket.CMSG_SPACE(4))
        self.assertEqual(reply,b'W\0');self.assertEqual(flags,0)
        for level,kind,data in ancillary:
            for fd in array.array('i',data):os.close(fd)
        self.assertEqual(memo(b'plan-1399').recv(65536),large+b'\0')


class PlanDependenciesTest(unittest.TestCase):
    def prepare(self):
        self.build=Path.cwd()
        self.storage=tempfile.TemporaryDirectory(prefix='plan-dependencies-')
        self.addCleanup(self.storage.cleanup)
        self.work=Path(self.storage.name)
        self.env=os.environ.copy()
        self.sources=self.work/'sources';self.sources.mkdir()
        (self.sources/'a.c').write_text('int value;\n')

    def journal(self, operation, path, valid=True):
        raw=self.work/'journal';memo=self.work/'journal.memo'
        result=subprocess.run([str(self.build/'h_plan_dependencies'),str(raw),operation,str(path)],
                              env=self.env,cwd=self.work,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        encoded=subprocess.run([str(self.build/'h_compile'),'plan_journal',str(raw)],
                               env=self.env,cwd=self.work,capture_output=True)
        self.assertEqual(encoded.returncode,0 if valid else 1,encoded.stderr)
        memo.write_bytes(encoded.stdout)
        return memo

    def valid_journal(self, memo):
        return subprocess.run([str(self.build/'h_compile'),'plan_validate',str(memo)],
                              env=self.env,cwd=self.work,capture_output=True).returncode==0

    def test_directory_probe_and_enumeration_have_distinct_dependencies(self):
        self.prepare()
        directory=self.work/'index';directory.mkdir()
        for op in ('stat','lstat','stat64','lstat64','fstatat64','opendir'):
            with self.subTest(operation=op):
                memo=self.journal(op,directory)
                child=directory/'out.o';child.write_bytes(b'object')
                self.assertEqual(self.valid_journal(memo),op!='opendir')
                child.unlink()
                directory.chmod(0o700)
                self.assertFalse(self.valid_journal(memo))
                directory.chmod(0o755)
        memo=self.journal('stat',directory)
        directory.rename(self.work/'old-index');directory.mkdir()
        self.assertFalse(self.valid_journal(memo))

    def test_fortified_largefile_access_and_empty_path_boundaries(self):
        self.prepare()
        path=self.work/'input';path.write_bytes(b'old')
        for op in ('__open_2','__open64_2','__openat_2','__openat64_2','openat64',
                   'freopen64','fstatat-empty','fstatat64-empty','stat64','fstatat64'):
            with self.subTest(operation=op):
                memo=self.journal(op,path)
                self.assertTrue(self.valid_journal(memo))
                path.write_bytes(path.read_bytes()+b'new')
                self.assertFalse(self.valid_journal(memo))
                path.unlink()
                memo=self.journal(op,path,valid='empty' not in op)
                path.write_bytes(b'old')
                if 'empty' not in op:self.assertFalse(self.valid_journal(memo))
        for op in ('eaccess','euidaccess','faccessat'):
            memo=self.journal(op,path);self.assertTrue(self.valid_journal(memo))
            path.chmod(0)
            self.assertFalse(self.valid_journal(memo))
            denied=self.journal(op,path);self.assertTrue(self.valid_journal(denied))
            path.chmod(0o600);self.assertFalse(self.valid_journal(denied))
            memo=self.journal(op,path)
            path.unlink();self.assertFalse(self.valid_journal(memo));path.write_bytes(b'old')
        self.journal('faccessat-empty',path,valid=False)
        link=self.work/'alias';link.symlink_to(path)
        for op in ('readlinkat','lstat64','fstatat-empty'):
            memo=self.journal(op,link);self.assertTrue(self.valid_journal(memo))
            link.unlink();link.symlink_to(self.sources/'a.c')
            self.assertFalse(self.valid_journal(memo));link.unlink();link.symlink_to(path)

def load_tests(loader, tests, pattern):
    # Reuse the broker fixture, not all unrelated inherited scheduling tests.
    return unittest.TestSuite(case(name) for case in (PlanMemoTest,PlanDependenciesTest)
                              for name in case.__dict__ if name.startswith('test_'))


if __name__=='__main__': unittest.main()
