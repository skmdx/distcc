/* Isolated input-interposition canary, not a production filesystem provider.
 * One prepared native directory owns the snapshot. Returned descriptors and
 * streams are real OS objects: read/fstat/seek/mmap/close are not emulated.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root, *view;
static size_t root_length;
static int log_fd = -1;
static int (*native_open)(const char *, int, ...);
static int (*native_stat)(const char *, struct stat *);
static int (*native_lstat)(const char *, struct stat *);
static int (*native_fstatat)(int, const char *, struct stat *, int);
static FILE *(*native_fopen)(const char *, const char *);
static DIR *(*native_opendir)(const char *);
static ssize_t (*native_readlink)(const char *, char *, size_t);

__attribute__((constructor)) static void initialize(void)
{
#define BIND(name) do { native_##name = dlsym(RTLD_NEXT, #name); if (!native_##name) _exit(120); } while (0)
    BIND(open); BIND(stat); BIND(lstat); BIND(fstatat); BIND(fopen);
    BIND(opendir); BIND(readlink);
#undef BIND
    root = getenv("DCC_CANARY_ROOT"); view = getenv("DCC_CANARY_VIEW");
    const char *log = getenv("DCC_CANARY_LOG");
    if (!root || !view || !log) _exit(121);
    root_length = strlen(root);
    log_fd = native_open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) _exit(122);
    char image[PATH_MAX];
    ssize_t length = native_readlink("/proc/self/exe", image, sizeof(image)-1);
    if (length < 0 || length == (ssize_t)sizeof(image)-1) _exit(123);
    image[length] = 0;
    dprintf(log_fd, "%ld image 0 0 %s\n", (long)getpid(), image);
}

/* Preserve components for native symlink/.. resolution. This fixture mapping
 * is not export authorization; production must obtain a permitted snapshot
 * from the existing job owner rather than treat a prefix as authority. */
static const char *input(const char *path, char out[PATH_MAX])
{
    const char *original = path;
    out[0] = 0;
    char absolute[PATH_MAX];
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        if (snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path) >= PATH_MAX) {
            errno = ENAMETOOLONG; return NULL;
        }
        path = absolute;
    }
    if (strncmp(path, root, root_length) || (path[root_length] && path[root_length] != '/'))
        return original;
    if (snprintf(out, PATH_MAX, "%s%s", view, path + root_length) >= PATH_MAX) {
        errno = ENAMETOOLONG; return NULL;
    }
    return out;
}

static void record(const char *operation, const char *path, int result, int error)
{
    dprintf(log_fd, "%ld %s %d %d %s\n", (long)getpid(), operation, result, error, path);
    errno = error;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, unsigned); va_end(args);
    }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE)
        return native_open(path, flags, mode);
    char buffer[PATH_MAX]; const char *name = input(path, buffer);
    if (!name) return -1;
    int result = native_open(name, flags, mode), error = errno;
    if (buffer[0]) record("open", path, result, result < 0 ? error : 0);
    errno = error; return result;
}

#define PATH_STAT(name) \
int name(const char *path, struct stat *st) { \
    char buffer[PATH_MAX]; const char *p = input(path, buffer); \
    if (!p) return -1; \
    int result = native_##name(p, st), error = errno; \
    if (buffer[0]) record(#name, path, result, result < 0 ? error : 0); \
    errno = error; return result; \
}
PATH_STAT(stat)
PATH_STAT(lstat)

int fstatat(int fd, const char *path, struct stat *st, int flags)
{
    if (fd != AT_FDCWD && path[0] != '/') return native_fstatat(fd, path, st, flags);
    char buffer[PATH_MAX]; const char *p = input(path, buffer);
    return p ? native_fstatat(fd, p, st, flags) : -1;
}

FILE *fopen(const char *path, const char *mode)
{
    if (mode[0] != 'r' || strchr(mode, '+')) return native_fopen(path, mode);
    char buffer[PATH_MAX]; const char *p = input(path, buffer);
    if (!p) return NULL;
    FILE *result = native_fopen(p, mode); int error = errno;
    if (buffer[0]) record("fopen", path, result ? 0 : -1, result ? 0 : error);
    errno = error; return result;
}

DIR *opendir(const char *path)
{
    char buffer[PATH_MAX]; const char *p = input(path, buffer);
    if (!p) return NULL;
    DIR *result = native_opendir(p); int error = errno;
    if (buffer[0]) record("opendir", path, result ? 0 : -1, result ? 0 : error);
    errno = error; return result;
}

ssize_t readlink(const char *path, char *value, size_t size)
{
    char buffer[PATH_MAX]; const char *p = input(path, buffer);
    return p ? native_readlink(p, value, size) : -1;
}
