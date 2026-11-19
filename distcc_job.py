"""Immutable file snapshots and metadata for pooled compiler jobs."""
import fcntl
import mmap
import os
import shutil
import struct

import distcc_python
import xxhash


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


def manifest(kind, cwd, argv, inputs, *, rust=None):
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
    if kind == 3:
        if rust is None:
            raise ValueError('Rust command metadata is required')
        phase, compiler, host, cpu, features, directories, environment = rust
        number(phase)
        for field in (compiler, host, cpu, features, *directories, '', *environment, ''):
            string(field)
    for name, size, digest, cached in inputs:
        for value in (0, 0, cached, size, size):
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


def write_metadata(kind, cwd, helper, entries, path, *, work_key=0, work_units=0):
    prefix = struct.pack('!QI', work_key, min(work_units, 2**32 - 1))
    path.write_bytes(prefix + manifest(kind, cwd, [helper], [(*entry, 1) for entry in entries]))
