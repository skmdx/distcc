# Build-time model

Optimize the complete build, including preparation. A successful compile or a
smaller sum of queue times does not establish a shorter build. Keep the build
system, package generators, compiler options and configured job count unchanged
when evaluating a scheduling change.

## Observe compiler phases at their actual boundaries

With Clang codegen enabled locally or on a worker, `DISTCC_VERBOSE` emits one
JSON record prefixed by `distcc compiler phases:` for each completed source-to-IR action. It records
monotonic wall time and process CPU time, including reaped codegen children:

- `frontend`: action startup, preprocessing, parsing and incremental IR generation;
- `finalize_optimize`: Clang's `HandleTranslationUnit`, including deferred IR
  finalization and LLVM optimization;
- `ir_cleanup`: the rest of the IR action and module accounting;
- `codegen`: machine-code generation, partitioning, merging and module cleanup.

The trace excludes driver preparation and final `CompilerInstance` destruction.
IR input and failed frontend actions lack these AST boundaries and do not emit a
complete phase record. Unfinalized instruction counts are not final module size.
Concurrent wall intervals must not be added as elapsed build time. Preserve the
compiler's callbacks through its `MultiplexConsumer`; do not infer phase starts
from the first identifiable sampling-profiler symbol. Monotonic timestamps and
PIDs belong to the emitting host and PID namespace; join them to the matching
host's observations before comparing timelines.

A fixed learned cost can represent header/setup work missing from a raw-byte
model, but cannot identify arbitrary template or optimizer complexity. Lower
aggregate prediction error is insufficient for adopting a placement change:
the model can still misplace long jobs on completion-critical branches. Check
their actual finish times and the complete build before changing the policy.

## Work and dependency bounds

Model a build as a dependency graph of commands and compiler phases. For a
compiler invocation, distinguish driver planning, frontend processing, LLVM IR
optimization, machine-code generation, input access and result delivery. Nested
compiler trace events are not independent work: `Backend` includes both
`Optimizer` and `CodeGenPasses`.

Let `P` be preparation time, `D` the build-time target and `B = D - P` the
remaining budget. Holding preparation fixed is a scope assumption for an
allocation change, not a claim that preparation can never be shortened.

For a fixed assignment, let `W[h]` be CPU seconds charged to host `h`, and `C[h]`
its available logical CPUs. Under unchanged service demand and execution
conditions, the optimistic accounting bound is:

```
T >= P + max(critical_path, max_h(W[h] / C[h]))
```

The client build cgroup and its daemon cgroup must be disjoint before adding
them. Package work belongs to the client total but is not automatically movable
by distcc. CPU seconds from different hosts are not interchangeable. SMT, CPU
frequency and memory contention can change a job's CPU demand; the bound is
conditional on holding that demand fixed, not a hardware-independent limit.

A proposed placement must satisfy destination capacity as well as relieve the
source. For each job `j`, distinguish CPU service demand `w[j,h,m]` from elapsed
execution time `t[j,h,m]` on host `h` using execution method `m`. Whole compilation
and parallel machine-code generation are different methods: splitting can
shorten elapsed time while increasing total CPU work. A scalar host-speed ratio
does not describe this difference. A placement requires
`sum_j w[j,h,m] <= B*C[h]` on every host, plus its dependency and transfer
constraints. Input/output phases may overlap execution; sum only intervals on
the same dependency path when predicting wall time.

Use a build tool's emitted dependency graph for offline evaluation. Do not
change the tool's job ordering or add package names to the runtime scheduler.
Recorded command submission times change when predecessors finish earlier;
replaying them as fixed arrivals cannot prove a new full-build time. The broker
only knows submitted jobs and cannot promise an optimal schedule for unseen
work.

For a proposed tail-only improvement, first hold all other completion times
fixed. If the selected dependency chain ends at `E` and the last unaffected
output ends at `U`, eliminating that chain's remaining work saves at most
`max(0, E-U)`. Add these bounds only for sequential build phases. If their sum
is smaller than the required saving, the proposal also needs a demonstrated
reduction in other service demand or dependency timing. A long compiler
invocation is not necessarily on the completion barrier.

## Design the intervention before changing placement

First derive the required saving `delta = P + T - D` from a completed baseline.
Reduce total overhead cumulatively: group related ownership, copying, process
startup and intermediate-storage changes into one implementation unit. A small
reduction need not meet `delta` by itself to be useful. Preserve changes whose
structural simplification independently justifies them, without describing that
decision as evidence of a stable speedup.
For fixed service demand and assignment, the utilization required of host `h`
is `W[h] / (C[h] * (D-P))`. This distinguishes a capacity deficit from lost
overlap; an accounting bound below the target does not prove that a feasible
dependency-respecting schedule exists. Do not add CPU seconds across unlike
hosts to claim interchangeable capacity.

Use the complete set of outputs at the completion barrier. Accelerating one
translation unit may merely expose the next slowest branch. For each proposed
intervention, name the affected phases, the bytes or CPU work removed, and the
dependency edges whose completion changes. Validate individual reductions on
their real paths, then compare the combined implementation in complete builds.
Do not add overlapping phase savings as if they were independent elapsed time.
When assumptions or parameter values are missing, obtain those specific
measurements before extending the implementation.

## Requirements for placement forecasts

Separate the scheduling objective from its inputs. Minimizing the drain time
of currently known jobs can pack a fast host while leaving independent cores
unused, because it excludes future arrivals. Minimizing the sum of completion
times prices interference with existing jobs, but is not equivalent to
minimizing the whole build's final completion time either. Neither objective
alone establishes the requested build-time bound.

With known residual work and equal service rates at a given occupancy, sort
the residuals and integrate successive completion events. If `n` jobs remain,
the next work increment is `dw`, and the measured per-job service cost is
`f(n)`, that interval lasts `dw*f(n)` and contributes `n*dw*f(n)` to the sum of
remaining completion times. Comparing schedules with and without a candidate
then includes the finite delay it adds to other jobs. Do not charge the initial
contention for the candidate's entire lifetime.

If a forecast also assumes `b` persistent background tasks, price their lost
progress over that finite schedule. For a work increment `dw`, `n` known jobs
take `dt = dw*f(n+b)`. The known completion sum grows by `n*dt`. Background
progress is `b*dw`, which would take `b*dw*f(b)` seconds of aggregate service
without the known jobs. The combined area is therefore
`((n+b)*f(n+b) - b*f(b))*dw`. Subtract the baseline area from the candidate
area. An inverse marginal-throughput price describes indefinitely replaced
work; it is not this finite added delay. This derivation assumes equal service
rates, persistent background demand and known residuals. It does not prove
those assumptions for a sampled runnable count.

This calculation requires actual residual estimates. An aggregate count of
unknown jobs does not establish their completion order; assigning the same mean
residual to all of them must not be interpreted as simultaneous completion.
External runnable counts likewise do not establish a finite interference
duration. Record which decisions satisfy the model's premises before attributing
whole-build results to that model.

A job that has exceeded its work estimate has an unknown residual. Its observed
age is not a measurement of remaining work. Using age to avoid an indefinitely
stale queue-release prediction does not justify treating the same value as a
known completion event in an interference calculation.

Check work-estimate error before making placement more elaborate. Raw source
bytes, preprocessed tree bytes, LLVM instruction counts and compiler CPU time
describe different work. In particular, two source-size features are collinear
when both measure the same raw file, and neither predicts arbitrary optimizer
complexity. Compare predicted and observed execution in the same units and
execution method. A source-content timing prior is evidence from completed
work; a cold size-based estimate is not. Do not hide a large prediction error
by adjusting host speed or contention coefficients.

The existing `observe` implementation still trains a normalized work value by
dividing measured execution time by the factor predicted at dispatch. This is
not an independently measured service demand. A replacement must separate:

- CPU work consumed by the compiler and its descendants, measured by `wait4`;
- elapsed execution, preparation and queue time, already returned separately;
- the worker and execution method, particularly serial execution versus local
  partitioning;
- the assumptions used to predict future concurrency, frequency and transfer.

Keep measurements in their native units at the observation boundary. Do not
infer an idle service time from a predicted contention multiplier and then use
that inferred value to validate the same multiplier. CPU work alone also does
not determine elapsed time: parallel codegen can consume several CPU seconds
in one wall second. Method and capacity assumptions belong in the prediction,
not in the stored observation.

The prediction boundary is therefore
`finish = max(cpu_ready, input_ready) + w/r + delivery`, where `w` is predicted
CPU seconds for this worker and execution method and `r` is effective CPU
seconds per wall second over execution, including any demand-paged input stalls.
Input preparation and the CPU queue overlap; do not add their full durations.
Work and effective rate are separate predictions. A serial backend cannot receive the same parallel share
as a split backend. Training pairs of CPU and elapsed time can check `r` under
observed conditions; they do not identify its value under unobserved occupancy.
Keep such missing counterfactuals explicit instead of treating an inverse
dispatch factor as a measurement.

Evaluate forecasts in request order, using only jobs completed before that
request. Separate exact-command observations, source-family priors across
different commands, and cold size estimates. For the existing execution
forecast, report `sum(abs(predicted-actual))/sum(actual)` as a diagnostic, with
its sample count and phase boundaries. This is an aggregate error measure, not
a probability or a bound on the next job. In particular, a later ABI's result
cannot be used to predict an earlier ABI's request.

The next placement design must treat uncertainty as information about the
forecast, never as a reason to reject a compatible compile. Queueing behind a
busy worker requires evidence that it beats the available alternative after
prediction error and added interference are accounted for. Without that
evidence, dispatch available work using measured capacity; do not fabricate a
precise release time from source bytes. Before adopting a more complex policy,
show which decisions its independently checked forecasts can actually resolve.
An offline replay of recorded arrivals can test those decisions, but cannot
establish the resulting build completion time because arrivals can change.

## Distinguish service demand from runnable counts

The broker distinguishes daemon compilers, direct local compilers and input
producers. Account for each owner once. A system runnable count includes these
owners and short-lived activity; it does not measure external CPU seconds or
establish how long that activity will persist.

For an interval of length `B`, the capacity constraint is on CPU service:
compiler CPU seconds plus input CPU seconds plus external CPU seconds must fit
within `B*C`. Subtracting instantaneous external runnable tasks from the CPU
count is not equivalent to this condition. It may leave capacity unused after
short-lived tasks finish or move a long job to a slower host that extends the
completion barrier.

A mean-reverting model can estimate the average background over a candidate's
service horizon from preceding observations. Validate it in time order against
the time-weighted future count, without fitting to future samples. Lower error
in runnable-count prediction is only an intermediate result: it neither measures
CPU service nor validates residual-work estimates or the placement objective.
Require a repeated complete-build comparison before retaining the extra model
state solely for speed. Moving one formerly slow job can expose another branch
or increase service demand elsewhere.

Prepared jobs must still compare completion times, including contention. A
hard limit inferred from background runnable counts can leave them waiting
indefinitely even when an available worker would finish sooner. Preserve input
progress and distinguish actual memory admission from an uncertain CPU forecast.
Transient demand must not silently change the configured whole-build job count.

When a queued candidate competes with immediate execution, compare it with the
ready worker that the placement policy would actually choose. The fastest
individual ready completion and the smallest throughput displacement can refer
to different workers. Using the former to reject waiting and then assigning the
latter can select a strictly slower completion with more displaced work.

Use the same contention objective before and after input preparation. A local
preflight still places a whole compiler invocation: bypassing the displacement
cost there fills SMT siblings before independent remote cores, even when the
prepared-job scheduler would make the opposite choice. Extra codegen leases
have a different feasible set. Their empty work hint represents a local CPU
borrow, so an unavailable remote execution method must not prevent borrowing a
free local lane. They need no input-producer reservation.

Lower client CPU demand and more remote completions are useful intermediate
observations for an offload proposal, not acceptance criteria. Evaluate their
effect on destination service demand and the dependency path together.

For an isolated worker, before/after daemon cgroup counters can establish
service demand without a new protocol. Verify that the
daemon did not restart and that exactly the intended job completed. Keep client
input-service CPU separate, preserve I/O counters, and include helper startup
in the stated measurement scope. Under concurrent execution, attribution needs
per-job child usage rather than a shared cgroup delta. The completion feedback
therefore carries `cpu_ns` from `wait4` alongside preparation, queue and execution
durations. Reused compiler helpers report the individual child, including its
reaped descendants, rather than their lifetime usage. Zero means no measured
compiler service; a job rejected before starting a compiler must not invent CPU
work. Verbose broker job observations expose this as `cpu` in seconds. The CPU
measurement does not change the existing placement policy by itself.

An idle-host measurement is a service-demand observation under those conditions,
not a calibration of SMT or a promise of loaded completion time. Before changing
placement, account for the remaining work of already-owned jobs and which
execution method will actually be available. Model a phase's release of CPU
leases when it finishes; do not assume that the initial occupancy persists for
the entire candidate job.

Measure a capacity curve with matched compiler inputs at one job, one job per
physical core and one per logical CPU. Record busy-weighted CPU frequency during
those same batches when the hardware exposes it. Otherwise a frequency change
between batches can appear to be a different SMT gain. The three measured
frequencies are the references for the existing live clock correction; a
missing frequency source leaves that correction unavailable, not zero. A short
calibration does not establish sustained throughput: verify the curve against
clock and CPU observations during the complete build, including long jobs.

Clock sampling belongs to host observation, not daemon occupancy. Statistics
requests and active-worker polling share one sampling deadline, so direct
client compilation is observable and unqueried idle daemons need no timer.
Capacity getters remain passive. Keep the previous CPU-counter snapshot
separate from the daemon-busy state used for recovery admission. The lowest
calibration point represents one continuously busy CPU; intervals with less
aggregate activity must not train the speed correction from an idle
power-saving clock. They retain the previous valid observation and its age.

## Evaluate partitioning against the remaining path

For one invocation with frontend time `F`, whole-module optimization time `O`,
machine-code generation time `G`, and other serial time `R`, ideal `k`-way
machine-code generation cannot beat:

```
T(k) >= F + O + R + G/k
```

Actual partitioning also pays split/merge work, imbalance and contention.
Compiler CPU seconds can increase even when that one invocation finishes
earlier. A faster invocation only shortens the build to the extent that it
advances the completion barrier. Check both that barrier and the host-capacity
budget before adding partitioning or speculative duplicate execution.

The common codegen planner estimates serial codegen work `S` from completed
instruction-count/CPU-time samples and per-part split/merge time `H` from
completed splits. Its model is `T(1)=S`, and `T(k)=S/k+H*k` for `k>=2`.
Choose the minimum, not the largest `k` that beats serial execution. For example,
`S=1s` and `H=.05s` predict `.45s` with four parts but `.8625s` with sixteen:
both beat serial execution, yet the larger split almost doubles elapsed time.
The model is convex beyond two parts, so stop when another part ceases to help;
fewer available leases do not require a second profitability rule. Before the
first overhead observation, use the smallest split, two parts, to obtain one.
This model assumes independent equal-rate lanes. SMT, changing contention and
unequal backend work remain reasons to verify complete-build results rather
than claiming the predicted minimum is the actual optimum.

The broker or worker daemon therefore lends extra codegen CPUs only while
independent physical cores remain available after existing compiler and input demand. An unused SMT
slot is still available to ordinary compilation, but does not justify `S/k`
for optional partitioning. Declining an extra lease leaves the caller's existing
compile running. Check this at the codegen phase: testing spare capacity before
the frontend and optimizer does not establish availability when codegen begins.
Availability later in the build does not establish an immediate grant either.
Match compiler lifetimes and phase samples to the same capacity timeline. With
constant per-lane service and perfectly divisible remaining work `S`, even an
ideal dynamic executor needs `integral_0^T k(t) dt >= S`, where `k(t)` counts
lanes it can actually use at time `t`. A fixed partition plan cannot use a later
free core once only one indivisible partition remains. Dynamic borrowing must
therefore pay for partition preparation, preserve unfinished parallel work and
include interference with ordinary jobs; it is not a free extension of `S/k`.

Timestamp alignment and symbol coverage limit this analysis. A first sampled
backend function is evidence that codegen has begun, not its exact start. Match
the sample lifetime and sampled CPU total against the job's completion and
`wait4` counters before assigning it to a job. Runnable compiler counts do not
include all external work and are not measured CPU service. Missing symbols or
call chains must not be assigned to a convenient phase.

For local code generation, the splitter hands verified IR directly to a child
through `fork`. Partition construction overlaps earlier children, and the last
partition uses the caller's existing CPU lease. This removes bitcode encoding,
file input/output and decoding from that path without changing optimization or
partition ownership. Intermediate objects remain in anonymous tmpfs files,
which the resident LLD reads directly; only the symbol-localization command
requires an external process. Retain the borrowed leases through the internal merge and
give LLD that same thread budget; returning the leases before a merge that uses
all hardware threads would leave its CPU demand unaccounted. The network backend still serializes partitions because
its consumer is outside the process tree. Count these avoided operations as
reduced work, not as an equal reduction in build wall time: parallel phases and
the build's completion barrier determine the observable saving.

## Account for reusable frontend work

Frontend library samples include parsing, semantic analysis and IR generation;
they do not directly measure reusable header work. Derive compatible shared
prefixes with Clang, preserving command options, source identity, macro context
and dependencies. Count exact full-prefix reuse separately from partial-prefix
or normalized-prefix opportunities, which require additional correctness work.

For a proposed cache, subtract construction, validation and deserialization CPU
from the parsing CPU actually avoided over its lifetime. Count every cache
construction on every worker. State created only in a per-job child cannot serve
the next child. A successful warm hit does not establish positive amortized
savings, and a positive CPU balance does not establish a shorter dependency
path. Reuse a prior coverage or cost measurement when its inputs and execution
path still apply; differences between analyzer-only and compiler AST caches must
remain explicit. Do not repeat a whole-build cache trial before establishing
both compatible reuse and a sufficient net-work reduction.

## Evidence required for adoption

Frontend command preparation and local compilation share the caller's resident
Clang libraries. Each request still runs in an isolated child with its own
working directory and descriptors; the compiler's driver owns argument
interpretation and input roots. Preparation and auxiliary-input discovery share
the driver's plan. Inspect its parsed invocation for input metadata, but forward
the driver's cc1 command directly. Tooling's invocation wrapper clears
`DisableFree` for long-lived callers; serializing that modified invocation loses
the actual driver's process-exit reclamation policy. Avoid this wrapper and
argument regeneration in disposable compiler preparation, while preserving
Clang's own job selection, option parsing and diagnostics.
Preparation reuses its consumed command memfd
for the response. This avoids loading LLVM into every remote-preparation client
without introducing another daemon or serializing compiler invocations.
Resident parents dispatch descriptors; their isolated native children decode and
own command data through the same codec as the C client. Lend strings in the
received allocation directly to the driver or frontend, without
per-argument allocations or Python string conversions. Preparation writes the
captured command directly into the consumed request file. This keeps request
preparation off the shared acceptance loop and contains malformed commands within
the failed invocation. Resolve static service tables before forking.
Measure that reduction separately from placement: fewer loader CPU samples do
not establish an earlier build completion when different long jobs own the tail.

Initialize remote compiler targets once in the resident parent. Disposable
compiler children honor the driver's `-disable-free` contract: finalize owned
diagnostic consumers, then let process exit reclaim compiler, module and LLVM
context storage. Flush LLVM output streams before that exit, including native
actions such as version queries. The long-lived analyzer retains normal destruction.
Remote header buffers share immutable received or cached storage; LLVM buffer
views own that storage even after their VFS file closes. Validate ownership on
both network and cache-hit paths, including empty headers and chunk boundaries.
Keep status, version, contents and directory-query state in one record per path;
ancestor lookups can use string views without constructing every path prefix.
Store attributes only in the wire version record; construct LLVM's named status
only when requested, rather than for every sibling in a directory snapshot.
Directory iterators own the reply and decode entries as they advance, avoiding
an eager vector of copied paths. Both buffers and iterators may outlive the VFS;
short entries must still report errors through the iterator contract.
Changed metadata invalidates that record's buffer, while already opened views
retain their original immutable storage. Receive directly into uninitialized
payload storage and publish it only after the complete reply arrives.
The versioned disk cache retains validated root and shard directory handles for
each compiler invocation. Read a whole record into one allocation, validate its
key and digest, and lend the payload view to LLVM with that allocation as owner.
Batch header, key and payload writes before atomic publication. Eviction scans
use independent directory offsets even when a compiler forks with cached handles;
closing the cache cannot invalidate already returned buffers.

Local and remote resident helpers share native descriptor dispatch and child
ownership. Python selects and loads the configured frontend once; invocation
children never enter the interpreter. Each request owns its command file, reply
channel and standard streams; the remote frontend uses its input stream for
filesystem RPC. Install these descriptors, close unrelated inherited descriptors
in one operation, then execute in a separate process group. Do not run Python's
fork callbacks or rebuild Python descriptor/argument objects per invocation.
The same reply reports exit status, peak RSS and descendant CPU time on both
paths. Read signal notifications through the event loop and drain completed
children with `wait4(-1, WNOHANG)` after each event batch. Work scales with received
events and completions instead of their product with the live child count.
The daemon owns one lazy helper per selected compiler, shared by all execution
slots. Spawn it without forking daemon library state; requests can queue during
startup, so loading libraries never blocks the daemon event loop. Compiler
children join the requesting execution worker's process group. Client closure
cancels only that group; helper shutdown reaps all its invocations before the
daemon removes their trees. Helper exits do not consume execution slots, and
replacement workers close inherited helper channels without terminating them.
Local invocations retain independent groups. Validate
out-of-order exits, malformed commands, active shutdown and per-job accounting
through real compiler requests, then measure fork/dispatch overhead separately
from compilation and complete-build time.

Compiler request and preparation-result memfds are complete before their
descriptor is handed off. Read each complete remaining extent once, then parse
its argument groups in memory using the stream protocol's token decoder. This
removes per-argument read syscalls in both resident helpers and the preparation
caller, without adding buffered socket state or changing the command format.

Filesystem responses retain metadata in memory and an optional open source FD.
Receive buffers are reused within a connection and charged by allocated
capacity. Reader and asynchronous decoder exchange exclusive buffer ownership;
oversized manifest storage is released after consumption. Filesystem replies
borrow the receive or decompression buffer until IPC transmission completes,
while the existing receive backpressure protects that storage from reuse.
The sender joins the prefix and file extent in the existing compressed chunks;
it does not stage each response in another file or add a frame boundary.
Mutable file bytes are read into a private chunk before sending, and premature
EOF fails the job. Directory listings reuse reported entry types when available;
full metadata queries still obtain current file versions. Compare generation,
payload reading and encoding together so deferred work is not counted as saved.

Derive the expected saving and its limiting assumptions before changing the
implementation. Use existing traces to rule out proposals whose optimistic
saving is insufficient. If destination service demand or dependency timing is
missing, identify the missing quantity instead of inventing it from job counts
or dividing wall time by an unvalidated contention estimate.

First exercise the changed ownership/admission contract and compiler correctness.
Then compare full builds under matching settings and instrumentation, recording
all outcomes, CPU demand, I/O, distribution and errors. Treat one favorable run
as a measurement, not proof of a stable improvement. Preserve a low-impact
change only when its maintenance or structural benefit independently justifies
it. Completion still requires the requested full-build time through the actual
distributed path.
