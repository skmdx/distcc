/* Verified snapshots; shared inputs have a compiler read-only contract.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <xxhash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/uio.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#include "distcc.h"
#include "file_cache.h"

/* Separate shard locks bound both directory scans and concurrent storage.
 * FIFO eviction avoids a metadata write on every cache hit. */
#define CACHE_SHARDS DCC_FILE_CACHE_SHARDS
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

static int cache_root(const char *tmp)
{
    const char *setting = getenv("DISTCCD_FILE_CACHE");
    char *path = NULL;
    int root;
    if (setting && !*setting) return -1;
    if (!setting) {
        if (!tmp ||
            asprintf(&path, "%s/distcc-cache-%lu", tmp, (unsigned long)geteuid()) < 0)
            return -1;
        setting = path;
    }
    root = private_directory(AT_FDCWD, setting);
    free(path);
    return root;
}

static int cache_shard(const unsigned char *hash, char key[17], const char *tmp)
{
    char shard[3];
    int root = cache_root(tmp), fd, i;
    if (root < 0) return -1;
    snprintf(shard, sizeof(shard), "%02x", hash[0] % CACHE_SHARDS);
    fd = private_directory(root, shard);
    close(root);
    for (i = 0; i < DCC_FILE_HASH_SIZE; ++i) sprintf(key + 2*i, "%02x", hash[i]);
    return fd;
}

static int copy_snapshot(int from, int to, const unsigned char *expected)
{
    char buffer[65536];
    XXH3_state_t *xxh = NULL;
    int ret = -1;
    ssize_t n;
#ifdef __linux__
    if (to >= 0 && !ioctl(to, FICLONE, from))
        return expected ? copy_snapshot(to, -1, expected) : 0;
#endif
    if (lseek(from, 0, SEEK_SET) < 0) goto done;
    if (expected) {
        xxh = XXH3_createState();
        if (!xxh || XXH3_64bits_reset(xxh) != XXH_OK) goto done;
    }
    while ((n = read(from, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        if ((xxh && XXH3_64bits_update(xxh, buffer, n) != XXH_OK) ||
            (to >= 0 && dcc_writex(to, buffer, n))) goto done;
    }
    if (xxh) {
        XXH64_canonical_t canonical;
        XXH64_canonicalFromHash(&canonical, XXH3_64bits_digest(xxh));
        if (memcmp(canonical.digest, expected, DCC_FILE_HASH_SIZE)) goto done;
    }
    ret = 0;
done:
    XXH3_freeState(xxh);
    return ret;
}

static int snapshot(int from, const char *destination, const unsigned char *hash, int shared)
{
    int to, ok;
    if (dcc_mk_tmp_ancestor_dirs(destination)) return 0;
    if (shared) {
        char source[64];
        if (hash && copy_snapshot(from, -1, hash)) return 0;
        snprintf(source, sizeof(source), "/proc/self/fd/%d", from);
        if (!linkat(AT_FDCWD, source, AT_FDCWD, destination, AT_SYMLINK_FOLLOW)) return 1;
        /* A separately configured cache can be on another filesystem; an
         * open entry also remains readable if eviction removed its last name. */
        if (errno != EXDEV && errno != ENOENT) return 0;
    }
    to = open(destination, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (to < 0) return 0;
    ok = !copy_snapshot(from, to, hash);
    close(to);
    if (!ok) unlink(destination);
    return ok;
}

int dcc_file_cache_copy(const unsigned char *hash, const char *source, const char *destination, int shared)
{
    int from = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), ok;
    if (from < 0) return 0;
    ok = snapshot(from, destination, hash, shared);
    close(from);
    return ok;
}

int dcc_file_cache_get(const unsigned char *hash, const char *destination, int shared, const char *tmp)
{
    char key[17];
    struct stat st;
    int shard = cache_shard(hash, key, tmp), from = -1, ok = 0;
    if (shard < 0) return 0;
    from = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode))
        goto done;
    /* An open entry survives concurrent eviction. Verify the private snapshot
     * too, so a corrupt cache entry is a miss, never a wrong compilation. */
    ok = snapshot(from, destination, hash, shared);
done:
    if (from >= 0) close(from);
    close(shard);
    return ok;
}

struct entry { char key[17]; off_t size; time_t age; };

static int make_room(int shard, off_t size, const char *replaced)
{
    struct entry entries[CACHE_FILES];
    struct dirent *de;
    struct stat st;
    off_t bytes = 0;
    int count = 0, i, oldest;
    /* A retained shard can be shared by forked compilers. Each scan needs
     * its own open file description, rather than dup's shared offset. */
    int iterator = openat(shard, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (iterator < 0) return -1;
    DIR *dir = fdopendir(iterator);
    if (!dir) { close(iterator); return -1; }
    while ((de = readdir(dir))) {
        if (!strncmp(de->d_name, ".tmp-", 5)) {
            if (unlinkat(shard, de->d_name, 0)) { closedir(dir); return -1; }
            continue;
        }
        if (strlen(de->d_name) != 16 || strspn(de->d_name, "0123456789abcdef") != 16)
            continue;
        if (!strcmp(de->d_name, replaced)) continue;
        if (fstatat(shard, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        /* Active read-only jobs pin their entries with hard links. The idle
         * cache bound must not evict and retransmit those same live inputs. */
        if (S_ISREG(st.st_mode) && st.st_nlink > 1) continue;
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
    while (count && (count + (*replaced != 0) > CACHE_FILES || bytes + size > CACHE_BYTES)) {
        oldest = 0;
        for (i = 1; i < count; ++i)
            if (entries[i].age < entries[oldest].age) oldest = i;
        if (unlinkat(shard, entries[oldest].key, 0)) return -1;
        bytes -= entries[oldest].size;
        entries[oldest] = entries[--count];
    }
    return 0;
}

void dcc_file_cache_put(const unsigned char *hash, const char *path, int shared, const char *tmp)
{
    char key[17], temporary[40];
    struct stat st;
    int shard = -1, lock = -1, from = -1, to = -1;
    from = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode) || (!shared && st.st_size > CACHE_BYTES))
        goto done;
    shard = cache_shard(hash, key, tmp);
    if (shard < 0) goto done;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    /* Publish shared inputs reliably; private cache insertion is best effort. */
    if (lock < 0 || flock(lock, LOCK_EX | (shared ? 0 : LOCK_NB))) goto done;
    to = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (to >= 0) {
        struct stat existing;
        int present = !fstat(to, &existing) && S_ISREG(existing.st_mode) &&
                      existing.st_size == st.st_size && !copy_snapshot(to, -1, hash);
        close(to);
        to = -1;
        if (present) goto done;
    }
    if (make_room(shard, st.st_size, key)) goto done;
    snprintf(temporary, sizeof(temporary), ".tmp-%lu", (unsigned long)getpid());
    if (shared) {
        if (!fchmod(from, 0400) && !linkat(AT_FDCWD, path, shard, temporary, 0)) {
            renameat(shard, temporary, shard, key);
            unlinkat(shard, temporary, 0);
            goto done;
        }
        if (errno != EXDEV || st.st_size > CACHE_BYTES) goto done;
    }
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

void dcc_file_cache_prune(const unsigned char *hash, const char *tmp)
{
    char key[17];
    int shard = cache_shard(hash, key, tmp), lock;
    if (shard < 0) return;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock >= 0 && !flock(lock, LOCK_EX)) make_room(shard, 0, "");
    if (lock >= 0) close(lock);
    close(shard);
}

struct record_header { uint64_t key_size, size, digest; };
struct dcc_record_cache { int root, shards[CACHE_SHARDS]; };

static struct dcc_record_cache *record_open(const char *tmp)
{
    int root = cache_root(tmp);
    if (root < 0) return NULL;
    struct dcc_record_cache *cache = malloc(sizeof(*cache));
    if (!cache) { close(root); return NULL; }
    cache->root = root;
    for (unsigned i = 0; i < CACHE_SHARDS; ++i) cache->shards[i] = -1;
    return cache;
}

static void record_close(struct dcc_record_cache *cache)
{
    if (!cache) return;
    for (unsigned i = 0; i < CACHE_SHARDS; ++i)
        if (cache->shards[i] >= 0) close(cache->shards[i]);
    close(cache->root);
    free(cache);
}

static int record_shard(struct dcc_record_cache *cache, const void *key, size_t length, char name[17])
{
    if (!cache) return -1;
    uint64_t hash = XXH3_64bits(key, length);
    unsigned index = (hash >> 56) % CACHE_SHARDS;
    snprintf(name, 17, "%016llx", (unsigned long long)hash);
    if (cache->shards[index] < 0) {
        char shard[3];
        snprintf(shard, sizeof(shard), "%02x", index);
        cache->shards[index] = private_directory(cache->root, shard);
    }
    return cache->shards[index];
}
static int read_record_bytes(int fd, void *data, size_t size)
{
    char *bytes = data;
    while (size) {
        ssize_t n = read(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n; size -= n;
    }
    return 0;
}
static void *record_read(struct dcc_record_cache *cache, const void *key, size_t key_size,
                         const void **data, size_t *size)
{
    char name[17];
    struct stat st;
    struct record_header header;
    char *storage = NULL;
    int shard = record_shard(cache, key, key_size, name), fd = -1, valid = 0;
    if (shard < 0) return NULL;
    fd = openat(shard, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)sizeof(header) || st.st_size > CACHE_BYTES) goto done;
    storage = malloc(st.st_size + 1);
    if (!storage || read_record_bytes(fd, storage, st.st_size)) goto done;
    memcpy(&header, storage, sizeof(header));
    if (header.key_size != key_size || header.size > CACHE_BYTES || key_size > CACHE_BYTES - sizeof(header) ||
        header.size > CACHE_BYTES - sizeof(header) - key_size ||
        (uint64_t)st.st_size != sizeof(header) + key_size + header.size) goto done;
    if (memcmp(storage + sizeof(header), key, key_size) ||
        XXH3_64bits(storage + sizeof(header) + key_size, header.size) != header.digest) goto done;
    storage[st.st_size] = 0;
    *data = storage + sizeof(header) + key_size;
    *size = header.size;
    valid = 1;
done:
    if (fd >= 0) close(fd);
    if (!valid) { free(storage); return NULL; }
    return storage;
}

static int write_record(int fd, struct record_header *header, const void *key, const void *data)
{
    struct iovec parts[] = {{header, sizeof(*header)}, {(void *)key, header->key_size},
                           {(void *)data, header->size}};
    struct iovec *part = parts;
    int count = 3;
    while (count) {
        ssize_t written = writev(fd, part, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        size_t consumed = written;
        while (count && consumed >= part->iov_len) {
            consumed -= part->iov_len;
            ++part; --count;
        }
        if (count) { part->iov_base = (char *)part->iov_base + consumed; part->iov_len -= consumed; }
    }
    return 0;
}

static void record_write(struct dcc_record_cache *cache, const void *key, size_t key_size,
                         const void *data, size_t size)
{
    if (!cache) return;
    struct record_header header = {key_size, size, 0};
    if (key_size > CACHE_BYTES - sizeof(header) || size > CACHE_BYTES - sizeof(header) - key_size) return;
    header.digest = XXH3_64bits(data, size);
    char name[17], temporary[40];
    int shard = record_shard(cache, key, key_size, name), lock = -1, fd = -1;
    if (shard < 0) return;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) ||
        make_room(shard, sizeof(header) + key_size + size, name)) goto done;
    snprintf(temporary, sizeof(temporary), ".tmp-%lu", (unsigned long)getpid());
    fd = openat(shard, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) goto done;
    if (!write_record(fd, &header, key, data) && !fchmod(fd, 0400))
        renameat(shard, temporary, shard, name);
    unlinkat(shard, temporary, 0);
done:
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
}
const struct dcc_file_cache_api dcc_file_cache_records = {record_open, record_close, record_read, record_write};
