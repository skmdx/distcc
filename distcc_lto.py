#!/usr/bin/env python3
"""LLVM-generated link jobs over distcc's shared input transport."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import xxhash

from distcc_job import LocalOnly, fingerprint, snapshot, write_metadata

CONTROL = '/__distcc_lto'
RUNTIME = ('/lib', '/lib64', '/usr/lib64', '/usr/lib/gcc', '/usr/lib/llvm',
           '/etc/ld.so.cache')


def absolute(name):
    return str(Path(os.path.abspath(name)))


def llvm_tools(compiler):
    """Find the helpers registered with the compiler's own LLVM installation."""
    binary = shutil.which(compiler) or compiler
    bindir = Path(binary).resolve().parent
    config = bindir / 'llvm-config'
    if not config.is_file():
        return None
    directory = Path(subprocess.check_output([config, '--libdir'], text=True).strip()) / 'distcc'
    paths = dict(plan=directory / 'distcc-lto', prepare=directory / 'lto-prepare.so',
                 linker=bindir / 'ld.lld', objcopy=bindir / 'llvm-objcopy')
    return paths if all(path.is_file() for path in paths.values()) else None


def work_key(job, files):
    """Identify computation independently of temporary input/output paths."""
    paths = {name: ('input', i) for i, name in enumerate(job['inputs'])}
    paths.update({name: ('output', i) for i, name in enumerate(job['outputs'])})

    def argument(arg):
        if absolute(arg) in paths:
            return paths[absolute(arg)]
        option, separator, value = arg.partition('=')
        if separator and absolute(value) in paths:
            return (option, paths[absolute(value)])
        return arg

    identity = [job['cwd'], [argument(arg) for arg in job['args']],
                [(files[name][1], files[name][2].hex()) for name in job['inputs']]]
    return xxhash.xxh3_64_intdigest(json.dumps(identity).encode()) or 1


def publish_archive(archive, destinations):
    """Stage the complete result before replacing any declared artifact."""
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        if ([m.name for m in members] != [str(i) for i in range(len(destinations))]
                or any(not m.isfile() for m in members)):
            raise ValueError('invalid LLVM job result archive')
        staged = []
        try:
            for member, destination in zip(members, destinations):
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


def distribute(description, kind):
    # Only the client schedules tasks. Importing its executor on every worker
    # job also initializes logging/traceback, before LLVM can start.
    from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
    import heapq
    import itertools
    import math
    import shlex

    request = json.loads(Path(description).read_text())
    common, jobs = request['common'], request['jobs']
    if not jobs:
        return 0
    transport = str(Path(__file__).resolve().with_name('distcc'))
    capacity = json.loads(subprocess.check_output([transport, '--lto-capacity'], text=True))
    slots = max(1, capacity['jobs'])
    # A shard should also fit the slowest worker's share of the link, rather
    # than giving equal-size shards to CPUs with unequal service times.
    parallelism = capacity['work_rate'] / capacity['min_rate'] if capacity['min_rate'] else slots
    with tempfile.TemporaryDirectory(prefix='distcc-lto-') as directory:
        work = Path(directory)
        # Snapshot and hash every distinct input once per link. Per-job trees
        # and the worker's persistent cache share the immutable backing files.
        files = {}
        def register(task):
            for name in [*task['inputs'], *common['inputs']]:
                name = absolute(name)
                if name not in files:
                    path = work / ('input-' + str(len(files)))
                    snapshot(name, path)
                    files[name] = (path, *fingerprint(path))
            names = task['inputs'][:1] if kind == 4 else [*task['inputs'], *common['inputs']]
            task['bytes'] = sum(files[name][1] for name in {absolute(p) for p in names})

        for task in jobs:
            register(task)
            task['instructions'] = 0
            task['partitions'] = 1
        total_bytes = sum(task['bytes'] for task in jobs)
        total_instructions = 0
        tools = llvm_tools(common['args'][0]) if kind == 4 else None
        indexes = [next((arg.split('=', 1)[1] for arg in task['args']
                         if arg.startswith('-fthinlto-index=')), None) for task in jobs]
        if tools and all(indexes):
            plan = work / 'plan.json'
            plan.write_text(json.dumps([dict(source=task['inputs'][0], index=index)
                                        for task, index in zip(jobs, indexes)]))
            weights = json.loads(subprocess.check_output([tools['plan'], 'plan', plan], text=True))
            if len(weights) != len(jobs):
                raise ValueError('incomplete LLVM work estimates')
            total_instructions = sum(row['instructions'] for row in weights)
            for task, row, index in zip(jobs, weights, indexes):
                task['instructions'] = row['instructions']
                task['index'] = index
                if total_instructions and len(task['outputs']) == 1:
                    useful = row['instructions'] // row['largest_function'] if row['largest_function'] else 1
                    task['partitions'] = max(1, min(slots, row['functions'], useful,
                        math.ceil(row['instructions'] * parallelism / total_instructions)))

        def priority(task):
            # Rank by the dominant share of this link's compute or data work.
            # Instruction-only ranking leaves large constant-data modules until
            # last; byte-only ranking misses expensive small-code backends.
            return max(task['instructions'] / total_instructions if total_instructions else 0,
                       task['bytes'] / total_bytes if total_bytes else 0)

        helper = 'distcc-thinlto' if kind == 4 else 'distcc-offload-link'
        env = dict(os.environ)
        env.pop('INCLUDE_SERVER_PORT', None)

        def describe(index, task):
            jobdir = work / str(index)
            jobdir.mkdir()
            names = list(dict.fromkeys(absolute(p) for p in [*task['inputs'], *common['inputs']]))
            job = {'cwd': os.getcwd(), 'args': [*common['args'], *task['args']],
                   'inputs': names, 'outputs': [absolute(p) for p in task['outputs']]}
            return jobdir, job

        def start(index, task):
            jobdir, job = describe(index, task)
            if env.get('DISTCC_VERBOSE') == '1':
                print(helper + ': submitting ' + job['inputs'][0]
                      + ' instructions=' + str(task['instructions'])
                      + ' bytes=' + str(task['bytes']), file=sys.stderr)
            inputs = jobdir / 'inputs'
            inputs.mkdir()
            entries = []
            for name in job['inputs']:
                source, size, digest = files[name]
                os.link(source, inputs / str(len(entries)))
                entries.append((name, size, digest))
            manifest = inputs / str(len(entries))
            manifest.write_text(json.dumps(job))
            entries.append((CONTROL + '/job.json', *fingerprint(manifest)))
            write_metadata(kind, job['cwd'], helper, entries, jobdir / 'job',
                           work_key=work_key(job, files), work_units=task['instructions'])
            archive = jobdir / 'result.tar'
            read_fd, write_fd = os.pipe()
            with os.fdopen(read_fd, 'rb') as queued, os.fdopen(write_fd, 'wb') as channel:
                process = subprocess.Popen([transport, '--file-job', str(jobdir / 'job'),
                    str(inputs), str(archive), str(write_fd)], env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(write_fd,))
                channel.close()
                try:
                    # Preserve ranked submission despite differing manifest
                    # sizes and process startup times. Admission, transfer and
                    # execution remain concurrent; only request enqueueing is
                    # ordered. Early failures close the pipe and are collected.
                    queued.read(1)
                except BaseException:
                    process.kill()
                    process.communicate()
                    raise
            return process, job, archive

        def collect(started):
            process, job, archive = started
            stdout, stderr = process.communicate()
            sys.stdout.buffer.write(stdout)
            sys.stderr.buffer.write(stderr)
            if process.returncode:
                if env.get('DISTCC_FALLBACK') != '0' and env.get('DISTCC_SKIP_LOCAL_RETRY') != '1':
                    print(helper + ': retrying backend locally', file=sys.stderr)
                    return subprocess.call(job['args'])
                return process.returncode
            publish_archive(archive, job['outputs'])
            return 0

        def prepare(index, task):
            assert tools is not None
            jobdir, job = describe(index, task)
            root = jobdir / 'root'
            root.mkdir()
            for name in job['inputs']:
                path = root / name.lstrip('/')
                path.parent.mkdir(parents=True, exist_ok=True)
                os.link(files[name][0], path)
            manifest = str(jobdir / 'partitions.json')
            job['outputs'] = [manifest]
            job['partitions'] = task['partitions']
            job['args'][job['args'].index('-o') + 1] = manifest + '.unused.o'
            result = run_job(root, job, kind, tools['prepare'])
            if result:
                raise subprocess.CalledProcessError(result, job['args'])
            partition = json.loads((root / manifest.lstrip('/')).read_text())
            for part in partition['parts']:
                part['path'] = str(root / part['path'].lstrip('/'))
            if env.get('DISTCC_VERBOSE') == '1':
                print(helper + ': partitioned ' + task['inputs'][0] + ' into '
                      + str(len(partition['parts'])), file=sys.stderr)
            outputs = [str(jobdir / f'part-{i}.o') for i in range(len(partition['parts']))]
            group = dict(directory=jobdir, outputs=outputs, locals=partition['locals'],
                         destination=absolute(task['outputs'][0]), remaining=len(outputs))
            children = []
            primary = absolute(task['inputs'][0])
            for part, output in zip(partition['parts'], outputs):
                args = [part['path'] if absolute(arg) == primary else arg for arg in task['args']]
                args[args.index('-o') + 1] = output
                args.extend(['-mllvm', '-thinlto-assume-merged'])
                children.append(dict(args=args, inputs=[part['path'], task['index']],
                    outputs=[output], instructions=part['instructions'], group=group))
            return children

        def merge(group):
            assert tools is not None
            merged = group['directory'] / 'merged.o'
            subprocess.run([tools['linker'], '-r', '-o', merged, *group['outputs']], check=True)
            with tempfile.NamedTemporaryFile(dir=Path(group['destination']).parent,
                                             prefix='.distcc-', delete=False) as output:
                staged = output.name
            try:
                response = group['directory'] / 'localize.rsp'
                response.write_text(shlex.join([*['--localize-symbol=' + name for name in group['locals']],
                                                str(merged), staged]))
                subprocess.run([tools['objcopy'], '@' + str(response)], check=True)
                os.replace(staged, group['destination'])
            finally:
                Path(staged).unlink(missing_ok=True)

        # Preparation and ordinary backends share one bounded scheduler. Ready
        # partitions rejoin its priority queue immediately; there is no barrier
        # waiting for every import or for all unsplit backends to finish.
        ready, serial = [], itertools.count()

        def enqueue(action, task, weight):
            heapq.heappush(ready, (-weight, next(serial), action, task))

        for task in jobs:
            enqueue('prepare' if task['partitions'] > 1 else 'backend', task, priority(task))
        with ThreadPoolExecutor(max_workers=slots) as pool:
            pending = {}
            while ready or pending:
                while ready and len(pending) < slots:
                    _, index, action, task = heapq.heappop(ready)
                    if action == 'backend':
                        future = pool.submit(collect, start(index, task))
                    elif action == 'prepare':
                        future = pool.submit(prepare, index, task)
                    else:
                        future = pool.submit(merge, task)
                    pending[future] = action, task
                finished, _ = wait(pending, return_when=FIRST_COMPLETED)
                for future in finished:
                    action, task = pending.pop(future)
                    result = future.result()
                    if action == 'prepare':
                        for child in result:
                            register(child)
                            enqueue('backend', child, priority(child))
                    elif action == 'backend':
                        if result:
                            raise subprocess.CalledProcessError(result, task['args'])
                        if 'group' in task:
                            group = task['group']
                            group['remaining'] -= 1
                            if not group['remaining']:
                                enqueue('merge', group, float('inf'))
        return 0


def run_job(root, job, kind, prepare_library=None):
    root = Path(root)

    def rooted(name):
        path = Path(name)
        if not path.is_absolute() or '..' in path.parts:
            raise ValueError('invalid LLVM job file path')
        return root / name.lstrip('/')

    requested, *args = job['args']
    # Invoke administrator-installed LLVM tools; never an uploaded executable.
    name = Path(requested).name
    if not re.fullmatch(r'(?:[\w.+]+-)*(?:clang(?:\+\+)?|lld|ld\.lld)(?:-\d+(?:\.\d+)*)?', name):
        raise ValueError('LLVM jobs must invoke an installed compiler or linker')
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
                    '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
                    '--dir', job['cwd']])
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
    if prepare_library is not None:
        sandbox.extend(['--setenv', 'LD_PRELOAD', str(prepare_library),
                        '--setenv', 'DISTCC_LTO_PARTITION_FILE', job['outputs'][0],
                        '--setenv', 'DISTCC_LTO_PARTITIONS', str(job['partitions'])])
    result = subprocess.run([*sandbox, compiler, *args])
    if result.returncode:
        return result.returncode if result.returncode > 0 else 128 - result.returncode
    if not outputs or not outputs[0].stat().st_size:
        raise ValueError('LLVM job produced no output')
    return 0


def worker(root, archive, kind):
    root = Path(root)
    job = json.loads((root / CONTROL.lstrip('/') / 'job.json').read_text())
    result = run_job(root, job, kind)
    if result:
        return result
    with tarfile.open(archive, 'w') as tar:
        for index, name in enumerate(job['outputs']):
            path = root / name.lstrip('/')
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
