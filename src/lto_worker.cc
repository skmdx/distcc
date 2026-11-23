/* Run a staged LLVM backend and expose only its declared output files. */
#include "rust/sandbox.hh"
#include "rust/runtime.hh"
#include <fstream>
#include <limits.h>
#include <regex>
#include <spawn.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/resource.h>
extern "C" {
#include "exec.h"
#include "exitcode.h"
#include "helpers.h"
#include "clang_worker.h"
}
using namespace rust;
namespace {
fs::path installed_tool(const fs::path &requested, bool &linker) {
    const auto name = requested.filename().string();
    static const std::regex pattern(R"((?:[[:alnum:]_.+]+-)*(clang(?:\+\+)?|lld|ld\.lld)(?:-[0-9]+(?:\.[0-9]+)*)?)");
    std::smatch match;
    if (name.size() > NAME_MAX || !std::regex_match(name, match, pattern))
        throw std::runtime_error("expected an installed LLVM compiler or linker");
    linker = match[1] == "lld" || match[1] == "ld.lld";
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

fs::path installed_plugin(const fs::path &linker, const fs::path &requested) {
    auto prefix = fs::canonical(linker).parent_path().parent_path();
    auto name = requested.filename();
    for (auto directory : {"lib64", "lib"}) {
        auto library = prefix / directory / name;
        if (fs::is_regular_file(library)) return library;
    }
    throw std::runtime_error("LLVM linker plugin is unavailable: " + name.string());
}
}

extern "C" int dcc_lto_helper(const dcc_job *job, char *path) {
    if (job->kind != DCC_JOB_THINLTO || !job->llvm_outputs[0] || job->llvm_outputs[1]) return 0;
    for (unsigned i = 0; i < job->count; ++i)
        if (!dcc_job_file(&job->inputs[i])) return 0;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) return -1;
    if (rlim_t(job->count) + 32 > std::min<rlim_t>(limit.rlim_max, DCC_JOB_INPUT_MAX)) return 0;
    bool object = false, ir = false, thinlto_index = false;
    for (auto p = job->argv + 1; *p; ++p) {
        if (!strcmp(*p, "-c") || !strcmp(*p, "-emit-obj")) object = true;
        if (!strncmp(*p, "-x", 2)) {
            const char *language = (*p)[2] ? *p + 2 : *++p;
            if (!language || strcmp(language, "ir")) return 0;
            ir = true;
        }
        if (!strncmp(*p, "-fthinlto-index=", sizeof("-fthinlto-index=") - 1)) thinlto_index = true;
        if (!strncmp(*p, "-save-temps", 11) || !strncmp(*p, "--save-temps", 12)) return 0;
    }
    if (!object || !job->count) return 0;
    auto extension = fs::path(job->inputs[0].path).extension();
    if (!ir && !thinlto_index && extension != ".bc" && extension != ".ll") return 0;
    try {
        bool linker;
        auto tool = fs::canonical(installed_tool(job->argv[0], linker));
        if (linker) return 0;
        std::smatch match;
        static const std::regex version(R"(clang(?:\+\+)?-([0-9]+)(?:\.[0-9]+)*)");
        auto name = tool.filename().string();
        if (!std::regex_match(name, match, version)) return 0;
        char storage[PATH_MAX];
        auto helper = dcc_helper_path(("distcc-clang-" + match[1].str()).c_str(), storage);
        if (access(helper, X_OK)) return errno == ENOENT ? 0 : -1;
        return realpath(helper, path) ? 1 : -1;
    } catch (const std::exception &) { return 0; }
}

extern "C" int dcc_spawn_backend(pid_t *pid, int channel, const char *helper, const dcc_job *job) {
    try {
        Sandbox box(Root::ResidentLLVM);
        if (job->kind == DCC_JOB_RUST_CGU) {
            box.mount(helper, "/backend");
            box.command({"/backend", "--worker", "3"});
        } else {
            bool linker;
            auto tool_dir = fs::canonical(installed_tool(job->argv[0], linker)).parent_path();
            box.mount(tool_dir, tool_dir);
            box.mount(helper, "/distcc-clang");
            box.command({"/distcc-clang", "--lto-worker", "3"});
        }
        auto argv = pointers(box.args);
        Fd filter(fcntl(box.filter.fd, F_DUPFD_CLOEXEC, 5));
        posix_spawn_file_actions_t actions;
        int rc = posix_spawn_file_actions_init(&actions);
        if (rc) return rc;
        rc = posix_spawn_file_actions_adddup2(&actions, channel, 3);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, filter.fd, 4);
        if (!rc) rc = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&actions, 5);
        if (!rc) rc = posix_spawn(pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        return rc;
    } catch (const std::exception &) { return EIO; }
}

namespace {
struct Channel {
    int fd[2];
    Channel() { check(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fd), "create job channel"); }
    ~Channel() { close(fd[0]); if (fd[1] >= 0) close(fd[1]); }
    void sent() { close(fd[1]); fd[1] = -1; }
};
int resident(const dcc_job *job, const char *root, const fs::path &output,
             const char *out, const char *err, int cancel, dcc_job_result *result) {
    Fd object(open(output.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
    Fd diagnostics(open(err, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
    Fd stdout_log(open(out, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
    Fd spec(dcc_job_memfd("distcc-lto-command"));
    Channel reply, transfer;
    char *directory[] = {job->cwd, job->llvm_outputs[0], nullptr};
    std::vector<char *> names;
    for (unsigned i = 0; i != job->count; ++i) names.push_back(job->inputs[i].path);
    names.push_back(nullptr);
    for (auto words : {directory + 0, job->argv, names.data()})
        check(!dcc_x_argv(spec.fd, "ARGC", "ARGV", words), "write backend command");
    check(lseek(spec.fd, 0, SEEK_SET) == 0, "rewind backend command");
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int fds[] = {spec.fd, reply.fd[1], transfer.fd[1], stdout_log.fd, diagnostics.fd, object.fd};
    dcc_native_request run{'T', 0};
    check(!dcc_send_fds(job->compiler_channel - 1, &run, sizeof(run), fds, std::size(fds)), "submit backend");
    reply.sent(); transfer.sent();
    Fd staged(open(root, O_PATH | O_DIRECTORY | O_CLOEXEC));
    struct open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
    check(!fcntl(transfer.fd[0], F_SETFL, O_NONBLOCK), "set input channel nonblocking");
    for (unsigned i = 0; i < job->count;) {
        unsigned char count = std::min<unsigned>(job->count - i, DCC_IPC_FDS_MAX);
        std::vector<Fd> batch;
        int descriptors[DCC_IPC_FDS_MAX];
        for (unsigned n = 0; n < count; ++n, ++i) {
            auto file = fs::path(job->inputs[i].path).relative_path();
            batch.emplace_back(syscall(SYS_openat2, staged.fd, file.c_str(), &how, sizeof(how)));
            struct stat st;
            check(!fstat(batch.back().fd, &st) && S_ISREG(st.st_mode), "expected staged regular input");
            descriptors[n] = batch.back().fd;
        }
        for (;;) {
            int rc = dcc_wait_compiler_fd(transfer.fd[0], POLLOUT, cancel, &started);
            if (rc) return rc << 8;
            if (!dcc_send_fds(transfer.fd[0], &count, 1, descriptors, count)) break;
            check(errno == EAGAIN, "send backend inputs");
        }
    }
    dcc_native_result usage{};
    int rc = dcc_compiler_result(reply.fd[0], cancel, &started, &usage);
    if (rc) return rc << 8;
    result->peak_bytes = usage.peak_bytes; result->cpu_ns = usage.cpu_ns;
    if (!usage.status) {
        struct stat st;
        check(!fstat(object.fd, &st) && S_ISREG(st.st_mode) && st.st_size, "missing backend output");
    }
    return usage.status;
}
}

extern "C" int dcc_llvm_worker(const dcc_job *job, const char *root, int cancel, const char *out,
                               const char *err, dcc_job_result *result) {
    try {
        bool linker;
        auto compiler = installed_tool(job->argv[0], linker);
        auto tool_dir = compiler.parent_path();
        auto private_dir = fs::path(root) / fs::path(DCC_LLVM_ROOT).relative_path();
        auto results = private_dir / "results";
        fs::create_directories(results);
        if (job->compiler_channel)
            return resident(job, root, results / "0", out, err, cancel, result);
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
        for (char **p = job->argv + 1; *p; ++p) {
            string arg = *p;
            auto equal = arg.find('=');
            auto option = arg.substr(0, equal);
            if (linker && (option == "-plugin" || option == "--plugin")) {
                if (equal == string::npos && !p[1])
                    throw std::runtime_error("missing LLVM linker plugin");
                command.push_back(option);
                command.push_back(installed_plugin(compiler, equal == string::npos ? *++p : arg.substr(equal + 1)));
            } else {
                command.push_back(arg);
            }
        }
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
