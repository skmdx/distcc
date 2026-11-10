# Saved compiler replay

Run from the checkout, using a fixture and a **new empty** execution directory:

```sh
python3 bench/replay/run.py --fixture /path/to/fixture.json --workdir /path/to/empty-run
```

The entrypoint stages this directory's canonical code, validates the running
worker/listener and compiler-wrapper PATH, smokes both observers, then runs an
unobserved control and an observed replay. Both restore the same supplied input
caches and use `distcc -j`. It verifies every output against its saved reference,
worker completions, complete two-end RPC streams, scheduler coverage and errors.
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

`probe.py`/`probe.sh` own replay and pump/PATH setup. `capture.py` owns recorder
attributes, process lifetime and packet output permissions. `analyze.py` owns
continuous-state reconstruction, sampled callers and TCP frame/clock matching.
`run.py` owns staging, the two-run lifecycle, cache restoration and acceptance.
Temporary directories contain execution copies and results, never editable
sources. Existing evidence snapshots are not modified or executable prerequisites.

## Validity and overhead

Scheduler states use period=1 **without callchains**. CPU callers and a separate
frequency-sampled switch stream supply callsites. Smoke checks actual sample
counts as well as `perf evlist`: some kernels record every tracepoint despite a
larger period. Sampled sites identify callers; they do not supply exact blocked
durations or prove that every FUSE-service PID has been identified.

Packet files are opened before the capture tool drops privileges. Smoke must
capture real traffic, not merely print a start message. Missing events, packet
gaps/drops, incomplete replies, wrong endpoints, unmatched outputs or warnings
prevent a valid result. Paired packet timestamps bound the hosts' clock offset;
unrelated monotonic clocks are never directly subtracted.

`summary.json` distinguishes capture completeness from the observed/control
wall-time difference. A complete capture is **not** proof of negligible overhead.
Inspect that difference before using timing or runnable shares to diagnose the
uninstrumented workload. One pair is not a statistical estimate or a full-build
speed claim. Raw perf/pcap, clocks, identities, seed hashes and logs remain in the
run directory; retain it with the environment's evidence before removing scratch.

Run the focused parser/validity regressions with:

```sh
TMPDIR=/path/to/test-scratch python3 -m unittest discover -s bench/replay -p 'test_*.py'
```
