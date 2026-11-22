# Project constraints

## Standalone architecture
- This fork is a standalone project; do not preserve legacy upstream `distcc` compatibility shims or retired protocols (such as Pump mode or superseded wire versions). Evolve internal client/broker/worker protocols together and remove obsolete paths in the same change unit.

## Compiler and toolchain integration
- Do not modify GCC, Clang, or `rustc` source, binaries, or system packages, and do not depend on compiler forks.
- Integrate through documented compiler/linker interfaces, existing plugin APIs, or `distcc`-side runtime interposition. Do not add version-pinned flag/option tables, package- or crate-name special cases, or single-stdlib (`libstdc++`-only) ABI assumptions; verify unavoidable symbol or ABI bindings at build or load time.

## Packaging, cross-builds, and rollout
- Install internal-only helpers and private modules under `$(libexecdir)/distcc` (not public `PATH` where compiler masquerade wrappers live), and keep build-host sysroot prefixes out of cross-compiled target artifacts (`DT_RUNPATH` and compiled-in tool paths).
- Before retiring or relocating installed helpers, hooks, or config options, audit external/host callers and update them so rollout and package removal stay safe.
- Qualify bug fixes and cluster deployments on clean builds under normal execution limits and configuration; never relax process limits, enable `keepwork`, or add per-package exemptions to work around a defect in `distcc`.
