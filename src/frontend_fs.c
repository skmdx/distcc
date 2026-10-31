/* Serve the compiler's actual lookups without a second preprocessing pass.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
static int metadata(struct reply_writer *writer, const struct stat *st)
{
    unsigned char data[DCC_FS_METADATA];
    dcc_tcp_put64(data, st->st_dev); dcc_tcp_put64(data + 8, st->st_ino);
    dcc_tcp_put64(data + 16, (uint64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec);
    dcc_tcp_put64(data + 24, st->st_size); dcc_tcp_put32(data + 32, st->st_mode);
    dcc_tcp_put32(data + 36, st->st_uid); dcc_tcp_put32(data + 40, st->st_gid);
    dcc_tcp_put64(data + 44, (uint64_t)st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec);
    return append(writer, data, sizeof(data));
}
static int directory(struct reply_writer *writer, const char *path, int statuses)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;
    size_t offset = writer->reply->size;
    unsigned char header[8] = {0};
    uint32_t count = 0;
    int result = statuses ? append(writer, header, 4) : 0;
    struct dirent *entry;
    if (!result) errno = 0;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        int error = 0;
        if (statuses || entry->d_type == DT_UNKNOWN)
            error = fstatat(dirfd(dir), entry->d_name, &st, statuses ? 0 : AT_SYMLINK_NOFOLLOW) ? errno : 0;
        else st.st_mode = DTTOIF(entry->d_type); /* LIST exposes only the file type. */
        size_t length = strlen(entry->d_name);
        dcc_tcp_put32(header, length);
        dcc_tcp_put32(header + 4, statuses ? (uint32_t)error : error ? 0 : st.st_mode);
        result = append(writer, header, 8) || append(writer, entry->d_name, length) ||
            (statuses && !error && metadata(writer, &st));
        if (result) break;
        ++count;
        errno = 0;
    }
    if (!result && errno) result = -1;
    int error = errno;
    closedir(dir);
    if (!result && statuses) {
        dcc_tcp_put32(writer->reply->data + offset, count);
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
int dcc_frontend_fs_reply(const struct dcc_job *job, const void *data, size_t size,
                          struct dcc_frontend_reply *reply)
{
    *reply = (struct dcc_frontend_reply){.fd = -1};
    if (!job->fs_roots || size < 5 || size > 65536 ||
        ((const char *)data)[4] != '/' || memchr((const char *)data + 4, 0, size - 4)) return -1;
    unsigned operation = dcc_tcp_get32(data);
    char *path = strndup((const char *)data + 4, size - 4);
    int error = 0;
    struct reply_writer writer = {.reply = reply};
    unsigned char header[8] = {0};
    struct stat st;
    if (!path || append(&writer, header, 8)) goto failed;
    int access = allowed(job, path);
    int required = operation == DCC_FS_STAT || operation == DCC_FS_REALPATH ? PATH_METADATA : PATH_CONTENTS;
    if (!(access & required)) error = EACCES;
    else if (operation == DCC_FS_STAT) {
        if (stat(path, &st) || metadata(&writer, &st) ||
            (S_ISDIR(st.st_mode) && (access & PATH_CONTENTS) && directory(&writer, path, 1))) error = errno;
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
        if (directory(&writer, path, 0)) error = errno;
    } else error = EINVAL;
    if (error) {
        if (reply->fd >= 0) close(reply->fd);
        reply->fd = -1; reply->file_size = 0; reply->size = 8;
    }
    dcc_tcp_put32(reply->data, error);
    dcc_tcp_put32(reply->data + 4, reply->size + reply->file_size - 8);
    free(path);
    return 0;
failed:
    dcc_frontend_fs_clear(reply);
    free(path);
    return -1;
}
