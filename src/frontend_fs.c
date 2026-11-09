/* Serve the compiler's actual lookups without a second preprocessing pass.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "distcc.h"
#include "tcp_job.h"
#include "frontend_fs.h"

/* Authorization normalizes the spelling, but the syscall keeps the original
 * path: collapsing symlink/.. would change the compiler's lookup semantics. */
static int normalized(const char *path, char result[PATH_MAX])
{
    size_t used = 0;
    while (*path) {
        if (*path == '/') { ++path; continue; }
        const char *part = path;
        while (*path && *path != '/') ++path;
        size_t size = path - part;
        if (size == 1 && part[0] == '.') continue;
        if (size == 2 && part[0] == '.' && part[1] == '.') {
            while (used && result[--used] != '/') {}
        } else {
            if (size >= PATH_MAX - used - 1) { errno = ENAMETOOLONG; return -1; }
            result[used++] = '/';
            memcpy(result + used, part, size); used += size;
        }
    }
    if (!used) result[used++] = '/';
    result[used] = 0;
    return 0;
}
static int within(const char *path, const char *root)
{
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || path[n] == '/' || !strcmp(root, "/"));
}
enum { PATH_METADATA = 1, PATH_CONTENTS = 2 };
int dcc_frontend_fs_add_root(struct dcc_job *job, const char *path)
{
    char absolute[PATH_MAX], root[PATH_MAX];
    if (*path != '/') {
        int n = snprintf(absolute, sizeof(absolute), "%s/%s", job->cwd, path);
        if (n < 0 || n >= (int)sizeof(absolute)) { errno = ENAMETOOLONG; return -1; }
        path = absolute;
    }
    if (normalized(path, root)) return -1;
    size_t count = job->fs_roots ? dcc_argv_len(job->fs_roots) : 0;
    for (size_t i = 0; i < count; ++i)
        if (within(root, job->fs_roots[i])) return 0;
    char **roots = realloc(job->fs_roots, (count+2)*sizeof(*roots));
    if (!roots) return -1;
    job->fs_roots = roots;
    roots[count] = strdup(root); roots[count+1] = NULL;
    return roots[count] ? 0 : -1;
}
int dcc_frontend_fs_add_roots(struct dcc_job *job, const char *paths)
{
    if (!paths || !*paths) return 0;
    char *copy = strdup(paths), *save = NULL;
    if (!copy) return -1;
    for (char *path = strtok_r(copy, ":", &save); path; path = strtok_r(NULL, ":", &save)) {
        if (dcc_frontend_fs_add_root(job, path)) goto failed;
    }
    free(copy);
    return 0;
failed:
    free(copy);
    return -1;
}
static int allowed(const struct dcc_job *job, const char *path)
{
    char name[PATH_MAX];
    if (normalized(path, name)) return 0;
    int result = 0;
    for (char **p = job->fs_roots; p && *p; ++p) {
        if (within(name, *p)) { result = PATH_METADATA | PATH_CONTENTS; break; }
        if (within(*p, name)) result = PATH_METADATA;
    }
    return result;
}
/* Metadata stays in one owned allocation. File contents are read only when
 * the transport can send its next chunk, without a temporary-file round trip. */
static int append(struct dcc_frontend_reply *reply, const void *bytes, size_t length)
{
    if (length > DCC_TCP_BYTES - reply->size) { errno = EFBIG; return -1; }
    size_t needed = reply->size + length;
    if (needed > reply->capacity) {
        size_t capacity = reply->capacity ? reply->capacity * 2 : 64;
        if (capacity < needed) capacity = needed;
        if (capacity > DCC_TCP_BYTES) capacity = DCC_TCP_BYTES;
        void *data = realloc(reply->data, capacity);
        if (!data) return -1;
        reply->data = data; reply->capacity = capacity;
    }
    memcpy(reply->data + reply->size, bytes, length);
    reply->size = needed;
    return 0;
}
static void encode_metadata(unsigned char data[DCC_FS_METADATA], const struct stat *st)
{
    dcc_tcp_put64(data, st->st_dev); dcc_tcp_put64(data + 8, st->st_ino);
    dcc_tcp_put64(data + 16, (uint64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec);
    dcc_tcp_put64(data + 24, st->st_size); dcc_tcp_put32(data + 32, st->st_mode);
    dcc_tcp_put32(data + 36, st->st_uid); dcc_tcp_put32(data + 40, st->st_gid);
    dcc_tcp_put64(data + 44, (uint64_t)st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec);
}
static int metadata(struct dcc_frontend_reply *reply, const struct stat *st)
{
    unsigned char data[DCC_FS_METADATA];
    encode_metadata(data, st);
    return append(reply, data, sizeof(data));
}
static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int directory_entry(struct dcc_frontend_reply *reply, int fd, const char *name,
                           unsigned type, int statuses, int inspect)
{
    struct stat st = {0};
    int error = 0;
    if (inspect || (!statuses && type == DT_UNKNOWN))
        error = fstatat(fd, name, &st, statuses == 1 ? 0 : AT_SYMLINK_NOFOLLOW) ? errno : 0;
    else if (!statuses) st.st_mode = DTTOIF(type);
    unsigned char header[8];
    size_t length = strlen(name);
    dcc_tcp_put32(header, length);
    dcc_tcp_put32(header + 4, statuses == 2 && !inspect ? DCC_FS_NO_METADATA :
                 statuses ? (uint32_t)error : error ? 0 : st.st_mode);
    return append(reply, header, 8) || append(reply, name, length) ||
        (inspect && !error && metadata(reply, &st));
}
/* Own fd. Cached names need fresh child metadata, but no DIR allocation or
 * the extra descriptor stat that opendir/fdopendir performs. */
static int directory(struct dcc_frontend_reply *reply, int fd, int statuses,
                     const char **hints, size_t hint_count, const char *previous,
                     const struct stat *initial)
{
    DIR *dir = NULL;
    size_t offset = reply->size;
    unsigned char header[8] = {0};
    uint32_t count = 0, flags = 0;
    int result = statuses ? append(reply, header, statuses == 2 ? 8 : 4) : 0;
    unsigned char version[DCC_FS_METADATA], observed[DCC_FS_METADATA];
    struct stat snapshot;
    if (!result && statuses == 2) {
        encode_metadata(version, initial);
        if (!memcmp(version, previous, sizeof(version))) flags |= DCC_FS_DIRECTORY_CACHED;
        else if (hint_count && S_ISDIR(dcc_tcp_get32(previous + 32)))
            flags |= DCC_FS_DIRECTORY_PARTIAL;
    }
    if (flags & (DCC_FS_DIRECTORY_CACHED | DCC_FS_DIRECTORY_PARTIAL)) {
        /* This is a lookup index, not a readdir result. The caller checked
         * current search permission; child attributes are never reused. */
        for (size_t i = 0; i < hint_count && !result; ++i) {
            result = directory_entry(reply, fd, hints[i], DT_UNKNOWN, statuses, 1);
            ++count;
        }
    } else if (!result) {
        dir = fdopendir(fd);
        if (!dir) result = -1;
        struct dirent *entry;
        if (!result) errno = 0;
        while (!result && (entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            const char *name = entry->d_name;
            int inspect = statuses == 1 || (statuses == 2 && hint_count &&
                bsearch(&name, hints, hint_count, sizeof(*hints), compare_names));
            result = directory_entry(reply, fd, name, entry->d_type, statuses, inspect);
            if (result) break;
            ++count;
            errno = 0;
        }
        if (!result && errno) result = -1;
    }
    /* Only freshly enumerated names need a publication stability check.
     * Cached names were matched to the fresh descriptor identity above and
     * are never republished; child attributes still come from this query. */
    if (!result && statuses == 2 && !flags) {
        result = fstat(fd, &snapshot);
        if (!result) {
            encode_metadata(observed, &snapshot);
            if (!memcmp(version, observed, sizeof(version))) flags |= DCC_FS_DIRECTORY_STABLE;
        }
    }
    int error = errno;
    if (dir) closedir(dir);
    else close(fd);
    if (!result && statuses) {
        if (statuses == 2) dcc_tcp_put32(reply->data + offset, flags);
        dcc_tcp_put32(reply->data + offset + (statuses == 2 ? 4 : 0), count);
    }
    errno = error;
    return result;
}

static int query_metadata(struct dcc_frontend_reply *reply, const char *path, int follow,
                          int contents, const char **hints, size_t hint_count,
                          const char *previous)
{
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW);
    int fd = -1;
    struct stat st;
    /* Cached names or a known lookup footprint avoid reading a directory.
     * Pin the same inode used for child attributes and check effective
     * search permissions (including ACLs). Actual readdir uses LIST and its
     * ordinary read-permission check, even when lookup names are cached. */
    if (contents && previous && S_ISDIR(dcc_tcp_get32(previous + 32))) {
        fd = open(path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            if (fstat(fd, &st)) goto failed;
            unsigned char version[DCC_FS_METADATA];
            encode_metadata(version, &st);
            if (!memcmp(previous, version, sizeof(version)) || hint_count) {
                if (faccessat(fd, "", X_OK, AT_EMPTY_PATH | AT_EACCESS)) {
                    if (errno != EACCES) goto failed;
                    close(fd);
                    return metadata(reply, &st);
                }
                if (metadata(reply, &st)) goto failed;
                return directory(reply, fd, 2, hints, hint_count, previous, &st);
            }
            close(fd);
        }
    }
    if ((follow ? stat(path, &st) : lstat(path, &st))) return -1;
    if (!S_ISDIR(st.st_mode) || !contents) return metadata(reply, &st);
    fd = open(path, flags);
    /* Execute-only directories still permit known header lookups. */
    if (fd < 0) return !follow && errno == EACCES ? metadata(reply, &st) : -1;
    if (!follow) {
        if (fstat(fd, &st)) goto failed;
        /* Reading names without search permission does not authorize even
         * negative child lookups. Such names belong only in a LIST reply. */
        if (faccessat(fd, "", X_OK, AT_EMPTY_PATH | AT_EACCESS)) {
            if (errno != EACCES) goto failed;
            close(fd);
            return metadata(reply, &st);
        }
    }
    if (metadata(reply, &st)) goto failed;
    return directory(reply, fd, follow ? 1 : 2, hints, hint_count, previous, &st);
failed:
    {
        int error = errno;
        close(fd); errno = error;
        return -1;
    }
}
void dcc_frontend_fs_reset(struct dcc_frontend_reply *reply)
{
    if (reply->fd >= 0) close(reply->fd);
    reply->fd = -1; reply->file_size = 0; reply->size = 0;
}
void dcc_frontend_fs_clear(struct dcc_frontend_reply *reply)
{
    dcc_frontend_fs_reset(reply);
    free(reply->data);
    *reply = (struct dcc_frontend_reply){.fd = -1};
}
static int append_query(struct dcc_job *, const void *, size_t, struct dcc_frontend_reply *);
static int directory_batch(struct dcc_job *job, const unsigned char *data, size_t size,
                           struct dcc_frontend_reply *reply)
{
    unsigned char header[8] = {0};
    if (!size || append(reply, header, sizeof(header))) return -1;
    while (size) {
        if (size < 4) return -1;
        unsigned length = dcc_tcp_get32(data);
        data += 4; size -= 4;
        /* Only metadata queries belong here: no nesting or open file bodies. */
        if (length < 4 || length > size) return -1;
        unsigned operation = dcc_tcp_get32(data);
        if ((operation != DCC_FS_LSTAT && operation != DCC_FS_ATTRIBUTES) ||
            append_query(job, data, length, reply)) return -1;
        data += length; size -= length;
    }
    dcc_tcp_put32(reply->data + 4, reply->size - 8);
    return 0;
}

static int append_query(struct dcc_job *job, const void *data, size_t size,
                  struct dcc_frontend_reply *reply)
{
    if (size < 5) return -1;
    unsigned operation = dcc_tcp_get32(data);
    if (((const char *)data)[4] != '/') return -1;
    if (operation == DCC_FS_STDIN && size != 5) return -1;
    const char *names = (const char *)data + 4, *end = (const char *)data + size;
    const char *separator = memchr(names, 0, end - names), **hints = NULL, *version = NULL;
    size_t hint_count = 0;
    if (operation == DCC_FS_LSTAT) {
        if (!separator || end - separator - 1 < DCC_FS_METADATA) return -1;
        version = separator + 1;
        const char *previous = NULL;
        for (const char *name = version + DCC_FS_METADATA; name < end;) {
            const char *zero = memchr(name, 0, end - name);
            if (!zero || zero == name || memchr(name, '/', zero - name) ||
                !strcmp(name, ".") || !strcmp(name, "..") ||
                (previous && strcmp(previous, name) >= 0)) return -1;
            previous = name; name = zero + 1; ++hint_count;
        }
        if (hint_count) {
            hints = malloc(hint_count * sizeof(*hints));
            if (!hints) return -1;
            const char *name = version + DCC_FS_METADATA;
            for (size_t i = 0; i < hint_count; ++i) { hints[i] = name; name += strlen(name) + 1; }
        }
    } else if (separator) return -1;
    char *path = strndup(names, separator ? separator - names : end - names);
    int error = 0;
    size_t start = reply->size;
    unsigned char header[8] = {0};
    struct stat st;
    if (!path || append(reply, header, 8)) goto failed;
    int access = operation == DCC_FS_STDIN ? PATH_CONTENTS : allowed(job, path);
    int required = operation == DCC_FS_STAT || operation == DCC_FS_LSTAT || operation == DCC_FS_ATTRIBUTES ||
        operation == DCC_FS_READLINK || operation == DCC_FS_REALPATH ? PATH_METADATA : PATH_CONTENTS;
    if (!(access & required)) {
        /* Missing include candidates must retain their lookup error so Clang
         * can continue searching. Existing unexported files stay inaccessible. */
        error = (operation == DCC_FS_LSTAT || operation == DCC_FS_ATTRIBUTES || operation == DCC_FS_READLINK
                 ? lstat(path, &st) : stat(path, &st)) ? errno : EACCES;
    } else if (operation == DCC_FS_STAT || operation == DCC_FS_LSTAT || operation == DCC_FS_ATTRIBUTES) {
        if (query_metadata(reply, path, operation == DCC_FS_STAT,
                           operation == DCC_FS_ATTRIBUTES ? 0 : access & PATH_CONTENTS,
                           hints, hint_count, version)) error = errno;
    } else if (operation == DCC_FS_READ || operation == DCC_FS_STDIN) {
        reply->fd = operation == DCC_FS_STDIN ? fcntl(job->stdin_fd - 1, F_DUPFD_CLOEXEC, 3) :
            open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (reply->fd < 0 || fstat(reply->fd, &st)) error = errno;
        else if (!S_ISREG(st.st_mode)) error = EINVAL;
        else if ((uint64_t)st.st_size > DCC_TCP_BYTES - 8 - DCC_FS_METADATA) error = EFBIG;
        else if (metadata(reply, &st)) error = errno;
        else reply->file_size = st.st_size;
    } else if (operation == DCC_FS_REALPATH) {
        char *resolved = realpath(path, NULL);
        if (!resolved || append(reply, resolved, strlen(resolved))) error = errno;
        free(resolved);
    } else if (operation == DCC_FS_LIST) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0 || directory(reply, fd, 0, NULL, 0, NULL, NULL)) error = errno;
    } else if (operation == DCC_FS_READLINK) {
        char target[PATH_MAX];
        ssize_t length = readlink(path, target, sizeof(target));
        if (length < 0 || (size_t)length == sizeof(target)) error = length < 0 ? errno : ENAMETOOLONG;
        else if (append(reply, target, length)) error = errno;
        else {
            /* The kernel follows the link before its next FUSE request.
             * Authorize only targets reached through a client-authorized
             * link, not arbitrary canonical paths proposed by the worker. */
            if (access & PATH_CONTENTS) {
                char *resolved = realpath(path, NULL);
                if (resolved && dcc_frontend_fs_add_root(job, resolved)) error = ENOMEM;
                free(resolved);
            } else {
                /* A symlink above an export grants only the exports below
                 * it, rather than every file in the symlink's target. */
                size_t count = dcc_argv_len(job->fs_roots);
                for (size_t i = 0; i < count && !error; ++i) {
                    if (!within(job->fs_roots[i], path)) continue;
                    char *resolved = realpath(job->fs_roots[i], NULL);
                    if (resolved && dcc_frontend_fs_add_root(job, resolved)) error = ENOMEM;
                    free(resolved);
                }
            }
        }
    } else error = EINVAL;
    if (error) {
        if (reply->fd >= 0) close(reply->fd);
        reply->fd = -1; reply->file_size = 0; reply->size = start + 8;
    }
    dcc_tcp_put32(reply->data + start, error);
    dcc_tcp_put32(reply->data + start + 4, reply->size + reply->file_size - start - 8);
    free(path); free(hints);
    return 0;
failed:
    free(path); free(hints);
    return -1;
}
int dcc_frontend_fs_reply(struct dcc_job *job, const void *data, size_t size,
                          struct dcc_frontend_reply *reply)
{
    dcc_frontend_fs_reset(reply);
    if (!job->fs_roots || size < 5 || size > DCC_FS_QUERY_MAX) return -1;
    int ret = dcc_tcp_get32(data) == DCC_FS_DIRECTORIES
        ? directory_batch(job, (const unsigned char *)data + 4, size - 4, reply)
        : append_query(job, data, size, reply);
    if (ret) dcc_frontend_fs_reset(reply);
    return ret;
}
