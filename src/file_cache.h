/* Content-addressed, bounded server input cache. */
#ifndef DCC_FILE_CACHE_H
#define DCC_FILE_CACHE_H
#include <stddef.h>

#define DCC_FILE_HASH_SIZE 8
#define DCC_FILE_MANIFEST_LIMIT 65536
#define DCC_FILE_CACHE_MAX (4 * 1024 * 1024)
#define DCC_FILE_CACHE_SHARDS 64

enum dcc_input_kind { DCC_INPUT_OTHER, DCC_INPUT_CACHED, DCC_INPUT_INLINE };
struct dcc_input_digest {
    unsigned char hash[DCC_FILE_HASH_SIZE];
    enum dcc_input_kind kind;
};

struct dcc_input_digest *dcc_file_hashes(char **files, unsigned count);
/* Shared snapshots require read-only mounts or compiler-owned read-only
 * inputs (such as profile data). Otherwise return a
 * private inode. NULL hash requires an already verified, immutable source. */
int dcc_file_cache_copy(const unsigned char *hash, const char *source, const char *destination, int shared);
int dcc_file_cache_get(const unsigned char *hash, const char *destination, int shared, const char *tmp);
void dcc_file_cache_put(const unsigned char *hash, const char *path, int shared, const char *tmp);
void dcc_file_cache_prune(const unsigned char *hash, const char *tmp);

/* Versioned filesystem records share the same shard limits and publication
 * rules as content snapshots. A hit verifies both the full key and payload. */
struct dcc_file_cache_api {
    /* A hit returns a malloc-owned allocation, with an extra NUL after *size
     * payload bytes. The terminator is not part of the stored record/hash. */
    void *(*read)(const void *, size_t, size_t *, const char *);
    void (*write)(const void *, size_t, const void *, size_t, const char *);
};
extern const struct dcc_file_cache_api dcc_file_cache_records;

#endif
