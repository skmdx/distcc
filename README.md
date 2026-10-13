# distcc -- a free distributed C/C++ compiler system

by Martin Pool

Current Documents: <https://distcc.github.io/>

Formerly <http://distcc.org/>

"pump" functionality added by
Fergus Henderson, Nils Klarlund, Manos Renieris, and Craig Silverstein (Google Inc.)

distcc is a program to distribute compilation of C or C++ code across
several machines on a network. distcc should always generate the same
results as a local compile, is simple to install and use, and is often
two or more times faster than a local compile.

Unlike other distributed build systems, distcc does not require all
machines to share a filesystem, have synchronized clocks, or to have
the same libraries or header files installed. Machines can be running
different operating systems, as long as they have compatible binary
formats or cross-compilers.

By default, distcc sends the complete preprocessed source code across
the network for each job, so all it requires of the volunteer machines
is that they be running the distccd daemon, and that they have an
appropriate compiler installed.

The distcc "pump" functionality, added in distcc 3.0, improves on
distcc by distributing not only compilation but also preprocessing to
distcc servers. This requires that the server and client have the same
system headers (the client takes responsibility for transmitting
application-specific headers).  Given that, distcc in pump mode yields
the same results that distcc would in non-pump mode, but faster, since
the preprocessor no longer runs locally. For more details on the pump
functionality, see README.pump.

distcc is not itself a compiler, but rather a front-end to the GNU
C/C++ compiler (gcc), or another compiler of your choice. All the
regular gcc options and features work as normal.

distcc is designed to be used with GNU make's parallel-build feature
(-j). Shipping files across the network takes time, but few cycles on
the client machine. Any files that can be built remotely are
essentially "for free" in terms of client CPU.  This is even more true
in "pump" mode, where the client does not even have to take time to
preprocess the source files.  distcc has been successfully used in
environments with hundreds of distcc servers, supporting dozens of
simultaneous compiles.

distcc is now reasonably stable and can successfully compile the Linux
kernel, rsync, KDE, GNOME (via GARNOME), Samba and Ethereal.  distcc
is nearly linearly scalable for small numbers of machines: for a
typical case, three machines are 2.6 times faster than one.

## Adaptive scheduling (this fork)

This fork adds opt-in allocation by online CPU count, one-minute load average,
and relative per-core compilation speed. Start each server with `distccd --stats`
in addition to its usual listen/access options, then configure the client:

```sh
export DISTCC_HOSTS='builder-a,stats=3633,speed=1 builder-b,stats=3633,speed=2'
make -j"$(distcc -j)" CC='distcc gcc' CXX='distcc g++'
```

`speed=2` means twice the per-core compile throughput of `speed=1`. Measure a
representative single-job compile with the same compiler/options on idle hosts
and use `reference_time / host_time`. The default is 1; CPU clock frequency is
not used as a performance estimate. Valid factors range from 0.01 to 1000.

With `stats=PORT`, the slot limit defaults to the smaller of the server's online
CPU count and `--jobs` limit. An explicit `/LIMIT`, for example
`builder-a/8,stats=3633`, adds a client-side ceiling. `distcc -j` reports these
limits. Servers without statistics retain their configured slot count (default
4). An old server without the new `dcc_cpus` metric also uses this fallback.

For each host with a free local slot, the client estimates one job's relative
completion time: `max(1, (occupied + queued + 1) / parallelism,
(runnable + 1) / cpus) / speed`. Parallelism is the smaller of CPUs and the
server's job limit; occupied is the larger of local reservations and reported
server occupancy, avoiding double counting. Faster cores win while capacity
is free. Equal estimates favor the lower local slot utilization, then list
order. Missing live runnable counts fall back to the one-minute load average.
Reported memory PSI (`some avg10`, percent) multiplies the estimate by
`1 + pressure / 100`. Estimates do not predict individual translation-unit
sizes, transfer time, compiler-specific speed or container CPU quotas.

With the Linux broker and a supporting server, each legacy pooled connection reserves
a server worker before transferring a job. Ordinary broker clients finish
local preprocessing before reserving that worker and reuse the preprocessed
input on retries. A full server
refuses immediately so the broker can try another host, even when several
brokers saw the same idle statistics. Refused connections remain reusable;
retries to a full host are limited to once per 50ms per broker. Idle pooled
connections reserve no workers. Direct/older clients retain local slot locks
and can queue at the server. The server's `--jobs` limit applies to both.

Link requests bypass local preprocessing slots. Admission permits one link
per server at a time. Linux admission for both compiles and links checks host
available memory, accessible cgroup memory limits and memory PSI once per second.
`DISTCC_JOB_MEMORY` sets the estimated memory per job in MiB (default 1024),
with an additional quarter-job margin. New work waits when PSI is at least 10%
or available headroom is too small. The same check limits local compile and
preprocessing slots. Existing work finishes normally; these estimates are not
hard per-job limits or a common scheduler for unrelated build processes.

Failed remote transfers mark the broker's worker unavailable for
`DISTCC_BACKOFF_PERIOD` seconds (default 60, set on the broker). A compile tries
at most three remote jobs even when backoff is disabled, then follows
`DISTCC_FALLBACK`. Server protocol reads and writes use `DISTCC_IO_TIMEOUT`
(default 300 seconds), so a silent partial request cannot retain a worker forever.

Statistics successes and failures are shared within each wall-clock second in
`DISTCC_DIR/lock`. Each uncached probe has a 250ms deadline including DNS; probes
are sequential across hosts. Unavailable or invalid statistics produce a warning
and use configured slots/speed until the next refresh. The stats endpoint must
be reachable from the client and uses the daemon's existing access rules; it is
plain HTTP, including when compilation uses GSSAPI. `stats` requires a TCP host.
`speed` alone also enables weighted selection and can be used with SSH hosts.
In mixed lists, entries without metrics use their configured slots and speed 1.
Final ties follow list order (`--randomize` can change that order). Repeated entries for
the same lock endpoint use the first entry. Lists without either new option
retain the original selection algorithm. Compression and pump options can be
combined with adaptive options. Use `builder-a,lz4` for LZ4 compression, or
`builder-a,cpp,lz4` under `pump` for remote preprocessing. Both ends must run
this fork with LZ4 support. `,lzo` retains upstream-compatible LZO compression;
the two codecs can coexist in a host list, including with the broker and pump.
Upgrade servers before changing client host entries to `,lz4`. Restart brokers
and active pump sessions after upgrading the client.

## Prepared TCP jobs

Use `,tcp` to select the prepared-job protocol and `,cpp,tcp` under `pump`:

```sh
export DISTCC_HOSTS='builder-a/8,tcp,stats=3633'
make -j"$(distcc -j)" CC='distcc gcc' CXX='distcc g++'
```

Both endpoints must run this version. Restart the client broker and active
pump sessions after upgrading. This path requires the Linux broker and the
daemon's prefork mode. It uses the existing TCP listener and IP access rules;
SSH and GSSAPI are unsupported. Do not combine `,tcp` and `,h2`.

The broker lends one reusable TCP connection per outstanding job. The server
prepares inputs before reserving a compiler worker and releases the worker
before returning results. LZ4 chunks, verified shared-input caching, remote
preprocessing and GCC/Clang distributed linking are supported. Link execution
retains the one-link-per-server limit. Preparing and returning jobs can exceed
the compiler-slot count, but share a bounded 512 MiB storage budget; compiler
working memory and the persistent input cache are separate. Set the daemon's
`TMPDIR` to suitable tmpfs storage when available.

This is the recommended opt-in path for shared-input and transfer-heavy builds.
Small builds need not beat HTTP/2; compare your workload with `bench/rpc.py`
before changing an existing setup. The stats counter `dcc_tcp_ready_idle_ns`
records cumulative unused admissible slot nanoseconds while an eligible
prepared job is queued. It excludes time waiting for inputs and resource
admission. Wire details and limits are in [the protocol specification](doc/protocol-6.txt).

Build using the usual `./autogen.sh`, `./configure`, and `make` steps, with the
LZ4, nghttp2 and xxHash (with XXH3 support) libraries and
development headers installed. Pump also requires Python `xxhash>=3.0.0`.
`make adaptive-check` runs real GCC compiles through two local daemons, checks
the linked executables, and exercises controlled load/error responses and slot
locking. Set `TMPDIR` to a suitable temporary directory. `make check` includes
these tests and the existing regression suites.

## Licence

distcc is distributed under the GNU General Public Licence v2.

## Resources

* Repo, questions, and bugs: https://github.com/distcc/distcc

* [Stack Overflow questions](http://stackoverflow.com/questions/tagged/distcc)
