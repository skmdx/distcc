/* Content-addressed, bounded server input cache. */
#ifndef DCC_FILE_CACHE_H
#define DCC_FILE_CACHE_H

#define DCC_FILE_HASH_SIZE 8
#define DCC_FILE_MANIFEST_LIMIT 65536
#define DCC_FILE_CACHE_MAX (4 * 1024 * 1024)

enum dcc_input_kind { DCC_INPUT_OTHER, DCC_INPUT_CACHED, DCC_INPUT_INLINE };
struct dcc_input_digest {
    unsigned char hash[DCC_FILE_HASH_SIZE];
    enum dcc_input_kind kind;
};

struct dcc_input_digest *dcc_file_hashes(char **files, unsigned count);
int dcc_file_cache_lock(const unsigned char **hashes, unsigned count, int input, int *lock);
/* NULL hash is allowed only for a verified source held immutable until copy
 * completion. The destination always gets a private inode. */
int dcc_file_cache_copy(const unsigned char *hash, const char *source, const char *destination);
int dcc_file_cache_get(const unsigned char *hash, const char *destination);
int dcc_file_cache_verify(const unsigned char *hash, const char *path);
void dcc_file_cache_put(const unsigned char *hash, const char *path);

#endif
