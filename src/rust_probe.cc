/* Audit only the compiler started for this dependency pass. The loader records
 * the start of dlopen before constructors run. Kernel syscall observations
 * across its threads cover native code that bypasses libc interposition. */
#include "rust_probe.h"
#include <fstream>
#include <seccomp.h>
namespace rust {
void probe(const Words &cmd, const Env &envs, const fs::path &work, const fs::path &outputs, bool trace,
           int out, int err) {
    if (!trace) {
        if (run(cmd, envs, out, err))
            throw Local("dependency expansion failed");
        return;
    }
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
    Words traced = {"strace",
                    "-ff",
                    "-qq",
                    "-ttt",
                    "-xx",
                    "-y",
                    "-s",
                    "4096",
                    "-o",
                    work / "trace",
                    "-e",
                    "trace=%file,%network,read,readv,getdents64,io_uring_setup,io_setup,ptrace,pidfd_getfd",
                    "-e",
                    "raw=read,readv",
                    "/usr/bin/bwrap",
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
        traced.insert(traced.end(), {"--setenv", k, v});
    traced.insert(traced.end(), {"--"});
    traced.insert(traced.end(), cmd.begin(), cmd.end());
    std::vector<int> inherited = {filter.fd, std::stoi(envs.at("DISTCC_RUST_LOADS_FD"))};
    if (run(traced, {}, out, err, &inherited))
        throw Local("isolated dependency expansion failed");
}
namespace {
string decoded(const string &s) {
    if (s.size() % 4)
        throw Local("truncated dependency trace path");
    string path;
    for (size_t i = 0; i < s.size(); i += 4) {
        if (s[i] != '\\' || s[i + 1] != 'x' || !isxdigit(s[i + 2]) || !isxdigit(s[i + 3]))
            throw Local("invalid dependency trace path");
        path.push_back(char(std::stoi(s.substr(i + 2, 2), nullptr, 16)));
    }
    return path;
}
bool system_path(const fs::path &p, const fs::path &sysroot) {
    return below(p, sysroot) ||
           std::any_of(runtime.begin(), runtime.end(), [&](auto &r) { return below(p, r); });
}
} // namespace
bool audit(const fs::path &work, const fs::path &depfile, int loads, const fs::path &sysroot,
           const std::set<fs::path> &search, std::set<fs::path> &inputs, std::set<fs::path> &dirs) {
    auto records = split(contents(loads), '\0');
    if (records.size() % 3)
        throw Local("incomplete compiler loader record");
    string start;
    std::set<fs::path> roots = search;
    for (auto &p : inputs)
        if (!system_path(p, sysroot))
            roots.insert(p.parent_path());
    auto allowed = [&](const fs::path &p) {
        return inputs.count(p) ||
               std::any_of(roots.begin(), roots.end(), [&](auto &r) { return below(p, r); });
    };
    for (size_t i = 0; i < records.size(); i += 3) {
        fs::path loaded = records[i + 2];
        if (loaded.empty())
            continue;
        if (!loaded.is_absolute()) {
            if (!fs::exists(loaded))
                throw Local("unresolved compiler library");
            loaded = fs::absolute(loaded);
        }
        if (system_path(loaded, sysroot))
            continue;
        auto real = fs::canonical(loaded);
        bool declared = allowed(loaded) || (records[i + 1].empty() && !start.empty());
        for (auto &dir : search) {
            auto spell = dir / loaded.filename();
            if (fs::is_regular_file(spell) && fs::equivalent(spell, loaded)) {
                inputs.insert(spell);
                declared = true;
            }
        }
        if (!declared)
            throw Local("undeclared compiler plugin");
        for (auto &name : {loaded, fs::path(records[i + 1]), real})
            if (name.is_absolute() && fs::is_regular_file(name))
                inputs.insert(name);
        roots.insert(real.parent_path());
        if (start.empty() || records[i] < start)
            start = records[i];
    }
    if (start.empty())
        return false;
    bool observed = false;
    for (auto &entry : fs::directory_iterator(work)) {
        if (entry.path().filename().string().compare(0, 6, "trace."))
            continue;
        std::ifstream stream(entry.path());
        string line;
        while (std::getline(stream, line)) {
            auto space = line.find(' '), paren = line.find('(', space), result = line.rfind(" = ");
            if (space == string::npos || line.compare(0, space, start) < 0 || paren == string::npos)
                continue;
            string op = line.substr(space + 1, paren - space - 1);
            if (op.empty() || op[0] == '-' || op[0] == '+')
                continue;
            observed = true;
            if (op == "getcwd")
                continue; // The worker recreates the client's working directory.
            if (op == "read" || op == "readv") {
                auto first = line.substr(paren + 1, line.find(',', paren) - paren - 1);
                if (std::stoul(first, nullptr, 0) == 0)
                    throw Local("macro read standard input");
                continue;
            }
            bool open = op == "openat" || op == "open";
            bool stat = op == "stat" || op == "lstat" || op == "newfstatat" || op == "statx" ||
                        op == "access" || op == "faccessat" || op == "faccessat2" || op == "readlink" ||
                        op == "readlinkat";
            if (!open && !stat)
                throw Local("macro dependency probe used " + op);
            auto q = line.find('"', paren), end = q == string::npos ? q : line.find('"', q + 1);
            if (end == string::npos || result == string::npos || line.compare(end + 1, 3, "...") == 0)
                throw Local("incomplete macro dependency trace");
            fs::path p = decoded(line.substr(q + 1, end - q - 1));
            if (p.is_relative()) {
                auto l = line.find('<', paren), r = line.find('>', l);
                auto parent =
                    l < q && r < q ? fs::path(decoded(line.substr(l + 1, r - l - 1))) : fs::current_path();
                p = p.empty() ? parent : parent / p;
            }
            p = p.lexically_normal();
            if (p == depfile)
                continue;
            if (open && line.find_first_of("|,", end) != string::npos &&
                (line.find("O_WRONLY", end) != string::npos || line.find("O_RDWR", end) != string::npos ||
                 line.find("O_TRUNC", end) != string::npos || line.find("O_CREAT", end) != string::npos))
                throw Local("macro wrote outside dependency output");
            if (line.compare(result + 3, 3, "-1 ") == 0)
                continue;
            if (system_path(p, sysroot))
                continue;
            if (!allowed(p))
                throw Local("macro read outside declared source or library roots: " + p.string());
            if (fs::is_directory(p))
                dirs.insert(p);
            else if (fs::is_regular_file(p)) {
                auto real = fs::canonical(p);
                if (real != p && !allowed(real) && !inputs.count(p))
                    throw Local("macro input symlink leaves declared roots");
                inputs.insert(p);
                inputs.insert(real);
            } else
                throw Local("non-regular macro dependency");
        }
        if (!stream.eof())
            throw Local("incomplete macro audit");
    }
    if (!observed)
        throw Local("missing macro syscall audit");
    return true;
}
} // namespace rust
