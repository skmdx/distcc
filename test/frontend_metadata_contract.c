/* Exercise the real client filesystem replies and directory snapshot lifetime.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "frontend_fs.c"
#include <assert.h>

static int mutate, descriptor_stats, readable_directory_stats, canonicalizations;
char *__real_realpath(const char *, char *);
char *__wrap_realpath(const char *path, char *buffer)
{
    ++canonicalizations;
    return __real_realpath(path, buffer);
}
int __real_fstat(int, struct stat *);
int __wrap_fstat(int fd, struct stat *st)
{
    ++descriptor_stats;
    int result = __real_fstat(fd, st);
    if (!result && S_ISDIR(st->st_mode) && !(fcntl(fd, F_GETFL) & O_PATH))
        ++readable_directory_stats;
    if (!result && mutate && S_ISDIR(st->st_mode)) {
        mutate = 0;
        int child = openat(fd, "changed", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(child >= 0);
        assert(!close(child));
    }
    return result;
}

static void contents(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!close(fd));
}

static struct dcc_frontend_reply query(struct dcc_job *job, const char *path,
                                      unsigned operation, const unsigned char *previous)
{
    size_t length = strlen(path), size = 4 + length;
    if (operation == DCC_FS_LSTAT) size += 1 + DCC_FS_METADATA + sizeof("header");
    unsigned char *bytes = calloc(1, size);
    assert(bytes);
    dcc_tcp_put32(bytes, operation);
    memcpy(bytes + 4, path, length);
    if (operation == DCC_FS_LSTAT) {
        if (previous) memcpy(bytes + 5 + length, previous, DCC_FS_METADATA);
        memcpy(bytes + 5 + length + DCC_FS_METADATA, "header", sizeof("header"));
    }
    struct dcc_frontend_reply reply = {.fd = -1};
    assert(!dcc_frontend_fs_reply(job, bytes, size, &reply));
    free(bytes);
    assert(reply.size >= 8 && dcc_tcp_get32(reply.data) == 0);
    assert(dcc_tcp_get32(reply.data + 4) == reply.size + reply.file_size - 8);
    return reply;
}

static uint64_t header_size(const struct dcc_frontend_reply *reply)
{
    assert(reply->size >= 68 && reply->fd < 0 && !reply->file_size);
    unsigned count = dcc_tcp_get32(reply->data + 64);
    size_t offset = 68;
    for (unsigned i = 0; i < count; ++i) {
        assert(reply->size - offset >= 8);
        unsigned length = dcc_tcp_get32(reply->data + offset);
        unsigned error = dcc_tcp_get32(reply->data + offset + 4);
        offset += 8;
        assert(length <= reply->size - offset);
        int header = length == 6 && !memcmp(reply->data + offset, "header", 6);
        offset += length;
        if (!error) {
            assert(reply->size - offset >= DCC_FS_METADATA);
            if (header) return dcc_tcp_get64(reply->data + offset + 24);
            offset += DCC_FS_METADATA;
        } else assert(!header);
    }
    abort();
}

static void reuse_reply(struct dcc_job *job, const char *path)
{
    size_t size = 4 + strlen(path);
    unsigned char *request = malloc(size);
    assert(request);
    dcc_tcp_put32(request, DCC_FS_READ);
    memcpy(request + 4, path, size - 4);
    struct dcc_frontend_reply reply = {.fd = -1};
    unsigned char *storage = NULL;
    const char *versions[] = {"one", "a fresh longer file"};
    for (unsigned i = 0; i < 2; ++i) {
        contents(path, versions[i]);
        assert(!dcc_frontend_fs_reply(job, request, size, &reply));
        assert(!dcc_tcp_get32(reply.data) && reply.file_size == strlen(versions[i]));
        if (storage) assert(storage == reply.data);
        storage = reply.data;
        char bytes[32];
        assert(pread(reply.fd, bytes, reply.file_size, 0) == (ssize_t)reply.file_size);
        assert(!memcmp(bytes, versions[i], reply.file_size));
        int fd = reply.fd;
        dcc_frontend_fs_reset(&reply);
        assert(reply.data == storage && reply.capacity >= 60 && !reply.size && !reply.file_size);
        assert(reply.fd == -1 && fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    }
    assert(!dcc_frontend_fs_reply(job, request, size, &reply));
    int fd = reply.fd;
    assert(dcc_frontend_fs_reply(job, request, 4, &reply) == -1);
    assert(!reply.size && reply.fd == -1 && reply.data == storage);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    dcc_frontend_fs_clear(&reply);
    assert(!reply.data && !reply.capacity && !reply.size && reply.fd == -1);
    free(request);
}

static void authorization(const char *root)
{
    char link[PATH_MAX], physical[PATH_MAX], nested[PATH_MAX], leaf[PATH_MAX];
    snprintf(link, sizeof(link), "%s/alias", root);
    snprintf(physical, sizeof(physical), "%s/physical", root);
    snprintf(nested, sizeof(nested), "%s/physical/child", root);
    snprintf(leaf, sizeof(leaf), "%s/alias/child/header", root);
    assert(!mkdir(physical, 0755));
    assert(!symlink("physical", link));
    assert(!symlink("../view", nested));
    struct dcc_job job = {.cwd = (char *)root};
    assert(!dcc_frontend_fs_add_root(&job, leaf));
    assert(!dcc_frontend_fs_add_root(&job, "alias/child/another"));
    assert(allowed(&job, link) == PATH_METADATA);
    canonicalizations = 0;
    struct dcc_frontend_reply reply = query(&job, link, DCC_FS_READLINK, NULL);
    assert(canonicalizations == 1); // One hop, not one realpath per export.
    dcc_frontend_fs_clear(&reply);
    assert(allowed(&job, physical) == PATH_METADATA);
    assert(allowed(&job, nested) == PATH_METADATA);
    assert(!(allowed(&job, root) & PATH_CONTENTS));
    snprintf(leaf, sizeof(leaf), "%s/physical/child/header", root);
    assert(allowed(&job, leaf) == (PATH_METADATA | PATH_CONTENTS));
    reply = query(&job, nested, DCC_FS_READLINK, NULL);
    dcc_frontend_fs_clear(&reply);
    snprintf(leaf, sizeof(leaf), "%s/view/header", root);
    assert(allowed(&job, leaf) == (PATH_METADATA | PATH_CONTENTS));
    snprintf(leaf, sizeof(leaf), "%s/view/private", root);
    assert(!(allowed(&job, leaf) & PATH_CONTENTS));
    assert(!dcc_frontend_fs_add_root(&job, root));
    assert(dcc_argv_len(job.fs_roots) == 1 && !strcmp(job.fs_roots[0], root));
    assert(!dcc_frontend_fs_add_root(&job, leaf));
    assert(dcc_argv_len(job.fs_roots) == 1);
    dcc_free_argv(job.fs_roots);
}

static void empty_footprint(struct dcc_job *job, const char *path)
{
    unsigned char request[PATH_MAX + 5 + DCC_FS_METADATA] = {0};
    dcc_tcp_put32(request, DCC_FS_LSTAT);
    size_t n = strlen(path);
    memcpy(request + 4, path, n);
    struct stat st;
    assert(!stat(path, &st));
    /* Changed timestamp, known directory, no predicted child names. */
    encode_metadata(request + 5 + n, &st);
    request[5 + n + 16] ^= 1;
    struct dcc_frontend_reply reply = {.fd = -1};
    readable_directory_stats = 0;
    assert(!dcc_frontend_fs_reply(job, request, 5 + n + DCC_FS_METADATA, &reply));
    assert(!dcc_tcp_get32(reply.data));
    assert(reply.size == 68 && dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_PARTIAL);
    assert(!dcc_tcp_get32(reply.data + 64) && !readable_directory_stats);
    dcc_frontend_fs_clear(&reply);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    char *directory_path, *header, *saved;
    assert(asprintf(&directory_path, "%s/view", argv[1]) >= 0);
    assert(asprintf(&header, "%s/header", directory_path) >= 0);
    assert(asprintf(&saved, "%s/saved", argv[1]) >= 0);
    assert(!mkdir(directory_path, 0755));
    contents(header, "one");
    authorization(argv[1]);
    char *roots[] = {argv[1], NULL};
    struct dcc_job job = {.fs_roots = roots};
    empty_footprint(&job, directory_path);
    descriptor_stats = 0;
    struct dcc_frontend_reply reply = query(&job, directory_path, DCC_FS_ATTRIBUTES, NULL);
    assert(reply.size == 8 + DCC_FS_METADATA && !descriptor_stats && reply.fd == -1);
    assert(S_ISDIR(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_LSTAT, NULL);
    assert(dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_STABLE);
    assert(header_size(&reply) == 3);
    unsigned char previous[DCC_FS_METADATA];
    memcpy(previous, reply.data + 8, sizeof(previous));
    dcc_frontend_fs_clear(&reply);
    unsigned repeat = argc == 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 1;
    for (unsigned i = 0; i < repeat; ++i) {
        descriptor_stats = 0;
        readable_directory_stats = 0;
        reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
        assert(descriptor_stats == 1);
        assert(!readable_directory_stats);
        assert(dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_CACHED);
        assert(header_size(&reply) == 3);
        dcc_frontend_fs_clear(&reply);
    }
    if (argc == 3) return 0;
    reuse_reply(&job, header);
    unsigned char stdin_request[5] = {0, 0, 0, 0, '/'};
    dcc_tcp_put32(stdin_request, DCC_FS_STDIN);
    reply = (struct dcc_frontend_reply){.fd = -1};
    assert(!dcc_frontend_fs_reply(&job, stdin_request, sizeof(stdin_request), &reply));
    assert(dcc_tcp_get32(reply.data) == EBADF && reply.fd == -1 && !reply.file_size);
    dcc_frontend_fs_clear(&reply);
    job.stdin_fd = open(header, O_RDONLY | O_CLOEXEC) + 1;
    assert(job.stdin_fd);
    reply = (struct dcc_frontend_reply){.fd = -1};
    assert(!dcc_frontend_fs_reply(&job, stdin_request, sizeof(stdin_request), &reply));
    assert(!dcc_tcp_get32(reply.data) && reply.fd >= 0 && reply.fd != job.stdin_fd - 1);
    close(job.stdin_fd - 1); job.stdin_fd = 0;
    assert(fcntl(reply.fd, F_GETFD) >= 0);
    dcc_frontend_fs_clear(&reply);

    /* Cached directory names never freeze an existing child's attributes. */
    contents(header, "changed contents");
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    assert(dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_CACHED);
    assert(header_size(&reply) == strlen("changed contents"));
    dcc_frontend_fs_clear(&reply);

    /* A real namespace mutation after the initial descriptor snapshot must
     * prevent publication of a stable directory version. */
    mutate = 1;
    reply = query(&job, directory_path, DCC_FS_LSTAT, NULL);
    assert(!mutate && !dcc_tcp_get32(reply.data + 60));
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    /* A stale version refreshes the supplied lookup footprint. It does not
     * enumerate the newly added sibling until the compiler asks for it. */
    assert(dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_PARTIAL);
    assert(dcc_tcp_get32(reply.data + 64) == 1);
    memcpy(previous, reply.data + 8, sizeof(previous));
    dcc_frontend_fs_clear(&reply);

    /* Revoking listing permission still permits a known file in an
     * execute-only directory, with no cached listing disclosed. */
    assert(geteuid() != 0);
    assert(!chmod(directory_path, 0111));
    reply = query(&job, directory_path, DCC_FS_ATTRIBUTES, NULL);
    assert(reply.size == 60 && (dcc_tcp_get32(reply.data + 40) & 0777) == 0111);
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    assert(S_ISDIR(dcc_tcp_get32(reply.data + 40)));
    assert(dcc_tcp_get32(reply.data + 60) == DCC_FS_DIRECTORY_PARTIAL);
    assert(dcc_tcp_get32(reply.data + 64) == 1);
    assert(header_size(&reply) == strlen("changed contents"));
    assert((dcc_tcp_get32(reply.data + 40) & 0777) == 0111);
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, header, DCC_FS_READ, NULL);
    assert(reply.fd >= 0 && reply.file_size == strlen("changed contents"));
    dcc_frontend_fs_clear(&reply);
    /* Readable names without search permission cannot answer child lookups,
     * but an explicit directory enumeration remains legal. */
    assert(!chmod(directory_path, 0444));
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    assert(reply.size == 60 && S_ISDIR(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_LIST, NULL);
    assert(reply.size > 8);
    dcc_frontend_fs_clear(&reply);
    assert(!chmod(directory_path, 0755));

    /* A directory hint is not authority to follow a replacement symlink. */
    assert(!rename(directory_path, saved));
    assert(!symlink(saved, directory_path));
    reply = query(&job, directory_path, DCC_FS_ATTRIBUTES, NULL);
    assert(reply.size == 60 && S_ISLNK(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    assert(reply.size == 60 && S_ISLNK(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    reply = query(&job, directory_path, DCC_FS_STAT, NULL);
    assert(S_ISDIR(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    assert(!unlink(directory_path));
    contents(directory_path, "replacement");
    reply = query(&job, directory_path, DCC_FS_LSTAT, previous);
    assert(reply.size == 60 && S_ISREG(dcc_tcp_get32(reply.data + 40)));
    dcc_frontend_fs_clear(&reply);
    free(directory_path); free(header); free(saved);
    return 0;
}
