# Build-time model

Optimize the complete build, including preparation. A successful compile or a
smaller sum of queue times does not establish a shorter build. Keep the build
system, package generators, compiler options and configured job count unchanged
when evaluating a scheduling change.

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

Prepared jobs must still compare completion times, including contention. A
hard limit inferred from background runnable counts can leave them waiting
indefinitely even when an available worker would finish sooner. Preserve input
progress and distinguish actual memory admission from an uncertain CPU forecast.
Transient demand must not silently change the configured whole-build job count.

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

Do not derive service demand by dividing elapsed time by the same contention
forecast that the observation will train. That feeds prediction errors back
into the work estimate. CPU accounting and elapsed timing are independent
observations; retain both. For an isolated worker, before/after daemon cgroup
counters can establish service demand without a new protocol. Verify that the
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

## Evidence required for adoption

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
