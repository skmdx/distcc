#!/usr/bin/env python3
"""LLVM-generated link jobs over distcc's shared input transport."""
from contextlib import ExitStack
from functools import cache, cached_property
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import distcc_python
import xxhash

from distcc_job import JOB_THINLTO, LocalOnly, fingerprint, snapshot, write_metadata

CONTROL = '/__distcc_lto'
RUNTIME = ('/lib', '/lib64', '/usr/lib64', '/usr/lib/gcc', '/usr/lib/llvm',
           '/etc/ld.so.cache')


class InputSnapshot:
    def __init__(self, source, path):
        snapshot(source, path)
        self.path = path
        self.size = path.stat().st_size

    @cached_property
    def identity(self):
        # Local partition preparation only reads the snapshot. Hash bytes
        # when a remote job needs them, sharing that result across its users.
        return fingerprint(self.path)


def absolute(name):
    return str(Path(os.path.abspath(name)))


def open_regular(path, dir_fd=None):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=dir_fd)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise ValueError('expected regular ThinLTO file')
    return fd


@cache
def llvm_tools(compiler):
    """Find the helpers registered with the compiler's own LLVM installation."""
    binary = shutil.which(compiler) or compiler
    bindir = Path(binary).resolve().parent
    plan = (bindir / 'distcc-lto').resolve()
    paths = dict(plan=plan, prepare=plan.with_name('lto-prepare.so'),
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
                [(files[name].identity[0], files[name].identity[1].hex())
                 for name in job['inputs']]]
    return xxhash.xxh3_64_intdigest(json.dumps(identity).encode()) or 1


def stage_output(source, destination, stack):
    with tempfile.NamedTemporaryFile(dir=Path(destination).parent, prefix='.distcc-', delete=False) as out:
        stack.callback(Path(out.name).unlink, missing_ok=True)
        shutil.copyfileobj(source, out, 256 * 1024)
        return out.name


class FileJobs:
    """One serial distcc transport, with a separate enqueue and completion reply."""
    def __init__(self, transport, env, pinned, *, capacity=False):
        self.channel, child = socket.socketpair()
        try:
            with child:
                args = [transport, '--file-jobs']
                if capacity:
                    args.append('--lto-capacity')
                self.process = subprocess.Popen(args, stdin=child.fileno(),
                                                env=env, pass_fds=pinned)
        except BaseException:
            self.channel.close()
            raise

    def read_capacity(self):
        def read_exact(size):
            result = bytearray()
            while len(result) < size:
                chunk = self.channel.recv(size - len(result))
                if not chunk:
                    status = self.process.wait()
                    if status:
                        raise subprocess.CalledProcessError(status, self.process.args)
                    raise ValueError('incomplete file-jobs capacity frame')
                result.extend(chunk)
            return result

        header = read_exact(12)
        if header[:4] != b'CAPA':
            raise ValueError('invalid file-jobs capacity frame')
        return json.loads(read_exact(int(header[4:], 16)))

    def submit(self, args):
        fields = [os.fsencode(arg) for arg in args]
        self.channel.sendall(b'ARGC%08x' % len(fields) + b''.join(
            b'ARGV%08x' % len(field) + field for field in fields))
        # Only enqueueing is ordered. An invalid manifest completes before it queues.
        return self.channel.recv(1)

    def result(self, first):
        marker = self.channel.recv(1) if first == b'Q' else first
        if not marker:
            return self.process.wait() or 1
        if marker != b'D':
            raise ValueError('invalid file job completion')
        status = self.channel.recv(1)
        return status[0] if status else self.process.wait() or 1

    def close(self):
        try:
            self.channel.sendall(b'ARGC00000000')
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.channel.close()
            self.process.wait()


def distribute(request, kind, link=None):
    from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
    import heapq
    import itertools
    import math
    import queue
    import shlex

    common, jobs = request['common'], request['jobs']
    if not jobs:
        return 0
    transport = distcc_python.client_executable()
    env = dict(os.environ)
    pinned = {int(path.parent.name) for path in link.outputs.values()} if link else set()
    with tempfile.TemporaryDirectory(prefix='distcc-lto-') as directory, ExitStack() as lanes:
        idle = queue.LifoQueue()
        def open_lane(capacity=False):
            lane = FileJobs(transport, env, pinned, capacity=capacity)
            lanes.callback(lane.close)
            if env.get('DISTCC_VERBOSE') == '1':
                print(f'{helper}: transport pid={lane.process.pid}', file=sys.stderr)
            return lane

        helper = 'distcc-thinlto' if kind == JOB_THINLTO else 'distcc-offload-link'
        first_lane = open_lane(capacity=True)
        capacity = first_lane.read_capacity()
        idle.put(first_lane)
        slots = max(1, capacity['jobs'])
        parallelism = capacity['work_rate'] / capacity['min_rate'] if capacity['min_rate'] else slots
        work = Path(directory)
        # Freeze every input before jobs publish any outputs. Fingerprinting
        # is deferred until transfer; local preparation needs no content key.
        files = {}
        def register(task, opener):
            for name in [*task['inputs'], *common['inputs']]:
                name = absolute(name)
                if name not in files:
                    path = work / ('input-' + str(len(files)))
                    files[name] = InputSnapshot(opener(name) if opener else name, path)
            names = task['inputs'][:1] if kind == JOB_THINLTO else [*task['inputs'], *common['inputs']]
            task['bytes'] = sum(files[name].size for name in {absolute(p) for p in names})

        for task in jobs:
            register(task, link.open_input if link else None)
            task['destinations'] = [link.outputs[absolute(p)] if link else absolute(p) for p in task['outputs']]
            task['instructions'] = 0
            task['partitions'] = 1
        total_bytes = sum(task['bytes'] for task in jobs)
        total_instructions = 0
        tools = llvm_tools(common['args'][0]) if kind == JOB_THINLTO else None
        indexes = [next((arg.split('=', 1)[1] for arg in task['args']
                         if arg.startswith('-fthinlto-index=')), None) for task in jobs]
        if tools and all(indexes):
            plan = work / 'plan.json'
            # source is the module ID stored in the index. The planner opens only the frozen index.
            plan.write_text(json.dumps([dict(source=task['inputs'][0], index=str(files[absolute(index)].path))
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
                size, digest = files[name].identity
                os.link(files[name].path, inputs / str(len(entries)))
                entries.append((name, size, digest))
            write_metadata(kind, job, entries, jobdir / 'job',
                           work_key=work_key(job, files), work_units=task['instructions'])
            try:
                lane = idle.get_nowait()
            except queue.Empty:
                lane = open_lane()
            return lane, lane.submit([jobdir / 'job', inputs, *task['destinations']]), job

        def collect(started):
            lane, first, job = started
            status = lane.result(first)
            if lane.process.returncode is None:
                idle.put(lane)
            if status:
                if link is None and env.get('DISTCC_FALLBACK') != '0' and env.get('DISTCC_SKIP_LOCAL_RETRY') != '1':
                    print(helper + ': retrying backend locally', file=sys.stderr)
                    return subprocess.call(job['args'])
                return status
            return 0

        def prepare(index, task):
            assert tools is not None
            jobdir, job = describe(index, task)
            root = jobdir / 'root'
            root.mkdir()
            for name in job['inputs']:
                path = root / name.lstrip('/')
                path.parent.mkdir(parents=True, exist_ok=True)
                os.link(files[name].path, path)
            manifest = str(jobdir / 'partitions.json')
            job['outputs'] = [manifest]
            job['partitions'] = task['partitions']
            job['args'][len(common['args']) + task['args'].index('-o') + 1] = manifest + '.unused.o'
            result = run_preparation(root, job, tools['prepare'])
            if result:
                raise subprocess.CalledProcessError(result, job['args'])
            with os.fdopen(open_regular(root / manifest.lstrip('/')), 'rb') as source:
                partition = json.load(source)
            if not 0 < len(partition['parts']) <= task['partitions']:
                raise ValueError('unexpected LLVM partition count')
            if env.get('DISTCC_VERBOSE') == '1':
                print(helper + ': partitioned ' + task['inputs'][0] + ' into '
                      + str(len(partition['parts'])), file=sys.stderr)
            count = len(partition['parts'])
            group = None
            if count == 1:
                if partition['locals']:
                    raise ValueError('unsplit module has promoted symbols')
                outputs = [absolute(task['outputs'][0])]
            else:
                merge_dir = jobdir / 'merge'
                merge_dir.mkdir()
                outputs = [str(merge_dir / f'part-{i}.o') for i in range(count)]
                group = dict(directory=merge_dir, outputs=outputs, locals=partition['locals'],
                             destination=task['destinations'][0], remaining=count)
            children = []
            primary = absolute(task['inputs'][0])
            for i, (part, output) in enumerate(zip(partition['parts'], outputs)):
                expected = f'{manifest}.part-{i}.bc'
                if part['path'] != expected:
                    raise ValueError('unexpected LLVM partition path')
                path = str(root / expected.lstrip('/'))
                args = [path if absolute(arg) == primary else arg for arg in task['args']]
                args[args.index('-o') + 1] = output
                args.extend(['-mllvm', '-thinlto-assume-merged'])
                child = dict(args=args, inputs=[path, task['index']], outputs=[output],
                             destinations=[output] if group else task['destinations'],
                             instructions=part['instructions'])
                if group:
                    child['group'] = group
                children.append(child)
            return children

        def merge(group):
            assert tools is not None
            directory = group['directory']
            sandbox = llvm_sandbox(tools['linker'].parent)
            sandbox.extend(['--bind', str(directory), CONTROL, '--remount-ro', '/',
                            '--chdir', CONTROL, '--clearenv', '--'])
            environment = dict(os.environ, SANDBOX_ON='0')
            parts = [Path(p).name for p in group['outputs']]
            subprocess.run([*sandbox, tools['linker'], '-r', '-o', 'merged.o', *parts], env=environment, check=True)
            output = 'merged.o'
            if group['locals']:
                output = 'localized.o'
                (directory / 'localize.rsp').write_text(shlex.join(
                    ['--localize-symbol=' + name for name in group['locals']] + ['merged.o', output]))
                subprocess.run([*sandbox, tools['objcopy'], '@localize.rsp'], env=environment, check=True)
            destination = group['destination']
            with os.fdopen(open_regular(directory / output), 'rb') as source, ExitStack() as stack:
                os.replace(stage_output(source, destination, stack), destination)

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
                            register(child, open_regular)
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


def llvm_sandbox(tool_dir, root=None):
    namespaces = ['--unshare-all']
    if os.geteuid() == 0:
        # Portage installs as root below the build user's mode-0700 directory.
        # A new user namespace loses host DAC override before bwrap opens the
        # bind sources. Keep it for mount setup, then drop every capability
        # before executing Clang in the isolated, read-only filesystem.
        namespaces = ['--unshare-ipc', '--unshare-pid', '--unshare-net',
                      '--unshare-uts', '--unshare-cgroup-try', '--cap-drop', 'ALL']
    sandbox = [shutil.which('bwrap', path=os.defpath) or '/usr/bin/bwrap', *namespaces, '--die-with-parent']
    if root is not None:
        sandbox.extend(['--bind', str(root), '/'])
    for runtime in RUNTIME:
        if Path(runtime).exists():
            sandbox.extend(['--ro-bind', runtime, runtime])
    # Keep the installed tool directory and its symlinks intact: Clang locates
    # LLD and resource files relative to its executable. Binding a symlink as
    # an individual mount target is also rejected by bubblewrap.
    sandbox.extend(['--ro-bind', str(tool_dir), str(tool_dir), '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp'])
    return sandbox


def run_preparation(root, job, prepare_library):
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
    # LLVM backends may use either driver arguments or a compiler-generated
    # cc1 invocation. Let Clang validate its action; cc1 uses -emit-obj, not -c.
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
    rooted(job['cwd']).mkdir(parents=True, exist_ok=True)
    sandbox = llvm_sandbox(Path(compiler).parent, root)
    sandbox.extend(['--dir', job['cwd']])
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
    # Portage's preload cannot follow bubblewrap's UID maps and pivot_root.
    # Exempt only namespace setup; the compiler runs with a cleared environment
    # inside the read-only mounts above. The parent's sandbox stays active.
    result = subprocess.run([*sandbox, compiler, *args],
                            env=dict(os.environ, SANDBOX_ON='0'))
    if result.returncode:
        return result.returncode if result.returncode > 0 else 128 - result.returncode
    if not outputs or not outputs[0].stat().st_size:
        raise ValueError('LLVM job produced no output')
    return 0


class LinkFiles:
    def __init__(self, work, linked, temporary, stack):
        self.work = Path(work)
        self.linked = Path(linked)
        self.writable = (self.linked.parent, Path(temporary))
        self.stack = stack
        self.directories = {}
        self.outputs = {}
        for root in (self.work, *self.writable):
            self.directory(root, root)
        declared = (self.work / 'dtlto-inputs').read_bytes().split(b'\0')
        self.declared = {os.fsdecode(p): os.path.realpath(p) for p in declared if p}

    def directory(self, root, path):
        # Keep each parent pinned across the read or publication. The linker
        # can concurrently rename entries inside its two writable mounts.
        if path not in self.directories:
            parent = None if path == root else self.directory(root, path.parent)
            fd = os.open(path if parent is None else path.name,
                         os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            self.stack.callback(os.close, fd)
            self.directories[path] = fd
        return self.directories[path]

    def locate(self, path, roots):
        if not path.is_absolute() or '..' in path.parts:
            raise ValueError('invalid ThinLTO path')
        for root in roots:
            if root in path.parents:
                return self.directory(root, path.parent)
        raise ValueError('ThinLTO path outside link directories: ' + str(path))

    def open_input(self, name, *, description=False):
        path = Path(name)
        if not description and name in self.declared:
            return open_regular(self.declared[name])
        roots = self.writable if description else (*self.writable, self.work)
        return open_regular(path.name, self.locate(path, roots))

    def request(self, description, compiler):
        with os.fdopen(self.open_input(description, description=True), 'rb') as stream:
            limit = 16 * 1024 * 1024
            data = stream.read(limit + 1)
            if len(data) > limit:
                raise ValueError('ThinLTO description too large')
        request = json.loads(data)
        common = request['common']
        if common['linker_output'] != str(self.linked) or common.get('outputs'):
            raise ValueError('unexpected ThinLTO link output')
        args = common['args']
        if not isinstance(args, list) or args[:2] != [compiler, '-c'] or not all(
                isinstance(arg, str) and not arg.startswith('@') for arg in args):
            raise ValueError('unexpected ThinLTO compiler arguments')
        jobs = []
        self.outputs.clear()
        for job in request['jobs']:
            inputs = job['inputs']
            output, = job['outputs']
            path = Path(output)
            parent = self.locate(path, self.writable)
            if (str(path) != output or output in self.outputs or path == self.linked or len(inputs) < 2 or
                    job['args'] != [inputs[0], '-fthinlto-index=' + inputs[1], '-o', output]):
                raise ValueError('unexpected ThinLTO backend job')
            self.outputs[output] = Path('/proc/self/fd') / str(parent) / path.name
            jobs.append(dict(args=job['args'], inputs=inputs, outputs=[output]))
        return dict(common=dict(args=args, inputs=common['inputs']), jobs=jobs)


def run_link(work, linked, temporary, compiler, filter_fd, command):
    with ExitStack() as stack:
        files = LinkFiles(work, linked, temporary, stack)
        parent, child = socket.socketpair(type=socket.SOCK_SEQPACKET)
        stack.enter_context(parent)
        stack.enter_context(child)
        fd = child.fileno()
        process = subprocess.Popen(command, pass_fds=(int(filter_fd), fd),
                                   env=dict(os.environ, DISTCC_RUST_DTLTO_FD=str(fd)))
        child.close()
        try:
            failed = 0
            while message := parent.recv(4097):
                if len(message) > 4096:
                    raise ValueError('invalid ThinLTO description name')
                request = files.request(os.fsdecode(message), compiler)
                try:
                    status = distribute(request, JOB_THINLTO, files)
                except subprocess.CalledProcessError as error:
                    status = error.returncode
                status = 128 - status if status < 0 else status
                failed = failed or status
                parent.sendall(bytes([status]))
            status = process.wait()
            return failed or (128 - status if status < 0 else status)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main(kind):
    try:
        if kind == JOB_THINLTO and sys.argv[1:2] == ['--rust-link']:
            work, linked, temporary, compiler, filter_fd, *command = sys.argv[2:]
            return run_link(work, linked, temporary, compiler, filter_fd, command)
        if len(sys.argv) == 2:
            return distribute(json.loads(Path(sys.argv[1]).read_text()), kind)
        print('usage: ' + Path(sys.argv[0]).name + ' LLVM-JOB.json', file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(Path(sys.argv[0]).name + ': ' + str(error), file=sys.stderr)
        return error.returncode if error.returncode > 0 else 128 - error.returncode
    except (LocalOnly, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(Path(sys.argv[0]).name + ': ' + str(error), file=sys.stderr)
        return 1
