#!/usr/bin/env python3
"""LLVM-generated link jobs over distcc's shared input transport."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

from distcc_job import LocalOnly, fingerprint, snapshot, write_metadata

CONTROL = '/__distcc_lto'
RUNTIME = ('/lib', '/lib64', '/usr/lib64', '/usr/lib/gcc', '/usr/lib/llvm',
           '/etc/ld.so.cache')


def absolute(name):
    return str(Path(os.path.abspath(name)))


def distribute(description, kind):
    request = json.loads(Path(description).read_text())
    common, jobs = request['common'], request['jobs']
    if not jobs:
        return 0
    transport = str(Path(__file__).resolve().with_name('distcc'))
    slots = int(subprocess.check_output([transport, '-j'], text=True))
    with tempfile.TemporaryDirectory(prefix='distcc-lto-') as directory:
        work = Path(directory)
        # Snapshot and hash every distinct input once per link. Per-job trees
        # and the worker's persistent cache share the immutable backing files.
        files = {}
        for task in jobs:
            for name in [*task['inputs'], *common['inputs']]:
                name = absolute(name)
                if name not in files:
                    path = work / ('input-' + str(len(files)))
                    snapshot(name, path)
                    files[name] = (path, *fingerprint(path))

        def execute(item):
            index, task = item
            jobdir = work / str(index)
            inputs = jobdir / 'inputs'
            inputs.mkdir(parents=True)
            names = list(dict.fromkeys(absolute(p) for p in [*task['inputs'], *common['inputs']]))
            command = [*common['args'], *task['args']]
            job = {'cwd': os.getcwd(), 'args': command,
                   'inputs': names, 'outputs': [absolute(p) for p in task['outputs']]}
            entries = []
            for name in names:
                source, size, digest = files[name]
                os.link(source, inputs / str(len(entries)))
                entries.append((name, size, digest))
            manifest = inputs / str(len(entries))
            manifest.write_text(json.dumps(job))
            entries.append((CONTROL + '/job.json', *fingerprint(manifest)))
            helper = 'distcc-thinlto' if kind == 4 else 'distcc-offload-link'
            write_metadata(kind, job['cwd'], helper, entries, jobdir / 'job')
            archive = jobdir / 'result.tar'
            env = dict(os.environ)
            env.pop('INCLUDE_SERVER_PORT', None)
            result = subprocess.run([transport, '--file-job', str(jobdir / 'job'),
                                     str(inputs), str(archive)], env=env, capture_output=True)
            sys.stdout.buffer.write(result.stdout)
            sys.stderr.buffer.write(result.stderr)
            if result.returncode:
                if env.get('DISTCC_FALLBACK') != '0' and env.get('DISTCC_SKIP_LOCAL_RETRY') != '1':
                    print(helper + ': retrying backend locally', file=sys.stderr)
                    return subprocess.call(command)
                return result.returncode
            # Stage all declared artifacts before publishing any to LLD.
            with tarfile.open(archive) as tar:
                members = tar.getmembers()
                if ([m.name for m in members] != [str(i) for i in range(len(job['outputs']))]
                        or any(not m.isfile() for m in members)):
                    raise ValueError('invalid LLVM job result archive')
                staged = []
                try:
                    for member, destination in zip(members, job['outputs']):
                        with tempfile.NamedTemporaryFile(dir=Path(destination).parent,
                                                         prefix='.distcc-', delete=False) as output:
                            staged.append((output.name, destination))
                            source = tar.extractfile(member)
                            assert source is not None  # Regular members were validated above.
                            with source:
                                shutil.copyfileobj(source, output, 256 * 1024)
                    for source, destination in staged:
                        os.replace(source, destination)
                finally:
                    for source, _ in staged:
                        Path(source).unlink(missing_ok=True)
            return 0

        with ThreadPoolExecutor(max_workers=min(len(jobs), max(1, slots))) as pool:
            results = list(pool.map(execute, enumerate(jobs)))
        return next((result for result in results if result), 0)


def worker(root, archive, kind):
    root = Path(root)
    job = json.loads((root / CONTROL.lstrip('/') / 'job.json').read_text())

    def rooted(name):
        path = Path(name)
        if not path.is_absolute() or '..' in path.parts:
            raise ValueError('invalid LLVM job file path')
        return root / name.lstrip('/')

    requested, *args = job['args']
    # Invoke administrator-installed Clang only; never an uploaded executable.
    name = Path(requested).name
    if not re.fullmatch(r'(?:[\w.+]+-)*clang(?:\+\+)?(?:-\d+(?:\.\d+)*)?', name):
        raise ValueError('LLVM jobs must invoke an installed Clang')
    if kind == 4 and '-c' not in args:
        raise ValueError('ThinLTO backend must be a Clang compilation')
    compiler = None
    path = Path(requested)
    if (path.is_absolute() and path.parent.parent.parent == Path('/usr/lib/llvm')
            and path.parent.name == 'bin' and path.is_file()):
        compiler = str(path)
    compiler = compiler or shutil.which(name, path=os.environ.get('DISTCCD_PATH', os.defpath))
    if not compiler:
        raise OSError('worker Clang is not installed')
    # Keep embedded import paths in LLVM indexes unchanged. A private mount
    # namespace gives Clang the client path layout and read-only cached inputs.
    sandbox = [shutil.which('bwrap', path=os.defpath) or '/usr/bin/bwrap',
               '--unshare-all', '--die-with-parent', '--bind', str(root), '/']
    rooted(job['cwd']).mkdir(parents=True, exist_ok=True)
    for runtime in RUNTIME:
        if Path(runtime).exists():
            sandbox.extend(['--ro-bind', runtime, runtime])
    # Keep the installed tool directory and its symlinks intact: Clang locates
    # LLD and resource files relative to its executable. Binding a symlink as
    # an individual mount target is also rejected by bubblewrap.
    compiler_dir = str(Path(compiler).parent)
    sandbox.extend(['--ro-bind', compiler_dir, compiler_dir,
                    '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp'])
    outputs = [rooted(name) for name in job['outputs']]
    # LLD publishes with a temporary sibling and rename; a writable file mount
    # alone is insufficient. The per-job tree owns these writable directories.
    # Bind inputs last so cached files remain immutable even in the same directory.
    for parent in sorted({Path(name).parent for name in job['outputs']}, key=lambda p: len(p.parts)):
        directory = rooted(str(parent))
        directory.mkdir(parents=True, exist_ok=True)
        sandbox.extend(['--bind', str(directory), str(parent)])
    for name in job['inputs']:
        sandbox.extend(['--ro-bind', str(rooted(name)), name])
    sandbox.extend(['--remount-ro', '/', '--chdir', job['cwd'], '--clearenv',
                    '--setenv', 'PATH', os.defpath, '--setenv', 'TMPDIR', '/tmp'])
    result = subprocess.run([*sandbox, compiler, *args])
    if result.returncode:
        return result.returncode if result.returncode > 0 else 128 - result.returncode
    if not outputs or not outputs[0].stat().st_size:
        raise ValueError('LLVM job produced no output')
    with tarfile.open(archive, 'w') as tar:
        for index, path in enumerate(outputs):
            tar.add(path, arcname=str(index), recursive=False)
    return 0


def main(kind):
    try:
        if len(sys.argv) == 4 and sys.argv[1] == '--worker':
            return worker(sys.argv[2], sys.argv[3], kind)
        if len(sys.argv) == 2:
            return distribute(sys.argv[1], kind)
        print('usage: ' + Path(sys.argv[0]).name + ' LLVM-JOB.json', file=sys.stderr)
        return 1
    except (LocalOnly, OSError, ValueError, KeyError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(Path(sys.argv[0]).name + ': ' + str(error), file=sys.stderr)
        return 1
