#!/usr/bin/env python3
"""Exercise input interposition with unchanged GCC and a native snapshot tree.

This is a boundary canary, not a distcc provider or a performance benchmark.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import array
import fcntl
import struct
import subprocess
from typing import Any


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def elf(compiler, name) -> dict[str, Any]:
    binary = Path(subprocess.check_output([compiler, '-print-prog-name='+name], text=True).strip())
    symbols = subprocess.check_output(['nm', '-D', str(binary)], text=True)
    symbol = next(line.split()[-1] for line in symbols.splitlines() if 'cpp_read_main_file' in line)
    calls = subprocess.check_output(['objdump', '-d', '--disassemble='+symbol, str(binary)], text=True)
    return dict(binary=str(binary), sha256=digest(binary),
                dynamic=subprocess.check_output(['readelf', '-d', str(binary)], text=True),
                bindings=[line for line in subprocess.check_output(['readelf','-Wr',str(binary)],text=True).splitlines()
                          if 'JUMP_SLOT' in line and any(' '+n+'@' in line for n in
                          ('open','read','fstat','stat','lstat','fopen','readlink','opendir','mmap','__realpath_chk'))],
                internal_calls=[line.strip() for line in calls.splitlines() if 'call' in line and '_cpp_find_file' in line],
                libraries=subprocess.check_output(['ldd', str(binary)], text=True))


def owner_canary(args, work, source):
    """Real Files owner and client authorization, without a replacement tree."""
    project = source.parent.parent
    helper = project/'h_input_owner'
    built = run(['make','-f','Makefile','-f','bench/replay/owner.mk','h_input_owner'], cwd=project)
    (work/'owner-build.log').write_text(built.stdout+built.stderr)
    built.check_returncode()
    shutil.copy2(helper, work/helper.name)
    shutil.copy2(source/'owner_interpose.c',work/'owner_interpose.c')
    library = work/'owner_interpose.so'
    built = run([*shlex.split(os.environ.get('CC','cc')), *shlex.split(os.environ.get('CFLAGS','')),
                 '-shared','-fPIC','-Wall','-Wextra',work/'owner_interpose.c','-ldl','-pthread','-o',library])
    (work/'build.log').write_text(built.stdout+built.stderr); built.check_returncode()
    image = {'cc1':elf(args.cc,'cc1'), 'cc1plus':elf(args.cxx,'cc1plus')}
    (work/'elf.json').write_text(json.dumps(image,indent=2)+'\n')
    cache = work/'cache'; cache.mkdir()
    results = []
    for case, compiler, suffix in [('c',args.cc,'c'),('cxx',args.cxx,'cc'),
                                   ('diagnostic',args.cc,'c'),('missing',args.cc,'c'),('denied',args.cc,'c')]:
        directory = work/case; root = directory/'input'; root.mkdir(parents=True)
        main = root/('main.'+suffix); value = root/'value.h'
        private = directory/'unexported.h'; private.write_text('not exported\n')
        value.write_text('#pragma once\nenum { value = 37 };\n')
        os.link(value,root/'hard.h')
        (root/'denied.h').write_text('#error denied\n'); (root/'denied.h').chmod(0)
        contents = '#include "value.h"\n#include "hard.h"\nint answer(void) { return value; }\n'
        if case == 'diagnostic': contents += 'int invalid = missing_identifier;\n'
        if case in ('missing','denied'): contents += '#include "'+case+'.h"\n'
        main.write_text(contents)
        output = directory/'output.o'
        flags = shlex.split(os.environ.get('CXXFLAGS' if suffix == 'cc' else 'CFLAGS',''))
        argv = [compiler,*flags,'-c',str(main),'-o',str(output)]
        native = run(argv); (directory/'native.log').write_text(native.stderr)
        expected = output.read_bytes() if native.returncode == 0 else None
        if expected is not None: output.rename(directory/'native.o')
        client_end, owner_end = socket.socketpair()
        caller, service = socket.socketpair(type=socket.SOCK_SEQPACKET)
        with (directory/'client.log').open('w') as cl, (directory/'owner.log').open('w') as ol:
            peer = subprocess.Popen([work/helper.name,'client',str(client_end.fileno()),'0',root,cache],
                                    pass_fds=(client_end.fileno(),),stderr=cl)
            owner = subprocess.Popen([work/helper.name,'owner',str(owner_end.fileno()),str(service.fileno()),root,cache],
                                     pass_fds=(owner_end.fileno(),service.fileno()),stderr=ol)
            client_end.close(); owner_end.close(); service.close()
            environment = dict(os.environ, LD_PRELOAD=str(library), DCC_CANARY_FD=str(caller.fileno()),
                               DCC_CANARY_IMAGE=str(Path(image['cc1plus' if suffix == 'cc' else 'cc1']['binary']).resolve()),
                               DCC_CANARY_ROOT=str(directory))
            if os.environ.get('LD_PRELOAD'): environment['LD_PRELOAD'] += ':'+os.environ['LD_PRELOAD']
            try:
                actual = run(['strace','-f','-yy','-e','trace=openat,read','-o',directory/'access.trace',*argv],
                             env=environment, pass_fds=(caller.fileno(),), timeout=15)
                (directory/'interposed.log').write_text(actual.stderr)
                if actual.returncode != native.returncode or actual.stderr != native.stderr:
                    raise RuntimeError(f'{case}: status/diagnostic mismatch: {actual.stderr}')
                if expected is not None and output.read_bytes() != expected: raise RuntimeError('object mismatch')
                trace = (directory/'access.trace').read_text()
                if 'memfd:distcc-input' not in trace: raise RuntimeError('compiler did not read exported input FD')
                def get(path):
                    caller.send(os.fsencode(path))
                    data, ancillary, _, _ = caller.recvmsg(4,socket.CMSG_SPACE(4))
                    error, = struct.unpack('i',data)
                    if error: raise OSError(error,os.strerror(error),str(path))
                    return array.array('i',ancillary[0][2])[0]
                first, second, alias = get(value), get(value), get(root/'hard.h')
                try:
                    try:
                        unexpected = get(private)
                    except OSError as failure:
                        export_denied = failure.errno == 13
                    else:
                        os.close(unexpected); export_denied = False
                    if not export_denied: raise RuntimeError('client export authorization bypassed')
                    independent = os.read(first,4) == os.read(second,4)
                    identity = (os.fstat(first).st_dev,os.fstat(first).st_ino) == (os.fstat(alias).st_dev,os.fstat(alias).st_ino)
                    metadata_identity = (os.stat(value).st_dev,os.stat(value).st_ino) == (os.fstat(first).st_dev,os.fstat(first).st_ino)
                    seals = fcntl.fcntl(first,fcntl.F_GET_SEALS)
                    if not independent or not identity or seals != 15: raise RuntimeError('FD ownership contract failed')
                    caller.close(); owner.wait(timeout=5); peer.wait(timeout=5)
                    survives_owner = os.pread(first,4,0) == b'#pra'
                    if not survives_owner: raise RuntimeError('FD died with owner')
                finally:
                    for fd in (first,second,alias): os.close(fd)
            finally:
                caller.close()
                for process in (owner,peer):
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(); raise
            if owner.returncode or peer.returncode: raise RuntimeError('owner/client failed')
        operations = (directory/'owner.log').read_text()
        if 'export '+str(main) not in operations or 'export '+str(value) not in operations: raise RuntimeError('missing Files reachability')
        results.append(dict(case=case,status=actual.returncode,object_match=expected is not None,
                            diagnostics_match=True,independent_offsets=independent,hardlink_identity=identity,
                            native_stat_fstat_identity=metadata_identity,seals=seals,survives_owner=survives_owner,
                            unexported_input_denied=export_denied))
    unchanged = all(digest(item['binary']) == item['sha256'] for item in image.values())
    if not unchanged: raise RuntimeError('compiler binary changed')
    result = dict(valid=True,cases=results,compiler_binaries_unchanged=unchanged,
                  scope='Actual Files/Connection and frontend_fs authorization; no production adoption or timing claim.',
                  blocker='Native stat(path) and fstat(payload FD) expose different devices/inodes; path metadata has not been replaced.',
                  limits=['Symlink/canonicalization, PCH and directory consumers remain outside this connection canary.',
                          'One selected compiler child per owner; no concurrent multi-compiler claim.'])
    (work/'summary.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--workdir', type=Path, required=True)
    parser.add_argument('--owner', action='store_true', help='Use real Files/Connection instead of the directory fixture')
    args = parser.parse_args()
    work = args.workdir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise RuntimeError('workdir must be new and empty')
    source = Path(__file__).resolve().parent
    if args.owner:
        shutil.copy2(__file__,work/'interpose.py')
        return owner_canary(args,work,source)
    shutil.copy2(source/'interpose.c', work/'interpose.c')
    shutil.copy2(__file__, work/'interpose.py')
    library = work/'interpose.so'
    command = [*shlex.split(os.environ.get('CC','cc')), *shlex.split(os.environ.get('CFLAGS','')),
               '-shared','-fPIC','-Wall','-Wextra',str(work/'interpose.c'),'-ldl','-o',str(library)]
    built = run(command)
    (work/'build.log').write_text(built.stdout+built.stderr)
    built.check_returncode()
    image = {'cc1':elf(args.cc,'cc1'), 'cc1plus':elf(args.cxx,'cc1plus')}
    (work/'elf.json').write_text(json.dumps(image, indent=2)+'\n')
    results = []
    base = os.environ.copy()
    for case, compiler, suffix in [('c',args.cc,'c'),('cxx',args.cxx,'cc'),
                                   ('embed',args.cc,'c'),('pch',args.cxx,'cc'),('pch-directory',args.cxx,'cc'),
                                   ('diagnostic',args.cc,'c'),('missing',args.cc,'c'),
                                   ('denied',args.cc,'c'),('fresh',args.cc,'c')]:
        directory = work/case
        root, view = directory/'input', directory/'snapshot'
        root.mkdir(parents=True)
        main_file = root/('main.'+suffix)
        (root/'value.h').write_text('#pragma once\nenum { selected = '+('41' if case == 'fresh' else '37')+' };\n')
        (root/'alias.h').symlink_to('value.h')
        os.link(root/'value.h', root/'hard.h')
        (root/'bytes.bin').write_bytes(bytes([3,7,11,19]))
        (root/'denied.h').write_text('#define DENIED 1\n')
        (root/'denied.h').chmod(0)
        contents = '#include "value.h"\n#include "alias.h"\n#include "hard.h"\nint value(void) { return selected; }\n'
        if case == 'embed':
            contents += 'unsigned char data[] = {\n#embed "bytes.bin"\n};\n'
        elif case == 'diagnostic':
            contents += 'int bad = missing_identifier;\n'
        elif case == 'missing':
            contents += '#include "missing.h"\n'
        elif case == 'denied':
            contents += '#include "denied.h"\n'
        main_file.write_text(contents)
        flags = shlex.split(os.environ.get('CXXFLAGS' if suffix == 'cc' else 'CFLAGS',''))
        extra = []
        if case.startswith('pch'):
            (root/'precompiled.h').write_text('#define PRECOMPILED 23\n')
            prepared = run([compiler,*flags,'-x','c++-header',root/'precompiled.h','-o',root/'precompiled.h.gch'])
            (directory/'pch.log').write_text(prepared.stderr)
            prepared.check_returncode()
            if case == 'pch-directory':
                (root/'precompiled.h.gch').rename(root/'valid')
                (root/'precompiled.h.gch').mkdir()
                (root/'valid').rename(root/'precompiled.h.gch/valid')
            extra = ['-include',str(root/'precompiled.h'),'-H']
        output = directory/'output.o'
        argv = [compiler,*flags,*extra,'-c',str(main_file),'-o',str(output)]
        native = run(argv, env=base)
        (directory/'native.log').write_text(native.stderr)
        expected = output.read_bytes() if native.returncode == 0 else None
        if expected is not None:
            output.rename(directory/'native.o')
        # Retain symlinks, hardlink identity, modes and timestamps.
        # Temporarily allow copying the denied input, then restore both modes.
        (root/'denied.h').chmod(0o600)
        shutil.copytree(root, view, symlinks=True, copy_function=shutil.copy2)
        (view/'hard.h').unlink(); os.link(view/'value.h',view/'hard.h')
        (root/'denied.h').chmod(0); (view/'denied.h').chmod(0)
        for header in ('value.h',):
            (root/header).write_text('#error live header must not be read\n')
        main_file.write_text('#error live main must not be read\n')
        rejected = run(argv, env=base)
        if rejected.returncode == 0 or 'live main' not in rejected.stderr:
            raise RuntimeError('negative control did not reach changed original input')
        log = directory/'access.log'
        environment = dict(base, LD_PRELOAD=str(library), DCC_CANARY_ROOT=str(root),
                           DCC_CANARY_VIEW=str(view), DCC_CANARY_LOG=str(log))
        if base.get('LD_PRELOAD'):
            environment['LD_PRELOAD'] += ':'+base['LD_PRELOAD']
        actual = run(argv, env=environment)
        (directory/'interposed.log').write_text(actual.stderr)
        if actual.returncode != native.returncode:
            raise RuntimeError(f'{case}: return status mismatch: {actual.stderr}')
        if expected is not None:
            if output.read_bytes() != expected:
                raise RuntimeError(f'{case}: object mismatch')
        elif actual.stderr != native.stderr:
            raise RuntimeError(f'{case}: diagnostic mismatch')
        lines = log.read_text().splitlines()
        operations = collections.Counter(line.split()[1] for line in lines)
        images = {int(parts[0]):parts[4] for line in lines
                  if (parts := line.split(maxsplit=4))[1] == 'image'}
        expected_binary = Path(image['cc1plus' if suffix == 'cc' else 'cc1']['binary']).resolve()
        main_owners = {int(line.split()[0]) for line in lines
                       if ' open ' in line and str(main_file) in line}
        if not main_owners or any(Path(images[pid]).resolve() != expected_binary for pid in main_owners):
            raise RuntimeError('main access did not come from the inspected compiler binary')
        if not any(' open ' in line and str(main_file) in line for line in lines):
            raise RuntimeError('main input was not intercepted')
        if not any('value.h' in line and ' open ' in line for line in lines):
            raise RuntimeError('header input was not intercepted')
        if case.startswith('pch') and '! '+str(root/'precompiled.h.gch') not in actual.stderr:
            raise RuntimeError('precompiled header was not used')
        results.append(dict(case=case, argv=argv, status=actual.returncode,
                            object_sha256=hashlib.sha256(expected).hexdigest() if expected else None,
                            diagnostics_match=actual.stderr==native.stderr, operations=operations,
                            images=images, main_owners=sorted(main_owners)))
    for entry in image.values():
        if digest(entry['binary']) != entry['sha256']:
            raise RuntimeError('compiler binary changed')
    result = dict(valid=True, cases=results, compiler_binaries_unchanged=True,
                  scope='Actual input substitution with native snapshot files, not a production Files-owner integration.',
                  limits=['No performance claim.', 'No mount/ACL/export authorization emulation.',
                          'External absolute symlinks, cancellation and simultaneous jobs are not covered.',
                          'The selected __realpath_chk boundary is recorded but not interposed by this canary.'])
    (work/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
