/* Observe public libc calls made by the live GCC planner. Actual I/O and
 * authorization remain in libc/libsandbox. Never emulate a file operation.
 * This journal is only active in an isolated invocation child, not its parent. */
#define _GNU_SOURCE 1
#include "plan_memo.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <link.h>
#include <limits.h>
#include <unistd.h>
#include <sys/uio.h>
#include <map>
#include <string>

namespace {
int journal = -1, completion;
bool observing, invalid;
struct observation { struct stat metadata; int error; std::string target; };
std::map<std::string,observation> observed;
template<class Function, class... Args> auto forward(Function function, Args... args) {
    // A public call may enter libsandbox and issue its own implementation
    // probes. Validate the original operation through that same sandbox, not
    // its private /proc descriptor bookkeeping as compiler dependencies.
    bool previous=observing; observing=true;
    auto result=function(args...);
    observing=previous;
    return result;
}
template<class T> T next(const char *name) {
    auto function = reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
    if (!function) abort();
    return function;
}
void reject(unsigned char reason = 1) {
    invalid = true;
    if (journal >= 0) { if (pwrite(journal, &reason, 1, 2) != 1) journal = -1; }
}
void record(unsigned operation, const char *path, int flags, int error,
            const struct stat *metadata = nullptr, const char *target = nullptr) {
    if (journal < 0 || observing || invalid) return;
    int saved = errno;
    observing = true;
    // Record the spelling actually resolved by the kernel, not a lexical
    // normalization which would change symlink/.. and search permissions.
    char directory[PATH_MAX];
    std::string name;
    if (!path || !*path) reject();
    else if (*path == '/') name = path;
    else if (!getcwd(directory, sizeof(directory))) reject();
    else name = std::string(directory) + "/" + path;
    if (name.compare(0, 6, "/proc/") == 0 || name.compare(0, 5, "/sys/") == 0)
        reject(4); // process-relative and synthetic contents are not filesystem versions
    std::string identity = std::to_string(operation)+":"+std::to_string(flags)+":"+name;
    struct stat empty{};
    observation current{metadata ? *metadata : empty,error,target ? target : ""};
    auto prior=observed.find(identity);
    if (prior!=observed.end()) {
        if (prior->second.error!=current.error || prior->second.target!=current.target ||
            !dcc_plan_stat_equal(&prior->second.metadata,&current.metadata)) reject();
    } else if (!invalid) {
        observed.emplace(identity,current);
        dcc_plan_dependency item{};
        item.operation = operation; item.flags = flags; item.error = error;
        item.length = name.size() + 1;
        item.target_length = target ? strlen(target) + 1 : 0;
        if (metadata) item.metadata = *metadata;
        off_t offset = lseek(journal, 0, SEEK_CUR);
        if (offset < 4 || offset + sizeof(item) + item.length + item.target_length > DCC_PLAN_JOURNAL_LIMIT) reject(7);
        else {
            iovec parts[] = {{&item, sizeof(item)}, {const_cast<char *>(name.c_str()), item.length},
                            {const_cast<char *>(target), item.target_length}};
            if (writev(journal, parts, 3) != ssize_t(sizeof(item)+item.length+item.target_length)) reject();
        }
    }
    observing = false;
    errno = saved;
}
void opened(const char *path, int flags, int fd, int error) {
    if (journal < 0 || observing) return;
    if (strcmp(path,"/dev/null") && ((flags & O_ACCMODE) != O_RDONLY || flags & (O_CREAT | O_TRUNC) ||
        (flags & O_TMPFILE) == O_TMPFILE)) {
        reject(5); return;
    }
    struct stat st{};
    if (fd >= 0 && fstat(fd, &st)) { reject(); return; }
    record(DCC_PLAN_OPEN, path, flags, error, &st);
}
void stream(const char *path, const char *mode, FILE *file, int error) {
    if (journal < 0 || observing) return;
    // The planner explicitly directs its discarded output to /dev/null.
    // Preserve that real fopen authorization too; arbitrary writes are not memoized.
    if (!path || ((*mode!='r' || strchr(mode,'+')) && strcmp(path,"/dev/null"))) {
        reject(5); return;
    }
    // External specs can make otherwise normalized operands influence the
    // invocation. GCC has already identified/opened the file: no specs parser.
    const char *base=strrchr(path,'/'); base=base ? base+1 : path;
    if (file && !strcmp(base,"specs")) { reject(6); return; }
    struct stat st{};
    if (file && fstat(fileno(file), &st)) { reject(); return; }
    record(DCC_PLAN_STREAM,path,0,error,&st,mode);
}
int images(dl_phdr_info *image, size_t, void *) {
    const char *path = image->dlpi_name;
    if (!path || !*path || !strcmp(path, "linux-vdso.so.1")) return 0;
    struct stat st{};
    static auto call = next<int (*)(const char *, struct stat *)>("stat");
    int result = forward(call,path, &st), error = result ? errno : 0;
    record(DCC_PLAN_STAT, path, 0, error, &st);
    return 0;
}
}

extern "C" void dcc_plan_record_begin(int fd, int driver) {
    journal = fd; completion = driver ? 0 : 1;
    observed.clear(); invalid = observing = false;
    dl_iterate_phdr(images, nullptr);
    char executable[PATH_MAX];
    static auto link=next<decltype(&readlink)>("readlink");
    ssize_t size=forward(link,"/proc/self/exe",executable,sizeof(executable)-1);
    if (size<=0 || size==sizeof(executable)-1) reject();
    else {
        executable[size]=0;
        struct stat st{};
        static auto info=next<decltype(&stat)>("stat");
        int result=forward(info,executable,&st);
        record(DCC_PLAN_STAT,executable,0,result ? errno : 0,&st);
    }
}
extern "C" void dcc_plan_record_finish(int status) {
    if (journal < 0) return;
    dl_iterate_phdr(images, nullptr);
    if (!status && !invalid) { unsigned char done = 1; if (pwrite(journal, &done, 1, completion) != 1) invalid = true; }
    journal = -1;
}

#define EXPORT extern "C" __attribute__((visibility("default")))
EXPORT int stat(const char *path, struct stat *st) {
    static auto call = next<decltype(&stat)>("stat");
    int r = forward(call,path, st), e = r ? errno : 0;
    record(DCC_PLAN_STAT, path, 0, e, r ? nullptr : st); errno = e; return r;
}
EXPORT int lstat(const char *path, struct stat *st) {
    static auto call = next<decltype(&lstat)>("lstat");
    int r = forward(call,path, st), e = r ? errno : 0;
    record(DCC_PLAN_LSTAT, path, 0, e, r ? nullptr : st); errno = e; return r;
}
EXPORT int access(const char *path, int mode) {
    static auto call = next<decltype(&access)>("access");
    int r = forward(call,path, mode), e = r ? errno : 0;
    record(DCC_PLAN_ACCESS, path, mode, e); errno = e; return r;
}
EXPORT int open(const char *path, int flags, ...) {
    static auto call = next<decltype(&open)>("open");
    mode_t mode = 0;
    if (flags & O_CREAT || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    int fd = forward(call,path, flags, mode), e = fd < 0 ? errno : 0;
    opened(path, flags, fd, e); errno = e; return fd;
}
EXPORT FILE *fopen(const char *path, const char *mode) {
    static auto call = next<decltype(&fopen)>("fopen");
    FILE *file = forward(call,path, mode); int e = file ? 0 : errno;
    stream(path,mode,file,e);
    errno = e; return file;
}
EXPORT DIR *opendir(const char *path) {
    static auto call = next<decltype(&opendir)>("opendir");
    DIR *dir = forward(call,path); int e = dir ? 0 : errno;
    struct stat st{};
    if (dir && fstat(dirfd(dir), &st)) reject();
    else record(DCC_PLAN_DIRECTORY, path, 0, e, &st);
    errno = e; return dir;
}
EXPORT ssize_t readlink(const char *path, char *buffer, size_t size) {
    static auto call = next<decltype(&readlink)>("readlink");
    ssize_t r = forward(call,path, buffer, size); int e = r < 0 ? errno : 0;
    struct stat st{};
    static auto info = next<decltype(&lstat)>("lstat");
    std::string target;
    if (r >= 0 && (size_t(r) == size || forward(info,path, &st))) reject();
    else {
        if (r >= 0) target.assign(buffer, r);
        record(DCC_PLAN_LINK, path, 0, e, &st, r >= 0 ? target.c_str() : nullptr);
    }
    errno = e; return r;
}
EXPORT char *realpath(const char *path, char *resolved) {
    static auto call = next<decltype(&realpath)>("realpath");
    char *result = forward(call,path, resolved); int e = result ? 0 : errno;
    struct stat st{};
    static auto info = next<decltype(&stat)>("stat");
    if (result && forward(info,result, &st)) reject();
    else record(DCC_PLAN_REALPATH, path, 0, e, &st, result);
    errno = e; return result;
}
EXPORT char *__realpath_chk(const char *path, char *resolved, size_t size) {
    using checked = char *(*)(const char *, char *, size_t);
    static auto call = next<checked>("__realpath_chk");
    char *result = forward(call,path, resolved, size); int e = result ? 0 : errno;
    struct stat st{};
    static auto info = next<decltype(&stat)>("stat");
    if (result && forward(info,result, &st)) reject();
    else record(DCC_PLAN_REALPATH, path, 0, e, &st, result);
    errno = e; return result;
}
// The libc implementations of these wrappers use hidden open/stat entrypoints.
// Observe their public boundary instead of hoping nested calls are interposable.
EXPORT int open64(const char *path, int flags, ...) {
    static auto call = next<decltype(&open64)>("open64");
    mode_t mode = 0;
    if (flags & O_CREAT || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    int fd = forward(call,path, flags, mode), e = fd < 0 ? errno : 0;
    opened(path, flags, fd, e); errno = e; return fd;
}
EXPORT FILE *fopen64(const char *path, const char *mode) {
    static auto call = next<decltype(&fopen64)>("fopen64");
    FILE *file = forward(call,path, mode); int e = file ? 0 : errno;
    stream(path,mode,file,e);
    errno = e; return file;
}
EXPORT FILE *freopen(const char *path, const char *mode, FILE *file) {
    static auto call = next<decltype(&freopen)>("freopen");
    FILE *result = forward(call,path, mode, file); int e = result ? 0 : errno;
    stream(path,mode,result,e);
    errno = e; return result;
}
namespace {
std::string at_path(int fd, const char *path) {
    if (fd == AT_FDCWD || *path == '/') return path;
    char link[64], target[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    static auto call = next<decltype(&readlink)>("readlink");
    ssize_t n = forward(call,link, target, sizeof(target)-1);
    if (n < 0 || n == sizeof(target)-1) { reject(); return {}; }
    target[n] = 0;
    return std::string(target) + (*path ? "/" : "") + path;
}
}
EXPORT int openat(int fd, const char *path, int flags, ...) {
    static auto call = next<decltype(&openat)>("openat");
    mode_t mode = 0;
    if (flags & O_CREAT || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    int file = forward(call,fd, path, flags, mode), e = file < 0 ? errno : 0;
    if (journal >= 0 && !observing) opened(at_path(fd,path).c_str(), flags, file, e);
    errno = e; return file;
}
EXPORT int fstatat(int fd, const char *path, struct stat *st, int flags) {
    static auto call = next<decltype(&fstatat)>("fstatat");
    int r = forward(call,fd, path, st, flags), e = r ? errno : 0;
    if (journal >= 0 && !observing) {
        if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) reject();
        else record(flags & AT_SYMLINK_NOFOLLOW ? DCC_PLAN_LSTAT : DCC_PLAN_STAT,
                    at_path(fd,path).c_str(), 0, e, r ? nullptr : st);
    }
    errno = e; return r;
}
EXPORT int faccessat(int fd, const char *path, int mode, int flags) {
    static auto call = next<decltype(&faccessat)>("faccessat");
    int r = forward(call,fd, path, mode, flags), e = r ? errno : 0;
    if (journal >= 0 && !observing) {
        if (flags) reject();
        else record(DCC_PLAN_ACCESS, at_path(fd,path).c_str(), mode, e);
    }
    errno = e; return r;
}
EXPORT void _exit(int status) {
    dcc_plan_record_finish(status);
    static auto call = next<decltype(&_exit)>("_exit");
    call(status); __builtin_unreachable();
}
