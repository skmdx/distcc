/* Verified input snapshots; never share writable inodes with compiler jobs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#include "distcc.h"
#include "file_cache.h"

/* Separate shard locks bound both directory scans and concurrent storage.
 * FIFO eviction avoids a metadata write on every cache hit. */
#define CACHE_SHARDS 64
#define CACHE_FILES 64
#define CACHE_BYTES (4 * 1024 * 1024)

static int private_directory(int parent, const char *name)
{
    struct stat st;
    int fd;
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 077))) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static int cache_shard(const unsigned char *hash, char key[65])
{
    const char *setting = getenv("DISTCCD_FILE_CACHE"), *tmp;
    char *path = NULL, shard[3];
    int root, fd, i;
    if (setting && !*setting) return -1;
    if (!setting) {
        if (dcc_get_tmp_top(&tmp) ||
            asprintf(&path, "%s/distcc-cache-%lu", tmp, (unsigned long)geteuid()) < 0)
            return -1;
        setting = path;
    }
    root = private_directory(AT_FDCWD, setting);
    free(path);
    if (root < 0) return -1;
    snprintf(shard, sizeof(shard), "%02x", hash[0] % CACHE_SHARDS);
    fd = private_directory(root, shard);
    close(root);
    for (i = 0; i < DCC_FILE_HASH_SIZE; ++i) sprintf(key + 2*i, "%02x", hash[i]);
    return fd;
}

static int digest(int fd, const unsigned char *expected)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char buffer[65536], hash[EVP_MAX_MD_SIZE];
    unsigned length;
    ssize_t n;
    int ok = 0;
    if (!ctx || lseek(fd, 0, SEEK_SET) < 0 || !EVP_DigestInit_ex(ctx, EVP_sha256(), NULL))
        goto done;
    while ((n = read(fd, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        if (!EVP_DigestUpdate(ctx, buffer, n)) goto done;
    }
    ok = EVP_DigestFinal_ex(ctx, hash, &length) && length == DCC_FILE_HASH_SIZE &&
         !memcmp(hash, expected, length);
done:
    EVP_MD_CTX_free(ctx);
    return ok;
}

int dcc_file_cache_verify(const unsigned char *hash, const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), ok;
    if (fd < 0) return 0;
    ok = digest(fd, hash);
    close(fd);
    return ok;
}

static int copy_snapshot(int from, int to)
{
    char buffer[65536];
    ssize_t n;
#ifdef __linux__
    if (!ioctl(to, FICLONE, from)) return 0;
#endif
    while ((n = read(from, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (dcc_writex(to, buffer, n)) return -1;
    }
    return 0;
}

int dcc_file_cache_get(const unsigned char *hash, const char *destination)
{
    char key[65];
    struct stat st;
    int shard = cache_shard(hash, key), from = -1, to = -1, ok = 0;
    if (shard < 0) return 0;
    from = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode) || st.st_size > CACHE_BYTES)
        goto done;
    if (dcc_mk_tmp_ancestor_dirs(destination)) goto done;
    to = open(destination, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (to < 0) goto done;
    /* An open entry survives concurrent eviction. Verify the private snapshot
     * too, so a corrupt cache entry is a miss, never a wrong compilation. */
    ok = !copy_snapshot(from, to) && digest(to, hash);
    if (!ok) unlink(destination);
done:
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    close(shard);
    return ok;
}

struct entry { char key[65]; off_t size; time_t age; };

static int make_room(int shard, off_t size)
{
    struct entry entries[CACHE_FILES];
    struct dirent *de;
    struct stat st;
    off_t bytes = 0;
    int count = 0, i, oldest;
    DIR *dir = fdopendir(dup(shard));
    if (!dir) return -1;
    while ((de = readdir(dir))) {
        if (!strncmp(de->d_name, ".tmp-", 5)) {
            if (unlinkat(shard, de->d_name, 0)) { closedir(dir); return -1; }
            continue;
        }
        if (strlen(de->d_name) != 64 || strspn(de->d_name, "0123456789abcdef") != 64)
            continue;
        if (fstatat(shard, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (!S_ISREG(st.st_mode) || count == CACHE_FILES) {
            if (unlinkat(shard, de->d_name, 0)) { closedir(dir); return -1; }
            continue;
        }
        strcpy(entries[count].key, de->d_name);
        entries[count].size = st.st_size;
        entries[count++].age = st.st_mtime;
        bytes += st.st_size;
    }
    closedir(dir);
    while (count && (count >= CACHE_FILES || bytes + size > CACHE_BYTES)) {
        oldest = 0;
        for (i = 1; i < count; ++i)
            if (entries[i].age < entries[oldest].age) oldest = i;
        if (unlinkat(shard, entries[oldest].key, 0)) return -1;
        bytes -= entries[oldest].size;
        entries[oldest] = entries[--count];
    }
    return 0;
}

void dcc_file_cache_put(const unsigned char *hash, const char *path)
{
    char key[65], temporary[40];
    struct stat st;
    int shard = -1, lock = -1, from = -1, to = -1;
    from = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode) || st.st_size > CACHE_BYTES)
        goto done;
    shard = cache_shard(hash, key);
    if (shard < 0) goto done;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    /* Caching must never queue a compilation behind another uploader. */
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    if (make_room(shard, st.st_size)) goto done;
    snprintf(temporary, sizeof(temporary), ".tmp-%lu", (unsigned long)getpid());
    to = openat(shard, temporary, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (to < 0) goto done;
    if (!copy_snapshot(from, to) && !fchmod(to, 0400))
        renameat(shard, temporary, shard, key);
    unlinkat(shard, temporary, 0);
done:
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    if (lock >= 0) close(lock);
    if (shard >= 0) close(shard);
}
