/* Client job description and prepared TCP exchange.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>
#include "distcc.h"
#include "hosts.h"
#include "bulk.h"
#include "broker.h"
#include "job.h"
#include "tcp_job.h"
#include "file_cache.h"
#include "include_server_if.h"
#include "exitcode.h"
#include "trace.h"
#include "workload.h"

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

struct dcc_job *dcc_job_describe(char **argv, const char *cpp, int cpp_fd, char **files)
{
    struct dcc_job *job = calloc(1, sizeof(*job));
    struct dcc_input_digest *digests = NULL;
    char *source, *output;
    unsigned i;
    if (!job) return NULL;
    job->kind = files ? DCC_JOB_PUMP : DCC_JOB_COMPILE;
    job->cwd = getcwd(NULL, 0);
    if (!job->cwd || dcc_scan_args(argv, &source, &output, &job->argv, 0)) goto failed;
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
            const char *extension = cpp_fd >= 0 ?
                dcc_preproc_exten(dcc_find_extension(source)) : strrchr(cpp, '.');
            /* Preprocessed files can omit line markers; explicit-language
             * input may still contain macros. Both need the source basename
             * for compiler file symbols and diagnostics. */
            if (dcc_is_preprocessed(source) || dcc_argv_search(argv, "-x")) {
                if (asprintf(&in->path, "/%s", dcc_find_basename(source)) < 0) goto failed;
            } else if (asprintf(&in->path, "/input%s", extension ? extension : ".i") < 0) goto failed;
        }
        if (files && str_endswith("/forcing_technique_271828", in->path)) {
            in->kind = DCC_JOB_EMPTY;
            continue;
        }
        if ((!files && cpp_fd >= 0) ? fstat(cpp_fd, &st) : lstat(name, &st)) goto failed;
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
            in->fd = cpp_fd >= 0 ? fcntl(cpp_fd, F_DUPFD_CLOEXEC, 0) :
                open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (in->fd < 0) goto failed;
            in->encoding = DCC_JOB_RAW;
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
static int publish(int *source, struct dcc_job_result *result, unsigned index, const char *path)
{
    if (!path || !strcmp(path, "-") || !strcmp(path, "/dev/null")) return 0;
    if (!result->temporary[index]) {
        if (asprintf(&result->temporary[index], "%s.distcc-XXXXXX", path) < 0) {
            result->temporary[index] = NULL; return EXIT_OUT_OF_MEMORY;
        }
        int destination = mkstemp(result->temporary[index]);
        if (destination < 0) return EXIT_IO_ERROR;
        off_t offset = 0;
        while ((uint64_t)offset < result->size[index]) {
            uint64_t left = result->size[index] - offset;
            ssize_t copied = sendfile(destination, *source, &offset,
                left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK);
            if (copied < 0 && errno == EINTR) continue;
            if (copied <= 0) { close(destination); return EXIT_IO_ERROR; }
        }
        close(*source); *source = destination;
    }
    mode_t mask = umask(0);
    umask(mask);
    if (fchmod(*source, 0666 & ~mask)) return EXIT_IO_ERROR;
    int failed = close(*source);
    *source = -1;
    return failed || rename(result->temporary[index], path) ? EXIT_IO_ERROR : 0;
}
int dcc_compile_direct(struct dcc_hostdef *host, struct dcc_job *prepared, char **argv, const char *cpp, int cpp_fd, char **files,
                       const char *output, const char *deps, struct dcc_buffer *err,
                       struct dcc_buffer *out, int *status)
{
    struct dcc_job *job = prepared ? prepared : dcc_job_describe(argv, cpp, cpp_fd, files);
    struct dcc_job_result result;
    int ret = EXIT_IO_ERROR, transport_complete = 0, need_cpp = 0;
    dcc_job_result_init(&result);
    /* Input metadata survives worker selection and retries. Final driver
     * arguments can add dependency targets after selection. */
    if (prepared) {
        dcc_free_argv(job->argv);
        job->argv = NULL;
        if (dcc_copy_argv(argv, &job->argv, 0)) goto done;
    }
    if (!job || (ret = dcc_tcp_exchange(host, job, files, cpp, output, deps, &result))) goto done;
    need_cpp = files && WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_LOCAL_CPP;
    /* No compiler ran: do not train the scheduler on header verification. */
    if (need_cpp) result.execute_ns = 0;
    /* COMPLETE ends the network transaction; publication is local work. */
    dcc_broker_return(host, host->broker_fd, &result);
    transport_complete = 1;
    *status = result.status;
    rs_trace("pooled TCP job completed");
    if ((ret = buffer_result(result.fd[DCC_JOB_STDERR], &result, DCC_JOB_STDERR, err)) ||
        (ret = buffer_result(result.fd[DCC_JOB_STDOUT], &result, DCC_JOB_STDOUT, out))) goto done;
    if (!*status) {
        if ((ret = publish(&result.fd[DCC_JOB_OBJECT], &result, DCC_JOB_OBJECT, output))) goto done;
        if (files) ret = publish(&result.fd[DCC_JOB_DEPS], &result, DCC_JOB_DEPS, deps);
    } else if (!need_cpp && dcc_argv_search(argv, "-E") && strcmp(output, "-")) {
        struct stat st;
        if (!stat(output, &st) && S_ISREG(st.st_mode)) unlink(output);
    }
done:
    dcc_job_result_free(&result);
    if (!prepared) dcc_job_free(job);
    return transport_complete && ret ? DCC_REMOTE_LOCAL_ERROR : ret;
}

/* The Rust planner owns immutable numbered snapshots. Scheduling, caching,
 * compression, cancellation and connection reuse stay on the ordinary path. */
int dcc_rust_transfer(const char *metadata, const char *snapshots, const char *output)
{
    struct dcc_job *job = NULL;
    struct dcc_hostdef *host = NULL;
    struct dcc_job_result result;
    struct dcc_work_hint hint = {0};
    char **files = NULL;
    int lease = -1, ret = EXIT_IO_ERROR;
    int fd = open(metadata, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    dcc_job_result_init(&result);
    if (fd < 0) goto done;
    ret = dcc_job_decode(fd, &job);
    close(fd);
    if (ret || job->kind != DCC_JOB_RUST || !job->count) { ret = EXIT_PROTOCOL_ERROR; goto done; }
    files = calloc(job->count + 1, sizeof(*files));
    if (!files) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    for (unsigned i = 0; i < job->count; ++i) {
        if (asprintf(&files[i], "%s/%u", snapshots, i) < 0) {
            files[i] = NULL; ret = EXIT_OUT_OF_MEMORY; goto done;
        }
    }
    if ((ret = dcc_broker_acquire(&host, &lease, 1, &hint))) goto done;
    if (!host->tcp_jobs) { ret = EXIT_PROTOCOL_ERROR; goto done; }
    dcc_broker_cpp_done(host);
    if ((ret = dcc_tcp_exchange(host, job, files, NULL, output, NULL, &result))) {
        dcc_broker_failed(host);
        goto done;
    }
    dcc_broker_return(host, host->broker_fd, &result);
    for (unsigned i = DCC_JOB_STDERR; i <= DCC_JOB_STDOUT; ++i) {
        struct dcc_buffer buffer = {0};
        if ((ret = buffer_result(result.fd[i], &result, i, &buffer))) goto done;
        ret = dcc_writex(i == DCC_JOB_STDERR ? STDERR_FILENO : STDOUT_FILENO,
                        buffer.data, buffer.size);
        dcc_free_buffer(&buffer);
        if (ret) goto done;
    }
    if (!result.status && (ret = publish(&result.fd[DCC_JOB_OBJECT], &result,
                                         DCC_JOB_OBJECT, output))) goto done;
    ret = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
    if (!ret && getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
        fprintf(stderr, "distcc-rust: compiled on %s\n", host->hostname);
done:
    dcc_job_result_free(&result);
    if (host) dcc_free_hostdef(host);
    if (lease >= 0) close(lease);
    if (files) { for (unsigned i = 0; job && i < job->count; ++i) free(files[i]); free(files); }
    dcc_job_free(job);
    return ret < 0 ? EXIT_PROTOCOL_ERROR : ret;
}
