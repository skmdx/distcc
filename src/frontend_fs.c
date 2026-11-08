/* Serve the compiler's actual lookups without a second preprocessing pass.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
static char *normalized(const char *path)
{
    char *copy = strdup(path), *result = malloc(strlen(path) + 2), *save = NULL;
    if (!copy || !result) { free(copy); free(result); return NULL; }
    size_t used = 0;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            while (used && result[--used] != '/') {}
        } else {
            result[used++] = '/';
            size_t size = strlen(part); memcpy(result + used, part, size); used += size;
        }
    }
    if (!used) result[used++] = '/';
    result[used] = 0;
    free(copy);
    return result;
}
static int within(const char *path, const char *root)
{
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || path[n] == '/' || !strcmp(root, "/"));
}
enum { PATH_METADATA = 1, PATH_CONTENTS = 2 };
int dcc_frontend_fs_add_root(struct dcc_job *job, const char *path)
{
    char *absolute = NULL;
    if (*path != '/' && asprintf(&absolute, "%s/%s", job->cwd, path) < 0) return -1;
    char *root = normalized(absolute ? absolute : path);
    free(absolute);
    if (!root) return -1;
    size_t count = job->fs_roots ? dcc_argv_len(job->fs_roots) : 0;
    for (size_t i = 0; i < count; ++i)
        if (within(root, job->fs_roots[i])) { free(root); return 0; }
    char **roots = realloc(job->fs_roots, (count+2)*sizeof(*roots));
    if (!roots) { free(root); return -1; }
    job->fs_roots = roots;
    roots[count] = root; roots[count+1] = NULL;
    return 0;
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
    char *name = normalized(path);
    if (!name) return 0;
    int result = 0;
    for (char **p = job->fs_roots; p && *p; ++p) {
        if (within(name, *p)) { result = PATH_METADATA | PATH_CONTENTS; break; }
        if (within(*p, name)) result = PATH_METADATA;
    }
    free(name);
    return result;
}
/* Metadata stays in one owned allocation. File contents are read only when
 * the transport can send its next chunk, without a temporary-file round trip. */
struct reply_writer {
    struct dcc_frontend_reply *reply;
    size_t capacity;
};
static int append(struct reply_writer *writer, const void *bytes, size_t length)
{
    struct dcc_frontend_reply *reply = writer->reply;
    if (length > DCC_TCP_BYTES - reply->size) { errno = EFBIG; return -1; }
    size_t needed = reply->size + length;
    if (needed > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity * 2 : 64;
        if (capacity < needed) capacity = needed;
        if (capacity > DCC_TCP_BYTES) capacity = DCC_TCP_BYTES;
        void *data = realloc(reply->data, capacity);
        if (!data) return -1;
        reply->data = data; writer->capacity = capacity;
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
static int metadata(struct reply_writer *writer, const struct stat *st)
{
    unsigned char data[DCC_FS_METADATA];
    encode_metadata(data, st);
    return append(writer, data, sizeof(data));
}
static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int directory_entry(struct reply_writer *writer, int fd, const char *name,
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
    return append(writer, header, 8) || append(writer, name, length) ||
        (inspect && !error && metadata(writer, &st));
}
static int directory(struct reply_writer *writer, const char *path, int statuses,
                     const char **hints, size_t hint_count, const char *previous,
                     const struct stat *initial)
{
    DIR *dir = opendir(path);
    /* GCC can open a known header in an execute-only directory. Without
     * listing permission, return its metadata and query children individually. */
    if (!dir) return statuses == 2 && errno == EACCES ? 0 : -1;
    size_t offset = writer->reply->size;
    unsigned char header[8] = {0};
    uint32_t count = 0, flags = 0;
    int result = statuses ? append(writer, header, statuses == 2 ? 8 : 4) : 0;
    unsigned char version[DCC_FS_METADATA], observed[DCC_FS_METADATA];
    struct stat snapshot;
    if (!result && statuses == 2) {
        encode_metadata(version, initial);
        result = fstat(dirfd(dir), &snapshot);
        if (!result) {
            encode_metadata(observed, &snapshot);
            if (!memcmp(version, observed, sizeof(version)) &&
                !memcmp(version, previous, sizeof(version))) flags |= DCC_FS_DIRECTORY_CACHED;
        }
    }
    if (flags & DCC_FS_DIRECTORY_CACHED) {
        /* Opening the directory still checks the caller's read permission.
         * Reusing names must never reuse child attributes from a prior job. */
        for (size_t i = 0; i < hint_count && !result; ++i) {
            result = directory_entry(writer, dirfd(dir), hints[i], DT_UNKNOWN, statuses, 1);
            ++count;
        }
    } else if (!result) {
        struct dirent *entry;
        errno = 0;
        while (!result && (entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            const char *name = entry->d_name;
            int inspect = statuses == 1 || (statuses == 2 && hint_count &&
                bsearch(&name, hints, hint_count, sizeof(*hints), compare_names));
            result = directory_entry(writer, dirfd(dir), name, entry->d_type, statuses, inspect);
            if (result) break;
            ++count;
            errno = 0;
        }
        if (!result && errno) result = -1;
    }
    if (!result && statuses == 2) {
        result = fstat(dirfd(dir), &snapshot);
        if (!result) {
            encode_metadata(observed, &snapshot);
            if (!memcmp(version, observed, sizeof(version))) flags |= DCC_FS_DIRECTORY_STABLE;
        }
    }
    int error = errno;
    closedir(dir);
    if (!result && statuses) {
        if (statuses == 2) dcc_tcp_put32(writer->reply->data + offset, flags);
        dcc_tcp_put32(writer->reply->data + offset + (statuses == 2 ? 4 : 0), count);
    }
    errno = error;
    return result;
}
void dcc_frontend_fs_clear(struct dcc_frontend_reply *reply)
{
    if (reply->fd >= 0) close(reply->fd);
    free(reply->data);
    *reply = (struct dcc_frontend_reply){.fd = -1};
}
int dcc_frontend_fs_reply(struct dcc_job *job, const void *data, size_t size,
                          struct dcc_frontend_reply *reply)
{
    *reply = (struct dcc_frontend_reply){.fd = -1};
    if (!job->fs_roots || size < 5 || size > DCC_FS_QUERY_MAX ||
        ((const char *)data)[4] != '/') return -1;
    unsigned operation = dcc_tcp_get32(data);
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
    struct reply_writer writer = {.reply = reply};
    unsigned char header[8] = {0};
    struct stat st;
    if (!path || append(&writer, header, 8)) goto failed;
    int access = allowed(job, path);
    int required = operation == DCC_FS_STAT || operation == DCC_FS_LSTAT ||
        operation == DCC_FS_READLINK || operation == DCC_FS_REALPATH ? PATH_METADATA : PATH_CONTENTS;
    if (!(access & required)) {
        /* Missing include candidates must retain their lookup error so Clang
         * can continue searching. Existing unexported files stay inaccessible. */
        error = (operation == DCC_FS_LSTAT || operation == DCC_FS_READLINK
                 ? lstat(path, &st) : stat(path, &st)) ? errno : EACCES;
    } else if (operation == DCC_FS_STAT || operation == DCC_FS_LSTAT) {
        if ((operation == DCC_FS_STAT ? stat(path, &st) : lstat(path, &st)) || metadata(&writer, &st) ||
            (S_ISDIR(st.st_mode) && (access & PATH_CONTENTS) &&
             directory(&writer, path, operation == DCC_FS_STAT ? 1 : 2, hints, hint_count,
                       version, &st))) error = errno;
    } else if (operation == DCC_FS_READ) {
        reply->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (reply->fd < 0 || fstat(reply->fd, &st)) error = errno;
        else if (!S_ISREG(st.st_mode)) error = EINVAL;
        else if ((uint64_t)st.st_size > DCC_TCP_BYTES - 8 - DCC_FS_METADATA) error = EFBIG;
        else if (metadata(&writer, &st)) error = errno;
        else reply->file_size = st.st_size;
    } else if (operation == DCC_FS_REALPATH) {
        char *resolved = realpath(path, NULL);
        if (!resolved || append(&writer, resolved, strlen(resolved))) error = errno;
        free(resolved);
    } else if (operation == DCC_FS_LIST) {
        if (directory(&writer, path, 0, NULL, 0, NULL, NULL)) error = errno;
    } else if (operation == DCC_FS_READLINK) {
        char target[PATH_MAX];
        ssize_t length = readlink(path, target, sizeof(target));
        if (length < 0 || (size_t)length == sizeof(target)) error = length < 0 ? errno : ENAMETOOLONG;
        else if (append(&writer, target, length)) error = errno;
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
        reply->fd = -1; reply->file_size = 0; reply->size = 8;
    }
    dcc_tcp_put32(reply->data, error);
    dcc_tcp_put32(reply->data + 4, reply->size + reply->file_size - 8);
    free(path); free(hints);
    return 0;
failed:
    dcc_frontend_fs_clear(reply);
    free(path); free(hints);
    return -1;
}
