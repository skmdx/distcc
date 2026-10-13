/* Verified input snapshots; never share writable inodes with compiler jobs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
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
#include "exitcode.h"

/* Separate shard locks bound both directory scans and concurrent storage.
 * FIFO eviction avoids a metadata write on every cache hit. */
#define CACHE_SHARDS 64
#define CACHE_FILES 64
#define CACHE_BYTES DCC_FILE_CACHE_MAX

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

static int cache_root(void)
{
    const char *setting = getenv("DISTCCD_FILE_CACHE"), *tmp;
    char *path = NULL;
    int root;
    if (setting && !*setting) return -1;
    if (!setting) {
        if (dcc_get_tmp_top(&tmp) ||
            asprintf(&path, "%s/distcc-cache-%lu", tmp, (unsigned long)geteuid()) < 0)
            return -1;
        setting = path;
    }
    root = private_directory(AT_FDCWD, setting);
    free(path);
    return root;
}

static int cache_shard(const unsigned char *hash, char key[65])
{
    char shard[3];
    int root = cache_root(), fd, i;
    if (root < 0) return -1;
    snprintf(shard, sizeof(shard), "%02x", hash[0] % CACHE_SHARDS);
    fd = private_directory(root, shard);
    close(root);
    for (i = 0; i < DCC_FILE_HASH_SIZE; ++i) sprintf(key + 2*i, "%02x", hash[i]);
    return fd;
}

static int compare_slots(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int dcc_file_cache_lock(const unsigned char **hashes, unsigned count, int input, int *lock)
{
    uint32_t *slots;
    unsigned i;
    struct stat st;
    struct timespec start, now;
    int root, ret = 0;
    *lock = -1;
    if (!count || (root = cache_root()) < 0) return 0;
    *lock = openat(root, ".uploads", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(root);
    if (*lock < 0) return 0;
    if (fstat(*lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(*lock);
        *lock = -1;
        return 0;
    }
    slots = malloc(count * sizeof(*slots));
    if (!slots) return EXIT_OUT_OF_MEMORY;
    for (i = 0; i < count; ++i)
        slots[i] = ((uint32_t)(hashes[i][0] & 127) << 24) |
                   ((uint32_t)hashes[i][1] << 16) | ((uint32_t)hashes[i][2] << 8) | hashes[i][3];
    qsort(slots, count, sizeof(*slots), compare_slots);
    clock_gettime(CLOCK_MONOTONIC, &start);
    /* One persistent lock inode, no payload storage or per-content lock files.
     * Every job takes byte-range locks in the same order: overlapping manifests
     * cannot deadlock. Closing the descriptor (including worker death) releases
     * the whole set. Cache insertion itself remains nonblocking. */
    for (i = 0; i < count && !ret; ++i) {
        struct flock range = { .l_type = F_WRLCK, .l_whence = SEEK_SET,
                               .l_start = slots[i], .l_len = 1 };
        if (i && slots[i] == slots[i-1]) continue;
        while (fcntl(*lock, F_SETLK, &range)) {
            struct pollfd p = { .fd = input, .events = POLLIN };
            if (errno != EACCES && errno != EAGAIN && errno != EINTR) {
                /* A filesystem without record locks can still serve inputs. */
                close(*lock);
                *lock = -1;
                goto done;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec >= dcc_get_io_timeout()) { ret = EXIT_TIMEOUT; break; }
            if (poll(&p, 1, 10) < 0 && errno != EINTR) { ret = EXIT_IO_ERROR; break; }
            /* Clients must await MISS here. Readability is cancellation or an
             * unsolicited payload; neither may hold another uploader hostage. */
            if (p.revents) { ret = EXIT_IO_ERROR; break; }
        }
    }
done:
    free(slots);
    return ret;
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

static int copy_snapshot(int from, int to, const unsigned char *expected)
{
    char buffer[65536];
    EVP_MD_CTX *ctx = NULL;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned length;
    int ret = -1;
    ssize_t n;
#ifdef __linux__
    if (!ioctl(to, FICLONE, from)) return !expected || digest(to, expected) ? 0 : -1;
#endif
    if (expected) {
        ctx = EVP_MD_CTX_new();
        if (!ctx || !EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) goto done;
    }
    while ((n = read(from, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        if ((ctx && !EVP_DigestUpdate(ctx, buffer, n)) || dcc_writex(to, buffer, n)) goto done;
    }
    if (ctx && (!EVP_DigestFinal_ex(ctx, hash, &length) || length != DCC_FILE_HASH_SIZE ||
                memcmp(hash, expected, length))) goto done;
    ret = 0;
done:
    EVP_MD_CTX_free(ctx);
    return ret;
}

static int snapshot(int from, const char *destination, const unsigned char *hash)
{
    int to, ok;
    if (dcc_mk_tmp_ancestor_dirs(destination)) return 0;
    to = open(destination, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (to < 0) return 0;
    ok = !copy_snapshot(from, to, hash);
    close(to);
    if (!ok) unlink(destination);
    return ok;
}

int dcc_file_cache_copy(const unsigned char *hash, const char *source, const char *destination)
{
    int from = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), ok;
    if (from < 0) return 0;
    ok = snapshot(from, destination, hash);
    close(from);
    return ok;
}

int dcc_file_cache_get(const unsigned char *hash, const char *destination)
{
    char key[65];
    struct stat st;
    int shard = cache_shard(hash, key), from = -1, ok = 0;
    if (shard < 0) return 0;
    from = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode) || st.st_size > CACHE_BYTES)
        goto done;
    /* An open entry survives concurrent eviction. Verify the private snapshot
     * too, so a corrupt cache entry is a miss, never a wrong compilation. */
    ok = snapshot(from, destination, hash);
done:
    if (from >= 0) close(from);
    close(shard);
    return ok;
}

struct entry { char key[65]; off_t size; time_t age; };

static int make_room(int shard, off_t size, const char *replaced)
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
        if (!strcmp(de->d_name, replaced)) continue;
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
    /* Directory maintenance never queues behind another shard insertion. */
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    to = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (to >= 0) {
        struct stat existing;
        int present = !fstat(to, &existing) && S_ISREG(existing.st_mode) &&
                      existing.st_size == st.st_size && digest(to, hash);
        close(to);
        to = -1;
        if (present) goto done;
    }
    if (make_room(shard, st.st_size, key)) goto done;
    snprintf(temporary, sizeof(temporary), ".tmp-%lu", (unsigned long)getpid());
    to = openat(shard, temporary, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (to < 0) goto done;
    if (!copy_snapshot(from, to, NULL) && !fchmod(to, 0400))
        renameat(shard, temporary, shard, key);
    unlinkat(shard, temporary, 0);
done:
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    if (lock >= 0) close(lock);
    if (shard >= 0) close(shard);
}
