/* Content-addressed, bounded server input cache. */
#ifndef DCC_FILE_CACHE_H
#define DCC_FILE_CACHE_H
#include <stddef.h>
#include <sys/types.h>

#define DCC_FILE_HASH_SIZE 8
#define DCC_FILE_MANIFEST_LIMIT 65536
#define DCC_FILE_CACHE_MAX (256 * 1024 * 1024)
#define DCC_FILE_CACHE_SHARDS 64

enum dcc_input_kind { DCC_INPUT_OTHER, DCC_INPUT_CACHED, DCC_INPUT_INLINE };
struct dcc_input_digest {
    unsigned char hash[DCC_FILE_HASH_SIZE];
    enum dcc_input_kind kind;
};

struct dcc_input_digest *dcc_file_hashes(char **files, unsigned count);
/* Hash the known extent [0, size) without changing the descriptor offset.
 * The caller owns a stable input; read errors and premature EOF return -1. */
int dcc_file_digest(int fd, off_t size, unsigned char hash[DCC_FILE_HASH_SIZE]);
/* Shared snapshots require read-only mounts or compiler-owned read-only
 * inputs (such as profile data). Otherwise return a
 * private inode. NULL hash requires an already verified, immutable source. */
int dcc_file_cache_copy(const unsigned char *hash, const char *source, const char *destination, int shared);
int dcc_file_cache_get(const unsigned char *hash, const char *destination, int shared, const char *tmp);
void dcc_file_cache_put(const unsigned char *hash, const char *path, int shared, const char *tmp);
void dcc_file_cache_prune(const char *tmp);

/* Versioned filesystem records share the same cache budget and publication
 * rules as content snapshots. A hit verifies both the full key and payload. */
struct dcc_record_cache;
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
