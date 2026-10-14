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

This fork adds opt-in allocation by physical cores, SMT contention, runnable
load and relative single-job compilation speed. Start each server with `distccd --stats`
in addition to its usual listen/access options, then configure the client:

```sh
export DISTCC_HOSTS='builder-a,stats=3633,speed=1 builder-b,stats=3633,speed=2'
make -j"$(distcc -j)" CC='distcc gcc' CXX='distcc g++'
```

Statistics report worker occupancy and memory availability without scanning
every process. The global D-state count and largest-process RSS diagnostics
are no longer included in the statistics response.

`speed=2` means twice the per-core compile throughput of `speed=1`. Measure a
representative single-job compile with the same compiler/options on idle hosts
and use `reference_time / host_time`. The default is 1; CPU clock frequency is
not used as a performance estimate. Valid factors range from 0.01 to 1000.

With `stats=PORT`, the slot limit defaults to the smaller of the server's allowed
logical CPU count and `--jobs` limit. An explicit `/LIMIT`, for example
`builder-a/8,stats=3633`, adds a client-side ceiling. `distcc -j` reports these
limits. Servers without statistics retain their configured slot count (default
4). Missing or invalid statistics also use this fallback. Clients and servers
must report the same statistics schema, including CPU topology and coefficients.

For each eligible host, CPU service cost is estimated from aggregate
throughput, expressed in units of one idle compile. Throughput interpolates
from 1 at one job to `physical_cores * core_scale` at one job per physical core.
Each additional SMT thread contributes `smt_gain * core_scale`; throughput is
bounded below by 1. Work beyond the execution limit queues rather than adding
throughput. Estimated service time is `max(1, demand / throughput) / speed`.
The broker takes the larger estimate from projected work and runnable system
load, without multiplying or adding these overlapping contention penalties.
Faster SMT threads can still beat idle cores on a slower host.
Parallelism is the smallest of CPUs, the server's job limit and its reported
memory admission capacity. Legacy paths use the larger of local reservations
and the sum of server execution and ready-queue counts; they do not add the
same queued jobs twice. Lower completion cost wins. Equal
estimates favor lower estimated work per configured slot, then list order.
Missing live runnable counts fall back to the one-minute load average.
Reported memory PSI (`some avg10`, percent) multiplies the estimate by
`1 + pressure / 100`. The CPU model does not measure compiler-specific host
speed or container CPU quotas.

Linux statistics count physical cores from the allowed CPUs' sysfs sibling
lists and cache the topology until CPU affinity changes. If topology cannot
be read, each allowed CPU is treated as a core. Configure the server environment
with `DISTCC_CORE_SCALE` (0..1, default 1) and `DISTCC_SMT_GAIN` (-1..1, default
0.25). Negative SMT gain represents an observed loss of aggregate throughput;
these coefficients affect selection, not hard worker or memory limits.
They are fixed calibration values, while occupancy and runnable load refresh
with statistics. Restart the daemon after changing them.

To account for changing CPU frequency, also set `DISTCC_CPU_FREQ_REF=F1,FP,FL`
on the server. These are hardware-observed frequencies in kHz, measured alongside
the calibration batches at one job, one job per physical core, and one job per
logical CPU. Use the same batches for the contention coefficients and frequency
references; using a maximum advertised clock would count normal multicore clock
reductions twice. Keep the client `speed` consistent with the single-job batch.

On Linux the daemon samples `cpuinfo_avg_freq` and per-CPU `/proc/stat` counters
at most twice per second while workers execute. Active CPU time weights the
frequency; idle intervals and samples covering less than 80% of busy CPU time
are excluded. The reference frequency interpolates at the observed active CPU
count. The current/reference ratio, capped at 1, scales the CPU service cost
once, including single-job work. Frequency is an approximation to compilation
capacity, not a cross-machine speed measurement. The filter uses
`alpha = dt / (tau + dt)`, with tau 0.5 seconds for decreases and 10 seconds for
increases. Idle time never advances recovery or changes the reference curve.

Statistics expose `dcc_cpu_scale`, `dcc_cpu_age` (seconds since the last valid
sample), `dcc_cpu_probe`, frequency/reference in kHz, active CPU count, and sample
count. A scale and age of -1 mean unavailable, including when calibration is
disabled or hardware feedback is absent. Temperature and privileged MSR reads
are not required. The samples share the existing statistics response and add no
per-job IPC or network round trip.

After ten seconds without a valid sample, an idle degraded host becomes eligible
for a real recovery job. Brokers offer at most one trial per ten seconds. The
server admits at most one compiler for a two-second trial, retaining waiting
jobs in the existing queues. A fresh sample during the trial extends the limit
for two seconds so clients can refresh. If feedback is still unavailable, the
trial expires rather than serializing short jobs indefinitely. Worker and memory
ceilings still apply.
Already running compilers continue. Missing telemetry retains the last measured
reduction; it is never treated as proof of recovery.

To calibrate, measure batch completion time for identical representative
compiles started together at concurrency 1, P (physical cores), and L (logical
CPUs). Average repeated batches to obtain T1, TP, TL.
Use `core_scale = T1 / TP` and `smt_gain = (L * TP / TL - P) / (L - P)`, bounded
to the accepted ranges. SMT gain is irrelevant when L equals P. Keep inputs,
compiler/options and memory limits constant, and warm up the CPU until batch
time, frequency and power have stabilized. Interleave concurrency levels and
record temperature and throttling: comparing an early turbo burst with a later
throttled batch can falsely produce a negative SMT gain. A fixed-frequency run
can isolate SMT sharing, but does not calibrate the host's normal turbo behavior.
The default gain is an initial estimate; validate calibrated values on a whole
build. A fixed coefficient cannot predict thermal history or changing power
limits, and workload changes can also alter memory bandwidth contention.

For a distributed Ninja build, bound total jobs with `-j`. A local load limit
(`-l`) can stop new remote jobs when the client also executes compiler work,
even while other workers are idle. Benchmark with the same explicit job count
and without that local load limit before tuning memory admission or transport.
Keep worker memory limits and the local preprocessing slot limit enabled.
The total job count also includes preprocessing and transfer waits: a CPU-sized
`-j` limit can leave remote workers idle. Compare a larger bounded job count
while holding preprocessing slots and worker memory limits constant.

With `,tcp` and live pipeline statistics, the broker projects the global server
snapshot instead of counting all local socket leases as CPU work. It adds
outstanding assignments made since the probe started, and subtracts completed
snapshot work using the existing connection-return notification. Completions
first consume the snapshot's returning count, so output-only jobs do not free
CPU capacity twice. No per-stage IPC or extra network round trip is required.
Input preparation can overlap execution within a pipeline of up to four jobs
(also capped by effective parallelism). For compile jobs with an input-size
hint, the broker selects the smallest predicted completion time:
`max(input_ready, cpu_slot_ready) + estimated_work * cpu_service_cost`.
The existing contention curve accounts for SMT, frequency and external CPU
load; memory capacity continues to bound execution.

Successful TCP results carry input preparation, queue and execution durations
in the existing reply and connection-return message. Only execution time is
normalized by the dispatch-time CPU service cost and learned as compile work.
The broker keeps 4096 bounded history entries keyed by working directory,
compiler arguments and input path, excluding output/dependency filenames.
Four-way buckets retain colliding keys across repeated builds. Repeated inputs
use their measured work, adjusted for size changes. Unseen inputs use an online
nonnegative fit of input size and original source size for C, C++ and assembler,
with separate classes for raw and preprocessed source. The source-size feature
is capped at 1 MiB so large generated tables cannot dominate the fit; input
size still represents parsing work. Initial rates and this cap are heuristics;
contents and include trees are not read to calculate a hint. History lasts
for the broker process lifetime. Frequency or contention changes during a job
and different optimization costs remain sources of prediction error.

Predicted remaining work determines when an execution slot will become free.
An overrun does not increase predicted remaining work; the bounded input
pipeline can overlap that uncertain tail while server admission enforces limits.
Unobserved jobs from other brokers use the host's recent mean work. Prepared
network inputs use measured transfer costs. Same-host inputs and raw sources
use measured preparation latency: FD handoffs do not upload the input, and raw
source size does not describe its include tree. Fast samples are included and
sub-chunk inputs do not train bandwidth, preventing a delayed small request
from dominating later estimates. Uploads are deferred while predicted CPU wait
exceeds preparation time by more than 20ms, retaining work at the broker so it
can choose another host. Completion notifications and bounded timed wakes
reconsider waiting jobs. Each pass prioritizes larger estimated jobs, which pay
the greatest execution penalty on slower hosts. Jobs waiting at least one second
take precedence by arrival time. A dispatch pass accounts for earlier waiting jobs
when forecasting later ones, without reserving server resources. Existing
recovery probes may bypass this advisory wait. Inputs without a usable hint,
links and legacy TCP use the aggregate CPU score.
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
input on retries. The broker grants local preprocessing leases up to its
`--localslots_cpp` limit and wakes waiters when a lease closes, without polling
slot files. Clients still check their own memory budget before starting
preprocessing; the broker may belong to a separate, smaller cgroup. A full server
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
lower 90th-percentile rank of the last 64 successful RSS observations plus 64 MiB, with a
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

Broker clients preprocess directly into a memfd and seal it after the compiler
exits. Local transport passes a read-only descriptor without copying that input.
Already-preprocessed named files and `DISTCC_SAVE_TEMPS` builds still get a
private sealed snapshot. The snapshot can be
opened by a compiler running under a different UID without changing the named
input's permissions. This removes input compression, socket payload transfer
and server-side materialization, rather than eliminating every copy. The
existing server worker receives it with the metadata in one IPC message and
inherits it only for that compilation. Input lifetime, cancellation and the
server's common admission limit are unchanged. Local results also pass read-only
descriptors instead of compressed payloads. The client copies objects and
dependencies into its own unpublished files after receiving COMPLETE and
returning the connection to the broker, then publishes by atomic rename. Diagnostics
use the received descriptors directly. Update both endpoints together.
Pump and link inputs still use framed payloads.
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
Up to two background threads handle large compressed chunks, cache lookup and
registration, shared-input snapshots, and detached input-tree cleanup.
Each connection can receive one chunk while decoding its predecessor,
or encode one output chunk while sending its predecessor, with at most two
chunks in flight and one background task per connection. Both buffers count against
the shared storage budget; result completion waits for the last byte.
Tree cleanup remains charged to that budget until completion. Shared-input
owners remain pinned until dependent snapshots finish, including on disconnect.
Worker completion queues wake the parent event loop; only the parent updates
connection state and dispatches compiler jobs, once per loop turn. Fork and
shutdown drain background work before reclaiming inherited resources.
`dcc_tcp_input_prefetch_bytes` counts wire bytes read during decoding, and
`dcc_tcp_output_overlaps` counts encodes started while an output chunk remains
queued. These demonstrate overlap, not time saved.
Compression reads sealed input and completed result extents through bounded
read-only mappings. Incompressible immutable extents use `sendfile`; mutable
named inputs retain a private read buffer, reused directly if compression fails.
Input preparation runs in bounded event-loop turns without depending on socket
writability. Result generation also runs as internal work; write readiness is
requested only for queued packets. Synchronous chunk encoding publishes its
result directly.
Uncached inputs start without a request round trip, and received
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
