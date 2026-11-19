/* Worker toolchain catalog and construction of the read-only rustc namespace. */
#include "rust_common.h"
#include "rust_job.h"
#include <fstream>
#include <glob.h>
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
    unsigned next;
};
Catalog *catalog;
string cached_identity(const fs::path &path) {
    string generation = path.string() + ":" + stamp(metadata(path));
    auto key = XXH3_128bits(generation.data(), generation.size());
    int rc = pthread_mutex_lock(&catalog->lock);
    if (rc == EOWNERDEAD) {
        memset(catalog->entries, 0, sizeof(catalog->entries));
        rc = pthread_mutex_consistent(&catalog->lock);
    }
    if (rc)
        throw std::system_error(rc, std::generic_category(), "Rust catalog");
    struct Unlock {
        ~Unlock() { pthread_mutex_unlock(&catalog->lock); }
    } guard;
    for (auto &e : catalog->entries)
        if (e.identity[0] && XXH128_isEqual(e.key, key))
            return e.identity;
    string id = identity(query(path, "-vV"));
    if (generation != path.string() + ":" + stamp(metadata(path)))
        throw Local("worker compiler changed");
    auto &e = catalog->entries[catalog->next++ % 32];
    if (id.size() < sizeof(e.identity)) {
        e.key = key;
        strcpy(e.identity, id.c_str());
    }
    return id;
}
fs::path select_compiler(const string &requested) {
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
            fs::path path = executable(candidate), root = path.parent_path().parent_path();
            if (!visited.insert(path).second)
                continue;
            string name = root.filename();
            if (name.compare(0, 12, "rust-distcc-") == 0 && name.size() == 76 &&
                read(root / ".distcc-complete") != name.substr(12) + "\n")
                continue;
            if (cached_identity(path) == requested)
                return path;
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
    emits(options(flags));
    auto compiler = select_compiler(job->rust.compiler);
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
extern "C" int dcc_rust_catalog_attach(int fd) {
    if (catalog)
        return 0;
    auto p = mmap(nullptr, sizeof(Catalog), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return -1;
    catalog = static_cast<Catalog *>(p);
    return 0;
}
extern "C" int dcc_rust_catalog_fd(void) {
    static int shared = -1;
    if (shared >= 0)
        return shared;
    int fd = dcc_job_memfd("distccd-rust-catalog");
    if (fd < 0)
        return -1;
    if (ftruncate(fd, sizeof(Catalog)) || dcc_rust_catalog_attach(fd)) {
        close(fd);
        return -1;
    }
    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if (!rc) {
        rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (!rc)
            rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (!rc)
            rc = pthread_mutex_init(&catalog->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    munmap(catalog, sizeof(Catalog));
    catalog = nullptr;
    if (rc) {
        close(fd);
        return -1;
    }
    return shared = fd;
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
