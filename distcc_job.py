"""Immutable file snapshots and metadata for pooled compiler jobs."""
import fcntl
import mmap
import os
import pwd
import shutil
import stat
import struct
from typing import NamedTuple

import distcc_python
import xxhash


(JOB_COMPILE, JOB_LINK, JOB_RUST, JOB_THINLTO, JOB_OFFLOAD_LINK,
 JOB_FRONTEND, JOB_GCC_FRONTEND, JOB_RUST_CGU) = range(8)
BROKER_VERSION = 11
BROKER_POOLED = 1
TCP_BEGIN = 0x54000400
TCP_RESULT_FD = TCP_BEGIN + 6
TCP_PROGRESS = TCP_BEGIN + 8
TCP_COMPLETE_SIZE = 8 + 5 * 8 + 12 * 4


def broker_path(run_dir='/run'):
    configured = os.environ.get('DISTCC_BROKER')
    if configured is not None:
        return configured or None
    if 'DISTCC_HOSTS' in os.environ or 'DISTCC_DIR' in os.environ:
        return None
    home = os.environ.get('HOME')
    if home is not None and os.access(home + '/.distcc/hosts', os.R_OK):
        return None
    uid = os.geteuid()
    try:
        name = pwd.getpwuid(uid).pw_name
        if not name or '/' in name:
            return None
        path = os.path.join(run_dir, f'distcc-broker-{name}.socket')
        info = os.lstat(path)
    except (OSError, KeyError):
        return None
    if stat.S_ISSOCK(info.st_mode) and info.st_uid == uid and not info.st_mode & 0o077:
        return path
    return None


def read_argv(fd):
    def read(size):
        data = bytearray()
        while len(data) < size:
            part = os.read(fd, min(size - len(data), 65536))
            if not part:
                raise ValueError('truncated argument file')
            data.extend(part)
        return data

    def number(token):
        header = read(12)
        digits = header[4:].lstrip(b' ')
        if header[:4] != token or not digits or any(c not in b'0123456789abcdefABCDEF' for c in digits):
            raise ValueError('invalid argument header')
        return int(digits, 16)

    return [os.fsdecode(bytes(read(number(b'ARGV')))) for _ in range(number(b'ARGC'))]


def write_argv(fd, argv):
    def header(tag, count):
        return f'{tag}{count:08x}'.encode('ascii')
    data = bytearray(header('ARGC', len(argv)))
    for arg in argv:
        word = os.fsencode(arg)
        if b'\0' in word:
            raise ValueError('NUL in argument')
        data.extend(header('ARGV', len(word)))
        data.extend(word)
    pending = memoryview(data)
    while pending:
        pending = pending[os.write(fd, pending):]


def tokenize_driver_line(line):
    words, word = [], []
    quote = None
    started = False
    chars = iter(line)
    for c in chars:
        if c == '\\':
            word.append(next(chars, c))
        elif quote:
            if c == quote:
                quote = None
            else:
                word.append(c)
        elif c in ('"', "'"):
            quote = c
        elif c.isspace():
            if started:
                words.append(''.join(word))
                word.clear()
            started = False
            continue
        else:
            word.append(c)
        started = True
    if started:
        words.append(''.join(word))
    return words


def format_driver_command(argv):
    def display(text, force):
        special = '\\"$' + chr(96)
        quoted = force or not text or any(c.isspace() or c in special or c == "'" for c in text)
        if quoted:
            return '"' + ''.join(('\\' if c in special else '') + c for c in text) + '"'
        return text
    return ''.join(' ' + display(arg, i == 0) for i, arg in enumerate(argv))


class LocalOnly(Exception):
    pass


def stamp(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def snapshot(source, destination, expected=None):
    with open(source, 'rb') as src, open(destination, 'xb') as dst:
        before = stamp(os.fstat(src.fileno()))
        if expected is not None and expected != before:
            raise LocalOnly('input changed after dependency discovery')
        try:
            fcntl.ioctl(dst.fileno(), 0x40049409, src.fileno())  # FICLONE
        except OSError:
            shutil.copyfileobj(src, dst, 256 * 1024)
        if before != stamp(os.fstat(src.fileno())):
            raise LocalOnly('input changed during snapshot')


def fingerprint(path):
    size = path.stat().st_size
    if size:
        with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            digest = xxhash.xxh3_64_digest(data)
    else:
        digest = xxhash.xxh3_64_digest(b'')
    return size, digest


class RustCommand(NamedTuple):
    phase: int
    compiler: str
    pipeline: bool = False
    linker: str = ''
    crate_types: str = ''
    host: str = ''
    cpu: str = ''
    features: str = ''
    directories: tuple[str, ...] = ()
    environment: tuple[str, ...] = ()


def manifest(kind, cwd, argv, inputs, *, rust=None, outputs=(), input_kind=0):
    """Encode the bounded BEGIN manifest, including its first-use parent table."""
    data = bytearray()
    parents = {}

    def number(value):
        if not 0 <= value < 1 << 64:
            raise ValueError('manifest integer out of range')
        while value >= 128:
            data.append(128 | (value & 127))
            value >>= 7
        data.append(value)

    def string(value):
        encoded = os.fsencode(value)
        number(len(encoded))
        data.extend(encoded)

    if len(inputs) > 65536:
        raise LocalOnly('too many job inputs')
    for value in (kind, len(argv), len(inputs)):
        number(value)
    string(cwd)
    for arg in argv:
        string(arg)
    if kind in (JOB_THINLTO, JOB_OFFLOAD_LINK):
        if not outputs:
            raise ValueError('LLVM job requires declared outputs')
        for name in outputs:
            string(name)
        string('')
    if kind == JOB_RUST:
        if rust is None:
            raise ValueError('Rust command metadata is required')
        number(rust.phase)
        number(rust.pipeline)
        for field in (rust.linker, rust.crate_types, rust.compiler, rust.host, rust.cpu, rust.features,
                      *rust.directories, '', *rust.environment, ''):
            string(field)
    for name, size, digest, cached in inputs:
        for value in (input_kind, cached, size):
            number(value)
        data.extend(digest)
        parent, slash, base = os.fsencode(name).rpartition(b'/')
        parent += slash
        number(parents.get(parent, 0))
        if parent not in parents:
            parents[parent] = len(parents) + 1
            string(parent)
        string(base)
        string('')
    if len(data) > 16 * 1024 * 1024:
        raise LocalOnly('job metadata is too large')
    return data


def write_metadata(kind, job, entries, path, *, work_key=0, work_units=0):
    prefix = struct.pack('!QI', work_key, min(work_units, 2**32 - 1))
    path.write_bytes(prefix + manifest(kind, job['cwd'], job['args'], [(*entry, 1) for entry in entries],
                                       outputs=job['outputs']))
