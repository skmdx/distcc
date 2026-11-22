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
#include <xxhash.h>
#include <arpa/inet.h>
#include "distcc.h"
#include "hosts.h"
#include "bulk.h"
#include "broker.h"
#include "job.h"
#include "tcp_job.h"
#include "file_cache.h"
#include "exitcode.h"
#include "trace.h"
#include "workload.h"
#include "resource.h"

struct dcc_job *dcc_job_describe(char **argv, const char *cpp, int cpp_fd)
{
    char *source, *output;
    struct dcc_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->kind = DCC_JOB_COMPILE;
    job->cwd = getcwd(NULL, 0);
    if (!job->cwd || dcc_scan_args(argv, &source, &output, &job->argv, 0)) goto failed;
    struct dcc_job_input *in = job->inputs = calloc(1, sizeof(*in));
    if (!in) goto failed;
    job->count = 1;
    in->fd = cpp_fd >= 0 ? fcntl(cpp_fd, F_DUPFD_CLOEXEC, 0) :
                           open(cpp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (in->fd < 0 || fstat(in->fd, &st) || !S_ISREG(st.st_mode)) goto failed;
    in->plain_size = st.st_size;
    if (dcc_is_preprocessed(source) || dcc_argv_search(argv, "-x")) {
        if (asprintf(&in->path, "/%s", dcc_find_basename(source)) < 0) goto failed;
    } else {
        const char *ext = cpp_fd >= 0 ? dcc_preproc_exten(dcc_find_extension(source)) : strrchr(cpp, '.');
        if (asprintf(&in->path, "/input%s", ext ? ext : ".i") < 0) goto failed;
    }
    return job;
failed:
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
    /* The command-line client keeps its inherited mask for its lifetime. */
    static mode_t mask = (mode_t)-1;
    if (mask == (mode_t)-1) {
        mask = umask(0);
        umask(mask);
    }
    if (fchmod(*source, 0666 & ~mask)) return EXIT_IO_ERROR;
    int failed = close(*source);
    *source = -1;
    return failed || rename(result->temporary[index], path) ? EXIT_IO_ERROR : 0;
}
/* Auxiliary files are read by code generation after preprocessing. Hold the
 * opened generation through retries; the receiver verifies the content hash. */
static int auxiliary_inputs(struct dcc_job *job, char **paths)
{
    for (char **p = paths; p && *p; ++p) {
        unsigned i;
        for (i = 0; i < job->count; ++i)
            if (!strcmp(job->inputs[i].path, *p)) break;
        if (i < job->count) continue;
        if (job->count == DCC_JOB_INPUT_MAX) return -1;
        void *allocation = realloc(job->inputs, (job->count + 1) * sizeof(*job->inputs));
        if (!allocation) return -1;
        job->inputs = allocation;
        struct dcc_job_input *in = &job->inputs[job->count++];
        *in = (struct dcc_job_input){ .kind = DCC_JOB_READONLY, .fd = -1 };
        struct stat st;
        in->path = strdup(*p);
        in->fd = open(*p, O_RDONLY | O_CLOEXEC);
        if (!in->path || in->fd < 0 || fstat(in->fd, &st) || !S_ISREG(st.st_mode)) return -1;
        in->plain_size = st.st_size;
        if (dcc_file_digest(in->fd, st.st_size, in->digest)) return -1;
        in->cached = 1;
    }
    return 0;
}
int dcc_compile_direct(struct dcc_hostdef *host, struct dcc_job *prepared, char **argv, const char *cpp, int cpp_fd, char **auxiliary,
                       const char *output, const char *deps, struct dcc_buffer *err,
                       struct dcc_buffer *out, int *status)
{
    struct dcc_job *job = prepared ? prepared : dcc_job_describe(argv, cpp, cpp_fd);
    struct dcc_job_result result;
    int ret = EXIT_IO_ERROR, transport_complete = 0;
    dcc_job_result_init(&result);
    /* Input metadata survives worker selection and retries. Final driver
     * arguments can add dependency targets after selection. */
    if (prepared && !dcc_job_frontend(prepared)) {
        dcc_free_argv(job->argv);
        job->argv = NULL;
        if (dcc_copy_argv(argv, &job->argv, 0)) goto done;
    }
    if (!job || auxiliary_inputs(job, auxiliary)) goto done;
    ret = dcc_tcp_exchange(host, job, NULL, cpp, output, deps, &result);
    if (ret == EXIT_BUSY) dcc_broker_defer(host);
    if (ret) goto done;
    /* COMPLETE ends the network transaction; publication is local work. */
    dcc_broker_return(host, host->broker_fd, &result);
    transport_complete = 1;
    *status = result.status;
    rs_trace("pooled TCP job completed");
    if ((ret = buffer_result(result.fd[DCC_JOB_STDERR], &result, DCC_JOB_STDERR, err)) ||
        (ret = buffer_result(result.fd[DCC_JOB_STDOUT], &result, DCC_JOB_STDOUT, out))) goto done;
    if (!*status) {
        if ((ret = publish(&result.fd[DCC_JOB_OBJECT], &result, DCC_JOB_OBJECT, output))) goto done;
        if (dcc_job_frontend(job))
            ret = publish(&result.fd[DCC_JOB_DEPS], &result, DCC_JOB_DEPS, deps);
    } else if (dcc_argv_search(argv, "-E") && strcmp(output, "-")) {
        struct stat st;
        if (!stat(output, &st) && S_ISREG(st.st_mode)) unlink(output);
    }
done:
    dcc_job_result_free(&result);
    if (!prepared) dcc_job_free(job);
    return transport_complete && ret ? DCC_REMOTE_LOCAL_ERROR : ret;
}

/* Planners own immutable numbered snapshots. Scheduling, caching, compression,
 * cancellation and connection reuse stay on the ordinary path. queued_fd is
 * consumed after enqueueing, or closed on an earlier failure. */
int dcc_file_transfer(const char *metadata, const char *snapshots, char **outputs, int queued_fd)
{
    struct dcc_job *job = NULL;
    struct dcc_job_result result;
    struct dcc_work_hint hint = {0};
    char **files = NULL;
    unsigned output_count = 0;
    int ret = EXIT_IO_ERROR;
    int fd = open(metadata, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint32_t estimate[3];
    dcc_job_result_init(&result);
    if (fd < 0) goto done;
    ret = dcc_readx(fd, estimate, sizeof(estimate));
    if (!ret) ret = dcc_job_decode(fd, sizeof(estimate), &job);
    close(fd);
    if (ret || (job->kind != DCC_JOB_THINLTO && job->kind != DCC_JOB_OFFLOAD_LINK) ||
        !job->count) { ret = EXIT_PROTOCOL_ERROR; goto done; }
    hint.kind = job->kind == DCC_JOB_OFFLOAD_LINK ? DCC_WORK_OFFLOAD_LINK : DCC_WORK_THINLTO;
    hint.bytes = dcc_resource_input_bytes(job);
    hint.memory_class = dcc_resource_class(job);
    hint.key = (uint64_t)ntohl(estimate[0]) << 32 | ntohl(estimate[1]);
    uint64_t key = hint.key ? 0 : XXH3_64bits(job->cwd, strlen(job->cwd));
    for (unsigned i = 0; i < job->count; ++i) {
        const struct dcc_job_input *input = &job->inputs[i];
        if (!hint.key) {
            key = XXH3_64bits_withSeed(input->path, strlen(input->path) + 1, key);
            key = XXH3_64bits_withSeed(input->digest, sizeof(input->digest), key);
        }
        const char *extension = strrchr(input->path, '.');
        if (extension && (!strcmp(extension, ".rs") || job->kind == DCC_JOB_OFFLOAD_LINK)) {
            uint64_t room = UINT32_MAX - hint.work_units;
            hint.work_units += input->plain_size < room ? input->plain_size : room;
        }
    }
    if (!hint.key) hint.key = key ? key : 1;
    if (job->kind == DCC_JOB_THINLTO) hint.work_units = ntohl(estimate[2]);
    files = calloc(job->count + 1, sizeof(*files));
    if (!files) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    for (unsigned i = 0; i < job->count; ++i) {
        if (asprintf(&files[i], "%s/%u", snapshots, i) < 0) {
            files[i] = NULL; ret = EXIT_OUT_OF_MEMORY; goto done;
        }
    }
    while (outputs[output_count]) ++output_count;
    if (output_count != dcc_argv_len(job->llvm_outputs)) { ret = EXIT_BAD_ARGUMENTS; goto done; }
    job->destinations = calloc(2 * output_count + 1, sizeof(char *));
    if (!job->destinations) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    for (unsigned i = 0; i < output_count; ++i) {
        char *name;
        if (asprintf(&name, "%u", i) < 0) { ret = EXIT_OUT_OF_MEMORY; goto done; }
        job->destinations[2 * i] = name;
        job->destinations[2 * i + 1] = outputs[i];
    }
    ret = dcc_transfer_prepared(job, files, NULL, &hint, queued_fd, &result);
    queued_fd = -1;
    if (ret) goto done;
    for (unsigned i = DCC_JOB_STDERR; i <= DCC_JOB_STDOUT; ++i) {
        struct dcc_buffer buffer = {0};
        if ((ret = buffer_result(result.fd[i], &result, i, &buffer))) goto done;
        ret = dcc_writex(i == DCC_JOB_STDERR ? STDERR_FILENO : STDOUT_FILENO,
                         buffer.data, buffer.size);
        dcc_free_buffer(&buffer);
        if (ret) goto done;
    }
    if (!result.status) {
        if (result.named_count != output_count) { ret = EXIT_PROTOCOL_ERROR; goto done; }
        for (unsigned i = 0; i < output_count; ++i) {
            struct dcc_named_output *f = result.named + i;
            if (rename(f->temporary, f->destination)) { ret = EXIT_IO_ERROR; goto done; }
            free(f->temporary);
            f->temporary = NULL;
        }
    }
    ret = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
done:
    if (queued_fd >= 0) close(queued_fd);
    dcc_job_result_free(&result);
    if (files) { for (unsigned i = 0; job && i < job->count; ++i) free(files[i]); free(files); }
    if (job && job->destinations) {
        for (unsigned i = 0; i < output_count; ++i) free(job->destinations[2 * i]);
        free(job->destinations);
    }
    dcc_job_free(job);
    return ret < 0 ? EXIT_PROTOCOL_ERROR : ret;
}

int dcc_transfer_prepared(struct dcc_job *job, char **files, const char *output,
                          const struct dcc_work_hint *hint, int queued_fd, struct dcc_job_result *result)
{
    unsigned failures = 0;
    struct dcc_hostdef *host = NULL;
    int lease = -1, ret;
    for (;;) {
        if ((ret = dcc_broker_acquire(&host, &lease, 1, 0, hint, &queued_fd))) goto done;
        if (!host->tcp_jobs) { ret = EXIT_PROTOCOL_ERROR; goto done; }
        dcc_broker_cpp_done(host);
        ret = dcc_tcp_exchange(host, job, files, NULL, output, NULL, result);
        if (!ret) break;
        if (job->metadata_published) { dcc_broker_failed(host); goto done; }
        /* A pooled connection can close between acquisition and BEGIN. Job
         * inputs are immutable and no result has been published: retry once
         * through a fresh reservation, including when local fallback is off. */
        if (ret == EXIT_BUSY) dcc_broker_defer(host);
        else {
            dcc_broker_failed(host);
            if (failures++ || ret != EXIT_IO_ERROR) goto done;
            rs_log_warning("retrying file job on a fresh connection");
        }
        dcc_job_result_free(result);
        dcc_job_result_init(result);
        dcc_free_hostdef(host); host = NULL;
        if (lease >= 0) dcc_broker_release(lease);
        lease = -1;
    }
    dcc_broker_return(host, host->broker_fd, result);
    if (!result->status && getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
        fprintf(stderr, "%s: compiled on %s\n",
                dcc_job_helper(job), host->hostname);
done:
    if (queued_fd >= 0) close(queued_fd);
    if (host) dcc_free_hostdef(host);
    if (lease >= 0) dcc_broker_release(lease);
    return ret < 0 ? EXIT_PROTOCOL_ERROR : ret;
}
