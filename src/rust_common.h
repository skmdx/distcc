/* Common compiler discovery and argv handling for native Rust jobs. */
#pragma once
#include <config.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <json-c/json.h>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
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
inline fs::path executable(const string &compiler) {
    if (compiler.find('/') != string::npos)
        return fs::canonical(compiler);
    for (const auto &dir : split(env("PATH"), ':')) {
        fs::path candidate = fs::path(dir) / compiler;
        if (!access(candidate.c_str(), X_OK))
            return fs::canonical(candidate);
    }
    throw Local("compiler unavailable");
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
inline int run(const Words &cmd, const Env &environment, int out, int err,
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
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            check(false, "wait for rustc");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
inline string field(const string &report, const string &name) {
    for (const auto &line : split(report, '\n'))
        if (line.compare(0, name.size(), name) == 0)
            return line.substr(name.size());
    throw Local("missing rustc identity field " + name);
}
inline string identity(const string &report) {
    return report.substr(0, report.find('\n')) + "\n" + field(report, "commit-hash: ") + "\n" +
           field(report, "LLVM version: ");
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
inline const Words runtime = {"/lib",         "/lib64",        "/usr/lib64",
                              "/usr/lib/gcc", "/usr/lib/llvm", "/etc/ld.so.cache"};
struct Option {
    string key;
    std::optional<string> value;
    void append(Words &v) const {
        if (!key.empty())
            v.push_back(key);
        if (value)
            v.push_back(*value);
    }
};
inline std::vector<Option> options(const Words &args) {
    static const std::set<string> values = {"--cap-lints",
                                            "--cfg",
                                            "--check-cfg",
                                            "--crate-name",
                                            "--crate-type",
                                            "--edition",
                                            "--error-format",
                                            "--extern",
                                            "--json",
                                            "--remap-path-prefix",
                                            "--target",
                                            "--sysroot",
                                            "--emit",
                                            "--out-dir",
                                            "-o",
                                            "-C",
                                            "-L",
                                            "-l",
                                            "-A",
                                            "-W",
                                            "-D",
                                            "-F",
                                            "--allow",
                                            "--warn",
                                            "--deny",
                                            "--forbid",
                                            "--force-warn",
                                            "--diagnostic-width",
                                            "--color",
                                            "--codegen",
                                            "-Z"};
    std::vector<Option> parsed;
    for (size_t i = 0; i < args.size(); ++i) {
        const string &word = args[i];
        if (word == "--") {
            for (++i; i < args.size(); ++i)
                parsed.push_back({"", args[i]});
            break;
        }
        auto eq = word.find('=');
        string key = word.substr(0, eq);
        if (values.count(key)) {
            if (eq == string::npos && i + 1 == args.size())
                throw Local("missing option value");
            parsed.push_back(
                {key == "--codegen" ? "-C" : key, eq == string::npos ? args[++i] : word.substr(eq + 1)});
        } else if (word.size() > 2 && values.count(word.substr(0, 2))) {
            parsed.push_back({word.substr(0, 2), word.substr(2)});
        } else if (!word.empty() && word[0] == '-')
            parsed.push_back({word, {}});
        else
            parsed.push_back({"", word});
    }
    return parsed;
}
inline bool metadata_notifications(const std::vector<Option> &opts) {
    bool meta = false, link = false, notify = false;
    for (const auto &o : opts) {
        if (!o.value || (o.key != "--emit" && o.key != "--json"))
            continue;
        for (auto &word : split(*o.value, ',')) {
            auto key = word.substr(0, word.find('='));
            if (o.key == "--json")
                notify |= key == "artifacts";
            else {
                meta |= key == "metadata";
                link |= key == "link";
            }
        }
    }
    return meta && link && notify;
}
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
inline string json_field(json_object *obj, const char *name) {
    json_object *v;
    return obj && json_object_object_get_ex(obj, name, &v) && json_object_is_type(v, json_type_string)
               ? string(json_object_get_string(v), json_object_get_string_len(v))
               : "";
}
inline Words emits(const std::vector<Option> &opts) {
    Words types, actions;
    for (const auto &o : opts) {
        if (o.key != "--emit" && o.key != "--crate-type")
            continue;
        auto &v = o.key == "--emit" ? actions : types;
        auto items = split(*o.value, ',');
        v.insert(v.end(), items.begin(), items.end());
    }
    if (actions.empty())
        actions.push_back("link");
    if (types.empty())
        types.push_back("bin");
    for (const auto &type : types)
        if (type == "proc-macro")
            throw Local("procedural macro compiler plugin");
    return actions;
}
// Match the linker driver name rustc uses for its target flavor. The actual
// executable is selected on the client from this local invocation.
inline string link_driver(const std::vector<Option> &opts) {
    bool native = false, explicit_type = false;
    string driver = "cc";
    auto actions = emits(opts);
    if (std::none_of(actions.begin(), actions.end(),
                     [](const string &a) { return a.substr(0, a.find('=')) == "link"; }))
        return {};
    for (auto &o : opts) {
        if (o.key == "--test")
            native = true;
        if (o.key == "--crate-type") {
            explicit_type = true;
            for (auto &kind : split(*o.value, ','))
                native |= kind == "bin" || kind == "cdylib" || kind == "dylib";
        }
        if (o.key == "-C" && o.value->compare(0, 7, "linker=") == 0)
            driver = o.value->substr(7);
    }
    return native || !explicit_type ? driver : "";
}
inline bool below(const string &path, const string &base) {
    return path == base || path.compare(0, base.size() + 1, base + "/") == 0;
}
} // namespace rust
