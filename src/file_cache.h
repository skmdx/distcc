/* Content-addressed, bounded server input cache. */
#ifndef DCC_FILE_CACHE_H
#define DCC_FILE_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DCC_FILE_HASH_SIZE 8
#define DCC_FILE_MANIFEST_LIMIT 65536
#define DCC_FILE_CACHE_MAX (256 * 1024 * 1024)
#define DCC_FILE_CACHE_SHARDS 64

int dcc_copy_snapshot(int from, int to, uint64_t size);
/* Hash the known extent [0, size) without changing the descriptor offset.
 * The caller owns a stable input; read errors and premature EOF return -1. */
int dcc_file_digest(int fd, off_t size, unsigned char hash[DCC_FILE_HASH_SIZE]);
/* One owner uses a cache handle at a time. Destination ancestors exist.
 * Shared inputs are immutable. Only verified content is published. */
struct dcc_record_cache;
struct dcc_record_cache *dcc_file_cache_open(const char *tmp);
void dcc_file_cache_close(struct dcc_record_cache *cache);
int dcc_file_cache_copy(const char *source, const char *destination, int shared);
int dcc_file_cache_get(struct dcc_record_cache *cache, const unsigned char *hash, uint64_t size,
                       const char *destination, int shared);
void dcc_file_cache_put(struct dcc_record_cache *cache, const unsigned char *hash, const char *path, int shared);
void dcc_file_cache_prune(struct dcc_record_cache *cache);

/* Versioned filesystem records share the same cache budget and publication
 * rules as content snapshots. A hit verifies both the full key and payload. */
struct dcc_file_cache_api {
    struct dcc_record_cache *(*open)(const char *);
    void (*close)(struct dcc_record_cache *);
    /* The caller owns the immutable record FD independently of this cache.
     * It chooses the buffer implementation, then validates the complete record
     * before exposing its payload. Atomic replacement never changes an open FD. */
    int (*lookup)(struct dcc_record_cache *, const void *, size_t, size_t *record_size);
    int (*contents)(const void *record, size_t record_size, const void *key, size_t key_size,
                    const void **data, size_t *size);
    void (*write)(struct dcc_record_cache *, const void *, size_t, const void *, size_t);
    /* An open-file-description lock for this record key. Close the returned
     * descriptor to release it, including when a compiling process exits.
     * A nonblocking attempt reports EAGAIN for an owned record. */
    int (*claim)(struct dcc_record_cache *, const void *, size_t, int wait);
};
extern const struct dcc_file_cache_api dcc_file_cache_records;

#endif
