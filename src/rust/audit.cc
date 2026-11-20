#include "rust/runtime.hh"
/* Path journal for the isolated dependency pass. Loader bookkeeping and
 * journal IO use raw filesystem syscalls so they cannot reenter these hooks. */
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {
std::atomic<int> journal{-1};
thread_local bool observing;
// libsandbox and libc can call other interposed functions while servicing
// the outer call. Only the operation requested by the compiler is an input.
struct Observation {
    bool previous = observing;
    Observation() { observing = true; }
    ~Observation() { observing = previous; }
    void path_event(const char *kind, int dir, const char *path, bool succeeded) const;
    void operation(const char *name) const;
};
bool below(const char *name, const char *root) {
    size_t n = strlen(root);
    return n && !strncmp(name, root, n) && (name[n] == '/' || !name[n]);
}
void record(int fd, const char *kind, const char *path, const char *real) {
    iovec vec[] = {{const_cast<char *>(kind), strlen(kind) + 1},
                   {const_cast<char *>(path), strlen(path) + 1},
                   {const_cast<char *>(real), strlen(real) + 1}};
    ssize_t total = vec[0].iov_len + vec[1].iov_len + vec[2].iov_len, n;
    do {
        n = syscall(SYS_writev, fd, vec, 3);
    } while (n < 0 && errno == EINTR);
    if (n != total)
        _exit(100);
}
void Observation::path_event(const char *kind, int dir, const char *path, bool succeeded) const {
    int fd = journal.load(std::memory_order_relaxed);
    if (previous || fd < 0)
        return;
    int saved = errno;
    if (!succeeded && (*kind == 'R' || *kind == 'S')) {
        kind = saved == ENOENT ? "N" : saved == ENOTDIR ? "T" : "X";
    }
    char base[PATH_MAX + 1], joined[PATH_MAX + 1];
    if (!path)
        path = "";
    if (*path != '/') {
        if (dir == AT_FDCWD) {
            if (syscall(SYS_getcwd, base, sizeof(base)) < 0)
                base[0] = 0;
        } else {
            char link[64];
            snprintf(link, sizeof(link), "/proc/self/fd/%d", dir);
            auto n = syscall(SYS_readlinkat, AT_FDCWD, link, base, PATH_MAX);
            base[n < 0 ? 0 : n] = 0;
        }
        if (!*base ||
            snprintf(joined, sizeof(joined), "%s%s%s", base, *path ? "/" : "", path) >= int(sizeof(joined)))
            kind = "X";
        else
            path = joined;
    }
    record(fd, kind, path, "");
    errno = saved;
}
void Observation::operation(const char *name) const {
    auto fd = journal.load(std::memory_order_relaxed);
    if (!previous && fd >= 0) {
        int saved = errno;
        record(fd, "X", name, "");
        errno = saved;
    }
}
} // namespace
extern "C" void dcc_rust_watch_library(int fd, const char *name) {
    if (!name || !*name)
        return;
    const char *root = getenv("DISTCC_RUST_SYSROOT");
    if (root && below(name, root))
        return;
    for (auto p : rust::runtime)
        if (below(name, p))
            return;
    journal.store(fd, std::memory_order_relaxed);
}
extern "C" void dcc_rust_library_record(int fd, const char *kind, const char *requested,
                                        const char *resolved) {
    record(fd, kind, requested, resolved);
}
#define NEXT(name) Observation observe; static auto fn = reinterpret_cast<decltype(&name)>(dlsym(RTLD_NEXT, #name))
#define FILE_OPEN(name, params, dir, args)                                                                   \
    extern "C" int name params {                                                                             \
        Observation observe;                                                                                 \
        using Open = int(*) params;                                                                          \
        static Open fn = reinterpret_cast<Open>(dlsym(RTLD_NEXT, #name));                                    \
        mode_t mode = 0;                                                                                     \
        if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {                                         \
            va_list ap;                                                                                      \
            va_start(ap, flags);                                                                             \
            mode = va_arg(ap, int);                                                                          \
            va_end(ap);                                                                                      \
        }                                                                                                    \
        int fd = fn args;                                                                                    \
        observe.path_event((flags & (O_ACCMODE | O_CREAT | O_TRUNC)) ? "W" : "R", dir, path, fd >= 0);       \
        return fd;                                                                                           \
    }
FILE_OPEN(open, (const char *path, int flags, ...), AT_FDCWD, (path, flags, mode))
FILE_OPEN(open64, (const char *path, int flags, ...), AT_FDCWD, (path, flags, mode))
FILE_OPEN(openat, (int dir, const char *path, int flags, ...), dir, (dir, path, flags, mode))
FILE_OPEN(openat64, (int dir, const char *path, int flags, ...), dir, (dir, path, flags, mode))
#define STREAM_OPEN(name)                                                                                    \
    extern "C" FILE *name(const char *path, const char *mode) {                                              \
        NEXT(name);                                                                                          \
        auto *f = fn(path, mode);                                                                            \
        observe.path_event(strpbrk(mode, "wa+") ? "W" : "R", AT_FDCWD, path, f);                             \
        return f;                                                                                            \
    }
STREAM_OPEN(fopen)
STREAM_OPEN(fopen64)
#define PATH_STAT(name, params, dir, args)                                                                   \
    extern "C" int name params noexcept {                                                                    \
        NEXT(name);                                                                                          \
        int rc = fn args;                                                                                    \
        observe.path_event("R", dir, path, !rc);                                                             \
        return rc;                                                                                           \
    }
PATH_STAT(stat, (const char *path, struct stat *st), AT_FDCWD, (path, st))
PATH_STAT(lstat, (const char *path, struct stat *st), AT_FDCWD, (path, st))
PATH_STAT(fstatat, (int dir, const char *path, struct stat *st, int flags), dir, (dir, path, st, flags))
PATH_STAT(stat64, (const char *path, struct stat64 *st), AT_FDCWD, (path, st))
PATH_STAT(lstat64, (const char *path, struct stat64 *st), AT_FDCWD, (path, st))
PATH_STAT(fstatat64, (int dir, const char *path, struct stat64 *st, int flags), dir, (dir, path, st, flags))
PATH_STAT(statx, (int dir, const char *path, int flags, unsigned mask, struct statx *st), dir,
          (dir, path, flags, mask, st))
extern "C" int newfstatat(int dir, const char *path, struct stat *st, int flags) {
    return fstatat(dir, path, st, flags);
}
PATH_STAT(access, (const char *path, int mode), AT_FDCWD, (path, mode))
PATH_STAT(faccessat, (int dir, const char *path, int mode, int flags), dir, (dir, path, mode, flags))
extern "C" ssize_t readlink(const char *path, char *buf, size_t n) noexcept {
    NEXT(readlink);
    auto size = fn(path, buf, n);
    observe.path_event("S", AT_FDCWD, path, size >= 0);
    return size;
}
extern "C" ssize_t readlinkat(int dir, const char *path, char *buf, size_t n) noexcept {
    NEXT(readlinkat);
    auto size = fn(dir, path, buf, n);
    observe.path_event("S", dir, path, size >= 0);
    return size;
}
extern "C" DIR *opendir(const char *path) {
    NEXT(opendir);
    auto *d = fn(path);
    observe.path_event("directory enumeration", AT_FDCWD, path, d);
    return d;
}
// Read-only mounts enforce the write policy even when a macro ignores errno.
#define MUTATION(name, params, args)                                                                         \
    extern "C" int name params noexcept {                                                                    \
        NEXT(name);                                                                                          \
        observe.operation(#name);                                                                            \
        return fn args;                                                                                      \
    }
MUTATION(unlink, (const char *path), (path))
MUTATION(unlinkat, (int dir, const char *path, int flags), (dir, path, flags))
MUTATION(rmdir, (const char *path), (path))
MUTATION(mkdir, (const char *path, mode_t mode), (path, mode))
MUTATION(mkdirat, (int dir, const char *path, mode_t mode), (dir, path, mode))
MUTATION(rename, (const char *from, const char *to), (from, to))
MUTATION(renameat, (int a, const char *from, int b, const char *to), (a, from, b, to))
MUTATION(renameat2, (int a, const char *from, int b, const char *to, unsigned flags), (a, from, b, to, flags))
MUTATION(symlink, (const char *to, const char *path), (to, path))
MUTATION(symlinkat, (const char *to, int dir, const char *path), (to, dir, path))
MUTATION(link, (const char *src, const char *dest), (src, dest))
MUTATION(linkat, (int a, const char *src, int b, const char *dest, int flags), (a, src, b, dest, flags))
MUTATION(chmod, (const char *path, mode_t mode), (path, mode))
MUTATION(truncate, (const char *path, off_t length), (path, length))
MUTATION(execve, (const char *path, char *const argv[], char *const envp[]), (path, argv, envp))
#define SPAWN(name)                                                                                          \
    extern "C" int name(pid_t *pid, const char *path, const posix_spawn_file_actions_t *files,               \
                        const posix_spawnattr_t *attr, char *const args[], char *const vars[]) {             \
        NEXT(name);                                                                                          \
        observe.operation(#name);                                                                            \
        return fn(pid, path, files, attr, args, vars);                                                       \
    }
SPAWN(posix_spawn)
SPAWN(posix_spawnp)
extern "C" int socket(int domain, int type, int protocol) noexcept {
    NEXT(socket);
    observe.operation("socket");
    return fn(domain, type, protocol);
}
extern "C" int connect(int fd, const sockaddr *addr, socklen_t n) {
    NEXT(connect);
    observe.operation("connect");
    return fn(fd, addr, n);
}
