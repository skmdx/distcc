/* Worker toolchain catalog and construction of the read-only rustc namespace. */
#include "rust/api.h"
#include "rust/common.hh"
#include "rust/sandbox.hh"

#include <chrono>
#include <fstream>
#include <glob.h>
#include <json-c/json.h>
#include <limits.h>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <set>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>

extern "C" {
#include "exec.h"
#include "clang_worker.h"
#include "fd_io.h"
#include "exitcode.h"
#include "workload.h"
}
using namespace rust;
namespace {
struct Catalog {
    pthread_mutex_t lock;
    struct Entry {
        XXH128_hash_t key;
        char identity[2048];
        char features[DCC_RUST_FEATURE_BYTES];
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
XXH128_hash_t fingerprint(const string &text) {
    return XXH3_128bits(text.data(), text.size());
}
std::optional<XXH128_hash_t> compiler_generation(const fs::path &path, bool validate) {
    auto root = path.parent_path().parent_path();
    string state = path.string() + stamp(metadata(path)) + affinity_key(), name = root.filename();
    if (name.compare(0, 12, "rust-distcc-") == 0) {
        auto digest = name.substr(12);
        auto marker = root / ".distcc-complete";
        if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != string::npos ||
            !fs::is_regular_file(fs::symlink_status(marker)))
            return {};
        state += stamp(metadata(marker));
        if (validate && read(marker) != "isa-checked " + digest + "\n")
            return {};
    }
    return fingerprint(state);
}
string cached_identity(const fs::path &path, XXH128_hash_t key, string &features) {
    for (auto &e : catalog->entries)
        if (XXH128_isEqual(e.key, key)) {
            features = e.features;
            return e.identity;
        }
    string id;
    try {
        auto report = query(path, "-vV");
        compiler_key(report); // Validate the identity before caching it.
        // A compiler can start successfully but fault during LLVM codegen.
        Work probe(fs::temp_directory_path());
        auto source = probe.dir / "probe.rs", output = probe.dir / "probe.o";
        Fd file(open(source.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
        put(file.fd, "pub fn value(x: u64) -> u64 { x.rotate_left(7) }");
        auto command = compiler_command(path);
        if (command.size() > 1)
            command.insert(command.end(), {"--sysroot", path.parent_path().parent_path()});
        command.insert(command.end(), {"--crate-name", "distcc_probe", "--crate-type=lib", "--emit=obj",
                                       "-O", source, "-o", output});
        if (run(command, {}, 2, 2) || !fs::is_regular_file(output) || !fs::file_size(output))
            throw Local("worker compiler failed codegen probe");
        id = report;
        features = capture_cpu_profile(path, probe, CpuProfile::HostIntersection);
        if (features.size() >= DCC_RUST_FEATURE_BYTES)
            features.clear();
    } catch (const Local &e) {
        fprintf(stderr, "distccd: %s probe: %s\n", path.c_str(), e.what());
    }
    auto state = compiler_generation(path, false);
    if (!state || !XXH128_isEqual(key, *state))
        throw Local("worker compiler changed");
    auto &e = catalog->entries[catalog->next++ % 32];
    if (id.size() < sizeof(e.identity)) {
        e.key = key;
        strcpy(e.identity, id.c_str());
        strcpy(e.features, features.c_str());
    }
    return id;
}
struct CompilerOffer {
    string host = "-";
    std::map<uint64_t, string> keys;
};
fs::path select_compiler(const string &requested, const string &host, CompilerOffer *offer = nullptr) {
    if (dcc_rust_catalog_attach(-1))
        throw std::runtime_error("cannot initialize worker Rust catalog");
    CatalogGuard guard;
    auto key = fingerprint(requested + '\0' + host + '\0' + env("DISTCCD_RUSTC") + '\0' + env("PATH"));
    auto &route = catalog->routes[key.low64 % 32];
    if (!offer && route.candidate[0] && XXH128_isEqual(key, route.request)) {
        auto path = find_program(route.candidate);
        auto state = path.empty() ? std::nullopt : compiler_generation(path, false);
        if (state && XXH128_isEqual(*state, route.generation)) {
            return path;
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
    for (const auto &candidate : candidates) {
        auto path = find_program(candidate);
        if (path.empty() || !visited.insert(path).second)
            continue;
        auto current = compiler_generation(path, true);
        if (!current)
            continue;
        string features;
        auto report = cached_identity(path, *current, features);
        if (report.empty())
            continue;
        auto triple = field(report, "host: ");
        if (offer) {
            if (offer->keys.empty()) offer->host = triple;
            auto [slot, inserted] = offer->keys.emplace(compiler_key(report), features);
            if (!inserted && (slot->second.empty() || features.empty())) {
                slot->second.clear(); // Unknown for one candidate with this key.
            } else if (!inserted && slot->second != features) {
                auto a = split(slot->second, ','), b = split(features, ',');
                Words common;
                std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
                slot->second.clear();
                for (const auto &f : common) slot->second += (slot->second.empty() ? "" : ",") + f;
                if (common.empty()) slot->second = "-"; // Verified empty intersection.
            }
            continue;
        }
        if (identity(report) != requested || (!host.empty() && triple != host))
            continue;
        if (candidate.size() < sizeof(route.candidate)) {
            route.request = key;
            route.generation = *current;
            strcpy(route.candidate, candidate.c_str());
        }
        return path;
    }
    return {};
}
fs::path rooted(const fs::path &root, const fs::path &p) {
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
    if (*job->rust.crate_types)
        flags.emplace_back(string("--crate-type=") + job->rust.crate_types);
    string linker = job->rust.linker;
    auto compiler = select_compiler(job->rust.compiler, job->rust.host);
    if (compiler.empty()) {
        std::ofstream(err, std::ios::app) << "distcc-rust: worker has no matching Rust toolchain\n";
        return EXIT_LOCAL_COMPILE << 8;
    }
    fs::create_directories(rooted(root, job->cwd));
    fs::path output = rooted(root, DCC_RUST_OUT);
    fs::create_directories(output);
    Sandbox box(Root::Inputs, root);
    auto &cmd = box.args;
    if (fs::is_directory(root / "tmp"))
        for (auto &child : fs::directory_iterator(root / "tmp"))
            cmd.insert(cmd.end(), {"--ro-bind", child.path(), "/tmp/" + child.path().filename().string()});
    cmd.insert(cmd.end(), {"--bind", output, DCC_RUST_OUT, "--chdir", job->cwd});
    // The wire decoder admits only crate-owned keys. Install the worker's
    // loader, scratch and CPU capture settings after the crate environment.
    for (auto p = job->rust.environment; p && *p; ++p) {
        string entry = *p;
        auto eq = entry.find('=');
        cmd.insert(cmd.end(), {"--setenv", entry.substr(0, eq), entry.substr(eq + 1)});
    }
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
    cmd.insert(cmd.end(), {"--ro-bind", compiler.parent_path().parent_path(), DCC_RUST_COMPILER});
    auto rustc = compiler_command(compiler, DCC_RUST_COMPILER);
    rustc.insert(rustc.end(), flags.begin(), flags.end());
    box.command(rustc);
    auto argv = pointers(cmd);
    pid_t pid;
    int status = 0, rc = dcc_spawn_child(argv.data(), &pid, "/dev/null", out, err);
    if (!rc)
        rc = dcc_collect_child("rustc", pid, &status, cancel, &result->peak_bytes, &result->cpu_ns);
    if (rc)
        return rc << 8;
    if (!status && *job->rust.cpu &&
        (!fs::exists(profile / "cpu-used") || !fs::exists(profile / "features-used")))
        throw std::runtime_error("worker rustc did not consume native profile");
    return status;
}
struct Backends {
    std::map<string, fs::path> paths;
    string offer;
    Backends() {
        for (const auto &helper : backend_helpers()) {
            Fd out(dcc_job_memfd("llvm-capabilities"));
            if (run({helper, "--capabilities"}, {}, out.fd, 2))
                throw std::runtime_error("cannot query " + helper.string());
            auto caps = contents(out.fd);
            auto space = caps.find(' ');
            if (caps.empty() || caps.back() != '\n' || space == string::npos)
                throw std::runtime_error("invalid backend capability record");
            caps.pop_back();
            if (paths.emplace(caps.substr(0, space), helper).second) {
                if (!offer.empty())
                    offer += ';';
                offer += caps;
            }
        }
        if (offer.empty())
            offer = "-";
    }
};
const Backends &backends() {
    static const Backends value;
    return value;
}
} // namespace
extern "C" int dcc_rust_cgu_helper(const char *abi, char *path) {
    try {
        const auto &slots = backends().paths;
        auto slot = slots.find(abi);
        if (slot == slots.end()) return 0;
        return realpath(slot->second.c_str(), path) ? 1 : -1;
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: Rust CGU backend: %s\n", e.what());
        return -1;
    }
}

extern "C" int dcc_rust_cgu_worker(const dcc_job *job, const char *tree, int cancel,
                                   const char *out, const char *err, dcc_job_result *result) {
    if (!job->compiler_channel) return EXIT_LOCAL_COMPILE << 8;
    try {
        Fd input(job->inputs[0].fd >= 0 ? fcntl(job->inputs[0].fd, F_DUPFD_CLOEXEC, 0) :
                 open((fs::path(tree) / "cgu").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        struct stat st;
        if (fstat(input.fd, &st) || !S_ISREG(st.st_mode)) return EXIT_PROTOCOL_ERROR << 8;
        Fd object(dcc_job_memfd("rust-cgu-object"));
        Fd log(open(out, O_WRONLY | O_TRUNC | O_CLOEXEC));
        Fd diagnostics(open(err, O_WRONLY | O_APPEND | O_CLOEXEC));
        int pair[2];
        check(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair), "CGU reply channel");
        Fd reply(pair[0]);
        timespec started;
        clock_gettime(CLOCK_MONOTONIC, &started);
        {
            Fd completion(pair[1]);
            int fds[] = {object.fd, completion.fd, input.fd, log.fd, diagnostics.fd};
            dcc_native_request request{'R', 0};
            check(!dcc_send_fds(job->compiler_channel - 1, &request, sizeof(request), fds, std::size(fds)),
                  "submit Rust CGU");
        }
        dcc_native_result usage{};
        int rc = dcc_compiler_result(reply.fd, cancel, &started, &usage);
        if (rc) return rc << 8;
        result->peak_bytes = usage.peak_bytes;
        result->cpu_ns = usage.cpu_ns;
        if (!usage.status) {
            check(!fstat(object.fd, &st) && st.st_size > 8, "missing CGU result");
            result->size[DCC_JOB_OBJECT] = st.st_size;
            result->mode = 0600;
            result->fd[DCC_JOB_OBJECT] = open(("/proc/self/fd/" + std::to_string(object.fd)).c_str(),
                                              O_RDONLY | O_CLOEXEC);
            check(result->fd[DCC_JOB_OBJECT] >= 0, "open completed CGU result");
        }
        return usage.status;
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: Rust CGU: %s\n", e.what());
        return EXIT_IO_ERROR << 8;
    }
}
extern "C" const char *dcc_rust_cgu_capabilities(void) {
    try {
        return backends().offer.c_str();
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: LLVM capabilities: %s\n", e.what());
        return nullptr;
    }
}
extern "C" const char *dcc_rust_compilers(const char **first_host, const char **features) {
    static std::mutex lock;
    static std::chrono::steady_clock::time_point expires;
    struct Advertisement { string host, keys, features; };
    static Advertisement cached;
    thread_local Advertisement snapshot;
    std::lock_guard<std::mutex> guard(lock);
    auto now = std::chrono::steady_clock::now();
    if (now >= expires) {
        try {
            CompilerOffer offer;
            select_compiler("", "", &offer);
            string keys;
            std::map<string,string> by_features;
            for (auto &[key, features] : offer.keys) {
                char hex[17];
                snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)key);
                if (!keys.empty()) keys += ' ';
                keys += hex;
                if (!features.empty()) by_features[features] += string(" ") + hex;
            }
            string records;
            size_t used = 1; // Offset zero is the absent profile in dcc_capacity.
            for (const auto &[list, slots] : by_features) {
                if (list.size()+1 > DCC_RUST_FEATURE_POOL_BYTES-used) continue;
                used += list.size()+1;
                if (!records.empty()) records += ';';
                records += list + slots;
            }
            cached = {offer.host, keys.empty() ? "-" : keys, records.empty() ? "-" : records};
        } catch (const std::exception &e) {
            fprintf(stderr, "distccd: Rust host query: %s\n", e.what());
            return nullptr;
        }
        expires = now + std::chrono::seconds(5);
    }
    snapshot = cached;
    *first_host = snapshot.host.c_str();
    *features = snapshot.features.c_str();
    return snapshot.keys.c_str();
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
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: %s: %s\n", __func__, e.what());
        return nullptr;
    }
}
extern "C" void dcc_rust_watch_free(dcc_rust_artifacts *s) {
    delete s;
}
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
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: %s: %s\n", __func__, e.what());
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
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: %s: %s\n", __func__, e.what());
        return -1;
    }
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
                if (!rc)
                    rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
                if (!rc)
                    rc = pthread_mutex_init(&static_cast<Catalog *>(p)->lock, &attr);
                pthread_mutexattr_destroy(&attr);
            }
            munmap(p, sizeof(Catalog));
            if (rc)
                throw std::system_error(rc, std::generic_category(), "initialize Rust catalog");
            // Worker spawn actions install descriptors 3 through 5.
            return fcntl(file.fd, F_DUPFD_CLOEXEC, 6);
        }());
        return shared.fd;
    } catch (const std::exception &e) {
        fprintf(stderr, "distccd: %s: %s\n", __func__, e.what());
        return -1;
    }
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
