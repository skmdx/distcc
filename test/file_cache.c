/* Real shared-input publication, pinning and pooled-capacity reclamation. */
#include <config.h>
#include <sys/types.h>
#include <unistd.h>
static ssize_t test_pread(int, void *, size_t, off_t);
#define pread test_pread
#include "../src/file_cache.c"
#undef pread
#include "../src/remote_job.c"
#include <assert.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>

const char *rs_program_name = "file_cache";

static int partial_reads, interrupted, truncate_next;
static ssize_t test_pread(int fd, void *buffer, size_t length, off_t offset)
{
    if (partial_reads) {
        if (!interrupted++) { errno = EINTR; return -1; }
        if (length > 997) length = 997;
    }
    if (truncate_next && offset >= 65536) {
        assert(!ftruncate(fd, 0));
        truncate_next = 0;
    }
    return pread(fd, buffer, length, offset);
}

static void digest_contract(void)
{
    unsigned char data[2*65536 + 17], hash[DCC_FILE_HASH_SIZE];
    const off_t sizes[] = {0, 1, 65535, 65536, 65537, sizeof(data)};
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = i*131 + i/257;
    int fd = open("hash-source", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
        off_t size = sizes[i];
        assert(!ftruncate(fd, size) && pwrite(fd, data, size, 0) == size);
        XXH64_canonical_t expected;
        XXH64_canonicalFromHash(&expected, XXH3_64bits(data, size));
        for (partial_reads = 0; partial_reads < 2; ++partial_reads) {
            interrupted = 0;
            assert(lseek(fd, 3, SEEK_SET) == 3);
            assert(!dcc_file_digest(fd, size, hash));
            assert(!memcmp(hash, expected.digest, sizeof(hash)));
            assert(lseek(fd, 0, SEEK_CUR) == 3);
            assert(!verify_snapshot(fd, expected.digest));
            hash[0] ^= 1;
            assert(verify_snapshot(fd, hash) == -1);
        }
        partial_reads = 0;
        struct dcc_job job = {0};
        char path[] = "hash-source";
        char *paths[] = {path, path, NULL};
        assert(!auxiliary_inputs(&job, paths) && job.count == 1);
        assert(job.inputs[0].plain_size == (uint64_t)size && job.inputs[0].cached);
        assert(!memcmp(job.inputs[0].digest, expected.digest, sizeof(hash)));
        assert(!auxiliary_inputs(&job, paths) && job.count == 1);
        assert(lseek(job.inputs[0].fd, 0, SEEK_CUR) == 0);
        close(job.inputs[0].fd); free(job.inputs[0].path); free(job.inputs);
    }
    truncate_next = 1;
    assert(dcc_file_digest(fd, sizeof(data), hash) == -1 && errno == EIO);
    assert(!truncate_next);
    assert(dcc_file_digest(-1, 1, hash) == -1 && errno == EBADF);
    assert(!close(fd) && !unlink("hash-source"));
}

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

static void claims_contract(void)
{
    struct dcc_record_cache *cache = record_open(".");
    assert(cache);
    int ready[2];
    assert(!pipe(ready));
    pid_t owner = fork();
    assert(owner >= 0);
    if (!owner) {
        int lock = record_claim(cache, "action", 6, 1);
        assert(lock >= 0 && (fcntl(lock, F_GETFD) & FD_CLOEXEC));
        assert(write(ready[1], "R", 1) == 1);
        for (;;) pause();
    }
    char value;
    assert(read(ready[0], &value, 1) == 1 && value == 'R');
    assert(record_claim(cache, "action", 6, 0) == -1 && errno == EAGAIN);
    int other = record_claim(cache, "different", 9, 0);
    assert(other >= 0 && !close(other));
    pid_t waiter = fork();
    assert(waiter >= 0);
    if (!waiter) {
        int lock = record_claim(cache, "action", 6, 1);
        assert(lock >= 0);
        _exit(0); /* Exiting, as well as close/exec, releases the claim. */
    }
    assert(!kill(owner, SIGTERM));
    int status;
    assert(waitpid(owner, &status, 0) == owner && WIFSIGNALED(status));
    assert(waitpid(waiter, &status, 0) == waiter && WIFEXITED(status) && !WEXITSTATUS(status));
    int lock = record_claim(cache, "action", 6, 0);
    assert(lock >= 0 && !close(lock));
    close(ready[0]); close(ready[1]); record_close(cache);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !chdir(argv[1]));
    digest_contract();
    assert(!setenv("DISTCCD_FILE_CACHE", "cache", 1));
    claims_contract();
    unsigned char small[DCC_FILE_HASH_SIZE], large[DCC_FILE_HASH_SIZE];
    input("small", 8*1024*1024, small);
    dcc_file_cache_put(small, "small", 1, ".");
    assert(cached(small, "."));
    assert(!unlink("small"));
    dcc_file_cache_prune(".");
    assert(cached(small, ".")); /* No per-shard 4 MiB eviction. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {65536, 65536}, core = {0, 0};
        assert(!setrlimit(RLIMIT_CORE, &core) && !setrlimit(RLIMIT_FSIZE, &limit));
        signal(SIGXFSZ, SIG_DFL);
        unsigned char hash[DCC_FILE_HASH_SIZE];
        input("tiny", 4096, hash);
        dcc_file_cache_put(hash, "tiny", 1, ".");
        assert(cached(hash, ".") && !unlink("tiny"));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
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
