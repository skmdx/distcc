import hashlib
import contextlib
import difflib
import json
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import tempfile
import sys
import time

wrapper = Path('/usr/lib/distcc/gcc')
assert wrapper.samefile('/usr/bin/distcc'), 'GCC masquerade wrapper is missing'
with tempfile.TemporaryDirectory(prefix='distcc-native-', dir=os.environ['TMPDIR']) as temporary, contextlib.ExitStack() as resources:
    root = Path(temporary)
    (root/'state').mkdir()
    (root/'stdint.h').mkdir()
    (root/'counter.h').write_text('#define COUNTER_HEADER_SEEN 1\n')
    (root/'absolute.h').write_text('#define ABSOLUTE_HEADER_SEEN 1\n')
    env = dict(os.environ, PATH='/usr/lib/distcc:/usr/bin:/bin', TMPDIR=str(root),
               DISTCC_DIR=str(root/'state'),
               DISTCC_HOSTS=os.environ['VALIDATE_DISTCC_HOSTS'], DISTCC_FALLBACK='0',
               DISTCC_SKIP_LOCAL_RETRY='1', DISTCC_TESTING_INCLUDE_SERVER='1',
               DISTCC_VERBOSE='1', DISTCC_LOG=str(root/'client.log'), CCACHE_DISABLE='1')
    env['DISTCC_BROKER'] = str(root/'broker.sock')
    broker_log = resources.enter_context((root/'broker.log').open('w'))
    broker = subprocess.Popen(['/usr/bin/distccd', '--broker', env['DISTCC_BROKER']],
                              env=env, stdout=broker_log, stderr=broker_log)
    def stop_broker():
        broker.terminate()
        broker.wait(timeout=10)
    resources.callback(stop_broker)
    deadline = time.monotonic() + 5
    while not Path(env['DISTCC_BROKER']).exists():
        assert broker.poll() is None and time.monotonic() < deadline, 'Validation broker failed to start'
        time.sleep(0.01)
    env.pop('DISTCC_GCC_SCAN_LIBRARY', None)
    results = {}
    compilers = ['/usr/bin/gcc', '/usr/bin/g++'] + json.loads(os.environ.get('VALIDATE_DISTCC_COMPILERS', '[]'))
    # Test the names Portage actually invokes, not only target-prefixed aliases
    # explicitly listed for the server. CMDLIST matches the requested basename.
    search_path = ':'.join(dict.fromkeys([*(str(Path(p).parent) for p in compilers),
                                          *os.get_exec_path()]))
    for variable in ('CC', 'CXX'):
        # Unset variables select the default GCC pair already in compilers.
        # portageq reports this normal state with a nonzero exit status.
        result = subprocess.run(['portageq', 'envvar', variable], text=True, capture_output=True)
        if result.returncode != 1 or result.stdout.strip() or result.stderr.strip():
            result.check_returncode()
        value = result.stdout.strip()
        if not value:
            continue
        command = shlex.split(value)
        if len(command) != 1:
            raise RuntimeError('Cannot validate compound Portage ' + variable + ': ' + value)
        compiler = shutil.which(command[0], path=search_path)
        if not compiler or Path(compiler).samefile('/usr/bin/distcc'):
            raise RuntimeError('Cannot resolve real Portage ' + variable + ': ' + value)
        compilers.append(compiler)
    env['PATH'] = ':'.join(dict.fromkeys(['/usr/lib/distcc',
                                        *(str(Path(p).parent) for p in compilers), '/bin']))
    if '--rust-only' in sys.argv:
        assert ',tcp' in env['DISTCC_HOSTS'], 'Rust validation requires the TCP job protocol'
        compilers = []
    for compiler_path in dict.fromkeys(compilers):
        compiler = Path(compiler_path).name
        suffix, include = ('cc', 'cstdint') if '++' in compiler else ('c', 'stdint.h')
        # On x86-64, these pointer arguments exercise GOT stack-argument
        # relocations across the worker assembler and client linker.
        support = root/('support.'+suffix)
        support.write_text('int a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8;\n'
                           'int sum(int *a,int *b,int *c,int *d,int *e,int *f,int *g,int *h) '
                           '{ return *a+*b+*c+*d+*e+*f+*g+*h; }\n')
        support_obj = root/(compiler+'-support.o')
        subprocess.run([compiler_path, '-O2', '-c', str(support), '-o', str(support_obj)],
                       check=True, capture_output=True, timeout=30)
        source = root/('main.'+suffix)
        probe_header = 'absolute.h' if 'clang' in compiler else str(root/'absolute.h')
        source.write_text('#include <'+include+'>\n'
                          + ('#include <clocale>\n' if suffix == 'cc' else '') +
                          '#if !__has_include("'+probe_header+'")\n#error header probe\n#endif\n'
                          '#include "'+probe_header+'"\n'
                          'enum { before = __COUNTER__ };\n'
                          '#if __COUNTER__ == 1\n#include "counter.h"\n#endif\n'
                          '#define CAT_(a,b) a##b\n#define CAT(a,b) CAT_(a,b)\n'
                          '#if !CAT(__has_,include)(<stddef.h>)\n#error probe failed\n#endif\n'
                          'extern int a,b,c,d,e,f,g,h;\n'
                          'int sum(int *,int *,int *,int *,int *,int *,int *,int *);\n'
                          'int main(void) { return sum(&a,&b,&c,&d,&e,&f,&g,&h) != 36 || '
                          'sizeof(uint64_t) != 8 || COUNTER_HEADER_SEEN != 1 || ABSOLUTE_HEADER_SEEN != 1; }\n')
        hashes = {}
        pp_hashes = {}
        for kind in ('native', 'remote'):
            obj = root/(compiler+'-'+kind+'.o')
            args = ['-I'+str(root), '-O2', '-fPIC', '-c', str(source), '-o', str(obj)]
            command = [compiler_path, *args] if kind == 'native' else ['pump', 'distcc', compiler, *args]
            result = subprocess.run(command, env=env, capture_output=True, timeout=90)
            assert result.returncode == 0, (kind, compiler, result.returncode,
                (result.stdout+result.stderr).decode(errors='replace'),
                (root/'client.log').read_text()[-5000:] if (root/'client.log').exists() else '')
            assert b'gave up' not in result.stderr + result.stdout
            hashes[kind] = hashlib.sha256(obj.read_bytes()).hexdigest()
            if suffix == 'cc':
                preprocessed = root/(kind+'.ii')
                prefix = [compiler_path] if kind == 'native' else ['pump', 'distcc', compiler]
                # GCC translates synthetic line markers using the worker's
                # locale. Compare preprocessing output without those markers.
                subprocess.run([*prefix, '-I'+str(root), '-O2', '-E', '-P', str(source), '-o', str(preprocessed)],
                               env=env, check=True, capture_output=True, timeout=90)
                pp_hashes[kind] = hashlib.sha256(preprocessed.read_bytes()).hexdigest()
            if kind == 'remote':
                exe = root/('run-'+suffix)
                subprocess.run([compiler_path, str(obj), str(support_obj), '-o', str(exe)], check=True, timeout=30)
                subprocess.run([str(exe)], check=True, timeout=5)
        if pp_hashes and pp_hashes['native'] != pp_hashes['remote']:
            difference = list(difflib.unified_diff(
                (root/'native.ii').read_text().splitlines(),
                (root/'remote.ii').read_text().splitlines(),
                fromfile='native', tofile='remote'))
            raise AssertionError(compiler + '\n' + '\n'.join(difference[:80]))
        if hashes['native'] != hashes['remote']:
            assert '--assembly' in sys.argv, hashes
            assembly = {}
            for kind in ('native', 'remote'):
                output = root/(compiler+'-'+kind+'.s')
                prefix = [compiler_path] if kind == 'native' else ['pump', 'distcc', compiler]
                subprocess.run([*prefix, '-I'+str(root), '-O2', '-fPIC', '-S', str(source), '-o', str(output)],
                               env=env, check=True, capture_output=True, timeout=90)
                assembly[kind] = output.read_bytes()
            assert assembly['native'] == assembly['remote'], 'Compiler assembly differs'
            results[compiler] = {'objects': hashes, 'assembly_sha256': hashlib.sha256(assembly['native']).hexdigest()}
        else:
            results[compiler] = hashes['remote']
        if pp_hashes:
            results[compiler] = {'object': results[compiler], 'preprocessed': pp_hashes}
    if compilers:
        log = (root/'client.log').read_text()
        assert 'gave up' not in log
        marker = ('pooled TCP job completed' if ',tcp' in env['DISTCC_HOSTS'] else
                  'client finished sending request to server')
        assert log.count(marker) >= len(set(compilers)), log[-6000:]
    # The Rust wrapper shares this broker and must really reach the TCP worker.
    # Legacy C/C++ validation also runs; Rust is exercised on the TCP pass.
    if ',tcp' in env['DISTCC_HOSTS']:
        source = root / 'rust_library.rs'
        included = root / 'rust_value.rs'
        included.write_text('pub const VALUE: u32 = 42;')
        source.write_text('include!(concat!(env!("DISTCC_RUST_INPUT"), "/rust_value.rs"));\n'
                          'pub fn value() -> u32 { VALUE }\n')
        rust_env = dict(env, DISTCC_RUST_INPUT=str(root))
        library = root / 'librust_test.rlib'
        rust = subprocess.run(['distcc', 'rustc', '--crate-name=rust_test', '--crate-type=rlib',
                               '--emit=link,dep-info', str(source), '-o', str(library)],
                              env=rust_env, text=True, capture_output=True, timeout=180)
        assert rust.returncode == 0, rust.stdout + rust.stderr
        assert 'distcc-rust: compiled on ' in rust.stderr, rust.stderr
        source.write_text('fn main() { assert_eq!(rust_test::value(), 42); }')
        executable = root / 'rust-run'
        subprocess.run(['rustc', str(source), '--extern', 'rust_test=' + str(library),
                        '-o', str(executable)], env=rust_env, check=True, capture_output=True, timeout=90)
        subprocess.run([executable], check=True, timeout=10)
        results['rustc'] = {'version': subprocess.check_output(['rustc', '-V'], text=True).strip(),
                           'distributed': True, 'executed': True}
    print(json.dumps({'compilers': results, 'wrapper': str(wrapper.resolve()), 'fallback': False}))
