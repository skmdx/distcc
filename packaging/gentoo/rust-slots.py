"""Worker-local inspection and management of Gentoo Rust toolchain copies."""
import errno
import glob
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile


def cpu_features(cpuinfo):
    # A -march=native compiler or any library in its closure could have been
    # built on any source core. Every destination core must support that ISA.
    cores = []
    for section in cpuinfo.split('\n\n'):
        fields = {key.strip(): value.split() for key, value in
                  (line.split(':', 1) for line in section.splitlines() if ':' in line)}
        if 'processor' in fields:
            cores.append(set(fields.get('flags', fields.get('Features', []))))
    if not cores or not all(cores):
        return dict(machine=platform.machine(), required=[], available=[])
    return dict(machine=platform.machine(), required=sorted(set.union(*cores)),
                available=sorted(set.intersection(*cores)))


def inventory(temporary):
    paths = [shutil.which('rustc'), *glob.glob('/usr/lib/rust/*/bin/rustc'),
             *glob.glob('/opt/rust-bin-*/bin/rustc')]
    result = []
    for root in dict.fromkeys(Path(p).resolve().parent.parent for p in paths if p):
        compiler = root / 'bin/rustc'
        if root.name.startswith('rust-distcc-') or not compile_probe([compiler], temporary):
            continue
        version = subprocess.check_output([compiler, '-vV'], text=True)
        lines = version.splitlines()
        fields = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
        host = fields['host']
        identity = dict(version=lines[0], commit=fields['commit-hash'], llvm=fields['LLVM version'])
        # Preserve the resolved dependency closure and its ELF loader together.
        # Mixing a system-LLVM build with a peer's C++ or libc runtime is not a
        # portable toolchain, even when rustc -vV happens to start successfully.
        loaded = subprocess.check_output(['ldd', str(compiler)], text=True,
                                         env=dict(os.environ, LC_ALL='C'))
        runtime = [dict(name=name, path=path) for name, path in
                   re.findall(r'^\s*(\S+) => (/\S+)', loaded, re.M)
                   if not name.startswith('/') and not Path(path).resolve().is_relative_to(root)]
        loaders = re.findall(r'^\s*(/\S+)(?: => (/\S+))? \(', loaded, re.M)
        if len(loaders) != 1:
            raise RuntimeError('Rust ELF loader was not resolved: ' + str(compiler))
        runtime.append(dict(name='ld.so', path=loaders[0][1] or loaders[0][0]))
        digest = hashlib.sha256()
        files = [compiler, *sorted((root / 'lib').glob('librustc_driver*')),
                 *sorted((root / 'lib/rustlib' / host / 'codegen-backends').glob('*')),
                 *(Path(item['path']) for item in runtime)]
        for path in files:
            if path.is_file():
                digest.update(path.name.encode() + b'\0')
                with path.open('rb') as stream:
                    digest.update(hashlib.file_digest(stream, 'sha256').digest())
        result.append(dict(root=str(root), host=host, digest=digest.hexdigest(), identity=identity,
                           runtime=runtime))
    return dict(cpu=cpu_features(Path('/proc/cpuinfo').read_text()), slots=result)


def marker(root):
    path = root / '.distcc-complete'
    if not root.is_dir() or root.is_symlink() or path.is_symlink() or not path.is_file():
        return None
    with path.open('rb') as stream:
        return stream.read(96).decode('ascii', errors='replace').removesuffix('\n')


def owned(root, digest):
    return marker(root) in (digest, 'installing ' + digest, 'isa-checked ' + digest)


def write_marker(root, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=root, delete=False) as out:
        os.fchmod(out.fileno(), 0o644)
        out.write(value + '\n')
        out.flush()
        os.fsync(out.fileno())
    os.replace(out.name, root / '.distcc-complete')
    directory = os.open(root, os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def compile_probe(command, temporary):
    with tempfile.TemporaryDirectory(prefix='distcc-rust-check-', dir=temporary) as work:
        source = Path(work) / 'probe.rs'
        program = source.with_suffix('')
        source.write_text('fn main() { assert_eq!(std::hint::black_box(21u32) * 2, 42); }')
        try:
            subprocess.run([*command, '-O', source, '-o', program], check=True, stdout=subprocess.DEVNULL)
            subprocess.run([program], check=True, stdout=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            return False
        except OSError as error:
            if error.errno not in (errno.ENOENT, errno.ENOEXEC):
                raise
            return False
        return True


def manage(base, action, *args):
    if action == 'inventory':
        return inventory(args[0])
    if any(not re.fullmatch(r'[0-9a-f]{64}', digest) for digest in (args if action == 'prune' else args[:1])):
        raise ValueError('invalid Rust slot digest')
    if action == 'prune':
        keep = set(args)
        for root in base.glob('rust-distcc-*'):
            digest = root.name.removeprefix('rust-distcc-')
            if re.fullmatch(r'[0-9a-f]{64}', digest) and digest not in keep and owned(root, digest):
                shutil.rmtree(root)
        return
    digest = args[0]
    root = base / ('rust-distcc-' + digest)
    if action in ('ready', 'verify'):
        if action == 'ready' and marker(root) != 'isa-checked ' + digest:
            return False
        return compile_probe([root/'distcc-runtime/ld.so', '--library-path',
                              str(root/'lib') + ':' + str(root/'distcc-runtime'),
                              root/'bin/rustc', '--sysroot', root], args[1])
    if action == 'prepare':
        if os.path.lexists(root):
            if not owned(root, digest):
                raise RuntimeError('refusing to replace unmanaged Rust directory: ' + str(root))
        else:
            root.mkdir(mode=0o755)
        write_marker(root, 'installing ' + digest)
    elif action == 'publish':
        if marker(root) != 'installing ' + digest:
            raise RuntimeError('Rust slot is not being installed')
        write_marker(root, 'isa-checked ' + digest)
    else:
        raise ValueError(action)


if __name__ == '__main__':
    print(json.dumps(manage(Path('/opt'), *sys.argv[1:])))
