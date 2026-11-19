"""Service LLD's ThinLTO request while its Rust linker stays in bubblewrap."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess

from distcc_lto import distribute


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
        flags = os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW
        if not description and name in self.declared:
            fd = os.open(self.declared[name], flags)
        else:
            roots = self.writable if description else (*self.writable, self.work)
            fd = os.open(path.name, flags, dir_fd=self.locate(path, roots))
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            os.close(fd)
            raise ValueError('expected regular ThinLTO input')
        return fd

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
        if args[:2] != [compiler, '-c'] or not all(isinstance(a, str) for a in args):
            raise ValueError('unexpected ThinLTO compiler')
        # Only the LLVM backend action crosses the socket. Extra driver
        # actions or compiler plugins in a captured link remain sandboxed.
        options = iter(args[2:])
        for arg in options:
            if arg == '-mllvm' and next(options, '') == '-no-pgo-warn-mismatch':
                continue
            if arg.startswith('-fprofile-sample-use=') and arg.split('=', 1)[1] in common['inputs']:
                continue
            if not re.fullmatch(r'--target=[\w.+-]+|-O[0-3]|-f(?:addrsig|function-sections|data-sections|'
                                r'loop-interchange|pic)|-Wno-unused-command-line-argument', arg):
                raise ValueError('unexpected ThinLTO backend option: ' + arg)
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
                    status = distribute(request, 4, files)
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
