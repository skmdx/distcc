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
#include <sys/sendfile.h>
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
static int allowed(const struct dcc_job *job, const char *path, int metadata)
{
    char *name = normalized(path);
    if (!name) return 0;
    int result = 0;
    for (char **p = job->fs_roots; p && *p; ++p)
        if (within(name, *p) || (metadata && within(*p, name))) { result = 1; break; }
    free(name);
    return result;
}
static int append(int fd, const void *bytes, size_t length)
{
    off_t offset = lseek(fd, 0, SEEK_CUR);
    if (offset < 0 || length > DCC_TCP_BYTES - (uint64_t)offset) { errno = EFBIG; return -1; }
    const char *data = bytes;
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; length -= n;
    }
    return 0;
}
static int metadata(int fd, const struct stat *st)
{
    unsigned char data[DCC_FS_METADATA];
    dcc_tcp_put64(data, st->st_dev); dcc_tcp_put64(data + 8, st->st_ino);
    dcc_tcp_put64(data + 16, (uint64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec);
    dcc_tcp_put64(data + 24, st->st_size); dcc_tcp_put32(data + 32, st->st_mode);
    dcc_tcp_put32(data + 36, st->st_uid); dcc_tcp_put32(data + 40, st->st_gid);
    dcc_tcp_put64(data + 44, (uint64_t)st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec);
    return append(fd, data, sizeof(data));
}
static int directory(int fd, const char *path, int statuses)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;
    off_t offset = lseek(fd, 0, SEEK_CUR);
    unsigned char header[8] = {0};
    uint32_t count = 0;
    int result = statuses ? append(fd, header, 4) : 0;
    struct dirent *entry;
    if (!result) errno = 0;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        int error = fstatat(dirfd(dir), entry->d_name, &st, statuses ? 0 : AT_SYMLINK_NOFOLLOW) ? errno : 0;
        size_t length = strlen(entry->d_name);
        dcc_tcp_put32(header, length);
        dcc_tcp_put32(header + 4, statuses ? error : error ? 0 : st.st_mode);
        result = append(fd, header, 8) || append(fd, entry->d_name, length) ||
            (statuses && !error && metadata(fd, &st));
        if (result) break;
        ++count;
        errno = 0;
    }
    if (!result && errno) result = -1;
    int error = errno;
    closedir(dir);
    if (!result && statuses) {
        dcc_tcp_put32(header, count);
        result = pwrite(fd, header, 4, offset) != 4 ? -1 : 0;
        if (result) error = errno;
    }
    errno = error;
    return result;
}
int dcc_frontend_fs_reply(const struct dcc_job *job, const void *data, size_t size, uint64_t *length)
{
    if (!job->fs_roots || size < 5 || size > 65536 ||
        ((const char *)data)[4] != '/' || memchr((const char *)data + 4, 0, size - 4)) return -1;
    unsigned operation = dcc_tcp_get32(data);
    char *path = strndup((const char *)data + 4, size - 4);
    int fd = dcc_job_memfd("distcc-filesystem"), source = -1, error = 0;
    unsigned char header[8] = {0};
    struct stat st;
    if (!path || fd < 0 || append(fd, header, 8)) goto failed;
    if (!allowed(job, path, operation == DCC_FS_STAT || operation == DCC_FS_REALPATH)) error = EACCES;
    else if (operation == DCC_FS_STAT) {
        if (stat(path, &st) || metadata(fd, &st) ||
            (S_ISDIR(st.st_mode) && allowed(job, path, 0) && directory(fd, path, 1))) error = errno;
    } else if (operation == DCC_FS_READ) {
        source = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (source < 0 || fstat(source, &st)) error = errno;
        else if (!S_ISREG(st.st_mode)) error = EINVAL;
        else if ((uint64_t)st.st_size > DCC_TCP_BYTES - 8 - DCC_FS_METADATA) error = EFBIG;
        else if (metadata(fd, &st)) error = errno;
        else {
            off_t offset = 0;
            while (offset < st.st_size) {
                ssize_t n = sendfile(fd, source, &offset, st.st_size - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { error = n ? errno : EIO; break; }
            }
        }
    } else if (operation == DCC_FS_REALPATH) {
        char *resolved = realpath(path, NULL);
        if (!resolved || append(fd, resolved, strlen(resolved))) error = errno;
        free(resolved);
    } else if (operation == DCC_FS_LIST) {
        if (directory(fd, path, 0)) error = errno;
    } else error = EINVAL;
    if (source >= 0) { close(source); source = -1; }
    if (error && (ftruncate(fd, 8) || lseek(fd, 8, SEEK_SET) < 0)) goto failed;
    off_t end = lseek(fd, 0, SEEK_CUR);
    if (end < 8 || (uint64_t)end > DCC_TCP_BYTES) goto failed;
    dcc_tcp_put32(header, error); dcc_tcp_put32(header + 4, end - 8);
    if (pwrite(fd, header, 8, 0) != 8) goto failed;
    *length = end;
    free(path);
    return fd;
failed:
    if (source >= 0) close(source);
    if (fd >= 0) close(fd);
    free(path);
    return -1;
}
