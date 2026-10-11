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

For each host with a free slot, the client minimizes
`(busy_slots + 1) * (1 + load1 / cpus) / (slots * speed)`. This favors faster,
larger, less loaded hosts while keeping slot reservations until compilation
finishes. Load is a weighting signal, not a stop threshold: an entirely busy
cluster still makes progress. The slot counts coordinate processes sharing
`DISTCC_DIR`; the daemon's `--jobs` limit remains the cluster-wide safeguard.
Online CPUs and system-wide load do not model container CPU quotas or memory
pressure.

Statistics successes and failures are shared within each wall-clock second in
`DISTCC_DIR/lock`. Each uncached probe has a 250ms deadline including DNS; probes
are sequential across hosts. Unavailable or invalid statistics produce a warning
and use configured slots/speed until the next refresh. The stats endpoint must
be reachable from the client and uses the daemon's existing access rules; it is
plain HTTP, including when compilation uses GSSAPI. `stats` requires a TCP host.
`speed` alone also enables weighted selection and can be used with SSH hosts.
In mixed lists, entries without metrics use their configured slots and speed 1.
Ties follow list order (`--randomize` can change that order). Repeated entries for
the same lock endpoint use the first entry. Lists without either new option
retain the original selection algorithm. Compression and pump options can be
combined with adaptive options.

Build using the usual `./autogen.sh`, `./configure`, and `make` steps.
`make adaptive-check` runs real GCC compiles through two local daemons, checks
the linked executables, and exercises controlled load/error responses and slot
locking. Set `TMPDIR` to a suitable temporary directory. `make check` includes
these tests and the existing regression suites.

## Licence

distcc is distributed under the GNU General Public Licence v2.

## Resources

* Repo, questions, and bugs: https://github.com/distcc/distcc

* [Stack Overflow questions](http://stackoverflow.com/questions/tagged/distcc)
