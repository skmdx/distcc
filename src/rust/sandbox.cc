#include "rust/sandbox.hh"
#include "rust/runtime.hh"
#include "rust/api.h"
#include <seccomp.h>
namespace rust {
static Fd namespace_filter() {
    Fd filter(dcc_job_memfd("rust-probe-filter"));
    auto rules = seccomp_init(SCMP_ACT_ALLOW);
    if (!rules)
        throw std::runtime_error("cannot isolate dependency probe");
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
        throw std::runtime_error("cannot prepare dependency probe filter");
    return filter;
}
void Sandbox::mount(const fs::path &from, const fs::path &to, bool writable) {
    args.insert(args.end(), {writable ? "--bind" : "--ro-bind", from, to});
}
Sandbox::Sandbox(Root policy, const fs::path &tree)
    : filter(namespace_filter()), args{"/usr/bin/bwrap", "--unshare-all", "--die-with-parent"} {
    if (env("SANDBOX_ON") == "1") {
        // Keep preloads out of namespace setup. bwrap prepares the payload
        // environment before writing uid_map. Host probes inherit Portage's
        // policy at exec; linkers use the namespace's writable output mounts.
        args.insert(args.begin(), {"/usr/bin/env", "-u", "LD_PRELOAD", "SANDBOX_ON=0"});
        if (policy == Root::Host) {
            args.insert(args.end(), {"--setenv", "SANDBOX_ON", "1"});
            if (const char *preload = getenv("LD_PRELOAD"))
                args.insert(args.end(), {"--setenv", "LD_PRELOAD", preload});
        }
    }
    if (policy == Root::Host) {
        mount("/", "/");
        args.insert(args.end(), {"--unsetenv", "CARGO_MAKEFLAGS", "--unsetenv", "MAKEFLAGS"});
    } else if (policy == Root::HostWithOutputs) {
        // bwrap needs a writable namespace root to create the private mountpoints.
        for (const auto &entry : fs::directory_iterator("/")) {
            auto p = entry.path();
            if (p == "/dev" || p == "/proc" || p == "/tmp" || p == DCC_RUST_ROOT)
                continue;
            if (entry.is_symlink())
                args.insert(args.end(), {"--symlink", fs::read_symlink(p), p});
            else
                mount(p, p);
        }
    } else {
        if (policy == Root::Inputs)
            mount(tree, "/", true);
        if (policy == Root::Backend)
            mount("/usr", "/usr");
        for (auto p : runtime)
            if (fs::exists(p))
                mount(p, p);
    }
    args.insert(args.end(), {"--proc", "/proc", "--dev", "/dev", "--seccomp", std::to_string(filter.fd)});
    if (policy != Root::Host)
        args.insert(args.end(), {"--tmpfs", "/tmp"});
    if (policy == Root::Inputs || policy == Root::Backend || policy == Root::LLVM)
        args.push_back("--clearenv");
}
void Sandbox::command(const Words &argv) {
    args.insert(args.end(), {"--remount-ro", "/", "--"});
    args.insert(args.end(), argv.begin(), argv.end());
}
} // namespace rust
