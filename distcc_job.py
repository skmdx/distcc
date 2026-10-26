"""Immutable file snapshots and metadata for pooled compiler jobs."""
import fcntl
import mmap
import os
import shutil
import struct

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


def write_metadata(kind, cwd, helper, entries, path, *, work_key=0, work_units=0):
    # This prefix belongs to the local planner/transport contract. The TCP
    # worker receives the job description, not client scheduling estimates.
    data = bytearray(struct.pack('!QI', work_key, min(work_units, 2**32 - 1)))

    def number(value):
        data.extend(struct.pack('!Q', value))

    def string(value):
        encoded = os.fsencode(value)
        number(len(encoded))
        data.extend(encoded)

    if len(entries) > 65536:
        raise LocalOnly('too many job inputs')
    for value in (kind, 1, len(entries)):
        number(value)
    string(cwd)
    string(helper)
    for name, size, digest in entries:
        for value in (0, 0, 1, size, size):
            number(value)
        data.extend(digest)
        string(name)
        string('')
    if len(data) > 16 * 1024 * 1024:
        raise LocalOnly('job metadata is too large')
    path.write_bytes(data)
