/* Client job description and descriptor-only broker exchange.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include "distcc.h"
#include "hosts.h"
#include "bulk.h"
#include "broker.h"
#include "job.h"
#include "tcp_job.h"
#include "job_worker.h"
#include "file_cache.h"
#include "include_server_if.h"
#include "exitcode.h"
#include "trace.h"

/* Cache hashes are optional. Recover the expansion size from chunk headers
 * when their sidecar has disappeared, without decompressing the snapshot. */
static int snapshot_size(const char *name, uint64_t wire, uint64_t *plain)
{
    unsigned char data[17];
    char *sidecar;
    uint64_t offset = 0;
    int fd, ret = -1;
    ssize_t n;
    if (asprintf(&sidecar, "%s.xxh3", name) < 0) return -1;
    fd = open(sidecar, O_RDONLY | O_CLOEXEC);
    free(sidecar);
    if (fd >= 0) {
        n = read(fd, data, sizeof(data));
        close(fd);
        if (n == 16) {
            *plain = 0;
            for (unsigned k = 8; k < 16; ++k) *plain = (*plain << 8) | data[k];
            return 0;
        }
    }
    fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    *plain = 0;
    while (offset <= wire && wire - offset >= 8) {
        uint32_t header[2], size, compressed;
        if (pread(fd, header, sizeof(header), offset) != sizeof(header)) break;
        offset += 8;
        size = ntohl(header[0]);
        compressed = ntohl(header[1]);
        if (!size && !compressed) { ret = offset == wire ? 0 : -1; break; }
        if (!size || size > DCC_STREAM_CHUNK || compressed >= size || *plain > (uint64_t)INT64_MAX - size) break;
        offset += compressed ? compressed : size;
        *plain += size;
    }
    close(fd);
    return ret;
}

static struct dcc_job *describe(char **argv, const char *cpp, char **files, int tcp)
{
    struct dcc_job *job = calloc(1, sizeof(*job));
    struct dcc_input_digest *digests = NULL;
    unsigned i;
    if (!job) return NULL;
    job->kind = files ? DCC_JOB_PUMP : DCC_JOB_COMPILE;
    job->cwd = getcwd(NULL, 0);
    if (!job->cwd || dcc_copy_argv(argv, &job->argv, 0)) goto failed;
    job->count = files ? dcc_argv_len(files) : 1;
    if (job->count > DCC_JOB_INPUT_MAX ||
        !(job->inputs = calloc(job->count ? job->count : 1, sizeof(*job->inputs)))) {
        job->count = 0;
        goto failed;
    }
    for (i = 0; i < job->count; ++i) job->inputs[i].fd = -1;
    if (files) digests = dcc_file_hashes(files, job->count);
    for (i = 0; i < job->count; ++i) {
        struct dcc_job_input *in = &job->inputs[i];
        struct stat st;
        const char *name = files ? files[i] : cpp;
        if (files) {
            if (dcc_get_original_fname(name, &in->path)) goto failed;
        } else {
            const char *extension = strrchr(cpp, '.');
            if (asprintf(&in->path, "/input%s", extension ? extension : ".i") < 0) goto failed;
        }
        if (files && str_endswith("/forcing_technique_271828", in->path)) {
            in->kind = DCC_JOB_EMPTY;
            continue;
        }
        if (lstat(name, &st)) goto failed;
        if (S_ISLNK(st.st_mode)) {
            char target[4097];
            ssize_t n = readlink(name, target, sizeof(target) - 1);
            if (n < 0 || n == sizeof(target) - 1) goto failed;
            target[n] = 0;
            in->target = strdup(target);
            if (!in->target) goto failed;
            in->kind = DCC_JOB_SYMLINK;
            continue;
        }
        if (!S_ISREG(st.st_mode)) goto failed;
        in->wire_size = st.st_size;
        in->plain_size = st.st_size;
        if (files) {
            if (snapshot_size(name, in->wire_size, &in->plain_size)) goto failed;
            in->encoding = DCC_JOB_LZ4;
            if (digests && digests[i].kind == DCC_INPUT_CACHED) {
                in->cached = 1;
                memcpy(in->digest, digests[i].hash, sizeof(in->digest));
            }
        } else {
            int encoding;
            in->fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (in->fd < 0 || (encoding = tcp ? DCC_JOB_RAW : dcc_job_compress(&in->fd, in->plain_size, &in->wire_size)) < 0)
                goto failed;
            in->encoding = encoding;
        }
    }
    free(digests);
    return job;
failed:
    free(digests);
    dcc_job_free(job);
    return NULL;
}
static int buffer_result(int fd, const struct dcc_job_result *result, unsigned index,
                         struct dcc_buffer *buffer)
{
    size_t done = 0;
    uint64_t size = result->size[index];
    if (result->compressed & (1U << index))
        return dcc_job_expand(fd, result->wire_size[index], size, -1, buffer);
    if (size > SIZE_MAX - 1) return EXIT_OUT_OF_MEMORY;
    buffer->data = malloc(size + 1);
    if (!buffer->data) return EXIT_OUT_OF_MEMORY;
    buffer->size = 0;
    while (done < size) {
        ssize_t n = pread(fd, buffer->data + done, size - done, done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { dcc_free_buffer(buffer); return EXIT_IO_ERROR; }
        done += n;
    }
    buffer->data[size] = 0;
    buffer->size = size;
    return 0;
}
/* Complete output is published atomically. A transport failure never replaces
 * a previous object with a prefix, and successful empty outputs still exist. */
static int publish(int fd, const struct dcc_job_result *result, unsigned index, const char *path)
{
    char *temporary = NULL, data[65536];
    int output = -1, ret = EXIT_IO_ERROR;
    uint64_t offset = 0;
    uint64_t size = result->size[index];
    mode_t mask;
    if (!path || !strcmp(path, "-") || !strcmp(path, "/dev/null")) return 0;
    if (asprintf(&temporary, "%s.distcc-XXXXXX", path) < 0) return EXIT_OUT_OF_MEMORY;
    if ((output = mkstemp(temporary)) < 0) goto done;
    mask = umask(0);
    umask(mask);
    if (fchmod(output, 0666 & ~mask)) goto done;
    if (result->compressed & (1U << index)) {
        ret = dcc_job_expand(fd, result->wire_size[index], size, output, NULL);
        if (ret) goto done;
        ret = EXIT_IO_ERROR;
        offset = size;
    }
    while (offset < size) {
        size_t amount = size - offset < sizeof(data) ? size - offset : sizeof(data);
        ssize_t n = pread(fd, data, amount, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || dcc_writex(output, data, n)) goto done;
        offset += n;
    }
    if (close(output)) { output = -1; goto done; }
    output = -1;
    if (rename(temporary, path)) goto done;
    ret = 0;
done:
    if (output >= 0) close(output);
    if (temporary) unlink(temporary);
    free(temporary);
    return ret;
}
int dcc_compile_direct(struct dcc_hostdef *host, char **argv, const char *cpp, char **files,
                       const char *output, const char *deps, struct dcc_buffer *err,
                       struct dcc_buffer *out, int *status)
{
    struct dcc_job *job = describe(argv, cpp, files, host->tcp_jobs);
    struct dcc_job_result result;
    unsigned i, cached = 0, transferred = 0;
    int metadata = -1, ret = EXIT_IO_ERROR, ended = 0;
    int artifacts[DCC_JOB_RESULT_COUNT] = {-1, -1, -1, -1};
    dcc_job_result_init(&result);
    if (host->tcp_jobs) {
        if (!job || (ret = dcc_tcp_exchange(host, job, files, cpp, &result))) goto done;
        for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) artifacts[i] = result.fd[i];
        goto publish_results;
    }
    if (!job || (metadata = dcc_job_encode(job)) < 0 || dcc_broker_submit(host, metadata)) goto done;
    for (i = 0; i < job->count; ++i) cached += job->inputs[i].cached;
    while (!ended) {
        struct pollfd p = {host->broker_lease, POLLIN, 0};
        struct dcc_job_message message;
        int fd = -1, ready;
        do { ready = poll(&p, 1, dcc_get_io_timeout() * 1000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0) goto done;
        if (dcc_recv_fd(host->broker_lease, &message, sizeof(message), &fd) != sizeof(message)) {
            if (fd >= 0) close(fd);
            goto done;
        }
        if (message.operation == DCC_JOB_NEED && fd < 0 && message.index < job->count &&
            job->inputs[message.index].kind == DCC_JOB_FILE) {
            int retained = job->inputs[message.index].fd;
            fd = retained >= 0 ? fcntl(retained, F_DUPFD_CLOEXEC, 0) :
                open(files ? files[message.index] : cpp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) goto done;
            transferred += job->inputs[message.index].cached;
            message.operation = DCC_JOB_INPUT;
            int sent = dcc_send_fd(host->broker_lease, &message, sizeof(message), fd);
            close(fd);
            if (sent) goto done;
        } else if (message.operation == DCC_JOB_ARTIFACT && fd >= 0 &&
                   message.index < DCC_JOB_RESULT_COUNT && artifacts[message.index] < 0) {
            artifacts[message.index] = fd;
        } else if (message.operation == DCC_JOB_FINISHED && !message.index && fd >= 0) {
            int invalid = dcc_job_result_decode(fd, &result);
            close(fd);
            if (invalid) goto done;
            ended = 1;
        } else {
            if (fd >= 0) close(fd);
            goto done;
        }
    }
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        struct stat st;
        if ((artifacts[i] < 0 && result.wire_size[i]) ||
            (artifacts[i] >= 0 && (fstat(artifacts[i], &st) ||
            (uint64_t)st.st_size != result.wire_size[i]))) goto done;
    }
publish_results:
    *status = result.status;
    rs_trace("%s job completed", host->tcp_jobs ? "pooled TCP" : "direct HTTP/2");
    if (cached) rs_trace("input cache: %u of %u entries transferred", transferred, job->count);
    if ((ret = buffer_result(artifacts[DCC_JOB_STDERR], &result, DCC_JOB_STDERR, err)) ||
        (ret = buffer_result(artifacts[DCC_JOB_STDOUT], &result, DCC_JOB_STDOUT, out))) goto done;
    if (!*status) {
        if ((ret = publish(artifacts[DCC_JOB_OBJECT], &result, DCC_JOB_OBJECT, output))) goto done;
        if (files) ret = publish(artifacts[DCC_JOB_DEPS], &result, DCC_JOB_DEPS, deps);
    } else if (dcc_argv_search(argv, "-E") && strcmp(output, "-")) {
        struct stat st;
        if (!stat(output, &st) && S_ISREG(st.st_mode)) unlink(output);
    }
done:
    if (host->tcp_jobs && !ret) dcc_broker_return(host, host->broker_fd);
    if (metadata >= 0) close(metadata);
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) if (artifacts[i] >= 0) close(artifacts[i]);
    dcc_job_free(job);
    return ret;
}
