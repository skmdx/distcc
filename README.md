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

By default, distcc sends source code with its headers expanded across
the network for each job, so all it requires of the volunteer machines
is that they be running the distccd daemon, and that they have an
appropriate compiler installed.
Clang retains macros until remote compilation to preserve diagnostics;
see README.pump for preprocessing behavior and toolchain requirements.

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

For each eligible prepared-job host, the broker scores normalized demand as
`max((work + 1) / parallelism, (runnable + 1) / cpus) / speed`.
Legacy paths retain a minimum service cost of 1 before dividing by speed.
Parallelism is the smallest of CPUs, the server's job limit and its reported
memory admission capacity. Legacy paths use the larger of local reservations
and the sum of server execution and ready-queue counts; they do not add the
same queued jobs twice. Lower normalized demand wins. Equal
estimates favor lower estimated work per configured slot, then list order.
Missing live runnable counts fall back to the one-minute load average.
Reported memory PSI (`some avg10`, percent) multiplies the estimate by
`1 + pressure / 100`. Estimates do not predict individual translation-unit
sizes, transfer time, compiler-specific speed or container CPU quotas.

With `,tcp` and live pipeline statistics, the broker projects the global server
snapshot instead of counting all local socket leases as CPU work. It adds
outstanding assignments made since the probe started, and subtracts completed
snapshot work using the existing connection-return notification. Completions
first consume the snapshot's returning count, so output-only jobs do not free
CPU capacity twice. No per-stage IPC or extra network round trip is required.
Input preparation can overlap execution within a pipeline of up to four jobs
(also capped by effective parallelism); excess preparation increases the score.
Prepared-job hosts are ranked by projected work divided by execution capacity
and relative speed, with runnable CPU load and memory pressure as additional
costs. The score increases below saturation too, so raising memory capacity
does not give a fast host a flat score that starves the other workers.
Execution, ready jobs, preparation and new assignments share this estimated
global limit of parallelism plus pipeline depth. Returning jobs have a separate
bounded allowance for socket leases. A zero memory capacity stops new uploads;
busy compiler slots alone do not. Links avoid hosts with a reported active link
or a new unsampled link assignment.

These are bounded estimates, not distributed reservations: simultaneous
brokers and assignments in transit can temporarily exceed the estimated queue
limit. Server worker and storage admission remain authoritative. Jobs accepted
during a probe can be counted conservatively until the next update. Configure
statistics on every TCP worker to use this policy; without them, the broker
uses configured slots, speed and local leases.

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
or available headroom is too small. After a PSI stop, admission resumes below
5% once memory headroom is sufficient. The same check limits local compile and
preprocessing slots. Existing work finishes normally; these estimates are not
hard per-job limits or a common scheduler for unrelated build processes.

On Linux, `DISTCC_MEMORY_LEARNING=1` on the daemon uses compiler peak RSS
reported by `wait4` for prepared jobs. `DISTCC_JOB_MEMORY` is the cold-start
estimate. After eight successful observations, the estimate becomes twice the
90th percentile of the last 64 successful RSS observations plus 64 MiB, with a
256 MiB minimum. Spare capacity is the greater of a quarter-job margin and the
largest observed RSS, including failed compiles. This high-water reserve lasts
until daemon restart; an unusually large job does not inflate the estimate for
every subsequent small job. Cgroup limits, available headroom, PSI and `--jobs`
remain in force. Multiple simultaneous unseen large jobs can still exceed the
estimate; the service's memory limit must remain configured. When the combined
estimate and reserve exceed the budget, one job may run if the largest measured
RSS plus 25% and 64 MiB fits both the budget and available memory. This allows
new observations without opening a parallel burst. RSS
is an estimate, not a guarantee for an unseen workload or a hard job budget.
Statistics expose `dcc_job_memory_bytes`, `dcc_job_memory_samples` and
`dcc_job_peak_bytes` and `dcc_memory_spare_bytes`. Leave learning unset to keep a fixed estimate.

To avoid copying ordinary preprocessed input to a server on the same machine,
set `DISTCC_LOCAL_SOCKET` to the same absolute socket prefix on the daemon and
broker. The daemon creates `<prefix>-<port>.sock`; its parent directory must
already exist and be writable only by the daemon or administrator. Enabling
this listener permits all local users to submit prepared jobs (socket mode
0666), so it is intended for locally trusted build machines. The broker uses
it only when the worker address matches a local interface, and otherwise uses
TCP. A missing local listener also uses TCP.

The client copies input once in the kernel into an anonymous sealed memfd and
passes a read-only descriptor over this Unix stream. The snapshot can be
opened by a compiler running under a different UID without changing the named
input's permissions. This removes input compression, socket payload transfer
and server-side materialization, rather than eliminating every copy. The
existing server worker receives it with the metadata in one IPC message and
inherits it only for that compilation. Input lifetime, cancellation and the
server's common admission limit are unchanged. Results retain the existing
atomic publication path. Pump and link inputs still use framed payloads.
`dcc_local_input_jobs` and `dcc_local_input_bytes` count accepted input
descriptors and their uncompressed sizes, independently of TCP accept counts.

Failed transfers on reused connections discard that worker's idle pool so the
next attempt can open a fresh connection. Failure on a fresh connection marks
the worker unavailable for `DISTCC_BACKOFF_PERIOD` seconds (default 60, set on
the broker). A compile tries
at most three remote jobs even when backoff is disabled, then follows
`DISTCC_FALLBACK`. Server protocol reads and writes use `DISTCC_IO_TIMEOUT`
(default 300 seconds), so a silent partial request cannot retain a worker forever.

Without a broker, statistics successes and failures are shared within each wall-clock second in
`DISTCC_DIR/lock`. Each uncached probe has a 250ms deadline including DNS; probes
are sequential across hosts. Unavailable or invalid statistics produce a warning
and use configured slots/speed until the next refresh. The broker probes hosts
in parallel once per second and retains the last valid response for at most
five seconds after failures. The stats endpoint must
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
SSH and GSSAPI are unsupported with `,tcp`.

The broker lends one reusable TCP connection per outstanding job. The server
prepares inputs before reserving a compiler worker and releases the worker
before returning results. LZ4 chunks, verified shared-input caching, remote
preprocessing and GCC/Clang distributed linking are supported. Link execution
retains the one-link-per-server limit. Preparing and returning jobs can exceed
the compiler-slot count, but share a bounded 512 MiB storage budget; compiler
working memory and the persistent input cache are separate. Set the daemon's
`TMPDIR` to suitable tmpfs storage when available.
Large compressed inputs and result compression can use up to two background
threads. Uncached inputs start without a request round trip, and received
objects go directly into unpublished destination files. Update clients and
daemons together: this wire version does not accept earlier `,tcp` clients.

This is the recommended opt-in path for shared-input and transfer-heavy builds.
Compare your workload with the legacy TCP paths using `bench/rpc.py`
before changing an existing setup. The stats counter `dcc_tcp_ready_idle_ns`
records cumulative unused admissible slot nanoseconds while an eligible
prepared job is queued. It excludes time waiting for inputs and resource
admission. Wire details and limits are in [the protocol specification](doc/protocol-6.txt).

Build using the usual `./autogen.sh`, `./configure`, and `make` steps, with the
LZ4 and xxHash (with XXH3 support) libraries and
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
