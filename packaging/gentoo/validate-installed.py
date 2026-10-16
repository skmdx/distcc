import hashlib
import json
import os
import resource
from pathlib import Path
import subprocess
import tempfile
import sys

wrapper = Path('/usr/lib/distcc/gcc')
assert wrapper.samefile('/usr/bin/distcc'), 'GCC masquerade wrapper is missing'
with tempfile.TemporaryDirectory(prefix='distcc-native-', dir=os.environ['TMPDIR']) as temporary:
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
    env.pop('DISTCC_BROKER', None)
    env.pop('DISTCC_GCC_SCAN_LIBRARY', None)
    results = {}
    for compiler, suffix, include in [('gcc', 'c', 'stdint.h'), ('g++', 'cc', 'cstdint')]:
        source = root/('main.'+suffix)
        source.write_text('#include <'+include+'>\n'
                          + ('#include <clocale>\n' if suffix == 'cc' else '') +
                          '#if !__has_include("'+str(root/'absolute.h')+'")\n#error absolute probe\n#endif\n'
                          '#include "'+str(root/'absolute.h')+'"\n'
                          'enum { before = __COUNTER__ };\n'
                          '#if __COUNTER__ == 1\n#include "counter.h"\n#endif\n'
                          '#define CAT_(a,b) a##b\n#define CAT(a,b) CAT_(a,b)\n'
                          '#if !CAT(__has_,include)(<stddef.h>)\n#error probe failed\n#endif\n'
                          'int main(void) { return sizeof(uint64_t) != 8 || COUNTER_HEADER_SEEN != 1 || ABSOLUTE_HEADER_SEEN != 1; }\n')
        hashes = {}
        pp_hashes = {}
        for kind in ('native', 'remote'):
            obj = root/(compiler+'-'+kind+'.o')
            args = ['-I'+str(root), '-O2', '-c', str(source), '-o', str(obj)]
            command = ['/usr/bin/'+compiler, *args] if kind == 'native' else ['pump', '/usr/lib/distcc/'+compiler, *args]
            result = subprocess.run(command, env=env, capture_output=True, timeout=90)
            assert result.returncode == 0, (kind, compiler, result.returncode,
                (result.stdout+result.stderr).decode(errors='replace'),
                (root/'client.log').read_text()[-5000:] if (root/'client.log').exists() else '')
            assert b'gave up' not in result.stderr + result.stdout
            if kind == 'native':
                # Admission applies to native include analysis too. Size this
                # small validation workload from its real compiler peak, rather
                # than using a production C++ estimate larger than the bounded
                # validation cgroup. Linux reports ru_maxrss in KiB.
                peak_kib = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
                env['DISTCC_JOB_MEMORY'] = str(max(1, (peak_kib + 1023) // 1024))
            hashes[kind] = hashlib.sha256(obj.read_bytes()).hexdigest()
            if suffix == 'cc':
                preprocessed = root/(kind+'.ii')
                prefix = ['/usr/bin/'+compiler] if kind == 'native' else ['pump', '/usr/lib/distcc/'+compiler]
                subprocess.run([*prefix, '-I'+str(root), '-O2', '-E', str(source), '-o', str(preprocessed)],
                               env=env, check=True, capture_output=True, timeout=90)
                pp_hashes[kind] = hashlib.sha256(preprocessed.read_bytes()).hexdigest()
            if kind == 'remote':
                exe = root/('run-'+suffix)
                subprocess.run(['/usr/bin/'+compiler, str(obj), '-o', str(exe)], check=True, timeout=30)
                subprocess.run([str(exe)], check=True, timeout=5)
        if pp_hashes and pp_hashes['native'] != pp_hashes['remote']:
            raise AssertionError(pp_hashes)
        if hashes['native'] != hashes['remote']:
            assert '--assembly' in sys.argv, hashes
            assembly = {}
            for kind in ('native', 'remote'):
                output = root/(compiler+'-'+kind+'.s')
                prefix = ['/usr/bin/'+compiler] if kind == 'native' else ['pump', '/usr/lib/distcc/'+compiler]
                subprocess.run([*prefix, '-I'+str(root), '-O2', '-S', str(source), '-o', str(output)],
                               env=env, check=True, capture_output=True, timeout=90)
                assembly[kind] = output.read_bytes()
            assert assembly['native'] == assembly['remote'], 'Compiler assembly differs'
            results[compiler] = {'objects': hashes, 'assembly_sha256': hashlib.sha256(assembly['native']).hexdigest()}
        else:
            results[compiler] = hashes['remote']
        if pp_hashes:
            results[compiler] = {'object': results[compiler], 'preprocessed': pp_hashes}
    log = (root/'client.log').read_text()
    assert 'gave up' not in log
    assert 'client finished sending request to server' in log, log[-6000:]
    print(json.dumps({'compilers': results, 'wrapper': str(wrapper.resolve()), 'fallback': False}))
