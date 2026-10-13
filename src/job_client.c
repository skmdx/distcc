/* Submit typed jobs and transfer file bodies without local byte-stream IPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "distcc.h"
#include "exitcode.h"
#include "trace.h"
#include "job.h"
#include "job_client.h"

#define UPLOADS 4
enum operation { CREATE, INPUT, RESULT, OUTPUT };
struct request {
    struct request *next;
    struct dcc_client_job *job;
    enum operation operation;
    unsigned index;
    int id, fd, ended;
    uint64_t limit;
};
struct dcc_client_job {
    struct dcc_h2 *connection;
    int control, id, failed, finished, active, result_fd;
    struct dcc_job *description;
    struct dcc_job_result result;
    struct request *requests;
    unsigned char *wanted;
    unsigned next, remaining;
};
static int notify(struct dcc_client_job *j, unsigned op, unsigned index, int fd)
{
    struct dcc_job_message message = {op, index};
    return dcc_send_fd(j->control, &message, sizeof(message), fd);
}
static void fail(struct dcc_client_job *j)
{
    if (j->failed || j->finished) return;
    j->failed = 1;
    notify(j, DCC_JOB_FINISHED, EXIT_IO_ERROR, -1);
}
static int request(struct dcc_client_job *j, enum operation operation, unsigned index,
                    const char *method, const char *path, int input, uint64_t limit)
{
    struct request *r = calloc(1, sizeof(*r));
    struct dcc_h2_body body;
    struct dcc_h2_field field = { "content-encoding", DCC_JOB_CONTENT_ENCODING };
    int encoded = operation == INPUT && j->description->inputs[index].encoding == DCC_JOB_LZ4;
    struct stat st;
    if (!r) return -1;
    r->fd = -1;
    r->job = j;
    r->operation = operation;
    r->index = index;
    r->limit = limit;
    if (input >= 0) {
        if (fstat(input, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) goto failed;
        body = (struct dcc_h2_body){ .fd = input, .length = st.st_size };
    }
    r->id = dcc_h2_request(j->connection, method, path, encoded ? &field : NULL, encoded,
                         input >= 0 ? &body : NULL, r);
    if (r->id < 0) goto failed;
    r->next = j->requests;
    j->requests = r;
    if (operation == CREATE) j->id = r->id;
    return 0;
failed:
    free(r);
    return -1;
}
static int result_request(struct dcc_client_job *j)
{
    char path[64];
    snprintf(path, sizeof(path), "/jobs/%d/result", j->id);
    return request(j, RESULT, 0, "GET", path, -1, 512ULL * 1024 * 1024);
}
static int uploads(struct dcc_client_job *j)
{
    while (j->active < UPLOADS && j->next < j->description->count) {
        unsigned i = j->next++;
        if (!j->wanted[i]) continue;
        j->wanted[i] = 2; /* A requested local FD, not yet on the network. */
        ++j->active;
        if (notify(j, DCC_JOB_NEED, i, -1)) return -1;
    }
    return !j->remaining ? result_request(j) : 0;
}
static int finish(struct dcc_client_job *j)
{
    char path[64];
    unsigned i;
    j->result_fd = dcc_job_result_encode(&j->result);
    if (j->result_fd < 0) return -1;
    snprintf(path, sizeof(path), "/jobs/%d", j->id);
    if (dcc_h2_request(j->connection, "DELETE", path, NULL, 0, NULL, NULL) < 0) return -1;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i)
        if (j->result.fd[i] >= 0 && notify(j, DCC_JOB_ARTIFACT, i, j->result.fd[i])) return -1;
    if (notify(j, DCC_JOB_FINISHED, 0, j->result_fd)) return -1;
    j->finished = 1;
    return 0;
}
static int numeric_field(const struct dcc_h2_field *fields, size_t count, const char *name, uint64_t *out)
{
    const char *p = dcc_h2_field(fields, count, name);
    uint64_t n = 0;
    if (!p || !*p) return -1;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || n > ((uint64_t)INT64_MAX - (*p - '0')) / 10) return -1;
        n = n * 10 + *p - '0';
    }
    *out = n;
    return 0;
}
static int headers(struct dcc_h2 *c, int id, const struct dcc_h2_field *fields,
                    size_t count, void *context)
{
    struct request *r = context;
    const char *status = dcc_h2_field(fields, count, ":status");
    const char *expected;
    /* Detached DELETE responses own no body or client lifetime. */
    if (!r) return status && (!strcmp(status, "204") || !strcmp(status, "404")) ? 0 : -1;
    if (r->job->failed) return -1;
    expected = r->operation == CREATE ? "201" :
        r->operation == INPUT ? "204" : "200";
    if (!status || strcmp(status, expected)) {
        rs_log_warning("HTTP/2 job response: operation %d, status %s", r->operation, status ? status : "missing");
        return -1;
    }
    if (r->operation == CREATE) {
        const char *jobid = dcc_h2_field(fields, count, "x-distcc-job");
        char expected_id[24];
        snprintf(expected_id, sizeof(expected_id), "%d", id);
        if (!jobid || strcmp(jobid, expected_id)) return -1;
        r->job->id = id;
    }
    if (r->operation == RESULT) {
        const char *names[] = { "x-distcc-stderr-size", "x-distcc-stdout-size",
            "x-distcc-object-size", "x-distcc-deps-size" };
        struct dcc_job_result *result = &r->job->result;
        uint64_t value, total = 0;
        unsigned i;
        if (numeric_field(fields, count, "x-distcc-status", &value) || value > 65535) return -1;
        result->status = value;
        if (numeric_field(fields, count, "x-distcc-mode", &value) || value > 0777) return -1;
        result->mode = value;
        for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
            if (numeric_field(fields, count, names[i], &result->size[i]) ||
                result->size[i] > r->limit - total) return -1;
            total += result->size[i];
        }
        r->limit = result->size[DCC_JOB_OBJECT];
    }
    if (r->operation == RESULT || r->operation == OUTPUT) {
        struct dcc_job_result *result = &r->job->result;
        unsigned index = r->operation == RESULT ? DCC_JOB_OBJECT : r->index;
        const char *encoding = dcc_h2_field(fields, count, "content-encoding");
        uint64_t wire;
        if (numeric_field(fields, count, "content-length", &wire)) return -1;
        if (encoding) {
            if (strcmp(encoding, DCC_JOB_CONTENT_ENCODING) || wire < 8 || wire >= r->limit) return -1;
            result->compressed |= 1U << index;
        } else if (wire != r->limit) return -1;
        result->wire_size[index] = r->limit = wire;
    }
    r->fd = dcc_job_memfd("distcc-download");
    return r->fd < 0 ? -1 : dcc_h2_sink(c, id, r->fd, r->limit);
}
static int end_body(struct dcc_h2 *c, int id, void *context)
{
    struct request *r = context;
    (void)c; (void)id;
    if (!r) return 0;
    if (r->fd < 0 ||
        fcntl(r->fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) return -1;
    r->ended = 1;
    return 0;
}
static int complete(struct request *r)
{
    struct dcc_client_job *j = r->job;
    struct stat st;
    unsigned i;
    if (fstat(r->fd, &st)) return -1;
    switch (r->operation) {
    case CREATE:
        if ((uint64_t)st.st_size != j->description->count ||
            pread(r->fd, j->wanted, j->description->count, 0) != j->description->count) return -1;
        for (i = 0; i < j->description->count; ++i) {
            if (j->wanted[i] > 1 ||
                (j->wanted[i] && j->description->inputs[i].kind != DCC_JOB_FILE)) return -1;
            j->remaining += j->wanted[i];
        }
        return uploads(j);
    case INPUT:
        j->wanted[r->index] = 0;
        --j->active;
        --j->remaining;
        return uploads(j);
    case RESULT:
        if ((uint64_t)st.st_size != j->result.wire_size[DCC_JOB_OBJECT]) return -1;
        j->result.fd[DCC_JOB_OBJECT] = r->fd;
        r->fd = -1;
        j->remaining = 0;
        for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
            char path[80];
            if (i == DCC_JOB_OBJECT || !j->result.size[i]) continue;
            snprintf(path, sizeof(path), "/jobs/%d/outputs/%u", j->id, i);
            if (request(j, OUTPUT, i, "GET", path, -1, j->result.size[i])) return -1;
            ++j->remaining;
        }
        return j->remaining ? 0 : finish(j);
    case OUTPUT:
        if ((uint64_t)st.st_size != j->result.wire_size[r->index]) return -1;
        j->result.fd[r->index] = r->fd;
        r->fd = -1;
        return --j->remaining ? 0 : finish(j);
    }
    return -1;
}
static void closed(struct dcc_h2 *c, int id, unsigned error, void *context)
{
    struct request *r = context, **p;
    struct dcc_client_job *j;
    (void)c; (void)id;
    if (!r) return;
    j = r->job;
    for (p = &j->requests; *p != r; p = &(*p)->next) {}
    *p = r->next;
    if (!j->failed && (error || !r->ended || complete(r))) {
        rs_log_warning("HTTP/2 job request failed: operation %d, stream %d, error %u, ended %d",
                       r->operation, id, error, r->ended);
        fail(j);
    }
    if (r->fd >= 0) close(r->fd);
    free(r);
}
const struct dcc_h2_events dcc_job_client_events = { headers, end_body, closed };

struct dcc_client_job *dcc_job_client_start(struct dcc_h2 *c, int control, int metadata)
{
    struct dcc_client_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->connection = c;
    j->control = control;
    j->result_fd = -1;
    dcc_job_result_init(&j->result);
    if (dcc_job_decode(metadata, &j->description)) goto failed;
    j->wanted = calloc(j->description->count ? j->description->count : 1, 1);
    if (!j->wanted || request(j, CREATE, 0, "POST", "/jobs", metadata, j->description->count)) goto failed;
    return j;
failed:
    dcc_job_client_free(j);
    return NULL;
}
void dcc_job_client_input(struct dcc_client_job *j, const struct dcc_job_message *message, int fd)
{
    char path[80];
    struct stat st;
    unsigned index = message->index;
    if (j->failed || j->finished) goto done;
    if (message->operation != DCC_JOB_INPUT || index >= j->description->count ||
        j->wanted[index] != 2 || fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != j->description->inputs[index].wire_size) {
        fail(j);
        goto done;
    }
    snprintf(path, sizeof(path), "/jobs/%d/inputs/%u", j->id, index);
    j->wanted[index] = 3;
    if (request(j, INPUT, index, "PUT", path, fd, 0)) fail(j);
done:
    if (fd >= 0) close(fd);
}
void dcc_job_client_disconnected(struct dcc_client_job *j)
{
    if (!j) return;
    j->connection = NULL;
    fail(j);
}
void dcc_job_client_free(struct dcc_client_job *j)
{
    struct request *r;
    if (!j) return;
    while ((r = j->requests)) {
        j->requests = r->next;
        if (j->connection) {
            dcc_h2_set_context(j->connection, r->id, NULL);
            dcc_h2_cancel(j->connection, r->id);
        }
        if (r->fd >= 0) close(r->fd);
        free(r);
    }
    if (j->connection && j->id && !j->finished) {
        char path[64];
        snprintf(path, sizeof(path), "/jobs/%d", j->id);
        dcc_h2_request(j->connection, "DELETE", path, NULL, 0, NULL, NULL);
    }
    if (j->result_fd >= 0) close(j->result_fd);
    dcc_job_result_free(&j->result);
    dcc_job_free(j->description);
    free(j->wanted);
    free(j);
}
