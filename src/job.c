/* Bounded job representations; no legacy RPC tokens or payload staging.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "job.h"
#include "file_cache.h"
#include "distcc.h"
#include "exitcode.h"

struct metadata {
    unsigned char *data;
    size_t size, offset, capacity;
    int error;
};
int dcc_job_memfd(const char *name)
{
    return memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
}
int dcc_job_compress(int *fd, uint64_t plain, uint64_t *wire)
{
    int encoded;
    struct stat st;
    *wire = plain;
    if (!plain) return DCC_JOB_RAW;
    encoded = dcc_job_memfd("distcc-lz4");
    if (encoded < 0) return -1;
    if (lseek(*fd, 0, SEEK_SET) < 0 || dcc_x_lz4_stream(encoded, *fd, plain) ||
        fstat(encoded, &st) ||
        fcntl(encoded, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) {
        close(encoded);
        return -1;
    }
    if ((uint64_t)st.st_size >= plain) {
        close(encoded);
        return DCC_JOB_RAW;
    }
    close(*fd);
    *fd = encoded;
    *wire = st.st_size;
    return DCC_JOB_LZ4;
}
int dcc_job_expand(int fd, uint64_t wire, uint64_t plain, int output,
                   struct dcc_buffer *buffer)
{
    off_t start = output >= 0 ? lseek(output, 0, SEEK_CUR) : 0;
    int ret;
    if (start < 0 || lseek(fd, 0, SEEK_SET) < 0) return EXIT_IO_ERROR;
    ret = dcc_r_lz4_stream_bounded(output, buffer, fd, plain);
    if (!ret && ((uint64_t)lseek(fd, 0, SEEK_CUR) != wire ||
        (output >= 0 ? (uint64_t)(lseek(output, 0, SEEK_CUR) - start) : buffer->size) != plain))
        ret = EXIT_PROTOCOL_ERROR;
    if (ret && buffer) dcc_free_buffer(buffer);
    return ret;
}
static void put(struct metadata *m, const void *data, size_t size)
{
    size_t needed = m->size + size, capacity;
    unsigned char *next;
    if (m->error) return;
    if (size > DCC_JOB_METADATA_MAX - m->size) { m->error = 1; return; }
    if (needed > m->capacity) {
        capacity = m->capacity ? m->capacity : 4096;
        while (capacity < needed) capacity *= 2;
        if (!(next = realloc(m->data, capacity))) { m->error = 1; return; }
        m->data = next;
        m->capacity = capacity;
    }
    if (size) memcpy(m->data + m->size, data, size);
    m->size = needed;
}
static void number(struct metadata *m, uint64_t n)
{
    unsigned char bytes[8];
    int i;
    for (i = 7; i >= 0; --i) { bytes[i] = n & 255; n >>= 8; }
    put(m, bytes, sizeof(bytes));
}
static void string(struct metadata *m, const char *s)
{
    size_t size = s ? strlen(s) : 0;
    number(m, size);
    put(m, s, size);
}
static const unsigned char *take(struct metadata *m, size_t size)
{
    const unsigned char *p;
    if (m->error || size > m->size - m->offset) { m->error = 1; return NULL; }
    p = m->data + m->offset;
    m->offset += size;
    return p;
}
static uint64_t get_number(struct metadata *m)
{
    const unsigned char *p = take(m, 8);
    uint64_t n = 0;
    int i;
    if (p) for (i = 0; i < 8; ++i) n = (n << 8) | p[i];
    return n;
}
static char *get_string(struct metadata *m)
{
    uint64_t size = get_number(m);
    const unsigned char *p;
    char *s;
    if (m->error || size > DCC_JOB_METADATA_MAX) { m->error = 1; return NULL; }
    p = take(m, size);
    if (!p || memchr(p, 0, size)) { m->error = 1; return NULL; }
    s = malloc(size + 1);
    if (!s) { m->error = 1; return NULL; }
    memcpy(s, p, size);
    s[size] = 0;
    return s;
}
static int publish(struct metadata *m)
{
    int fd = -1;
    size_t done = 0;
    if (m->error || (fd = dcc_job_memfd("distcc-metadata")) < 0) goto failed;
    while (done < m->size) {
        ssize_t n = write(fd, m->data + done, m->size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        done += n;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) goto failed;
    free(m->data);
    return fd;
failed:
    if (fd >= 0) close(fd);
    free(m->data);
    return -1;
}
static int load(int fd, struct metadata *m)
{
    struct stat st;
    size_t done = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 8 ||
        st.st_size > DCC_JOB_METADATA_MAX) return -1;
    m->size = st.st_size;
    if (!(m->data = malloc(m->size))) return -1;
    while (done < m->size) {
        ssize_t n = pread(fd, m->data + done, m->size - done, done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(m->data); m->data = NULL; return -1; }
        done += n;
    }
    return 0;
}
void dcc_job_free(struct dcc_job *job)
{
    unsigned i;
    if (!job) return;
    if (job->argv) for (i = 0; job->argv[i]; ++i) free(job->argv[i]);
    free(job->argv);
    for (i = 0; i < job->count; ++i) {
        free(job->inputs[i].path);
        free(job->inputs[i].target);
        if (job->inputs[i].fd >= 0) close(job->inputs[i].fd);
    }
    free(job->inputs);
    free(job->cwd);
    free(job);
}
void dcc_job_result_init(struct dcc_job_result *r)
{
    unsigned i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) r->fd[i] = -1;
}
void dcc_job_result_free(struct dcc_job_result *r)
{
    unsigned i;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i)
        if (r->fd[i] >= 0) { close(r->fd[i]); r->fd[i] = -1; }
}
int dcc_job_encode(const struct dcc_job *job)
{
    struct metadata m = {0};
    unsigned argc = 0, i;
    if (!job || job->kind > DCC_JOB_LINK || job->count > DCC_JOB_INPUT_MAX ||
        (job->count && !job->inputs) || !job->argv || !job->cwd) return -1;
    while (job->argv[argc]) if (++argc > DCC_JOB_INPUT_MAX) return -1;
    number(&m, job->kind);
    number(&m, argc);
    number(&m, job->count);
    string(&m, job->cwd);
    for (i = 0; i < argc; ++i) string(&m, job->argv[i]);
    for (i = 0; i < job->count; ++i) {
        const struct dcc_job_input *in = &job->inputs[i];
        number(&m, in->kind);
        number(&m, in->encoding);
        number(&m, in->cached);
        number(&m, in->wire_size);
        number(&m, in->plain_size);
        put(&m, in->digest, sizeof(in->digest));
        string(&m, in->path);
        string(&m, in->target);
    }
    return publish(&m);
}
static int absolute_path(const char *s)
{
    return s && s[0] == '/' && !strstr(s, "/../") && !strstr(s, "/./") &&
        !strstr(s, "//") && (strlen(s) < 3 || strcmp(s + strlen(s) - 3, "/..")) &&
        (strlen(s) < 2 || strcmp(s + strlen(s) - 2, "/."));
}
int dcc_job_decode(int fd, struct dcc_job **out)
{
    struct metadata m = {0};
    struct dcc_job *job = NULL;
    uint64_t kind, argc, count;
    unsigned i;
    *out = NULL;
    if (load(fd, &m)) return -1;
    kind = get_number(&m);
    argc = get_number(&m);
    count = get_number(&m);
    if (m.error || kind > DCC_JOB_LINK || !argc || argc > DCC_JOB_INPUT_MAX ||
        count > DCC_JOB_INPUT_MAX || !(job = calloc(1, sizeof(*job)))) goto failed;
    job->kind = kind;
    if (!(job->argv = calloc(argc + 1, sizeof(*job->argv))) ||
        (count && !(job->inputs = calloc(count, sizeof(*job->inputs))))) goto failed;
    job->count = count;
    for (i = 0; i < count; ++i) job->inputs[i].fd = -1;
    job->cwd = get_string(&m);
    if (!absolute_path(job->cwd)) goto failed;
    for (i = 0; i < argc; ++i) if (!(job->argv[i] = get_string(&m))) goto failed;
    for (i = 0; i < count; ++i) {
        struct dcc_job_input *in = &job->inputs[i];
        uint64_t type = get_number(&m), encoding = get_number(&m), cached = get_number(&m);
        const unsigned char *digest;
        in->wire_size = get_number(&m);
        in->plain_size = get_number(&m);
        digest = take(&m, sizeof(in->digest));
        if (m.error || type > DCC_JOB_EMPTY || encoding > DCC_JOB_LZ4 || cached > 1 ||
            in->wire_size > INT64_MAX || in->plain_size > INT64_MAX) goto failed;
        in->kind = type;
        in->encoding = encoding;
        in->cached = cached;
        memcpy(in->digest, digest, sizeof(in->digest));
        in->path = get_string(&m);
        in->target = get_string(&m);
        if (!absolute_path(in->path) || !strcmp(in->path, "/") ||
            in->path[strlen(in->path) - 1] == '/' || !in->target ||
            (in->kind != DCC_JOB_SYMLINK && *in->target) ||
            (in->kind != DCC_JOB_FILE && (in->cached || in->wire_size || in->plain_size)) ||
            (in->kind == DCC_JOB_SYMLINK && !*in->target) ||
            (in->kind == DCC_JOB_FILE && in->encoding == DCC_JOB_RAW && in->wire_size != in->plain_size) ||
            (in->cached && in->plain_size > DCC_FILE_CACHE_MAX)) goto failed;
    }
    if (m.error || m.offset != m.size) goto failed;
    free(m.data);
    *out = job;
    return 0;
failed:
    free(m.data);
    dcc_job_free(job);
    return -1;
}
int dcc_job_result_encode(const struct dcc_job_result *r)
{
    struct metadata m = {0};
    unsigned i;
    if (r->status < 0 || r->mode < 0 || r->mode > 0777) return -1;
    number(&m, r->status);
    number(&m, r->mode);
    number(&m, r->compressed);
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) number(&m, r->size[i]);
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) number(&m, r->wire_size[i]);
    return publish(&m);
}
int dcc_job_result_decode(int fd, struct dcc_job_result *r)
{
    struct metadata m = {0};
    uint64_t status, mode, compressed;
    unsigned i;
    int ret = -1;
    dcc_job_result_init(r);
    if (load(fd, &m)) return -1;
    status = get_number(&m);
    mode = get_number(&m);
    compressed = get_number(&m);
    if (status > 65535 || mode > 0777 || compressed >= (1U << DCC_JOB_RESULT_COUNT)) goto done;
    r->status = status;
    r->mode = mode;
    r->compressed = compressed;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        r->size[i] = get_number(&m);
        if (r->size[i] > INT64_MAX) goto done;
    }
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        r->wire_size[i] = get_number(&m);
        if (compressed & (1U << i)) {
            if (r->wire_size[i] < 8 || r->wire_size[i] >= r->size[i]) goto done;
        } else if (r->wire_size[i] != r->size[i]) goto done;
    }
    if (!m.error && m.offset == m.size) ret = 0;
done:
    free(m.data);
    return ret;
}
