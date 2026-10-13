/* HTTP job/input/result resources. No compiler payload crosses local IPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "distcc.h"
#include "job.h"
#include "job_server.h"

#define STORAGE_LIMIT (512ULL * 1024 * 1024)
enum operation { CREATE, INPUT, RESULT, OUTPUT, RELEASE };
struct request {
    struct request *next;
    struct dcc_server_job *job;
    int id, fd, answered;
    unsigned index;
    enum operation operation;
    uint64_t reserved;
};
struct dcc_server_job {
    struct dcc_server_job *next;
    struct dcc_job_server *server;
    struct dcc_job *description;
    int id, worker, failed, finished;
    int artifacts[DCC_JOB_RESULT_COUNT];
    struct dcc_job_result outcome;
    unsigned char *wanted;
    int *uploads;
    uint64_t retained;
    time_t activity;
};
struct dcc_job_server {
    struct dcc_h2 *connection;
    struct dcc_job_dispatch dispatch;
    void *context;
    struct dcc_server_job *jobs;
    struct request *requests;
    unsigned count;
    int closing, inherited;
    uint64_t storage;
};
static struct request *find_request(struct dcc_job_server *server, int id)
{
    struct request *r;
    for (r = server->requests; r; r = r->next) if (r->id == id) return r;
    return NULL;
}
static struct dcc_server_job *find_job(struct dcc_job_server *server, unsigned id)
{
    struct dcc_server_job *j;
    for (j = server->jobs; j; j = j->next) if (j->id == (int)id) return j;
    return NULL;
}
static int file_body(int fd, struct dcc_h2_body *body)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) return -1;
    *body = (struct dcc_h2_body){ .fd = fd, .length = st.st_size };
    return 0;
}
static int answer(struct dcc_job_server *s, struct request *r, int status, int fd)
{
    struct dcc_h2_body body;
    struct dcc_h2_field fields[2];
    size_t count = 1;
    char id[24];
    if (!r || r->answered) return -1;
    if (fd >= 0 && file_body(fd, &body)) return -1;
    snprintf(id, sizeof(id), "%d", r->job ? r->job->id : 0);
    fields[0] = (struct dcc_h2_field){ "x-distcc-job", id };
    if (status == 200 && r->operation == OUTPUT &&
        (r->job->outcome.compressed & (1U << r->index)))
        fields[count++] = (struct dcc_h2_field){ "content-encoding", DCC_JOB_CONTENT_ENCODING };
    if (dcc_h2_response(s->connection, r->id, status, fields, count, fd >= 0 ? &body : NULL)) {
        dcc_h2_cancel(s->connection, r->id);
        return -1;
    }
    r->answered = 1;
    return 0;
}
static int answer_result(struct dcc_job_server *s, struct request *r)
{
    struct dcc_server_job *j = r->job;
    const char *names[] = { "x-distcc-status", "x-distcc-mode", "x-distcc-stderr-size",
        "x-distcc-stdout-size", "x-distcc-object-size", "x-distcc-deps-size" };
    struct dcc_h2_field fields[7];
    size_t count = 6;
    struct dcc_h2_body body;
    char values[6][24];
    unsigned i;
    int fd = j->artifacts[DCC_JOB_OBJECT];
    if (fd >= 0 && file_body(fd, &body)) return -1;
    for (i = 0; i < 6; ++i) {
        uint64_t value = i == 0 ? (uint64_t)j->outcome.status :
            i == 1 ? (uint64_t)j->outcome.mode : j->outcome.size[i - 2];
        snprintf(values[i], sizeof(values[i]), "%llu", (unsigned long long)value);
        fields[i] = (struct dcc_h2_field){names[i], values[i]};
    }
    if (j->outcome.compressed & (1U << DCC_JOB_OBJECT))
        fields[count++] = (struct dcc_h2_field){ "content-encoding", DCC_JOB_CONTENT_ENCODING };
    if (dcc_h2_response(s->connection, r->id, 200, fields, count, fd >= 0 ? &body : NULL)) return -1;
    r->answered = 1;
    return 0;
}
static void fail_job(struct dcc_server_job *job)
{
    struct dcc_job_server *s = job->server;
    struct request *r;
    job->failed = 1;
    if (job->worker >= 0) {
        s->dispatch.cancel(s->context, job);
    }
    job->worker = -1;
    for (r = s->requests; r; r = r->next)
        if (r->job == job && !r->answered) answer(s, r, 500, -1);
}
static void release_job(struct dcc_server_job *job)
{
    struct dcc_job_server *s = job->server;
    struct dcc_server_job **p;
    unsigned i;
    if (job->worker >= 0 && !s->inherited) s->dispatch.cancel(s->context, job);
    for (p = &s->jobs; *p != job; p = &(*p)->next) {}
    *p = job->next;
    --s->count;
    s->storage -= job->retained;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) if (job->artifacts[i] >= 0) close(job->artifacts[i]);
    dcc_job_free(job->description);
    free(job->wanted);
    free(job->uploads);
    free(job);
}
static int reserve_sink(struct dcc_job_server *s, struct request *r, uint64_t size)
{
    if (size > STORAGE_LIMIT - s->storage) return -1;
    r->fd = dcc_job_memfd("distcc-upload");
    if (r->fd < 0 || dcc_h2_sink(s->connection, r->id, r->fd, size)) return -1;
    r->reserved = size;
    s->storage += size;
    return 0;
}
static int headers(struct dcc_h2 *c, int id, const struct dcc_h2_field *fields,
                    size_t count, void *context)
{
    struct dcc_job_server *s = dcc_h2_owner(c);
    struct request *r = calloc(1, sizeof(*r));
    const char *method = dcc_h2_field(fields, count, ":method");
    const char *path = dcc_h2_field(fields, count, ":path");
    unsigned jobid, index;
    int end = 0;
    (void)context;
    if (!r) return -1;
    r->id = id;
    r->fd = -1;
    r->next = s->requests;
    s->requests = r;
    if (!method || !path) return -1;
    if (!strcmp(method, "POST") && !strcmp(path, "/jobs")) {
        unsigned i;
        if (s->count == DCC_H2_MAX_STREAMS) return answer(s, r, 503, -1);
        r->job = calloc(1, sizeof(*r->job));
        if (!r->job) return -1;
        r->job->server = s;
        r->job->activity = time(NULL);
        r->job->id = id;
        r->job->worker = -1;
        for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) r->job->artifacts[i] = -1;
        r->job->next = s->jobs;
        s->jobs = r->job;
        ++s->count;
        r->operation = CREATE;
        if (reserve_sink(s, r, DCC_JOB_METADATA_MAX)) return answer(s, r, 503, -1);
    } else {
        if (sscanf(path, "/jobs/%u%n", &jobid, &end) != 1 ||
            !(r->job = find_job(s, jobid))) return answer(s, r, 404, -1);
        const char *suffix = path + end;
        r->job->activity = time(NULL);
        end = 0;
        if (!strcmp(method, "PUT") && sscanf(suffix, "/inputs/%u%n", &index, &end) == 1 &&
            !suffix[end]) {
            struct dcc_server_job *j = r->job;
            r->operation = INPUT;
            r->index = index;
            if (!j->wanted || index >= j->description->count || !j->wanted[index] ||
                j->uploads[index] || j->failed || j->worker < 0) return answer(s, r, 409, -1);
            const char *encoding = dcc_h2_field(fields, count, "content-encoding");
            if (j->description->inputs[index].encoding == DCC_JOB_LZ4 ?
                (!encoding || strcmp(encoding, DCC_JOB_CONTENT_ENCODING)) : encoding != NULL)
                return answer(s, r, 415, -1);
            if (reserve_sink(s, r, j->description->inputs[index].wire_size)) return answer(s, r, 503, -1);
            j->uploads[index] = id;
        } else if (!strcmp(method, "GET") && !strcmp(suffix, "/result")) {
            r->operation = RESULT;
        } else if (!strcmp(method, "GET") && sscanf(suffix, "/outputs/%u%n", &index, &end) == 1 &&
                   !suffix[end] && index < DCC_JOB_RESULT_COUNT) {
            r->operation = OUTPUT;
            r->index = index;
        } else if (!strcmp(method, "DELETE") && !*suffix) r->operation = RELEASE;
        else return answer(s, r, 404, -1);
    }
    return 0;
}
static int end_body(struct dcc_h2 *c, int id, void *context)
{
    struct dcc_job_server *s = dcc_h2_owner(c);
    struct request *r = find_request(s, id);
    struct dcc_server_job *j;
    (void)context;
    if (!r || r->answered) return 0;
    j = r->job;
    if (!j) return -1;
    switch (r->operation) {
    case CREATE: {
        uint64_t expanded = 0;
        unsigned i;
        if (dcc_job_decode(r->fd, &j->description)) return answer(s, r, 400, -1);
        for (i = 0; i < j->description->count; ++i) {
            uint64_t size = j->description->inputs[i].plain_size;
            if (size > STORAGE_LIMIT - s->storage - expanded) return answer(s, r, 413, -1);
            expanded += size;
        }
        j->retained += expanded;
        s->storage += expanded;
        j->wanted = calloc(j->description->count ? j->description->count : 1, 1);
        j->uploads = calloc(j->description->count ? j->description->count : 1, sizeof(int));
        if (!j->wanted || !j->uploads) return -1;
        if (fcntl(r->fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) return -1;
        j->worker = s->dispatch.start(s->context, j, r->fd, j->description->kind == DCC_JOB_LINK);
        if (j->worker == -1) { j->failed = 1; return answer(s, r, 503, -1); }
        break;
    }
    case INPUT: {
        struct stat st;
        struct dcc_job_message message = { DCC_JOB_INPUT, r->index };
        if (fstat(r->fd, &st) || (uint64_t)st.st_size != j->description->inputs[r->index].wire_size ||
            fcntl(r->fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) ||
            dcc_send_fd(j->worker, &message, sizeof(message), r->fd)) return -1;
        break;
    }
    case RESULT:
        if (j->failed) return answer(s, r, 500, -1);
        if (j->finished) return answer_result(s, r);
        break;
    case OUTPUT:
        return answer(s, r, j->finished && !j->failed ? 200 : 409,
                      j->finished && !j->failed ? j->artifacts[r->index] : -1);
    case RELEASE: {
        struct request *other;
        for (other = s->requests; other; other = other->next)
            if (other != r && other->job == j) {
                other->job = NULL;
                dcc_h2_cancel(c, other->id);
            }
        answer(s, r, 204, -1);
        r->job = NULL;
        release_job(j);
        break;
    }
    }
    return 0;
}
static void closed(struct dcc_h2 *c, int id, unsigned error, void *context)
{
    struct dcc_job_server *s = dcc_h2_owner(c);
    struct request **p = &s->requests, *r;
    (void)context;
    while (*p && (*p)->id != id) p = &(*p)->next;
    if (!(r = *p)) return;
    *p = r->next;
    if (error && r->job && !s->closing) fail_job(r->job);
    if (r->fd >= 0) close(r->fd);
    s->storage -= r->reserved;
    free(r);
}
struct dcc_job_server *dcc_job_server_new(int fd, const struct dcc_job_dispatch *dispatch, void *context)
{
    static const struct dcc_h2_events events = { headers, end_body, closed };
    struct dcc_job_server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dispatch = *dispatch;
    s->context = context;
    s->connection = dcc_h2_new(fd, 1, &events, s);
    if (!s->connection) { free(s); return NULL; }
    return s;
}
struct dcc_h2 *dcc_job_server_connection(struct dcc_job_server *s) { return s->connection; }
int dcc_job_server_active(struct dcc_job_server *s) { return s->count != 0; }
int dcc_job_server_waiting(struct dcc_job_server *s)
{
    int count = 0;
    struct dcc_server_job *j;
    if (s) for (j = s->jobs; j; j = j->next)
        count += j->worker == DCC_JOB_DISPATCH_WAIT;
    return count;
}
void dcc_job_server_maintain(struct dcc_job_server *s, time_t now)
{
    struct dcc_server_job *j = s->jobs, *next;
    struct request *r;
    while (j) {
        next = j->next;
        for (r = s->requests; r; r = r->next) if (r->job == j) {
            time_t activity = dcc_h2_activity(s->connection, r->id);
            if (activity > j->activity) j->activity = activity;
        }
        if (now - j->activity >= dcc_get_io_timeout()) {
            for (r = s->requests; r; r = r->next) if (r->job == j) {
                r->job = NULL;
                dcc_h2_cancel(s->connection, r->id);
            }
            release_job(j);
        } else if (j->worker == DCC_JOB_DISPATCH_WAIT) {
            r = find_request(s, j->id);
            j->worker = s->dispatch.start(s->context, j, r->fd, j->description->kind == DCC_JOB_LINK);
            if (j->worker == -1) { j->failed = 1; answer(s, r, 503, -1); }
        }
        j = next;
    }
}
void dcc_job_server_free(struct dcc_job_server *s)
{
    if (!s) return;
    s->closing = 1;
    dcc_h2_free(s->connection);
    while (s->jobs) release_job(s->jobs);
    free(s);
}
void dcc_job_server_abandon(struct dcc_job_server *s)
{
    if (s) s->inherited = 1;
    dcc_job_server_free(s);
}
void dcc_job_server_worker_lost(struct dcc_server_job *job)
{
    if (job) { job->worker = -1; fail_job(job); }
}
void dcc_job_server_message(struct dcc_server_job *j, const struct dcc_job_message *message, int fd)
{
    struct dcc_job_server *s = j->server;
    struct request *r;
    struct stat st;
    if (j->failed) goto failed;
    j->activity = time(NULL);
    switch (message->operation) {
    case DCC_JOB_NEED: {
        unsigned i;
        if (fd < 0 || message->index != j->description->count || fstat(fd, &st) ||
            st.st_size != message->index || pread(fd, j->wanted, message->index, 0) != message->index) goto failed;
        for (i = 0; i < message->index; ++i)
            if (j->wanted[i] > 1 || (j->wanted[i] && j->description->inputs[i].kind != DCC_JOB_FILE)) goto failed;
        if (answer(s, find_request(s, j->id), 201, fd)) goto failed;
        break;
    }
    case DCC_JOB_INPUT_DONE:
        if (message->index >= j->description->count || !j->wanted[message->index]) goto failed;
        r = find_request(s, j->uploads[message->index]);
        if (!r || answer(s, r, 204, -1)) goto failed;
        j->wanted[message->index] = 0;
        break;
    case DCC_JOB_ARTIFACT:
        if (fd < 0 || message->index >= DCC_JOB_RESULT_COUNT ||
            j->artifacts[message->index] >= 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
            st.st_size < 0 || (uint64_t)st.st_size > STORAGE_LIMIT - s->storage) goto failed;
        j->artifacts[message->index] = fd;
        j->retained += st.st_size;
        s->storage += st.st_size;
        fd = -1;
        break;
    case DCC_JOB_FINISHED: {
        struct dcc_job_result result;
        unsigned i;
        j->worker = -1;
        if (message->index || fd < 0 || j->finished || dcc_job_result_decode(fd, &result)) goto failed;
        for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
            if (j->artifacts[i] < 0) { if (result.wire_size[i]) goto failed; }
            else if (fstat(j->artifacts[i], &st) || (uint64_t)st.st_size != result.wire_size[i]) goto failed;
        }
        j->outcome = result;
        j->finished = 1;
        for (r = s->requests; r; r = r->next)
            if (r->job == j && r->operation == RESULT && !r->answered && answer_result(s, r))
                dcc_h2_cancel(s->connection, r->id);
        break;
    }
    default:
        goto failed;
    }
    if (fd >= 0) close(fd);
    return;
failed:
    if (fd >= 0) close(fd);
    fail_job(j);
}
