# Saved compiler replay

Run from the checkout, using a fixture and a **new empty** execution directory:

```sh
python3 bench/replay/run.py --fixture /path/to/fixture.json --workdir /path/to/empty-run
```

The entrypoint stages this directory's canonical code, validates the running
worker/listener and compiler-wrapper PATH, smokes both observers, then runs an
unobserved control and an observed replay. Both restore the same supplied input
caches and use `distcc -j`. It verifies every output against its saved reference,
worker completions, complete two-end RPC streams, task accounting and errors.
Original caches are restored on exit. No fleet deployment or full build is run.

## Inputs and ownership

Keep host-specific configuration, compiler commands, reference build trees and
cache seeds with the environment's measurement evidence, outside public code.
`fixture.json` supplies:

- `commands`: a JSON list of `{argv, cwd, output}` invocations.
- `client_cache`, `worker_cache`: tar.zst archives containing each cache's
  basename, relative to the fixture. Only distcc input cache state is restored;
  Linux page-cache residency is not controlled.
- `remote_completions`: the fixture's expected remote count (intentional local
  commands remain allowed and still require matching outputs).
- `client`: SSH target, remote `scratch_root`, cache path, build `environment`
  and `frontend` shell sources, the fixture's `hosts`, expected `resolved`
  command paths and `binaries` path/SHA256 mapping.
- `worker`: optional SSH target (`null` means this host), remote `scratch_root`
  when needed, address, service, cache path and installed `binaries` mapping.
- Both hosts' `capture`: interface, peer IP, compile port, stats port and the
  installed `packet_tool` (`dumpcap` or `tcpdump`).

SSH must support noninteractive sudo and rsync. The fixture must exclusively
own idle worker/input caches during the run; the entrypoint checks worker
activity before replacing caches. It does not stop the worker daemon or change
compiler, optimization, user parallelism, service scheduling or memory policy.
The original build environment and reference outputs must still exist.
The worker needs Linux TASKSTATS/delay accounting, libnl-genl development files,
a C compiler and perf's software CPU/context-switch events. The observer enables
`kernel.task_delayacct` only for capture and restores its previous value on exit.

`probe.py`/`probe.sh` own replay and pump/PATH setup. `capture.py` owns recorder
attributes, process lifetime and packet output permissions. `analyze.py` owns
task aggregates, sampled callers and TCP frame/clock matching. `taskstats.c` is
a thin libnl/UAPI adapter; it exports one record per exiting task rather than
scheduler transitions. No client-side perf recorder is started.
`run.py` owns staging, the two-run lifecycle, cache restoration and acceptance.
Temporary directories contain execution copies and results, never editable
sources. Existing evidence snapshots are not modified or executable prerequisites.

## Input interposition canary

For the real Files/Connection input boundary, use:

```sh
python3 bench/replay/interpose.py --owner --cc /path/to/target-gcc --cxx /path/to/target-g++ --workdir /path/to/empty-run
```

This builds `h_input_owner` through the checkout's configured Makefile. One
owner runs the actual `Files::descriptor/open` and `Connection::read`; its client
peer runs `dcc_frontend_fs_reply`, including export roots and native permissions.
A private sequential socket passes a path and a real FD/error with SCM_RIGHTS.
Only the selected compiler image uses it. Other child images, outputs, loader,
directory operations and path metadata retain native access. This is a
connection canary, not a production route or a new general RPC service.

The existing Files inode owns a sealed payload FD and its mapped Buffer; aliases
reuse that inode only after their own authorized read. Reopening its proc FD
gives independent offsets, while exported FDs survive owner exit. Tests require
actual compiler reads of the memfd, C/C++ object equality, exact error/diagnostic
equality, export denial, hardlink identity, seals and compiler hash invariance.

**Unresolved contract:** native `stat(path)` describes the original inode,
whereas native `fstat(exported_fd)` describes a different payload inode. The
canary records this mismatch; it does not emulate fstat or add an FD table to
hide it. Production adoption requires the path and descriptor to describe the
same OS-owned object. Symlink/fortified realpath, directory PCH, ACL and active
cancellation contracts are not accepted by this test. FUSE remains the
production job's sole filesystem owner; no production routing is changed.

For the earlier, independent directory-fixture boundary check, omit `--owner`:

```sh
python3 bench/replay/interpose.py --cc /path/to/target-gcc --cxx /path/to/target-g++ --workdir /path/to/empty-run
```

This compiles a small preload library with the current host compiler/flags and
uses the existing target compilers unchanged. It saves actual ELF relocations,
direct internal calls and dependent libraries. Each native control is compared
with real snapshot-file substitution after poisoning the original inputs; a
native negative control must fail. Cases cover C/C++, aliases, PCH files and
directories, embed, diagnostic rereads, missing and denied inputs. Access logs
and output equality are required. No scheduler capture or performance claim.

`interpose.c` returns real native descriptors/streams, with no read/fstat/mmap
emulation and no second `Files` owner. The private tree is a fixture, not export
authorization. The selected fortified realpath boundary, external/retargeted
symlinks, ACLs, cancellation and concurrent job ownership remain outside this
canary's acceptance. Production integration must obtain authorized snapshots
from the existing owner; this library is not installed or used by distccd.

## Validity and overhead

TASKSTATS supplies CPU runtime, runnable delay and context-switch counts for
exiting descendants of the selected worker service. Persistent tasks are not
included. Software `cpu-clock` and `context-switches` events sample callers at
49 Hz; continuous scheduler tracepoints are not recorded. Smoke exercises two
contending disposable tasks and checks positive CPU/runnable counters, real
caller samples, sample counts below OS switch counts, and `perf evlist` attributes.

The non-CPU residual is lifetime minus scheduled runtime minus runnable delay.
It includes sleeping/stopped time and accounting error, **not** an exact FUSE or
RPC duration. Samples identify sites but neither provide blocked durations nor
identify every service PID. Interior totals include only entire task lifetimes
within the work-pending interval, using conservative bounds on the kernel's
second-resolution birth timestamp. Boundary tasks are not interpolated. An
empty interior is unavailable, not zero waiting.

Packet files are opened before the capture tool drops privileges. Smoke must
capture real traffic, not merely print a start message. Missing events, packet
gaps/drops, incomplete replies, wrong endpoints, unmatched outputs or warnings
prevent a valid result. Paired packet timestamps bound the hosts' clock offset;
unrelated monotonic clocks are never directly subtracted.

`summary.json` distinguishes capture completeness from the observed/control
wall-time difference. A complete capture is **not** proof of negligible overhead.
Inspect that difference before using timing or runnable shares to diagnose the
uninstrumented workload. One pair is not a statistical estimate or a full-build
speed claim. `observer-cost.json` saves recorder child CPU, block IO and output
bytes; it excludes kernel-hook work charged to measured tasks. Raw perf/pcap,
task aggregates, clocks, identities, seed hashes and logs remain in the
run directory; retain it with the environment's evidence before removing scratch.

Run the focused parser/validity regressions with:

```sh
TMPDIR=/path/to/test-scratch python3 -m unittest discover -s bench/replay -p 'test_*.py'
```
