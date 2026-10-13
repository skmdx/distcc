/* Materialize input snapshots and execute typed jobs in existing workers.
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
#include <sys/mman.h>
#include "distcc.h"
#include "bulk.h"
#include "exitcode.h"
#include "file_cache.h"
#include "job.h"
#include "job_worker.h"

struct input {
    char *path;
    const struct dcc_job_input *description;
    struct input *alias;
    unsigned index;
};
static int by_path(const void *a, const void *b)
{
    const struct input *x = *(const struct input *const *)a, *y = *(const struct input *const *)b;
    return strcmp(x->path, y->path);
}
static int by_digest(const void *a, const void *b)
{
    const struct input *x = *(const struct input *const *)a, *y = *(const struct input *const *)b;
    return memcmp(x->description->digest, y->description->digest, DCC_FILE_HASH_SIZE);
}
static int send_message(int channel, unsigned operation, unsigned index, int fd)
{
    struct dcc_job_message message = { operation, index };
    return dcc_send_fd(channel, &message, sizeof(message), fd) ? EXIT_IO_ERROR : 0;
}
static int receive_input(int channel, unsigned count, unsigned char *wanted, int *fd, unsigned *index)
{
    struct dcc_job_message message;
    struct pollfd p = { channel, POLLIN, 0 };
    int ready;
    *fd = -1;
    do { ready = poll(&p, 1, dcc_get_io_timeout() * 1000); } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return ready ? EXIT_IO_ERROR : EXIT_TIMEOUT;
    if (dcc_recv_fd(channel, &message, sizeof(message), fd) != sizeof(message) ||
        message.operation != DCC_JOB_INPUT || message.index >= count ||
        !wanted[message.index] || *fd < 0) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        return EXIT_PROTOCOL_ERROR;
    }
    *index = message.index;
    return 0;
}
static int materialize(int fd, const struct input *in)
{
    const struct dcc_job_input *entry = in->description;
    struct stat st;
    int ret;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != entry->wire_size || lseek(fd, 0, SEEK_SET) < 0)
        return EXIT_PROTOCOL_ERROR;
    if (entry->encoding == DCC_JOB_LZ4) {
        int output = open(in->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (output < 0) return EXIT_IO_ERROR;
        ret = dcc_r_lz4_stream_bounded(output, NULL, fd, entry->plain_size);
        if (close(output) && !ret) ret = EXIT_IO_ERROR;
    } else {
        /* Local preprocessor inputs and link archives are uncompressed. */
        if (entry->wire_size > UINT32_MAX) return EXIT_PROTOCOL_ERROR;
        ret = dcc_r_file(fd, in->path, entry->wire_size, DCC_COMPRESS_NONE);
    }
    if (ret) return ret;
    if ((uint64_t)lseek(fd, 0, SEEK_CUR) != entry->wire_size) return EXIT_PROTOCOL_ERROR;
    if (stat(in->path, &st) || (uint64_t)st.st_size != entry->plain_size)
        return EXIT_PROTOCOL_ERROR;
    if (entry->cached) {
        if (!dcc_file_cache_verify(entry->digest, in->path)) return EXIT_PROTOCOL_ERROR;
        dcc_file_cache_put(entry->digest, in->path);
    }
    return 0;
}
static int receive_inputs(int channel, const struct dcc_job *job, const char *root)
{
    struct input *inputs = calloc(job->count ? job->count : 1, sizeof(*inputs));
    struct input **ordered = calloc(job->count ? job->count : 1, sizeof(*ordered));
    const unsigned char **hashes = calloc(job->count ? job->count : 1, sizeof(*hashes));
    unsigned char *wanted = calloc(job->count ? job->count : 1, 1);
    unsigned i, used = 0, missing = 0;
    int ret = EXIT_OUT_OF_MEMORY, bitmap = -1, lock = -1;
    if (!inputs || !ordered || !hashes || !wanted) goto done;
    for (i = 0; i < job->count; ++i) {
        inputs[i].description = &job->inputs[i];
        inputs[i].index = i;
        if (asprintf(&inputs[i].path, "%s%s", root, job->inputs[i].path) < 0) goto done;
        ordered[i] = &inputs[i];
    }
    qsort(ordered, job->count, sizeof(*ordered), by_path);
    for (i = 0; i < job->count; ++i) {
        if (i && !strcmp(ordered[i - 1]->path, ordered[i]->path)) { ret = EXIT_PROTOCOL_ERROR; goto done; }
        /* No input file can be the parent of another input. In particular,
         * never write an upload through a client-supplied symlink. */
        if (i && !strncmp(ordered[i]->path, ordered[i - 1]->path, strlen(ordered[i - 1]->path)) &&
            ordered[i]->path[strlen(ordered[i - 1]->path)] == '/') { ret = EXIT_PROTOCOL_ERROR; goto done; }
    }
    for (i = 0; i < job->count; ++i) {
        const struct dcc_job_input *in = &job->inputs[i];
        if ((ret = dcc_mk_tmp_ancestor_dirs(inputs[i].path)) ||
            (ret = dcc_add_cleanup(inputs[i].path))) goto done;
        if (in->kind == DCC_JOB_FILE) {
            if (in->cached) ordered[used++] = &inputs[i];
            else wanted[i] = 1;
        }
    }
    qsort(ordered, used, sizeof(*ordered), by_digest);
    for (i = 0; i < used; ++i) {
        struct input *in = ordered[i];
        if (i && !memcmp(ordered[i - 1]->description->digest, in->description->digest, DCC_FILE_HASH_SIZE)) {
            in->alias = ordered[i - 1]->alias ? ordered[i - 1]->alias : ordered[i - 1];
        } else if (!dcc_file_cache_get(in->description->digest, in->path)) {
            wanted[in->index] = 1;
            hashes[missing++] = in->description->digest;
        }
    }
    if ((ret = dcc_file_cache_lock(hashes, missing, channel, &lock))) goto done;
    if (lock >= 0) for (i = 0; i < job->count; ++i)
        if (wanted[i] && inputs[i].description->cached &&
            dcc_file_cache_get(inputs[i].description->digest, inputs[i].path)) wanted[i] = 0;
    bitmap = dcc_job_memfd("distcc-missing");
    if (bitmap < 0) { ret = EXIT_IO_ERROR; goto done; }
    if (dcc_writex(bitmap, wanted, job->count) ||
        fcntl(bitmap, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ||
        (ret = send_message(channel, DCC_JOB_NEED, job->count, bitmap))) {
        ret = EXIT_IO_ERROR;
        goto done;
    }
    missing = 0;
    for (i = 0; i < job->count; ++i) missing += wanted[i];
    while (missing) {
        int fd;
        unsigned index;
        if ((ret = receive_input(channel, job->count, wanted, &fd, &index))) goto done;
        ret = materialize(fd, &inputs[index]);
        close(fd);
        if (ret) goto done;
        wanted[index] = 0;
        --missing;
        if ((ret = send_message(channel, DCC_JOB_INPUT_DONE, index, -1))) goto done;
    }
    if (lock >= 0) { close(lock); lock = -1; }
    /* Install links after all regular files: no upload can follow a link into
     * the host filesystem, even when the compiler's default include mirror
     * intentionally references host toolchain directories. */
    for (i = 0; i < job->count; ++i) {
        const struct dcc_job_input *in = inputs[i].description;
        if (inputs[i].alias && !dcc_file_cache_copy(in->digest, inputs[i].alias->path, inputs[i].path)) {
            ret = EXIT_IO_ERROR;
            goto done;
        }
        if (in->kind == DCC_JOB_EMPTY) {
            int fd = open(inputs[i].path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0) { ret = EXIT_IO_ERROR; goto done; }
            close(fd);
        } else if (in->kind == DCC_JOB_SYMLINK) {
            char *target = NULL;
            if (in->target[0] == '/') {
                if (asprintf(&target, "%s%s", root, in->target) < 0) { ret = EXIT_OUT_OF_MEMORY; goto done; }
            } else target = strdup(in->target);
            if (!target || symlink(target, inputs[i].path)) ret = EXIT_IO_ERROR;
            free(target);
            if (ret) goto done;
        }
    }
    ret = 0;
done:
    if (lock >= 0) close(lock);
    if (bitmap >= 0) close(bitmap);
    if (inputs) for (i = 0; i < job->count; ++i) free(inputs[i].path);
    free(inputs);
    free(ordered);
    free(hashes);
    free(wanted);
    return ret;
}
int dcc_job_worker(int channel, int metadata, int *result_fd)
{
    struct dcc_job *job = NULL;
    struct dcc_job_result result;
    char *root = NULL, *canonical;
    unsigned i;
    int ret = EXIT_PROTOCOL_ERROR, reply = -1;
    *result_fd = -1;
    dcc_job_result_init(&result);
    if (dcc_job_decode(metadata, &job)) goto done;
    if ((ret = dcc_get_new_tmpdir(&root))) goto done;
    canonical = realpath(root, NULL);
    if (!canonical) { ret = EXIT_IO_ERROR; goto done; }
    free(root);
    root = canonical;
    if ((ret = receive_inputs(channel, job, root)) ||
        (ret = dcc_execute_job(job, root, channel, &result))) goto done;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        int encoding = dcc_job_compress(&result.fd[i], result.size[i], &result.wire_size[i]);
        if (encoding < 0) { ret = EXIT_IO_ERROR; goto done; }
        if (encoding == DCC_JOB_LZ4) result.compressed |= 1U << i;
        if (result.fd[i] >= 0 && (ret = send_message(channel, DCC_JOB_ARTIFACT, i, result.fd[i]))) goto done;
    }
    reply = dcc_job_result_encode(&result);
    if (reply < 0) { ret = EXIT_IO_ERROR; goto done; }
    *result_fd = reply;
    reply = -1;
    ret = 0;
done:
    if (reply >= 0) close(reply);
    dcc_job_result_free(&result);
    dcc_job_free(job);
    free(root);
    dcc_cleanup_tempfiles();
    return ret;
}
