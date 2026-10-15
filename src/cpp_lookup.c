/* SPDX-License-Identifier: GPL-2.0-or-later
 * Request-local negative header lookup cache for GCC's preprocessor.
 *
 * Only absent names are answered here; existing files still use libc open.
 * Directory identities/timestamps are checked before accepting the output.
 * A changed namespace discards stdout and re-execs the same frontend without
 * caching. No snapshot survives a preprocessing request.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUCKETS 16384
#define BUDGET (32 * 1024 * 1024)
struct directory {
    char *path;
    struct stat stamp;
    char **names;
    size_t count;
    int missing;
    struct directory *next;
};
static struct directory *directories[BUCKETS];
static size_t allocated;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int enabled;
static _Atomic unsigned failed_lookups;
static int saved_output = -1;
static char *working_directory, *command;
static char **arguments;
static int (*next_open)(const char *, int, ...);
static int (*next_chdir)(const char *);
static int (*next_fchdir)(int);
static pthread_once_t bindings = PTHREAD_ONCE_INIT;

static void bind_libc(void)
{
    next_open = dlsym(RTLD_NEXT, "open");
    next_chdir = dlsym(RTLD_NEXT, "chdir");
    next_fchdir = dlsym(RTLD_NEXT, "fchdir");
}

static int unchanged(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void discard(struct directory *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->count; ++i) free(d->names[i]);
    free(d->names);
    free(d->path);
    free(d);
}

/* lock is held; recursion walks toward the root, never follows a symlink. */
static int absent(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || !slash[1] || slash == path) return 0;
    *slash = 0;
    char *name = slash + 1;
    unsigned hash = 5381;
    for (const unsigned char *p = (unsigned char *)path; *p; ++p) hash = hash * 33 + *p;
    hash %= BUCKETS;
    struct directory *d = directories[hash];
    while (d && strcmp(d->path, path)) d = d->next;
    if (!d && allocated < BUDGET) {
        /* Build/output trees change during parallel builds. Leave those
         * lookups to libc, rather than repeatedly discarding useful output. */
        size_t cwd_len = strlen(working_directory);
        if (!strncmp(path, working_directory, cwd_len) &&
            (!path[cwd_len] || path[cwd_len] == '/')) goto done;
        int missing = absent(path);
        d = calloc(1, sizeof(*d));
        if (!d) goto done;
        d->missing = missing;
        size_t bytes = sizeof(*d) + strlen(path) + 1;
        if (!missing) {
            DIR *stream = opendir(path);
            struct stat after;
            if (!stream) { discard(d); d = NULL; goto done; }
            int valid = !fstat(dirfd(stream), &d->stamp) && !faccessat(dirfd(stream), ".", X_OK, AT_EACCESS);
            size_t capacity = 0;
            while (valid) {
                errno = 0;
                struct dirent *entry = readdir(stream);
                if (!entry) { valid = !errno; break; }
                if (bytes + allocated + strlen(entry->d_name) + 1 + sizeof(char *) > BUDGET) { valid = 0; break; }
                if (d->count == capacity) {
                    size_t size = capacity ? capacity * 2 : 32;
                    char **names = realloc(d->names, size * sizeof(*names));
                    if (!names) { valid = 0; break; }
                    d->names = names;
                    capacity = size;
                }
                char *copy = strdup(entry->d_name);
                if (!copy) { valid = 0; break; }
                bytes += strlen(copy) + 1 + sizeof(char *);
                d->names[d->count++] = copy;
            }
            valid = valid && !fstat(dirfd(stream), &after) && unchanged(&d->stamp, &after);
            closedir(stream);
            if (!valid) { discard(d); d = NULL; goto done; }
            qsort(d->names, d->count, sizeof(*d->names), compare_names);
        }
        d->path = strdup(path);
        if (!d->path || allocated + bytes > BUDGET) { discard(d); d = NULL; goto done; }
        allocated += bytes;
        d->next = directories[hash];
        directories[hash] = d;
    }
done:
    *slash = '/';
    return d && (d->missing || !bsearch(&name, d->names, d->count, sizeof(*d->names), compare_names));
}

static int cached_open(const char *path, int flags, mode_t mode)
{
    pthread_once(&bindings, bind_libc);
    int saved_errno = errno;
    int eligible = enabled && (flags & (O_ACCMODE | O_CREAT | O_NOFOLLOW | O_DIRECTORY | O_PATH | O_TMPFILE)) == O_RDONLY;
    /* Directory enumeration costs more than a handful of ordinary probes.
     * Activate only after this request demonstrates a large search space. */
    if (eligible && failed_lookups >= 256) {
        char *absolute = NULL;
        if (path[0] == '/') absolute = strdup(path);
        else if (asprintf(&absolute, "%s/%s", working_directory, path) < 0) absolute = NULL;
        if (absolute) {
            pthread_mutex_lock(&lock);
            int missing = enabled && absent(absolute);
            pthread_mutex_unlock(&lock);
            free(absolute);
            if (missing) { errno = ENOENT; return -1; }
        }
    }
    errno = saved_errno;
    int result = next_open(path, flags, mode);
    if (eligible && result < 0 && errno == ENOENT && failed_lookups < 256) failed_lookups++;
    return result;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    return cached_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    return cached_open(path, flags | O_LARGEFILE, mode);
}

int __open_2(const char *path, int flags)
{
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        int (*fortified)(const char *, int) = dlsym(RTLD_NEXT, "__open_2");
        return fortified(path, flags);
    }
    return cached_open(path, flags, 0);
}

int chdir(const char *path)
{
    pthread_once(&bindings, bind_libc);
    pthread_mutex_lock(&lock);
    enabled = 0;
    pthread_mutex_unlock(&lock);
    return next_chdir(path);
}

int fchdir(int fd)
{
    pthread_once(&bindings, bind_libc);
    pthread_mutex_lock(&lock);
    enabled = 0;
    pthread_mutex_unlock(&lock);
    return next_fchdir(fd);
}

__attribute__((constructor)) static void initialize(void)
{
    pthread_once(&bindings, bind_libc);
    const char *setting = getenv("DISTCC_CPP_LOOKUP");
    if (!setting || strcmp(setting, "1")) return;
    /* The driver and unrelated children remain untouched. Plugins can change
     * the namespace or run threads; they are deliberately outside this path. */
    char exe[4096];
    ssize_t size = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (size < 0) return;
    exe[size] = 0;
    const char *name = strrchr(exe, '/');
    if (!name || (strcmp(name + 1, "cc1") && strcmp(name + 1, "cc1plus"))) return;
    struct stat output;
    if (fstat(STDOUT_FILENO, &output) || !S_ISREG(output.st_mode) || lseek(STDOUT_FILENO, 0, SEEK_CUR) != 0) return;
    FILE *stream = fopen("/proc/self/cmdline", "rb");
    if (!stream) return;
    size_t capacity = 0;
    ssize_t used = getdelim(&command, &capacity, EOF, stream);
    fclose(stream);
    if (used <= 0 || used > 2 * 1024 * 1024 || command[used - 1]) return;
    size_t count = 0;
    for (ssize_t i = 0; i < used; ++i) if (!command[i]) count++;
    arguments = calloc(count + 1, sizeof(*arguments));
    if (!arguments) return;
    int preprocess = 0;
    char *p = command;
    for (size_t i = 0; i < count; ++i) {
        arguments[i] = p;
        if (!strcmp(p, "-E")) preprocess = 1;
        if (!strncmp(p, "-fplugin", 8)) return;
        p += strlen(p) + 1;
    }
    working_directory = getcwd(NULL, 0);
    if (preprocess && working_directory) saved_output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    enabled = saved_output >= 0;
}

__attribute__((destructor)) static void validate(void)
{
    pthread_mutex_lock(&lock);
    enabled = 0;
    int changed = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        for (struct directory *d = directories[i]; d; d = d->next) {
            struct stat now;
            if (!d->missing && (stat(d->path, &now) || !unchanged(&d->stamp, &now))) changed = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    if (!changed) return;
    /* The caller has not consumed stdout yet. The original cc1 arguments
     * also replace its depfile on retry. Keep the same PID and exit status. */
    enabled = 0;
    if (setenv("DISTCC_CPP_LOOKUP", "0", 1) || next_chdir(working_directory) ||
        fflush(NULL) || ftruncate(saved_output, 0) || lseek(saved_output, 0, SEEK_SET) != 0 ||
        dup2(saved_output, STDOUT_FILENO) < 0) _exit(75);
    execv("/proc/self/exe", arguments);
    _exit(75);
}
