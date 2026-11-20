/* Common compiler discovery and argv handling for native Rust jobs. */
#pragma once
#include <config.h>
#include "affinity.hh"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <json-c/json.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <glob.h>
#include <linux/close_range.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xxhash.h>
extern "C" {
#include "job.h"
}
namespace rust {
namespace fs = std::filesystem;
using std::string;
using Words = std::vector<string>;
using Env = std::map<string, string>;
struct Local : std::runtime_error {
    using runtime_error::runtime_error;
};
inline void check(bool ok, const string &what) {
    if (!ok)
        throw std::system_error(errno, std::generic_category(), what);
}
struct Work {
    fs::path dir;
    explicit Work(const fs::path &parent) {
        string tmp = (parent / "distcc-rust-XXXXXX").string();
        check(mkdtemp(tmp.data()) != nullptr, "Rust working directory");
        dir = fs::absolute(tmp);
    }
    ~Work() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
struct Fd {
    int fd;
    explicit Fd(int n) : fd(n) { check(n >= 0, "open descriptor"); }
    Fd(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd(other.fd) { other.fd = -1; }
    ~Fd() {
        if (fd >= 0)
            close(fd);
    }
};
inline string env(const char *name) {
    const char *p = getenv(name);
    return p ? p : "";
}
inline string contents(int fd) {
    string result;
    char buf[8192];
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof(buf), result.size());
        if (n < 0 && errno == EINTR)
            continue;
        check(n >= 0, "read descriptor");
        if (!n)
            return result;
        result.append(buf, n);
    }
}
inline string read(const fs::path &path) {
    Fd file(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    return contents(file.fd);
}
inline void put(int fd, const string &data) {
    for (size_t off = 0; off < data.size();) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR)
            continue;
        check(n > 0, "write descriptor");
        off += n;
    }
}
inline Words split(const string &s, char delim) {
    Words words;
    std::istringstream stream(s);
    for (string word; std::getline(stream, word, delim);)
        words.push_back(word);
    return words;
}
inline std::vector<char *> pointers(const Words &words) {
    std::vector<char *> args;
    for (const auto &word : words)
        args.push_back(const_cast<char *>(word.c_str()));
    args.push_back(nullptr);
    return args;
}
inline fs::path find_program(const string &compiler) {
    auto dirs = compiler.find('/') == string::npos ? split(env("PATH"), ':') : Words{""};
    for (auto &dir : dirs) {
        fs::path path = fs::path(dir) / compiler;
        if (!access(path.c_str(), X_OK))
            return fs::canonical(path);
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES)
            check(false, path.string());
    }
    return {};
}
inline fs::path executable(const string &compiler) {
    fs::path path = find_program(compiler);
    if (path.empty())
        throw Local("compiler unavailable: " + compiler);
    return path;
}
inline fs::path native_library() {
    fs::path beside = fs::read_symlink("/proc/self/exe").parent_path() / "distcc-rust-native.so";
    return fs::exists(beside) ? beside : fs::path(RUST_NATIVE_LIBRARY);
}
inline Words compiler_command(const fs::path &compiler, const fs::path &mount = {}) {
    fs::path root = compiler.parent_path().parent_path();
    fs::path prefix = mount.empty() ? root : mount;
    if (fs::is_regular_file(root / "distcc-runtime/ld.so"))
        return {(prefix / "distcc-runtime/ld.so").string(), "--library-path",
                (prefix / "lib").string() + ":" + (prefix / "distcc-runtime").string(),
                (prefix / "bin/rustc").string()};
    return {mount.empty() ? compiler.string() : (mount / "bin/rustc").string()};
}
inline pid_t spawn(const Words &cmd, const Env &environment, int out, int err,
                   const std::vector<int> *inherited = nullptr) {
    auto args = pointers(cmd);
    pid_t pid = fork();
    check(pid >= 0, "fork rustc");
    if (!pid) {
        for (const auto &item : environment)
            if (setenv(item.first.c_str(), item.second.c_str(), 1))
                _exit(100);
        if (dup2(out, 1) < 0 || dup2(err, 2) < 0)
            _exit(100);
        if (inherited) {
            if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC))
                _exit(100);
            int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (input < 0 || dup2(input, 0) < 0 || fcntl(0, F_SETFD, 0) < 0)
                _exit(100);
            for (int fd : *inherited)
                if (fcntl(fd, F_SETFD, 0) < 0)
                    _exit(100);
        }
        execvp(args[0], args.data());
        _exit(110);
    }
    return pid;
}
inline int collect(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            check(false, "wait for rustc");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
inline int run(const Words &cmd, const Env &environment, int out, int err,
               const std::vector<int> *inherited = nullptr) {
    return collect(spawn(cmd, environment, out, err, inherited));
}
inline string query(const fs::path &compiler, const string &flag) {
    Fd output(dcc_job_memfd("rustc-query"));
    auto cmd = compiler_command(compiler);
    cmd.push_back(flag);
    if (run(cmd, {}, output.fd, 2))
        throw Local("compiler discovery failed");
    auto value = contents(output.fd);
    if (!value.empty() && value.back() == '\n')
        value.pop_back();
    return value;
}
inline string affinity_key() {
    auto mask = affinity();
    if (mask.empty()) throw Local("cannot determine CPU affinity");
    return string(reinterpret_cast<const char *>(mask.data()), mask.size() * sizeof(mask[0]));
}
inline string field(const string &report, const string &name) {
    for (const auto &line : split(report, '\n'))
        if (line.compare(0, name.size(), name) == 0)
            return line.substr(name.size());
    throw Local("missing rustc identity field " + name);
}
inline string llvm_abi(const string &version) {
    auto minor = version.find('.');
    if (minor == string::npos)
        throw Local("missing LLVM ABI in " + version);
    auto end = version.find_first_not_of("0123456789", minor + 1);
    return version.substr(0, end);
}
inline string identity(const string &report) {
    return report.substr(0, report.find('\n')) + "\n" + field(report, "commit-hash: ") + "\n" +
           llvm_abi(field(report, "LLVM version: "));
}
inline uint64_t compiler_key(const string &report) {
    auto key = identity(report) + '\n' + field(report, "host: ");
    return XXH3_64bits(key.data(), key.size());
}
inline struct stat metadata(const fs::path &p) {
    struct stat s;
    check(!stat(p.c_str(), &s), p);
    return s;
}
inline string stamp(const struct stat &s) {
    std::ostringstream out;
    out << s.st_dev << ':' << s.st_ino << ':' << s.st_size << ':' << s.st_mtim.tv_sec << ':'
        << s.st_mtim.tv_nsec << ':' << s.st_ctim.tv_sec << ':' << s.st_ctim.tv_nsec;
    return out.str();
}
inline string replace(string s, const string &from, const string &to) {
    for (size_t i = 0; (i = s.find(from, i)) != string::npos; i += to.size())
        s.replace(i, from.size(), to);
    return s;
}
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
inline string json_field(json_object *obj, const char *name) {
    json_object *v;
    return obj && json_object_object_get_ex(obj, name, &v) && json_object_is_type(v, json_type_string)
               ? string(json_object_get_string(v), json_object_get_string_len(v))
               : "";
}
inline std::vector<fs::path> backend_helpers(const fs::path &sysroot = {}, const string &abi = {}) {
    auto bin = fs::read_symlink("/proc/self/exe").parent_path();
    std::vector<fs::path> found;
    auto add = [&](const fs::path &path) {
        if (access(path.c_str(), X_OK) == 0) {
            auto real = fs::canonical(path);
            if (std::find(found.begin(), found.end(), real) == found.end())
                found.push_back(real);
        }
    };
    add(bin / "distcc-rust-cgu");
    if (!sysroot.empty()) {
        auto lib = sysroot / "lib";
        add(lib / "distcc/distcc-rust-cgu");
        auto llvm = lib / ("libLLVM.so." + abi);
        if (fs::exists(llvm))
            add(fs::canonical(llvm).parent_path() / "distcc/distcc-rust-cgu");
    }
    add(bin / "llvm-lto/distcc-rust-cgu");
    for (auto &pattern :
         Words{(bin / "llvm-lto-[0-9]*/distcc-rust-cgu").string(),
               "/usr/lib/llvm/*/lib*/distcc/distcc-rust-cgu", "/usr/lib/llvm-*/lib/distcc/distcc-rust-cgu"}) {
        glob_t paths{};
        int rc = glob(pattern.c_str(), GLOB_NOESCAPE, nullptr, &paths);
        if (rc && rc != GLOB_NOMATCH)
            throw std::runtime_error("cannot enumerate LLVM backends");
        for (size_t i = 0; i < paths.gl_pathc; ++i)
            add(paths.gl_pathv[i]);
        globfree(&paths);
    }
    for (const auto &dir : split(env("PATH"), ':'))
        add(fs::path(dir) / "distcc-rust-cgu");
    return found;
}
inline bool below(const string &path, const string &base) {
    return path == base || path.compare(0, base.size() + 1, base + "/") == 0;
}
} // namespace rust
