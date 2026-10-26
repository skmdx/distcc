# distcc -- a free distributed C/C++ and Rust compiler system

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

HIP compilation uses the client's Clang driver and the existing TCP input cache.
`distcc-offload` executes the driver's action graph: each target is preprocessed
with its own frontend options, compilation and device links run on available
workers, and LLVM assembles the resulting images before the host object is
published. This works with both the new and legacy HIP drivers, preserving
target macros, dependency output, compilation-unit identity and optimization
flags. Independent actions overlap; dependent actions start when their inputs
are ready. A failed action cancels its siblings and preserves the previous output.
The shared cache carries preprocessed inputs, device libraries and intermediate
objects. Offload stages learn execution costs separately from ordinary C++ and
host ThinLTO work. This reduces the serial tail of multi-target compilation;
it does not split a single target's compiler frontend.

ThinLTO links distribute individual optimization/code-generation backends:

```
distcc clang++ -flto=thin -fuse-ld=lld main.o library.a -o application
```

This requires `DISTCC_BROKER`, `,tcp` workers, Clang/LLD with the integrated
DTLTO interface, Python xxhash and bubblewrap with unprivileged user namespaces.
LLD performs the thin link, discovers imported modules and archive members, and
invokes `distcc-thinlto` with its JSON job description. Each backend uses the
broker's measured capacity and an independent learned ThinLTO workload class.
Inputs are snapshotted and hashed once per link, shared across backend jobs,
and transferred through the existing LZ4/content-cache path. When the selected
LLVM's partition helpers are installed, its summary indexes provide instruction
counts for primary and imported definitions. Jobs are ranked by the larger of
their share of the link's instructions and primary input bytes, so expensive
code and large constant-data modules both start early. The planner waits only
for each request to enter the broker queue
before submitting the next; transfer and execution overlap. Different manifest
sizes and process startup times therefore do not reverse submission priority.
Execution order does not change LLD's output or constructor order.
Workers preserve
LLVM's embedded import paths in a private namespace. The final native link
runs on the client; no client sysroot snapshot is needed. LLD's ThinLTO cache
remains in use, so warm cache entries do not start backend jobs.
Heavy modules are prepared through LLVM's post-import hook, partitioned with
LLVM's module splitter, and optimized/generated on multiple workers. The number
of partitions comes from the module's instruction share and `distcc -j`, bounded
by its function count. Preparation overlaps ordinary backends; ready partitions
join the same work queue. A relocatable merge restores private symbol bindings
before LLD consumes the result. Constructors, TLS and exception handling retain
their original link semantics. Assembly with private definitions or per-object
weak/versioned references stays together; LLVM's assembler identifies these
cases. Individual functions and the final native layout are not subdivided.

`make lto install-lto` builds and installs the helpers for `LLVM_CONFIG` under
its library directory's `distcc` subdirectory. Use a distinct `LTO_BUILD` when
building for multiple installed LLVM ABIs. A compiler without its matching
helpers still distributes whole-module backends.
Clang 22 and 23 are exercised by the integration tests. Compiler acceptance and
the final link determine compatibility; version metadata is not a rejection gate.
Existing whole-link offload remains available for other links with
`DISTCC_LINK_SYSROOT`. `DISTCC_VERBOSE=1` reports the worker for every completed
backend; `DISTCC_FALLBACK=0` makes distribution failures fail the link.

HIP links use `distcc-linker-wrapper`, built from upstream Clang's linker wrapper
with a device-job distributor hook. LLVM extracts and groups offload inputs,
then each AMDGPU target link runs as a separate cached TCP job. Device links
learn their own execution and memory costs. LLVM bundles the resulting images
and generates registration objects before the host ThinLTO backends run.
Existing optimization options, device ABI, target features and library ordering
are preserved. Saved-temporary-file device links remain local to keep those
artifacts available. Native host layout and writing the final executable remain
on the client; these dependency stages cannot run before their inputs exist.
Build the wrapper by setting `LLVM_WRAPPER_SOURCE` to the matching upstream
`clang/tools/clang-linker-wrapper` source directory, with `LLVM_CONFIG` selecting
the installed LLVM headers and libraries. The Gentoo package provides both.

The Portage hook exports CMake's C/C++/HIP linker launchers as well as compiler
launchers, respecting `RESTRICT=distcc` and explicit user overrides. Existing
CMake build directories must be reconfigured to populate these launcher values;
new Portage builds receive them automatically. Outside Portage, set
`CMAKE_C_LINKER_LAUNCHER=distcc`, `CMAKE_CXX_LINKER_LAUNCHER=distcc` and
`CMAKE_HIP_LINKER_LAUNCHER=distcc` when configuring. HIP linker launchers require
CMake 4.1 or later.

Rust builds use Cargo's compiler wrapper interface:

```
RUSTC_WRAPPER=distcc cargo build --release
distcc rustc --crate-type=rlib library.rs
```

Rust requires the local `DISTCC_BROKER` socket and workers configured with
`,tcp`, plus worker Rust, Python xxhash and bubblewrap. Workers prefer a matching
Rust build but do not reject differing version, commit or LLVM metadata.
Compatibility is determined by rustc accepting the inputs and the client
successfully linking the artifacts. Options and output kinds are passed to rustc;
the wrapper interprets only the operands needed for input/output transfer. Linux workers
must permit unprivileged user namespaces. The client supplies its target
standard library, so workers can compile for another architecture with the
compatible Rust compiler. Rust does not need an include server.

Distribution operates on library crates and explicit metadata/object emissions.
The real client rustc discovers modules, included data, binary dependencies and
observed environment variables. Its dependency-only probe uses
`-Zbinary-dep-depinfo` with `RUSTC_BOOTSTRAP=1`; actual compilation keeps the
caller's feature policy. Only the reported Rust dependency closure is sent,
instead of all crates in dependency search directories. Native archives from
library search directories are also included. Input snapshots and dependency
crates use the existing LZ4 TCP
transport and content cache. Large inputs use bounded cache chunks. The worker
compiles in a private namespace with original client paths and returns all
artifacts, including Cargo metadata and dependency files. Local incremental
state is not sent or updated by remote jobs.
Artifacts are installed before forwarding rustc's JSON notifications, so Cargo
can immediately start dependent crates when metadata becomes visible.
Rust `-Cprofile-use` data is transferred with the other read-only inputs.
Rust also reports snapshot and source sizes to the broker. Memory admission uses
the Rust workload class, while execution-time predictions learn from completed
Rust jobs. Before that first sample, placement uses available capacity without
assuming that precompiled dependencies cost the same as C++ source to compile.

`-C target-cpu=native` uses the client's CPU even on a different worker CPU or
architecture. The installed `distcc-rust-native.so` records the client's actual
LLVM CPU name and enabled/disabled features, cached by compiler, boot and CPU
affinity. It supplies those answers to the worker compiler's native discovery;
rustc still handles target defaults, `cfg` and explicit codegen overrides.
This avoids translating LLVM features into Rust's different option vocabulary.
LLVM development headers (`LLVM_CONFIG`) build the helper without linking an
LLVM version. Install it on clients and workers before using distributed native
jobs. A worker verifies that rustc consumed the profile before returning results.

Final executable/native links, build scripts, procedural macro crates,
explicit dynamic Rust dependencies and response files run locally. The client
also records libraries loaded by the real dependency pass, keeping transitive
procedural macro execution on the client CPU. Unrelated procedural macros in a dependency search directory do
not prevent distribution. Local linking
preserves access to native system libraries. Transport or toolchain failures
retry locally unless `DISTCC_FALLBACK=0`. Failed remote compilations also retry
locally unless `DISTCC_SKIP_LOCAL_RETRY=1`, so an incompatible worker does not
block a toolchain update. `DISTCC_VERBOSE=1` reports the selected worker or
the reason for local execution. The Gentoo jobs hook enables the Cargo wrapper
when `FEATURES=distcc`, respecting package restrictions and existing wrappers.
Gentoo's Rust eclass selects its compiler independently of `eselect` and prefers
source-built Rust. A fleet using `rust-bin` can select that package type with
`ERUST_TYPE_OVERRIDE=binary` in `package.env`, without pinning a Rust version.

The distcc "pump" functionality, added in distcc 3.0, improves on
distcc by distributing not only compilation but also preprocessing to
distcc servers. The client transmits snapshots of application and system headers
through the shared input cache, together with the compiler's default include
search paths. Workers preprocess against these inputs even when their installed
headers differ. For analyzer behavior and toolchain requirements, see README.pump.

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
same queued jobs twice. Lower service cost wins on legacy paths. Equal
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
`alpha = dt / (2 + dt)` in both directions. Symmetric smoothing avoids a
persistent low estimate when valid frequencies fluctuate. Idle time never
advances recovery or changes the reference curve.

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
snapshot. It adds
outstanding assignments made since the probe started, and subtracts completed
snapshot work using the existing connection-return notification. Completions
first consume the snapshot's returning count, so output-only jobs do not free
CPU capacity twice. Outstanding local leases provide a lower bound on projected
work: a request can still be preparing on the client when the worker reports
an idle snapshot. The pipeline bound takes the larger of local leases and global
occupancy, rather than adding preparation twice. No per-stage IPC or extra
network round trip is required.
Input preparation can overlap execution within a pipeline of up to four jobs
(also capped by effective parallelism). Jobs with an input-size hint compare
predicted completion time across both idle and occupied workers:
`max(input_ready, cpu_slot_ready) + estimated_work * cpu_service_cost`.
The existing contention curve accounts for SMT, frequency and external CPU
load; memory capacity continues to bound execution. An idle slow core does not
override a sooner finish on a busy fast host. The estimate also accounts for
throughput displaced by contention on every worker, including remote SMT lanes.
Waiting must still predict an earlier finish than every available placement;
an immediate placement's throughput penalty is not treated as elapsed time.
Without an input hint, selection uses marginal throughput and prefers available
execution capacity. A missed completion forecast uses the running job's service
age as a conservative residual estimate, so an overdue job cannot indefinitely
appear about to finish. Recovery trials use calibrated speed rather than bypassing
the completion-time comparison. Hinted jobs remain in the broker until the
selected worker has an execution lane available. An expired forecast alone does
not authorize uploading another job into that worker's queue. Keeping the job
movable lets a newly available worker take it; preparation and result transfer
still overlap execution, and completion feedback releases capacity immediately.

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
can choose another host. Waiting must finish sooner than every immediately
available placement. Otherwise, available workers retain their throughput
ranking, including the CPU work displaced by another compiler.
Completion notifications and bounded timed wakes
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

For an already configured Gentoo fleet, run `./deploy` without arguments.
The launcher can also be symlinked into `PATH` as `distcc-deploy`.
It reads the private `~/.config/distcc/deploy.json` (or `$XDG_CONFIG_HOME`),
selects the latest committed overlay ebuild, and installs and validates the first
configured target before upgrading the remaining targets in parallel. Each target
gets service settings and distributed C/C++ compilation with local-object linking
and execution checks. Different object bytes do not reject compatible toolchains.
Use names such as `clang` and `clang++` in `compiler_paths` to follow each target's
selected toolchain; they are resolved before sudo changes the environment.
Explicit paths select a specific installation. After rollout, every client compiles a Rust library on
every worker, links it locally and executes the result with fallback disabled.
Rust checks require compatible toolchains and worker addresses reachable from
all clients in `server_hosts`. Source archives are fetched and verified locally,
then synchronized to targets; remote internet access is not required. Source
builds use distcc. Cgroup resource limits are optional explicit configuration;
the deployment command does not impose a memory cap or disable swap.
A configured cross sysroot builds the
binary package for targets without native build capacity. See
`packaging/gentoo/deploy.example.json` for configuration fields; keep actual
host identities and filesystem paths outside the repository.

The command refuses uncommitted release inputs. Already installed revisions
are reused. Configured build services are stopped and resumed after validation;
the resume list survives failure so rerunning the same command can finish.
Failed stages retain their logs and return nonzero. Successful deployment logs
are moved to the configured results directory; intermediate binary packages
are removed. The command does not create commits or publish a new release.

Run `./benchmark` without arguments to measure the installed fleet. It can be
symlinked into `PATH` as `distcc-bench`. The private
`~/.config/distcc/bench.json` selects the build client and workers by their names
in `deploy.json`; see `bench/fleet.example.json`. Configure a known complete
Ninja build and an available `perf` executable. An expected object count and
systemd resource limits are optional; the tool does not impose memory or swap
limits. The example uses the previously
validated media-driver workload. Parallelism comes from the normal Portage
configuration, including distcc's automatic job calculation.
Observed automatic job counts are recorded as scheduler outcomes; changing an
explicit job limit changes the configuration and prevents comparison.

The command pauses configured build services, runs the configured number of clean builds
with a private Portage configuration and ccache disabled, and resumes the
services on success or failure. It records full build times, per-worker
completion/concurrency/queue statistics, broker preparation/queue/execution
feedback, TCP counters, cgroup CPU/memory/I/O, and a client CPU perf profile.
Phase totals sum overlapping jobs and are not elapsed wall time. Worker CPU
utilization includes unrelated host activity. Detailed logs and profiling have
overhead; compare results collected with the same instrumentation. One build
can verify distribution; use repeated builds to assess timing variation.
For long builds, `perf.call_graph` and `perf.frequency` control profiling cost.
To resume a failed build, set `build_root` to its retained `build-N` directory
and `repetitions` to 1. Portage reuses its preserved work tree. Incremental
recovery is labeled separately and is never used to claim a full-build speedup.
Remove `build_root` to return to clean measurements.
Set `keep_build` to true when a successful build tree is needed for controlled
compiler/linker comparisons. Its client-side run directory then requires cleanup.
An optional `verify` argument array runs an installed-product check after the
build; failure retains the inputs and prevents the run from becoming a baseline.

Each run saves `result.json`, `summary.txt` and its raw evidence under the
configured results directory. A successful clean run updates `latest.json`; matching
workload/compiler/configuration/resource conditions permit comparison with the
previous successful invocation. Different conditions, missing endpoint statistics,
worker restarts, incomplete object counts, analysis failures or memory-limit
events prevent a valid comparison. The first invocation establishes a baseline;
it does not establish a speedup. Failures exit nonzero and retain evidence.
Interior sampling gaps are reported as `failed_samples`; cumulative counters
remain usable when endpoints are present and the worker did not restart.
Those gaps do not establish idle time or complete concurrency coverage.
Interrupted resume state is retained for the next invocation. Deployment and
measurement share a lock so they cannot modify the fleet simultaneously.
Perf samples stay in a memory file charged to the build's cgroup. The command
checks that the report contains samples and no recording/decoding errors, then
saves a verified `perf.data.zst` archive. Decompress it before reading it with
perf. An exit code of zero alone does not establish a valid profile.

For Linux/systemd workers, `packaging/systemd/distccd-memory.py` and
`distccd-memory.conf` provide an optional kernel-derived service limit. Install
the helper as `/usr/lib/distcc/distccd-memory.py` and the configuration as a
`distccd.service.d` drop-in; remove a previous fixed `MemoryMax` when enabling
this policy. Before worker startup it sets `MemoryMax` to the smaller of:

* `MemAvailable` plus this service's resident anonymous and shmem pages, capped
  by `MemTotal`. File cache is not added again. The kernel's available-memory
  estimate already accounts for its watermarks and reclaimability.
* Each parent cgroup's limit minus the usage outside this service. Parent cache
  is counted conservatively as used.

The result is rounded down to whole pages. Swap remains available. The helper
prints the measured operands and resulting limit to the service journal;
invoking it with the unit name but without `--apply` prints a dry run. Each
application uses current accounting so temporary pressure during a previous
startup does not permanently reduce capacity after that memory is released.
The admission checks above continue to reduce concurrency when other processes
consume memory. Missing accounting or a proposed limit below current usage
prevents application/startup.
This policy requires a writable system systemd manager and cgroup v2 memory
accounting. It does not remove the uncertainty of an unseen job's memory demand.

On Linux, `DISTCC_MEMORY_LEARNING=1` on the daemon uses compiler peak RSS
reported by `wait4` for prepared jobs. `DISTCC_JOB_MEMORY` is the cold-start
estimate. Prepared jobs are classified by operation and the binary size interval
of their uncompressed inputs. C/C++ classes omit shared code-generation inputs
such as PGO profiles so a common large profile does not hide source complexity;
their storage remains part of actual cgroup usage. Each class reserves its
largest successful RSS
plus 25% and 64 MiB, with a 256 MiB minimum, until daemon restart. Unseen classes
use the larger of the configured estimate and the daemon's observed peak with
headroom. They run concurrently when those reservations fit the measured memory
budget; an unfamiliar size alone does not serialize work. Small completed jobs
cannot lower the estimate for unmeasured jobs. If memory admission changes after
input transfer, the server returns the unstarted job for reselection. The broker
refreshes that worker's capacity before assigning it again, retaining the pooled
connection and prepared inputs without counting a compiler failure or training
execution cost. Other jobs that fit continue to run. The server keeps link
ordering and stops scanning once all CPU execution lanes are occupied.

The daemon sums reservations for active jobs and excludes actual shared-memory
input/output storage from the compiler budget. A quarter-job spare applies when
other jobs are active. Cgroup limits, available headroom, PSI and `--jobs` remain
in force. RSS and input size are estimates: different compiler options or source
complexity within a class can demand more memory. Keep the service memory limit
configured. Statistics expose `dcc_job_memory_bytes`, `dcc_job_memory_samples`,
`dcc_job_peak_bytes`, `dcc_memory_spare_bytes`, `dcc_compile_capacity`,
`dcc_pump_capacity` and `dcc_rust_capacity`. The last three are separate lists of
64 capacities indexed by the binary input-size bucket. Pump and Rust construct
input metadata before selection and reuse it for transfer. Existing jobs retain
their own class reservations; each capacity adds the incoming class's available
slots to the current occupancy, subject to memory and CPU limits. Statistics also
report `dcc_memory_headroom_bytes`, `dcc_memory_reserved_bytes` and per-class byte estimates in
`dcc_compile_memory`, `dcc_pump_memory` and `dcc_rust_memory`. The broker subtracts
the memory cost of leases not yet represented by the worker's reservations before
placing more work, including input preparation across snapshot refreshes. A large
in-flight job can consume several small-job memory slots. Selection and queue bounds
use the incoming job's capacity, not the last completed job's memory estimate.
Leave learning unset to keep a fixed estimate.

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
A live broker's refusal when no workers are available follows this bounded retry
policy immediately; it does not enter the separate broker reconnection wait.

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
the compiler-slot count. RPC buffers share a 512 MiB budget; file-backed inputs
are admitted against available filesystem space. Rust inputs, including large
standard and dependency libraries, share verified whole-file inodes between
jobs. The compiler sees a read-only input tree and private writable outputs;
job links keep inputs alive through cache eviction and cancellation. Large
entries are reclaimed when their last job releases them, while small idle
entries remain in the bounded cache. Compiler working memory is separate.
C/C++ job trees use the daemon's `TMPDIR`, which can be tmpfs. Rust job trees and
their shared cache use `DISTCCD_RUST_TMPDIR` (default `/var/tmp`): large dependency
libraries, LTO intermediates and result archives must not exhaust a small runtime
tmpfs. Use a disk filesystem with enough space for concurrent Rust jobs. The
parent removes these trees after completion or cancellation.
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
