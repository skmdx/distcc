/* Real shared-input publication, pinning and pooled-capacity reclamation. */
#include "../src/file_cache.c"
#include <assert.h>

const char *rs_program_name = "file_cache";

static void input(const char *path, off_t size, unsigned char hash[DCC_FILE_HASH_SIZE])
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0 && !posix_fallocate(fd, 0, size));
    void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(data != MAP_FAILED);
    XXH64_canonical_t digest;
    XXH64_canonicalFromHash(&digest, XXH3_64bits(data, size));
    memcpy(hash, digest.digest, DCC_FILE_HASH_SIZE);
    assert(!munmap(data, size) && !close(fd));
}

static int cached(const unsigned char hash[DCC_FILE_HASH_SIZE], const char *root)
{
    char key[17];
    int shard = cache_shard(hash, key, root);
    assert(shard >= 0);
    int found = !faccessat(shard, key, F_OK, 0);
    close(shard);
    return found;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !chdir(argv[1]));
    assert(!setenv("DISTCCD_FILE_CACHE", "cache", 1));
    unsigned char small[DCC_FILE_HASH_SIZE], large[DCC_FILE_HASH_SIZE];
    input("small", 8*1024*1024, small);
    dcc_file_cache_put(small, "small", 1, ".");
    assert(cached(small, "."));
    assert(!unlink("small"));
    dcc_file_cache_prune(".");
    assert(cached(small, ".")); /* No per-shard 4 MiB eviction. */
    assert(dcc_file_cache_get(small, "small", 1, "."));
    input("large", DCC_FILE_CACHE_MAX, large);
    dcc_file_cache_put(large, "large", 1, ".");
    dcc_file_cache_prune(".");
    assert(cached(small, ".") && cached(large, ".")); /* Both pinned. */
    assert(!unlink("small"));
    dcc_file_cache_prune(".");
    assert(!cached(small, ".") && cached(large, "."));
    assert(!unlink("large"));
    dcc_file_cache_prune(".");
    assert(cached(large, ".")); /* Exactly the total budget fits. */
    int root = cache_root(".");
    off_t reserved;
    int quota = quota_lock(root, &reserved);
    assert(quota >= 0 && reserved == DCC_FILE_CACHE_MAX);
    close(quota); close(root);
    puts("file cache: large idle input, active pins and aggregate eviction passed");
    return 0;
}
