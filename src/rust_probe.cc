/* Macro probes and native links share the kernel-enforced namespace policy. */
#include "rust_probe.h"
#include <seccomp.h>
namespace rust {
Fd namespace_filter() {
    Fd filter(dcc_job_memfd("rust-probe-filter"));
    auto rules = seccomp_init(SCMP_ACT_ALLOW);
    if (!rules)
        throw Local("cannot isolate dependency probe");
    int rc = 0;
    for (auto name : {"socket", "connect", "sendto", "sendmsg", "sendmmsg", "io_uring_setup", "io_setup",
                      "open_by_handle_at", "ptrace", "pidfd_getfd", "mount", "unshare", "setns"}) {
        int nr = seccomp_syscall_resolve_name(name);
        if (nr != __NR_SCMP_ERROR)
            rc |= seccomp_rule_add(rules, SCMP_ACT_ERRNO(EPERM), nr, 0);
    }
    rc |= seccomp_export_bpf(rules, filter.fd);
    seccomp_release(rules);
    if (rc || lseek(filter.fd, 0, SEEK_SET) < 0 || fcntl(filter.fd, F_SETFD, 0) < 0)
        throw Local("cannot prepare dependency probe filter");
    return filter;
}
void probe(const Words &cmd, const Env &envs, const fs::path &outputs, bool isolate, int diagnostics) {
    if (!isolate) {
        if (run(cmd, envs, diagnostics, diagnostics))
            throw Local("dependency expansion failed");
        return;
    }
    auto filter = namespace_filter();
    Words sandbox = {"/usr/bin/bwrap",
                     "--unshare-all",
                     "--die-with-parent",
                     "--unsetenv",
                     "CARGO_MAKEFLAGS",
                     "--unsetenv",
                     "MAKEFLAGS",
                     "--ro-bind",
                     "/",
                     "/",
                     "--proc",
                     "/proc",
                     "--dev",
                     "/dev",
                     "--bind",
                     outputs,
                     outputs,
                     "--seccomp",
                     std::to_string(filter.fd)};
    for (auto &[k, v] : envs)
        sandbox.insert(sandbox.end(), {"--setenv", k, v});
    sandbox.push_back("--");
    sandbox.insert(sandbox.end(), cmd.begin(), cmd.end());
    std::vector<int> inherited = {filter.fd, std::stoi(envs.at("DISTCC_RUST_LOADS_FD"))};
    if (run(sandbox, {}, diagnostics, diagnostics, &inherited))
        throw Local("isolated dependency expansion failed");
}
bool runtime_path(const fs::path &p, const fs::path &sysroot) {
    return below(p, sysroot) ||
           std::any_of(runtime.begin(), runtime.end(), [&](auto &r) { return below(p, r); });
}
bool audit(const fs::path &depfile, int loads, const fs::path &sysroot, const std::set<fs::path> &search,
           std::set<fs::path> &inputs, std::set<fs::path> &dirs) {
    auto records = split(contents(loads), '\0');
    if (records.size() % 3)
        throw Local("incomplete Rust dependency journal");
    bool macro = false;
    std::set<fs::path> roots = search;
    for (const auto &p : inputs)
        if (!runtime_path(p, sysroot))
            roots.insert(p.parent_path());
    auto allowed = [&](const fs::path &p) {
        return inputs.count(p) ||
               std::any_of(roots.begin(), roots.end(), [&](auto &dir) { return below(p, dir); });
    };
    // Resolve libraries first. Their constructors can record reads before dlopen returns.
    for (size_t i = 0; i < records.size(); i += 3) {
        if (records[i] != "L" && records[i] != "D")
            continue;
        fs::path path = fs::absolute(records[i + 2]).lexically_normal();
        if (runtime_path(path, sysroot))
            continue;
        auto real = fs::canonical(path);
        bool declared = allowed(path) || (records[i] == "D" && macro);
        for (const auto &dir : search) {
            auto spelled = dir / path.filename();
            if (fs::is_regular_file(spelled) && fs::equivalent(spelled, path)) {
                inputs.insert(spelled);
                declared = true;
            }
        }
        if (!declared)
            throw Local("undeclared compiler plugin");
        for (const auto &p : {path, fs::path(records[i + 1]), real})
            if (p.is_absolute() && fs::is_regular_file(p))
                inputs.insert(p);
        roots.insert(real.parent_path());
        macro = true;
    }
    for (size_t i = 0; macro && i < records.size(); i += 3) {
        if (records[i] == "L" || records[i] == "D")
            continue;
        fs::path spelling(records[i + 1]);
        auto p = spelling.lexically_normal();
        if (p == depfile)
            continue;
        auto &kind = records[i];
        if (kind != "R" && kind != "N" && kind != "S")
            throw Local("macro requires local operation: " + records[i + 1]);
        if (runtime_path(p, sysroot))
            continue;
        if (!allowed(p))
            throw Local("macro read outside declared inputs: " + p.string());
        if (kind == "N") {
            if (fs::exists(spelling))
                throw Local("macro lookup changed during discovery");
            continue;
        }
        if (kind == "S" || (spelling != p && !fs::equivalent(spelling, p)))
            throw Local("macro observes a symbolic link: " + spelling.string());
        if (fs::is_directory(p))
            dirs.insert(p);
        else {
            auto real = fs::canonical(p);
            if (!fs::is_regular_file(real) || (!allowed(real) && !inputs.count(p)))
                throw Local("unsupported macro input " + p.string());
            inputs.insert(p);
            inputs.insert(real);
        }
    }
    return macro;
}
} // namespace rust
