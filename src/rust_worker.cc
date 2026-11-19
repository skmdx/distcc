/* Worker toolchain catalog and construction of the read-only rustc namespace. */
#include "rust_common.h"
#include "rust_job.h"
#include <fstream>
#include <glob.h>
#include <chrono>
#include <mutex>
#include <limits.h>
#include <json-c/json.h>
#include <sys/inotify.h>
#include <pthread.h>
#include <sys/mman.h>
extern "C" {
#include "exec.h"
#include "exitcode.h"
}
using namespace rust;
namespace {
struct Catalog {
    pthread_mutex_t lock;
    struct Entry {
        XXH128_hash_t key;
        char identity[2048];
    } entries[32];
    struct Route {
        XXH128_hash_t request, generation;
        char candidate[PATH_MAX];
    } routes[32];
    unsigned next;
};
Catalog *catalog;
struct CatalogGuard {
    CatalogGuard() {
        int rc = pthread_mutex_lock(&catalog->lock);
        if (rc == EOWNERDEAD) {
            memset(catalog->entries, 0, sizeof(catalog->entries));
            memset(catalog->routes, 0, sizeof(catalog->routes));
            rc = pthread_mutex_consistent(&catalog->lock);
        }
        if (rc)
            throw std::system_error(rc, std::generic_category(), "Rust catalog");
    }
    ~CatalogGuard() { pthread_mutex_unlock(&catalog->lock); }
};
XXH128_hash_t fingerprint(const string &text) { return XXH3_128bits(text.data(), text.size()); }
string cached_identity(const fs::path &path) {
    string generation = path.string() + ":" + stamp(metadata(path));
    auto key = fingerprint(generation);
    for (auto &e : catalog->entries)
        if (e.identity[0] && XXH128_isEqual(e.key, key))
            return e.identity;
    string id = query(path, "-vV");
    if (generation != path.string() + ":" + stamp(metadata(path)))
        throw Local("worker compiler changed");
    auto &e = catalog->entries[catalog->next++ % 32];
    if (id.size() < sizeof(e.identity)) {
        e.key = key;
        strcpy(e.identity, id.c_str());
    }
    return id;
}
fs::path select_compiler(const string &requested, const string &host, string *advertised = nullptr) {
    if (dcc_rust_catalog_attach(-1))
        throw std::runtime_error("cannot initialize worker Rust catalog");
    CatalogGuard guard;
    auto key = fingerprint(requested + '\0' + host + '\0' + env("DISTCCD_RUSTC") + '\0' + env("PATH"));
    auto &route = catalog->routes[key.low64 % 32];
    auto generation = [](const fs::path &path, bool validate) {
        auto root = path.parent_path().parent_path();
        string state = path.string() + stamp(metadata(path)), name = root.filename();
        if (name.compare(0, 12, "rust-distcc-") == 0 && name.size() == 76) {
            auto marker = root / ".distcc-complete";
            state += stamp(metadata(marker));
            if (validate && read(marker) != name.substr(12) + "\n")
                throw Local("incomplete Rust slot");
        }
        return fingerprint(state);
    };
    if (route.candidate[0] && XXH128_isEqual(key, route.request)) {
        try {
            auto path = executable(route.candidate);
            if (XXH128_isEqual(generation(path, false), route.generation)) {
                if (advertised)
                    *advertised = field(cached_identity(path), "host: ");
                return path;
            }
        } catch (const std::exception &) {
        }
        route.candidate[0] = 0;
    }
    Words candidates = split(env("DISTCCD_RUSTC"), ':');
    if (candidates.empty()) {
        candidates.push_back("rustc");
        for (const char *pattern : {"/usr/lib/rust/*/bin/rustc", "/opt/rust-*/bin/rustc"}) {
            glob_t paths{};
            if (!glob(pattern, 0, nullptr, &paths))
                candidates.insert(candidates.end(), paths.gl_pathv, paths.gl_pathv + paths.gl_pathc);
            globfree(&paths);
        }
    }
    std::set<fs::path> visited;
    for (auto &candidate : candidates)
        try {
            fs::path path = executable(candidate);
            if (!visited.insert(path).second)
                continue;
            auto current = generation(path, true);
            auto report = cached_identity(path);
            if ((advertised || identity(report) == requested) &&
                (host.empty() || field(report, "host: ") == host)) {
                if (advertised)
                    *advertised = field(report, "host: ");
                if (candidate.size() < sizeof(route.candidate)) {
                    route.request = key;
                    route.generation = current;
                    strcpy(route.candidate, candidate.c_str());
                }
                return path;
            }
        } catch (const std::exception &) {
            continue;
        }
    return {};
}
fs::path rooted(const fs::path &root, const fs::path &p) {
    if (!p.is_absolute() || p.lexically_normal() != p)
        throw Local("invalid worker path");
    return root / p.relative_path();
}
void write_profile(const fs::path &path, const string &data) {
    Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    put(fd.fd, data);
}
int execute(const dcc_job *job, const fs::path &root, int cancel, const char *out, const char *err,
            dcc_job_result *result) {
    Words flags;
    for (char **p = job->argv; *p; ++p)
        flags.emplace_back(*p);
    auto linker = link_driver(options(flags));
    auto compiler = select_compiler(job->rust.compiler, job->rust.host);
    if (compiler.empty()) {
        std::ofstream(err, std::ios::app) << "distcc-rust: worker has no matching Rust toolchain\n";
        return EXIT_LOCAL_COMPILE << 8;
    }
    fs::create_directories(rooted(root, job->cwd));
    fs::path output = rooted(root, DCC_RUST_OUT);
    fs::create_directories(output);
    Words cmd = {"/usr/bin/bwrap", "--unshare-all", "--die-with-parent", "--bind", root, "/"};
    for (auto &p : runtime)
        if (fs::exists(p))
            cmd.insert(cmd.end(), {"--ro-bind", p, p});
    cmd.insert(cmd.end(), {"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"});
    if (fs::is_directory(root / "tmp"))
        for (auto &child : fs::directory_iterator(root / "tmp"))
            cmd.insert(cmd.end(), {"--ro-bind", child.path(), "/tmp/" + child.path().filename().string()});
    cmd.insert(cmd.end(), {"--bind", output, DCC_RUST_OUT, "--chdir", job->cwd, "--clearenv"});
    if (job->rust.host && *job->rust.host) {
        std::set<fs::path> libraries;
        for (unsigned i = 0; i < job->count; ++i) {
            fs::path path = job->inputs[i].path;
            if (path.filename().string().find(".so") != string::npos)
                libraries.insert(path.parent_path());
        }
        string path;
        for (const auto &dir : libraries)
            path += (path.empty() ? "" : ":") + dir.string();
        if (!path.empty())
            cmd.insert(cmd.end(), {"--setenv", "LD_LIBRARY_PATH", path});
    }
    if (!linker.empty()) {
        auto capture = fs::path(DCC_RUST_LINKERS) / fs::path(linker).filename();
        cmd.insert(cmd.end(), {"--ro-bind", fs::read_symlink("/proc/self/exe"), capture, "--tmpfs",
                               DCC_RUST_SCRATCH, "--setenv", "TMPDIR", DCC_RUST_SCRATCH});
        flags.push_back("-Clinker=" + capture.string());
    }
    for (auto p = job->rust.directories; p && *p; ++p)
        fs::create_directories(rooted(root, *p));
    for (auto p = job->rust.environment; p && *p; ++p) {
        string entry = *p;
        auto eq = entry.find('=');
        string key = entry.substr(0, eq);
        if (eq == string::npos || key.empty() || key.compare(0, 3, "LD_") == 0 ||
            key.compare(0, 5, "DYLD_") == 0 || key == "RUSTC_WRAPPER" || key == "RUSTC_WORKSPACE_WRAPPER")
            throw Local("unsafe worker environment");
        cmd.insert(cmd.end(), {"--setenv", key, entry.substr(eq + 1)});
    }
    fs::path profile = rooted(root, DCC_RUST_PROFILE);
    if (*job->rust.cpu) {
        fs::create_directory(profile);
        write_profile(profile / "cpu", job->rust.cpu);
        write_profile(profile / "features", job->rust.features);
        cmd.insert(cmd.end(),
                   {"--ro-bind", native_library(), DCC_RUST_PRELOAD, "--bind", profile, DCC_RUST_PROFILE,
                    "--setenv", "LD_PRELOAD", DCC_RUST_PRELOAD, "--setenv", "DISTCC_RUST_NATIVE",
                    DCC_RUST_PROFILE, "--setenv", "DISTCC_RUST_NATIVE_CAPTURE", "0"});
    }
    cmd.insert(cmd.end(), {"--ro-bind", compiler.parent_path().parent_path(), DCC_RUST_COMPILER,
                           "--remount-ro", "/", "--"});
    auto rustc = compiler_command(compiler, DCC_RUST_COMPILER);
    cmd.insert(cmd.end(), rustc.begin(), rustc.end());
    cmd.insert(cmd.end(), flags.begin(), flags.end());
    auto argv = pointers(cmd);
    pid_t pid;
    int status = 0, rc = dcc_spawn_child(argv.data(), &pid, "/dev/null", out, err);
    if (!rc)
        rc = dcc_collect_child("rustc", pid, &status, cancel, &result->peak_bytes, &result->cpu_ns);
    if (rc)
        return rc << 8;
    if (!status && *job->rust.cpu &&
        (!fs::exists(profile / "cpu-used") || !fs::exists(profile / "features-used")))
        throw Local("worker rustc did not consume native profile");
    return status;
}
} // namespace
extern "C" int dcc_spawn_rust_cgu(pid_t *pid, const char *tree, const char *output, const char *stdout_path,
                                  const char *stderr_path) {
    try {
        // Pin the prepared input before crossing the mount boundary. Only
        // the result descriptor is writable, and LLVM starts inside bwrap.
        Fd in(open((fs::path(tree) / "cgu").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
        Fd out(open(output, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600));
        struct stat st;
        if (fstat(in.fd, &st) || !S_ISREG(st.st_mode))
            return EXIT_PROTOCOL_ERROR;
        auto descriptor = [](int fd) { return "/proc/self/fd/" + std::to_string(fd); };
        Words cmd{"/usr/bin/bwrap",
                  "--unshare-all",
                  "--die-with-parent",
                  "--clearenv",
                  "--ro-bind",
                  "/usr",
                  "/usr",
                  "--dev",
                  "/dev",
                  "--proc",
                  "/proc",
                  "--tmpfs",
                  "/tmp"};
        for (auto p : {"/lib", "/lib64", "/etc/ld.so.cache"})
            if (fs::exists(p))
                cmd.insert(cmd.end(), {"--ro-bind", p, p});
        cmd.insert(cmd.end(), {"--ro-bind", executable("distcc-rust-cgu"), "/backend", "--ro-bind",
                               descriptor(in.fd), "/input", "--bind", descriptor(out.fd), "/output",
                               "--remount-ro", "/", "--", "/backend", "--execute"});
        auto argv = pointers(cmd);
        return dcc_spawn_child(argv.data(), pid, "/dev/null", stdout_path, stderr_path);
    } catch (const std::exception &) {
        return EXIT_IO_ERROR;
    }
}
extern "C" const char *dcc_rust_cgu_capabilities(void) {
    static const string capabilities = []() -> string {
        try {
            Fd out(dcc_job_memfd("llvm-capabilities"));
            if (run({executable("distcc-rust-cgu"), "--capabilities"}, {}, out.fd, 2))
                return "-";
            auto data = contents(out.fd);
            if (data.empty() || data.back() != '\n' || data.size() >= 1024)
                return "-";
            data.pop_back();
            return data;
        } catch (const std::exception &) {
            return "-";
        }
    }();
    return capabilities.c_str();
}
extern "C" const char *dcc_rust_host(void) {
    static std::mutex lock;
    static std::chrono::steady_clock::time_point expires;
    static char host[96] = "-";
    thread_local char snapshot[sizeof(host)];
    std::lock_guard<std::mutex> guard(lock);
    auto now = std::chrono::steady_clock::now();
    if (now >= expires) {
        string found = "-";
        try {
            select_compiler("", "", &found);
        } catch (const std::exception &) {
        }
        snprintf(host, sizeof(host), "%s", found.size() < sizeof(host) ? found.c_str() : "-");
        expires = now + std::chrono::seconds(5);
    }
    memcpy(snapshot, host, sizeof(host));
    return snapshot;
}
extern "C" int dcc_rust_pipelined(const dcc_job *job) {
    if (!job || job->kind != DCC_JOB_RUST)
        return 0;
    Words argv;
    for (auto p = job->argv; *p; ++p)
        argv.emplace_back(*p);
    try {
        return metadata_notifications(options(argv));
    } catch (const std::exception &) {
        return 0;
    }
}
struct dcc_rust_artifacts {
    fs::path output;
    Fd log, events;
    off_t offset = 0;
    string partial, line, name;
    explicit dcc_rust_artifacts(const fs::path &root)
        : output(rooted(root, DCC_RUST_OUT)),
          log(open(rooted(root, DCC_RUST_STDERR).c_str(), O_CREAT | O_EXCL | O_RDONLY | O_CLOEXEC, 0600)),
          events(inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
        check(inotify_add_watch(events.fd, rooted(root, DCC_RUST_STDERR).c_str(),
                                IN_MODIFY | IN_CLOSE_WRITE) >= 0,
              "watch rustc diagnostics");
    }
};
extern "C" dcc_rust_artifacts *dcc_rust_watch(const char *root, int *fd) {
    try {
        fs::create_directories(rooted(root, DCC_RUST_ROOT));
        auto watch = new dcc_rust_artifacts(root);
        *fd = watch->events.fd;
        return watch;
    } catch (const std::exception &) {
        return nullptr;
    }
}
extern "C" void dcc_rust_watch_free(dcc_rust_artifacts *s) { delete s; }
extern "C" int dcc_rust_next_metadata(dcc_rust_artifacts *s, const char **name, const char **line,
                                      int *file) {
    try {
        char buf[8192];
        while (read(s->events.fd, buf, sizeof(buf)) > 0) {
        }
        for (;;) {
            auto end = s->partial.find('\n');
            if (end == string::npos) {
                ssize_t n = pread(s->log.fd, buf, sizeof(buf), s->offset);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    return int(n);
                s->offset += n;
                s->partial.append(buf, n);
                continue;
            }
            s->line.assign(s->partial, 0, end + 1);
            s->partial.erase(0, end + 1);
            if (s->line.empty() || s->line[0] != '{' || s->line.find("\"artifact\"") == string::npos)
                continue;
            Json obj(json_tokener_parse(s->line.c_str()), json_object_put);
            if (json_field(obj.get(), "$message_type") != "artifact" ||
                json_field(obj.get(), "emit") != "metadata")
                continue;
            fs::path artifact(json_field(obj.get(), "artifact"));
            if (artifact.parent_path() != DCC_RUST_OUT || artifact.filename() == "." ||
                artifact.filename() == "..")
                return -1;
            s->name = artifact.filename();
            *file = open((s->output / s->name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            *name = s->name.c_str();
            *line = s->line.c_str();
            return *file < 0 ? -1 : 1;
        }
    } catch (const std::exception &) {
        return -1;
    }
}
extern "C" int dcc_rust_catalog_attach(int fd) {
    static std::once_flag initialized;
    try {
        std::call_once(initialized, [fd] {
            int source = fd < 0 ? dcc_rust_catalog_fd() : fd;
            auto p = mmap(nullptr, sizeof(Catalog), PROT_READ | PROT_WRITE, MAP_SHARED, source, 0);
            check(p != MAP_FAILED, "attach Rust catalog");
            catalog = static_cast<Catalog *>(p);
        });
        return 0;
    } catch (const std::exception &) { return -1; }
}
extern "C" int dcc_rust_catalog_fd(void) {
    try {
        static const Fd shared([] {
            Fd file(dcc_job_memfd("distccd-rust-catalog"));
            check(!ftruncate(file.fd, sizeof(Catalog)), "size Rust catalog");
            auto p = mmap(nullptr, sizeof(Catalog), PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
            check(p != MAP_FAILED, "map Rust catalog");
            pthread_mutexattr_t attr;
            int rc = pthread_mutexattr_init(&attr);
            if (!rc) {
                rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
                if (!rc) rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
                if (!rc) rc = pthread_mutex_init(&static_cast<Catalog *>(p)->lock, &attr);
                pthread_mutexattr_destroy(&attr);
            }
            munmap(p, sizeof(Catalog));
            if (rc) throw std::system_error(rc, std::generic_category(), "initialize Rust catalog");
            // Worker spawn actions install descriptors 3 through 5.
            return fcntl(file.fd, F_DUPFD_CLOEXEC, 6);
        }());
        return shared.fd;
    } catch (const std::exception &) { return -1; }
}
extern "C" int dcc_rust_worker(const dcc_job *job, const char *root, int cancel, const char *out,
                               const char *err, dcc_job_result *result) {
    try {
        return execute(job, root, cancel, out, err, result);
    } catch (const std::exception &e) {
        std::ofstream(err, std::ios::app) << "distcc-rust: worker: " << e.what() << '\n';
        return EXIT_IO_ERROR << 8;
    }
}
