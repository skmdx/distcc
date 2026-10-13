/* Content-addressed, bounded server input cache. */
#ifndef DCC_FILE_CACHE_H
#define DCC_FILE_CACHE_H

#define DCC_FILE_HASH_SIZE 32
#define DCC_FILE_MANIFEST_LIMIT 65536

struct dcc_input_digest {
    unsigned char sha256[DCC_FILE_HASH_SIZE];
    int regular;
};

struct dcc_input_digest *dcc_file_hashes(char **files, unsigned count);
int dcc_file_cache_get(const unsigned char *hash, const char *destination);
int dcc_file_cache_verify(const unsigned char *hash, const char *path);
void dcc_file_cache_put(const unsigned char *hash, const char *path);

#endif
