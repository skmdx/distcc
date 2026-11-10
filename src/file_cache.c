/* Verified snapshots; shared inputs have a compiler read-only contract.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/uio.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#include "distcc.h"
#include "file_cache.h"

/* Shards serialize preparation, but share the former 64 * 4 MiB budget.
 * Capacity counts allocated bytes, including filesystem block rounding. */
#define CACHE_SHARDS DCC_FILE_CACHE_SHARDS
#define CACHE_BYTES DCC_FILE_CACHE_MAX

static void accessed(int fd, const struct stat *st)
{
    struct timespec now;
    /* Record use at most once per timestamp second. Keep mtime unchanged:
     * shared inputs can be hardlinked into a running compiler's filesystem. */
    if (!clock_gettime(CLOCK_REALTIME_COARSE, &now) && st->st_atim.tv_sec < now.tv_sec) {
        const struct timespec times[2] = {{.tv_nsec = UTIME_NOW}, {.tv_nsec = UTIME_OMIT}};
        (void)futimens(fd, times); /* Recency is best effort, like admission. */
    }
}

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

int dcc_file_digest(int fd, off_t size, unsigned char hash[DCC_FILE_HASH_SIZE])
{
    unsigned char buffer[65536];
    XXH3_state_t state;
    XXH64_hash_t value = 0;
    off_t offset = 0;
    if (size < 0) { errno = EINVAL; return -1; }
    int streaming = (uint64_t)size > sizeof(buffer);
    if (streaming) {
        XXH3_INITSTATE(&state);
        XXH3_64bits_reset(&state);
    }
    do {
        size_t length = (uint64_t)(size - offset) > sizeof(buffer) ?
            sizeof(buffer) : (size_t)(size - offset);
        for (size_t used = 0; used < length;) {
            ssize_t n = pread(fd, buffer + used, length - used, offset + used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (!n) errno = EIO; return -1; }
            used += n;
        }
        if (streaming) XXH3_64bits_update(&state, buffer, length);
        else value = XXH3_64bits(buffer, length);
        offset += length;
    } while (offset < size);
    if (streaming) value = XXH3_64bits_digest(&state);
    XXH64_canonical_t canonical;
    XXH64_canonicalFromHash(&canonical, value);
    memcpy(hash, canonical.digest, DCC_FILE_HASH_SIZE);
    return 0;
}

static int verify_snapshot(int from, const unsigned char *expected)
{
    struct stat st;
    unsigned char hash[DCC_FILE_HASH_SIZE];
    /* Cache publication replaces names, never the opened inode. The other
     * caller owns a private reflink. Both stay immutable during verification. */
    if (fstat(from, &st) || dcc_file_digest(from, st.st_size, hash)) return -1;
    return memcmp(hash, expected, DCC_FILE_HASH_SIZE) ? -1 : 0;
}

static int copy_snapshot(int from, int to, const unsigned char *expected)
{
    char buffer[65536];
    XXH3_state_t *xxh = NULL;
    int ret = -1;
    ssize_t n;
#ifdef __linux__
    if (!ioctl(to, FICLONE, from))
        return expected ? verify_snapshot(to, expected) : 0;
#endif
    if (lseek(from, 0, SEEK_SET) < 0) goto done;
    if (expected) {
        xxh = XXH3_createState();
        if (!xxh || XXH3_64bits_reset(xxh) != XXH_OK) goto done;
    }
    while ((n = read(from, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        if ((xxh && XXH3_64bits_update(xxh, buffer, n) != XXH_OK) ||
            dcc_writex(to, buffer, n)) goto done;
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
        if (hash && verify_snapshot(from, hash)) return 0;
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
    if (ok) accessed(from, &st);
done:
    if (from >= 0) close(from);
    close(shard);
    return ok;
}

struct entry { char path[20]; off_t size; struct timespec age; };

static int oldest_first(const void *left, const void *right)
{
    const struct timespec *a = &((const struct entry *)left)->age;
    const struct timespec *b = &((const struct entry *)right)->age;
    if (a->tv_sec != b->tv_sec) return (a->tv_sec > b->tv_sec) - (a->tv_sec < b->tv_sec);
    return (a->tv_nsec > b->tv_nsec) - (a->tv_nsec < b->tv_nsec);
}

/* Called only with the root quota locked, on first use or capacity pressure.
 * Publication is atomic; readers need neither the shard nor the quota lock. */
static off_t make_room(int root, off_t size)
{
    struct entry *entries = NULL;
    struct dirent *de;
    struct stat st;
    off_t bytes = 0;
    size_t count = 0, capacity = 0;
    off_t result = -1;
    DIR *dir = NULL;
    for (unsigned index = 0; index < CACHE_SHARDS; ++index) {
        char name[3];
        snprintf(name, sizeof(name), "%02x", index);
        /* Fresh open descriptions avoid sharing directory offsets after fork. */
        int shard = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (shard < 0) { if (errno == ENOENT) continue; goto done; }
        dir = fdopendir(shard);
        if (!dir) { close(shard); goto done; }
        while ((de = readdir(dir))) {
            /* Temporary names exist only inside the quota lock. Preparation
             * uses unnamed inodes, so these are abandoned publications. */
            if (!strncmp(de->d_name, ".tmp-", 5)) {
                if (unlinkat(shard, de->d_name, 0)) goto done;
                continue;
            }
            if (strlen(de->d_name) != 16 || strspn(de->d_name, "0123456789abcdef") != 16)
                continue;
            if (fstatat(shard, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) goto done;
            if (!S_ISREG(st.st_mode)) {
                if (unlinkat(shard, de->d_name, 0)) goto done;
                continue;
            }
            bytes += st.st_blocks * 512;
            /* Live read-only inputs remain shareable even above the budget. */
            if (st.st_nlink > 1) continue;
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 64;
                void *grown = realloc(entries, next * sizeof(*entries));
                if (!grown) goto done;
                entries = grown; capacity = next;
            }
            snprintf(entries[count].path, sizeof(entries[count].path), "%s/%.16s", name, de->d_name);
            entries[count].size = st.st_blocks * 512;
            entries[count].age = st.st_atim;
            if (st.st_mtim.tv_sec > st.st_atim.tv_sec ||
                (st.st_mtim.tv_sec == st.st_atim.tv_sec && st.st_mtim.tv_nsec > st.st_atim.tv_nsec))
                entries[count].age = st.st_mtim;
            ++count;
        }
        closedir(dir);
        dir = NULL;
    }
    if (count && bytes + size > CACHE_BYTES) {
        /* Reclaim at least one former shard's allowance to amortize scans. */
        off_t reserve = !size ? 0 : size > CACHE_BYTES / CACHE_SHARDS ? size : CACHE_BYTES / CACHE_SHARDS;
        off_t target = reserve < CACHE_BYTES ? CACHE_BYTES - reserve : 0;
        qsort(entries, count, sizeof(*entries), oldest_first);
        for (size_t i = 0; i < count && bytes > target; ++i) {
            if (fstatat(root, entries[i].path, &st, AT_SYMLINK_NOFOLLOW)) goto done;
            if (st.st_nlink > 1) continue;
            if (unlinkat(root, entries[i].path, 0)) goto done;
            bytes -= entries[i].size;
        }
    }
    result = bytes;
done:
    if (dir) closedir(dir);
    free(entries);
    return result;
}

/* All publications hold the root lock. Atomic replacement keeps both data
 * and its reservation intact across publisher death; missing quota is rebuilt.
 * Store a fixed-size counter, not a sparse extent subject to RLIMIT_FSIZE. */
static int publish_file(int directory, const char *name, int from)
{
    char source[64];
    const char *temporary = ".tmp-publish";
    snprintf(source, sizeof(source), "/proc/self/fd/%d", from);
    (void)unlinkat(directory, temporary, 0);
    if (linkat(AT_FDCWD, source, directory, temporary, AT_SYMLINK_FOLLOW)) return -1;
    int ret = renameat(directory, temporary, directory, name);
    (void)unlinkat(directory, temporary, 0);
    return ret;
}

static int quota_write(int root, off_t bytes)
{
    int fd = openat(root, ".", O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ret = dcc_writex(fd, &bytes, sizeof(bytes));
    if (!ret) ret = publish_file(root, ".quota", fd);
    close(fd);
    return ret;
}

static int quota_lock(int root, off_t *bytes)
{
    int fd = openat(root, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    *bytes = -1;
    int counter = openat(root, ".quota", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (counter >= 0) {
        unsigned char data[sizeof(*bytes) + 1];
        if (read(counter, data, sizeof(data)) == sizeof(*bytes)) memcpy(bytes, data, sizeof(*bytes));
        close(counter);
    }
    return fd;
}

static void publish_snapshot(int shard, const char *key, int from)
{
    struct stat st;
    off_t bytes;
    int root = openat(shard, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC), quota = -1;
    if (root < 0 || fstat(from, &st)) goto done;
    off_t size = st.st_blocks * 512;
    quota = quota_lock(root, &bytes);
    if (quota < 0) goto done;
    if (bytes < 0 || bytes + size > CACHE_BYTES) bytes = make_room(root, size);
    if (bytes < 0 || (bytes + size > CACHE_BYTES && !st.st_nlink) ||
        quota_write(root, bytes + size)) goto done;
    (void)publish_file(shard, key, from);
done:
    if (quota >= 0) close(quota);
    if (root >= 0) close(root);
}

void dcc_file_cache_put(const unsigned char *hash, const char *path, int shared, const char *tmp)
{
    char key[17];
    struct stat st, directory;
    int shard = -1, lock = -1, from = -1, to = -1;
    from = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (from < 0 || fstat(from, &st) || !S_ISREG(st.st_mode) || (!shared && st.st_size > CACHE_BYTES))
        goto done;
    shard = cache_shard(hash, key, tmp);
    if (shard < 0) goto done;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    /* The verified private input already belongs to the job. Cache publication
     * is optional even for shared inputs: blocking here can occupy every TCP
     * worker behind maintenance and prevent other jobs from reading the cache. */
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    to = openat(shard, key, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (to >= 0) {
        struct stat existing;
        int present = !fstat(to, &existing) && S_ISREG(existing.st_mode) &&
                      existing.st_size == st.st_size && !verify_snapshot(to, hash);
        close(to);
        to = -1;
        if (present) goto done;
    }
    if (fstat(shard, &directory)) goto done;
    if (shared && st.st_dev == directory.st_dev) {
        if (!fchmod(from, 0400)) publish_snapshot(shard, key, from);
        goto done;
    }
    if (st.st_size > CACHE_BYTES) goto done;
    to = openat(shard, ".", O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    if (to < 0) goto done;
    if (!copy_snapshot(from, to, NULL) && !fchmod(to, 0400))
        publish_snapshot(shard, key, to);
done:
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    if (lock >= 0) close(lock);
    if (shard >= 0) close(shard);
}

void dcc_file_cache_prune(const char *tmp)
{
    off_t bytes;
    int root = cache_root(tmp), quota;
    if (root < 0) return;
    quota = quota_lock(root, &bytes);
    /* RETRY cleanup shares the input-I/O workers. Never make retransmission
     * depend on another cache publisher releasing its maintenance lock. The
     * next successful publisher/pruner enforces the idle-cache bound. */
    if (quota >= 0 && (bytes < 0 || bytes > CACHE_BYTES)) {
        bytes = make_room(root, 0);
        if (bytes >= 0) (void)quota_write(root, bytes);
    }
    if (quota >= 0) close(quota);
    close(root);
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
static int record_lookup(struct dcc_record_cache *cache, const void *key, size_t key_size,
                          size_t *record_size)
{
    char name[17];
    struct stat st;
    int shard = record_shard(cache, key, key_size, name);
    if (shard < 0) return -1;
    int fd = openat(shard, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)sizeof(struct record_header) || st.st_size > CACHE_BYTES) {
        if (fd >= 0) close(fd);
        return -1;
    }
    *record_size = st.st_size;
    accessed(fd, &st);
    return fd;
}
static int record_contents(const void *record, size_t record_size, const void *key, size_t key_size,
                            const void **data, size_t *size)
{
    const char *storage = record;
    struct record_header header;
    if (record_size < sizeof(header)) return 0;
    memcpy(&header, storage, sizeof(header));
    if (header.key_size != key_size || header.size > CACHE_BYTES || key_size > CACHE_BYTES - sizeof(header) ||
        header.size > CACHE_BYTES - sizeof(header) - key_size ||
        record_size != sizeof(header) + key_size + header.size) return 0;
    if (memcmp(storage + sizeof(header), key, key_size) ||
        XXH3_64bits(storage + sizeof(header) + key_size, header.size) != header.digest) return 0;
    *data = storage + sizeof(header) + key_size;
    *size = header.size;
    return 1;
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
    char name[17];
    int shard = record_shard(cache, key, key_size, name), lock = -1, fd = -1;
    if (shard < 0) return;
    lock = openat(shard, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    fd = openat(shard, ".", O_WRONLY | O_TMPFILE | O_CLOEXEC, 0600);
    if (fd < 0) goto done;
    if (!write_record(fd, &header, key, data) && !fchmod(fd, 0400))
        publish_snapshot(shard, name, fd);
done:
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
}
const struct dcc_file_cache_api dcc_file_cache_records = {
    record_open, record_close, record_lookup, record_contents, record_write
};
