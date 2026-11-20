/* Discover dependencies and audit operations after a procedural macro loads. */
#include "rust/probe.h"
#include "rust/api.h"
#include "rust/runtime.hh"
#include "rust/sandbox.hh"
namespace rust {
int run_probe(const Words &cmd, Env envs, const fs::path &outputs, bool isolate, int diagnostics,
              std::vector<int> inherited) {
    envs["LD_PRELOAD"] = native_library().string() + " " + env("LD_PRELOAD");
    // Portage can trace rustc without forwarding LD_PRELOAD. Install the
    // instrumentation and caller's preload together at the confined exec.
    if (!isolate && env("SANDBOX_ON") != "1")
        return run(cmd, envs, diagnostics, diagnostics, &inherited);
    Sandbox box(Root::Host);
    box.mount(outputs, outputs, true);
    for (auto &[k,v] : envs) box.args.insert(box.args.end(), {"--setenv", k, v});
    box.command(cmd);
    inherited.push_back(box.filter.fd);
    return run(box.args, {}, diagnostics, diagnostics, &inherited);
}
bool runtime_path(const fs::path &p, const fs::path &sysroot) {
    return below(p, sysroot) ||
           std::any_of(std::begin(runtime), std::end(runtime), [&](auto &r) { return below(p, r); });
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
    std::vector<fs::path> exports;
    if (macro)
        for (const auto &dir : split(env("DISTCC_FRONTEND_ROOTS"), ':'))
            if (!dir.empty())
                exports.push_back(fs::absolute(dir).lexically_normal());
    for (size_t i = 0; macro && i < records.size(); i += 3) {
        if (records[i] == "L" || records[i] == "D")
            continue;
        fs::path spelling(records[i + 1]);
        auto p = spelling.lexically_normal();
        if (p == depfile)
            continue;
        auto &kind = records[i];
        if (kind != "R" && kind != "N" && kind != "T" && kind != "S")
            throw Local("macro requires local operation: " + records[i + 1]);
        if (runtime_path(p, sysroot))
            continue;
        if (kind == "N" || kind == "T") {
            if (below(p, DCC_RUST_ROOT) || below(p, "/dev") || below(p, "/proc") || below(p, "/sys"))
                throw Local("macro lookup overlaps worker runtime: " + p.string());
            if (kind == "N") {
                if (fs::exists(spelling))
                    throw Local("macro lookup changed during discovery");
                continue;
            }
            // Without the non-directory component the worker would report
            // ENOENT. Replay it under the same file export and stability rules.
            while (spelling.has_relative_path() && !fs::exists(spelling))
                spelling = spelling.parent_path();
            if (!fs::is_regular_file(spelling))
                throw Local("macro lookup changed during discovery");
            p = spelling.lexically_normal();
        }
        if (kind == "S" || (spelling != p && !fs::equivalent(spelling, p)))
            throw Local("macro observes a symbolic link: " + spelling.string());
        if (fs::is_directory(p)) {
            if (!allowed(p))
                throw Local("macro directory outside declared inputs: " + p.string());
            dirs.insert(p);
        } else {
            auto real = fs::canonical(p);
            // SourceMap reads can point into another vendored crate. Only the
            // observed file is staged from the caller's exported build tree.
            bool exported = real == p && std::any_of(exports.begin(), exports.end(), [&](const auto &dir) {
                return dir == "/" || below(real, dir);
            });
            if (!fs::is_regular_file(real) || (!exported && (!allowed(p) || (!allowed(real) && !inputs.count(p)))))
                throw Local("unsupported macro input " + p.string());
            inputs.insert(p);
            inputs.insert(real);
        }
    }
    return macro;
}
} // namespace rust
