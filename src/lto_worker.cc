/* Run a staged LLVM backend and expose only its declared output files. */
#include "rust/sandbox.hh"
#include "rust/runtime.hh"
#include <fstream>
#include <limits.h>
#include <regex>
extern "C" {
#include "exec.h"
#include "exitcode.h"
}
using namespace rust;
namespace {
fs::path installed_tool(const fs::path &requested) {
    const auto name = requested.filename().string();
    static const std::regex pattern(R"((([[:alnum:]_.+]+-)*(clang(\+\+)?|lld|ld\.lld))(-[0-9]+(\.[0-9]+)*)?)");
    if (name.size() > NAME_MAX || !std::regex_match(name, pattern))
        throw std::runtime_error("expected an installed LLVM compiler or linker");
    auto directory = requested.parent_path();
    if (directory.filename() == "bin" && directory.parent_path().parent_path() == "/usr/lib/llvm" &&
        requested.lexically_normal() == requested && access(requested.c_str(), X_OK) == 0)
        return requested;
    auto path = env("DISTCCD_PATH");
    for (const auto &dir : split(path.empty() ? "/bin:/usr/bin" : path, ':')) {
        auto candidate = fs::path(dir) / name;
        if (candidate.is_absolute() && access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    throw std::runtime_error("LLVM tool is unavailable: " + name);
}
}

extern "C" int dcc_llvm_worker(const dcc_job *job, const char *root, int cancel, const char *out,
                               const char *err, dcc_job_result *result) {
    try {
        auto compiler = installed_tool(job->argv[0]);
        auto tool_dir = compiler.parent_path();
        auto private_dir = fs::path(root) / fs::path(DCC_LLVM_ROOT).relative_path();
        auto results = private_dir / "results";
        fs::create_directories(results);
        Sandbox box(Root::LLVM);
        box.args.insert(box.args.end(), {"--cap-drop", "ALL"});
        box.mount(tool_dir, tool_dir);
        auto check_mount = [&](const fs::path &path) {
            auto overlaps = [&](const fs::path &fixed) {
                return below(path, fixed) || below(fixed, path);
            };
            if (path == "/" || overlaps(tool_dir) || overlaps("/dev") || overlaps("/proc") ||
                overlaps("/sys") || overlaps(DCC_LLVM_ROOT) ||
                std::any_of(std::begin(runtime), std::end(runtime), overlaps))
                throw std::runtime_error("LLVM mount overlaps worker runtime: " + path.string());
        };
        // LLD renames a temporary sibling at completion. Give each output
        // parent a private writable mount, independent of the input cache.
        std::map<fs::path, fs::path> directories;
        std::vector<fs::path> outputs;
        for (auto p = job->llvm_outputs; *p; ++p) {
            fs::path name(*p), parent = name.parent_path();
            if (name.filename().empty()) throw std::runtime_error("invalid LLVM output");
            check_mount(parent);
            auto [it, inserted] = directories.try_emplace(parent, private_dir / std::to_string(directories.size()));
            if (inserted) fs::create_directory(it->second);
            outputs.push_back(it->second / name.filename());
        }
        for (auto &[parent, dir] : directories) box.mount(dir, parent, true);
        box.args.insert(box.args.end(), {"--dir", job->cwd});
        for (unsigned i = 0; i < job->count; ++i) {
            fs::path name(job->inputs[i].path);
            check_mount(name);
            box.mount(fs::path(root) / name.relative_path(), name);
        }
        box.args.insert(box.args.end(), {"--chdir", job->cwd, "--setenv", "PATH", "/bin:/usr/bin",
                                         "--setenv", "TMPDIR", "/tmp"});
        Words command{compiler};
        for (char **p = job->argv + 1; *p; ++p) command.emplace_back(*p);
        box.command(command);
        auto argv = pointers(box.args);
        pid_t pid;
        int status = 0, rc = dcc_spawn_child(argv.data(), &pid, "/dev/null", out, err);
        if (!rc)
            rc = dcc_collect_child("LLVM backend", pid, &status, cancel, &result->peak_bytes, &result->cpu_ns);
        if (rc || status) return rc ? rc << 8 : status;
        for (size_t i = 0; i < outputs.size(); ++i) {
            struct stat st;
            if (lstat(outputs[i].c_str(), &st) || !S_ISREG(st.st_mode) || (!i && !st.st_size))
                throw std::runtime_error("missing or invalid LLVM output " + string(job->llvm_outputs[i]));
            fs::rename(outputs[i], results / std::to_string(i));
        }
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(err, std::ios::app) << "distccd: LLVM backend: " << e.what() << '\n';
        return EXIT_IO_ERROR << 8;
    }
}
