/* A single owner for TCP sockets, input trees, cache waiters and ready jobs.
 * Compiler workers receive only prepared inputs and return immutable FDs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <xxhash.h>
#include "distcc.h"
#include "file_cache.h"
#include "resource.h"
#include "daemon.h"
#include "tcp_job.h"
#include "tcp_async.h"

enum phase { PREPARING, RUNNING, RETURNING };
struct packet {
    struct packet *next;
    unsigned char *data;
    size_t size, sent, header_size, file_left;
    off_t file_offset;
    int file;
};
struct upload;
struct input {
    struct dcc_tcp_server *server;
    struct input *next;
    struct upload *upload;
    char *path;
    unsigned index;
    int wanted, done;
};
struct upload {
    struct upload *next;
    unsigned char digest[8];
    uint64_t size;
    struct input *owner, *waiters;
    int ready;
};
struct dcc_tcp_server {
    int socket, failed, metadata, input_fd;
    enum phase phase;
    time_t active;
    struct dcc_tcp_frame frame;
    struct packet *head, **tail;
    size_t queued;
    const struct dcc_tcp_dispatch *dispatch;
    struct dcc_job *job;
    struct dcc_tcp_run run;
    struct input *inputs;
    unsigned remaining, input_index, output_index, prepared;
    uint64_t input_offset, output_offset, reserved, input_bytes, result_bytes;
    XXH3_state_t *hash;
    struct dcc_job_result result;
    unsigned char *buffer;
    int complete_queued;
    struct dcc_tcp_server *ready_next;
    int enqueued;
    struct dcc_tcp_work work;
    struct packet *encoded;
    size_t encoding_bytes;
    int input_error;
};
/* The parent event loop alone owns these. Children abandon inherited FDs. */
static struct upload *uploads[256];
static uint64_t retained;
static uint64_t returning_bytes;
static struct dcc_tcp_server *ready_head, **ready_tail = &ready_head;
static uint64_t idle_ns, idle_at;
static unsigned idle_slots;
static struct dcc_tcp_server *abandoned;
static void completed_input(struct input *);
static void unhash(struct upload *);

int dcc_tcp_server_pending(void)
{
    for (unsigned i = 0; i < 256; ++i)
        for (struct upload *u = uploads[i]; u; u = u->next) if (u->ready) return 1;
    return 0;
}

void dcc_tcp_server_collect(void)
{
    struct dcc_tcp_server **p = &abandoned;
    while (*p) {
        struct dcc_tcp_server *s = *p;
        if (!atomic_load_explicit(&s->work.done, memory_order_acquire)) { p = &s->ready_next; continue; }
        *p = s->ready_next;
        s->work.run = NULL;
        dcc_tcp_server_free(s);
    }
    /* One private snapshot per turn. The verified source remains PREPARING
     * until every dependant has its own inode; no compiler can mutate it. */
    for (unsigned i = 0; i < 256; ++i) for (struct upload *u = uploads[i]; u; u = u->next) {
        if (!u->ready) continue;
        struct input *w = u->waiters;
        if (w) {
            u->waiters = w->next; w->next = NULL; w->upload = NULL;
            if (dcc_file_cache_copy(NULL, u->owner->path, w->path)) completed_input(w);
            else w->server->failed = 1;
        }
        if (!u->waiters) {
            struct input *owner = u->owner;
            owner->upload = NULL; completed_input(owner); unhash(u);
        }
        return;
    }
}
void dcc_tcp_server_quiesce(void)
{
    dcc_tcp_async_stop();
    dcc_tcp_server_collect();
}

uint64_t dcc_tcp_ready_idle_ns(void)
{
    struct dcc_server_load load;
    int eligible = 0, available = 0;
    if (ready_head) {
        dcc_prefork_status(&load);
        for (struct dcc_tcp_server *s = ready_head; s; s = s->ready_next)
            if (s->job->kind != DCC_JOB_LINK || !load.links) { eligible = 1; break; }
        if (eligible) available = dcc_resource_slots(dcc_max_kids) - load.busy;
    }
    unsigned slots = available > 0 ? available : 0;
    if (slots || idle_slots) {
        struct timespec time;
        clock_gettime(CLOCK_MONOTONIC, &time);
        uint64_t now = (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
        if (idle_slots) idle_ns += (now - idle_at) * idle_slots;
        idle_at = now;
    }
    idle_slots = slots;
    return idle_ns;
}

static void dequeue(struct dcc_tcp_server *s)
{
    if (!s->enqueued) return;
    dcc_tcp_ready_idle_ns();
    struct dcc_tcp_server **p = &ready_head;
    while (*p != s) p = &(*p)->ready_next;
    *p = s->ready_next;
    if (!*p) ready_tail = p;
    s->ready_next = NULL; s->enqueued = 0;
    dcc_tcp_ready_idle_ns();
}

static int queue_owned(struct dcc_tcp_server *s, unsigned char *data, size_t size)
{
    struct packet *p;
    if (!data || size > DCC_JOB_METADATA_MAX + 16U ||
        size > DCC_TCP_BYTES - retained ||
        s->queued > DCC_JOB_METADATA_MAX + 16U - size || !(p = malloc(sizeof(*p)))) {
        free(data);
        s->failed = 1;
        return -1;
    }
    *p = (struct packet){ .data = data, .size = size, .header_size = size, .file = -1 };
    *s->tail = p;
    s->tail = &p->next;
    s->queued += size;
    retained += size;
    return 0;
}
static int queue(struct dcc_tcp_server *s, uint32_t type, const void *data, size_t size)
{
    size_t length = 0;
    unsigned char *p = dcc_tcp_frame_make(type, data, size, &length);
    return queue_owned(s, p, length);
}
static void need(struct input *in)
{
    unsigned char index[4];
    in->wanted = 1;
    dcc_tcp_put32(index, in->index);
    queue(in->server, DCC_TCP_NEED, index, sizeof(index));
}
static void unhash(struct upload *u)
{
    struct upload **p = &uploads[u->digest[0]];
    while (*p != u) p = &(*p)->next;
    *p = u->next;
    free(u);
}
static void detach(struct input *in)
{
    struct upload *u = in->upload;
    if (!u) return;
    if (u->owner == in) {
        if (u->waiters) {
            u->ready = 0;
            u->owner = u->waiters;
            u->waiters = u->waiters->next;
            u->owner->next = NULL;
            need(u->owner);
        } else unhash(u);
    } else {
        struct input **p = &u->waiters;
        while (*p != in) p = &(*p)->next;
        *p = in->next;
    }
    in->upload = NULL;
}
/* Never follow client-provided links while deleting a private tree. */
static void remove_tree(int parent, const char *name)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        DIR *dir = fdopendir(fd);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)))
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                    remove_tree(fd, entry->d_name);
            closedir(dir);
        } else close(fd);
        unlinkat(parent, name, AT_REMOVEDIR);
    } else unlinkat(parent, name, 0);
}
static void release_inputs(struct dcc_tcp_server *s)
{
    if (s->job) for (unsigned i = 0; i < s->job->count; ++i) {
        if (s->inputs) {
            detach(&s->inputs[i]);
            free(s->inputs[i].path);
        }
    }
    if (s->input_fd >= 0) close(s->input_fd);
    s->input_fd = -1;
    if (s->metadata >= 0) close(s->metadata);
    s->metadata = -1;
    if (s->run.root[0]) remove_tree(AT_FDCWD, s->run.root);
    s->run.root[0] = 0;
    free(s->inputs); s->inputs = NULL;
    dcc_job_free(s->job); s->job = NULL;
    retained -= s->input_bytes;
    s->reserved -= s->input_bytes;
    s->input_bytes = 0;
}
static void reset_job(struct dcc_tcp_server *s)
{
    dequeue(s);
    release_inputs(s);
    memset(&s->run, 0, sizeof(s->run));
    dcc_job_result_free(&s->result);
    retained -= s->reserved;
    returning_bytes -= s->result_bytes;
    s->result_bytes = 0;
    s->reserved = 0;
    s->remaining = s->output_index = s->complete_queued = s->prepared = 0;
    s->input_offset = s->output_offset = 0;
    s->phase = PREPARING;
}
struct dcc_tcp_server *dcc_tcp_server_new(int fd, const struct dcc_tcp_dispatch *dispatch)
{
    if (retained > DCC_TCP_BYTES - DCC_TCP_CHUNK) return NULL;
    struct dcc_tcp_server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int one = 1;
    int domain;
    socklen_t domain_size = sizeof(domain);
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ||
        getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &domain_size) ||
        (domain != AF_UNIX && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)))) {
        free(s); return NULL;
    }
    s->socket = fd; s->metadata = s->input_fd = -1;
    atomic_init(&s->work.done, 1);
    s->dispatch = dispatch; s->tail = &s->head; s->active = time(NULL);
    s->buffer = malloc(DCC_TCP_CHUNK);
    s->hash = XXH3_createState();
    dcc_job_result_init(&s->result);
    if (!s->buffer || !s->hash) {
        free(s->buffer); XXH3_freeState(s->hash); free(s); return NULL;
    }
    retained += DCC_TCP_CHUNK;
    return s;
}
void dcc_tcp_server_abandon(struct dcc_tcp_server *s)
{
    if (!s) return;
    if (s->encoded) { free(s->encoded->data); free(s->encoded); }
    close(s->socket);
    if (s->metadata >= 0) close(s->metadata);
    if (s->input_fd >= 0) close(s->input_fd);
    dcc_job_result_free(&s->result);
    /* A forked worker must neither unlink parent trees nor promote uploads. */
    if (s->inputs) for (unsigned i = 0; i < s->job->count; ++i) free(s->inputs[i].path);
    free(s->inputs);
    dcc_job_free(s->job);
    dcc_tcp_frame_clear(&s->frame);
    while (s->head) {
        struct packet *p = s->head;
        s->head = p->next; free(p->data); free(p);
    }
    free(s->buffer); XXH3_freeState(s->hash); free(s);
    /* These references also belong to the parent. Free once in the child's
     * private address space so replacement workers do not pin old buffers. */
    for (unsigned i = 0; i < 256; ++i) while (uploads[i]) {
        struct upload *next = uploads[i]->next;
        free(uploads[i]); uploads[i] = next;
    }
    ready_head = NULL; ready_tail = &ready_head;
}
void dcc_tcp_server_free(struct dcc_tcp_server *s)
{
    if (!s) return;
    if (s->work.run && !atomic_load_explicit(&s->work.done, memory_order_acquire)) {
        close(s->socket); s->socket = -1;
        s->ready_next = abandoned; abandoned = s;
        return;
    }
    if (s->encoded) { free(s->encoded->data); free(s->encoded); }
    retained -= s->encoding_bytes;
    if (s->phase == RUNNING) {
        /* The worker may still read the tree. Its reap/completion owns the
         * final release, even though this connection has already gone away. */
        s->dispatch->cancel(s);
        close(s->socket); s->socket = -1;
        return;
    }
    reset_job(s);
    while (s->head) {
        struct packet *p = s->head;
        s->head = p->next;
        retained -= p->size;
        free(p->data); free(p);
    }
    close(s->socket);
    retained -= DCC_TCP_CHUNK + (s->frame.data ? s->frame.size : 0);
    dcc_tcp_frame_clear(&s->frame);
    free(s->buffer); XXH3_freeState(s->hash); free(s);
}
static int path_order(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int prepare(struct dcc_tcp_server *s)
{
    char **paths = calloc(s->job->count ? s->job->count : 1, sizeof(*paths));
    const char *tmp;
    if (!paths || dcc_get_tmp_top(&tmp)) { free(paths); return -1; }
    int length = snprintf(s->run.root, sizeof(s->run.root), "%s/distcc-tcp-XXXXXX", tmp);
    if (length < 0 || (size_t)length >= sizeof(s->run.root) || !mkdtemp(s->run.root)) {
        s->run.root[0] = 0; free(paths); return -1;
    }
    char *canonical = realpath(s->run.root, NULL);
    if (!canonical || strlen(canonical) >= sizeof(s->run.root)) { free(canonical); free(paths); return -1; }
    strcpy(s->run.root, canonical); free(canonical);
    s->inputs = calloc(s->job->count ? s->job->count : 1, sizeof(*s->inputs));
    if (!s->inputs) { free(paths); return -1; }
    for (unsigned i = 0; i < s->job->count; ++i) paths[i] = s->job->inputs[i].path;
    qsort(paths, s->job->count, sizeof(*paths), path_order);
    /* Check all ancestors, not merely adjacent lexicographic entries. */
    for (unsigned i = 0; i < s->job->count; ++i) {
        char *copy = strdup(paths[i]);
        if (!copy || (i && !strcmp(paths[i-1], paths[i]))) { free(copy); free(paths); return -1; }
        for (char *p = copy + 1; *p; ++p) if (*p == '/') {
            *p = 0;
            if (bsearch(&copy, paths, s->job->count, sizeof(*paths), path_order)) {
                free(copy); free(paths); return -1;
            }
            *p = '/';
        }
        free(copy);
    }
    free(paths);
    return 0;
}
static int prepare_inputs(struct dcc_tcp_server *s)
{
    unsigned limit = s->prepared + 8;
    for (; s->prepared < s->job->count && s->prepared < limit; ++s->prepared) {
        unsigned i = s->prepared;
        struct input *in = &s->inputs[i];
        const struct dcc_job_input *description = &s->job->inputs[i];
        *in = (struct input){ .server = s, .index = i };
        if (asprintf(&in->path, "%s%s", s->run.root, description->path) < 0) return -1;
        for (char *p = in->path + strlen(s->run.root) + 1; *p; ++p) if (*p == '/') {
            *p = 0;
            int ret = mkdir(in->path, 0700);
            *p = '/';
            if (ret && errno != EEXIST) return -1;
        }
        if (description->kind != DCC_JOB_FILE) continue;
        if (description->fd >= 0) { in->done = 1; continue; }
        if (description->cached && dcc_file_cache_get(description->digest, in->path)) {
            struct stat st;
            if (stat(in->path, &st) || (uint64_t)st.st_size != description->plain_size) return -1;
            in->done = 1;
            limit = i + 1; /* Bound cache copying/hashing to one file per turn. */
            continue;
        }
        ++s->remaining;
        if (!description->cached) { in->wanted = 1; continue; }
        struct upload **bucket = &uploads[description->digest[0]], *u;
        for (u = *bucket; u; u = u->next)
            if (u->size == description->plain_size && !memcmp(u->digest, description->digest, 8)) break;
        if (!u) {
            u = calloc(1, sizeof(*u));
            if (!u) return -1;
            memcpy(u->digest, description->digest, 8);
            u->size = description->plain_size;
            u->owner = in; u->next = *bucket; *bucket = u;
            in->wanted = 1;
        } else { in->next = u->waiters; u->waiters = in; }
        in->upload = u;
    }
    if (s->prepared != s->job->count) return 0;
    unsigned char *wanted = malloc(s->job->count * 4U + 1);
    if (!wanted) return -1;
    unsigned count = 0;
    for (unsigned i = 0; i < s->job->count; ++i) if (s->inputs[i].wanted && s->job->inputs[i].cached)
        dcc_tcp_put32(wanted + 4U * count++, i);
    int ret = count ? queue(s, DCC_TCP_NEED, wanted, count * 4U) : 0;
    free(wanted);
    if (ret) return ret;
    return 0;
}
static uint64_t local_input_jobs, local_input_bytes;
uint64_t dcc_tcp_local_input_jobs(void) { return local_input_jobs; }
uint64_t dcc_tcp_local_input_bytes(void) { return local_input_bytes; }

static int begin(struct dcc_tcp_server *s, struct dcc_tcp_frame *f)
{
    if (s->job || s->phase != PREPARING || f->size <= 8 || !(s->run.id = dcc_tcp_get64(f->data))) return -1;
    s->metadata = dcc_job_memfd("distcc-tcp-manifest");
    if (s->metadata < 0 || dcc_writex(s->metadata, f->data + 8, f->size - 8) ||
        dcc_job_decode_buffer(f->data + 8, f->size - 8, &s->job)) return -1;
    if (f->fd_plus_one) {
        struct stat st;
        int fd = f->fd_plus_one - 1;
        if (s->job->kind != DCC_JOB_COMPILE || s->job->count != 1 ||
            s->job->inputs[0].kind != DCC_JOB_FILE || s->job->inputs[0].cached ||
            s->job->inputs[0].encoding != DCC_JOB_RAW || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size != s->job->inputs[0].plain_size ||
            (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY) return -1;
        s->job->inputs[0].fd = fd; f->fd_plus_one = 0;
        ++local_input_jobs;
        local_input_bytes += st.st_size;
    }
    /* Include the encoded manifest, decoded strings and input bookkeeping,
     * in addition to the incoming frame already charged by the parser. */
    uint64_t amount = 3 * f->size + s->job->count * sizeof(struct input);
    for (unsigned i = 0; i < s->job->count; ++i) {
        uint64_t size = s->job->inputs[i].plain_size;
        if (size > DCC_TCP_BYTES - amount) return -1;
        amount += size;
    }
    if (amount > DCC_TCP_BYTES - retained) return -1;
    retained += amount; s->reserved = s->input_bytes = amount;
    return prepare(s);
}
static void completed_input(struct input *in)
{
    in->done = 1; in->wanted = 0; --in->server->remaining;
}
static void decode_input(void *argument)
{
    struct dcc_tcp_server *s = argument;
    unsigned index;
    size_t length;
    const void *data;
    s->input_error = dcc_tcp_unpack(&s->frame, &index, s->buffer, &data, &length);
    if (s->input_error) return;
    s->input_error = dcc_writex(s->input_fd, data, length);
    if (!s->input_error && s->job->inputs[index].cached)
        s->input_error = XXH3_64bits_update(s->hash, data, length) != XXH_OK;
}
static int finish_input(struct dcc_tcp_server *s)
{
    const struct dcc_tcp_frame *f = &s->frame;
    unsigned index = dcc_tcp_get32(f->data);
    size_t length = dcc_tcp_get32(f->data + 4);
    struct input *in = &s->inputs[index];
    const struct dcc_job_input *description = &s->job->inputs[index];
    if (s->input_error) return -1;
    s->input_offset += length;
    if (!(f->type & DCC_TCP_FINAL)) return s->input_offset < description->plain_size ? 0 : -1;
    if (s->input_offset != description->plain_size) return -1;
    close(s->input_fd); s->input_fd = -1;
    if (description->cached) {
        XXH64_canonical_t digest;
        XXH64_canonicalFromHash(&digest, XXH3_64bits_digest(s->hash));
        if (memcmp(digest.digest, description->digest, 8)) return -1;
        dcc_file_cache_put(description->digest, in->path);
    }
    if (in->upload) {
        in->wanted = 0;
        in->upload->ready = 1;
    } else completed_input(in);
    return 0;
}
static int receive_input(struct dcc_tcp_server *s, const struct dcc_tcp_frame *f)
{
    if (!s->job || s->phase != PREPARING || f->size < 8) return -1;
    unsigned index = dcc_tcp_get32(f->data), length = dcc_tcp_get32(f->data + 4);
    if (index >= s->job->count || !s->inputs[index].wanted || length > DCC_TCP_CHUNK) return -1;
    if (s->input_fd < 0) {
        s->input_fd = open(s->inputs[index].path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (s->input_fd < 0 || XXH3_64bits_reset(s->hash) != XXH_OK) return -1;
        s->input_index = index; s->input_offset = 0;
    }
    if (index != s->input_index || length > s->job->inputs[index].plain_size - s->input_offset) return -1;
    /* RAW already points into the receive frame: avoid a thread handoff for
     * a bounded write. Offload expansion, where independent jobs can use CPUs. */
    if (length >= 65536 && (f->type & DCC_TCP_COMPRESSED))
        return dcc_tcp_async_submit(&s->work, decode_input, s) ? -1 : 1;
    decode_input(s);
    return finish_input(s);
}
static int install_links(struct dcc_tcp_server *s)
{
    for (unsigned i = 0; i < s->job->count; ++i) {
        struct input *in = &s->inputs[i];
        const struct dcc_job_input *d = &s->job->inputs[i];
        if (in->done || d->kind == DCC_JOB_FILE) continue;
        if (d->kind == DCC_JOB_EMPTY) {
            int fd = open(in->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0) return -1;
            close(fd);
        } else {
            char *target = NULL;
            if (d->target[0] == '/') {
                if (asprintf(&target, "%s%s", s->run.root, d->target) < 0) return -1;
            } else target = strdup(d->target);
            int ret = target ? symlink(target, in->path) : -1;
            free(target);
            if (ret) return -1;
        }
        in->done = 1;
    }
    return 0;
}
void dcc_tcp_server_load(struct dcc_tcp_server *s, struct dcc_server_load *load)
{
    if (!s) return;
    if (s->job && s->phase == PREPARING) {
        if (s->enqueued) load->queued++;
        else load->preparing++;
    }
    if (s->phase == RETURNING) load->returning++;
}
void dcc_tcp_server_lost(struct dcc_tcp_server *s)
{
    if (s) {
        s->failed = 1; s->phase = RETURNING;
        if (s->socket < 0) dcc_tcp_server_free(s);
    }
}
void dcc_tcp_server_finished(struct dcc_tcp_server *s, const struct dcc_tcp_finished *r,
                             int *fds, unsigned count)
{
    unsigned used = 0;
    if (s && s->socket < 0) goto fail;
    if (!s || s->phase != RUNNING || r->id != s->run.id || r->error ||
        r->status > 65535 || r->mode > 0777 || r->present >= (1U << DCC_JOB_RESULT_COUNT)) goto fail;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        struct stat st;
        if (r->size[i] > DCC_TCP_BYTES - bytes) goto fail;
        bytes += r->size[i];
        if (r->present & (1U << i)) {
            if (used == count || fstat(fds[used], &st) || !S_ISREG(st.st_mode) ||
                (uint64_t)st.st_size != r->size[i]) goto fail;
            s->result.fd[i] = fds[used]; fds[used++] = -1;
        } else if (r->size[i]) goto fail;
        s->result.size[i] = r->size[i];
    }
    if (used != count) goto fail;
    /* Result FDs now own the output inodes. Input lifetime ends here, not
     * when a slow receiver eventually consumes the last result byte. */
    release_inputs(s);
    if (bytes > DCC_TCP_BYTES - retained) goto fail;
    retained += bytes; s->reserved += bytes;
    returning_bytes += bytes; s->result_bytes = bytes;
    s->result.status = r->status; s->result.mode = r->mode;
    unsigned char description[40];
    dcc_tcp_put32(description, r->status); dcc_tcp_put32(description + 4, r->mode);
    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i)
        dcc_tcp_put64(description + 8 + i * 8, r->size[i]);
    s->phase = RETURNING;
    s->active = time(NULL);
    queue(s, DCC_TCP_RESULT, description, sizeof(description));
    return;
fail:
    for (unsigned i = 0; i < count; ++i) if (fds[i] >= 0) close(fds[i]);
    dcc_tcp_server_lost(s);
}
static void encode_output(void *argument)
{
    struct dcc_tcp_server *s = argument;
    unsigned i = s->output_index;
    uint64_t left = s->result.size[i] - s->output_offset;
    size_t amount = left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK;
    struct packet *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->data = malloc(amount + 16);
    if (!p->data || pread(s->result.fd[i], s->buffer, amount, s->output_offset) != (ssize_t)amount) {
        free(p->data); free(p); return;
    }
    p->size = p->header_size = dcc_tcp_encode(DCC_TCP_OUTPUT, i, s->buffer, amount, amount == left, p->data);
    p->file = -1;
    if (p->size == 16) {
        p->file = s->result.fd[i]; p->file_left = amount; p->file_offset = s->output_offset;
    }
    unsigned char *compact = realloc(p->data, p->size);
    if (!compact) { free(p->data); free(p); return; }
    p->data = compact;
    s->encoded = p;
}
static int output(struct dcc_tcp_server *s)
{
    if (s->work.run) {
        if (!atomic_load_explicit(&s->work.done, memory_order_acquire)) return 0;
        s->work.run = NULL;
        struct packet *p = s->encoded; s->encoded = NULL;
        retained -= s->encoding_bytes; s->encoding_bytes = 0;
        if (!p) return -1;
        /* One packet at a time: the result FD outlives every queued extent. */
        *s->tail = p; s->tail = &p->next;
        s->queued += p->size; retained += p->size;
        s->output_offset += dcc_tcp_get32(p->data + 12);
        return 0;
    }
    while (s->output_index < DCC_JOB_RESULT_COUNT) {
        unsigned i = s->output_index;
        uint64_t left = s->result.size[i] - s->output_offset;
        if (!left) { ++s->output_index; s->output_offset = 0; continue; }
        size_t amount = left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK;
        if (amount + 16 > DCC_TCP_BYTES - retained) return -1;
        s->encoding_bytes = amount + 16; retained += s->encoding_bytes;
        if (amount >= 65536) {
            if (dcc_tcp_async_submit(&s->work, encode_output, s)) return -1;
            return 0;
        }
        encode_output(s);
        s->work.run = encode_output; atomic_store(&s->work.done, 1);
        return output(s);
    }
    if (!s->complete_queued) {
        unsigned char id[8];
        dcc_tcp_put64(id, s->run.id);
        /* Reclaim job resources before exposing the reusable boundary. */
        reset_job(s);
        s->complete_queued = 1;
        return queue(s, DCC_TCP_COMPLETE, id, sizeof(id));
    }
    return 0;
}
void dcc_tcp_server_poll(struct dcc_tcp_server *s, struct pollfd *p)
{
    p->fd = s->socket;
    int pending = s->work.run && !atomic_load_explicit(&s->work.done, memory_order_acquire);
    p->events = POLLIN | (s->head || (s->phase == RETURNING && !pending) ? POLLOUT : 0);
    if (pending && s->work.run == decode_input) p->events = POLLRDHUP;
    if (s->job && s->phase == PREPARING && s->prepared < s->job->count)
        p->events = POLLOUT;
    p->revents = 0;
}
int dcc_tcp_server_process(struct dcc_tcp_server *s, short events)
{
    time_t now = time(NULL);
    if (s->failed || now - s->active >= (s->job || s->phase == RETURNING ? dcc_get_io_timeout() : 30)) return -1;
    if (s->work.run == decode_input) {
        if (events & (POLLHUP | POLLERR | POLLRDHUP)) return -1;
        if (!atomic_load_explicit(&s->work.done, memory_order_acquire)) return 0;
        s->work.run = NULL;
        int ret = finish_input(s);
        retained -= s->frame.size; dcc_tcp_frame_clear(&s->frame);
        if (ret) return -1;
        events |= POLLIN;
    } else if (s->work.run && atomic_load_explicit(&s->work.done, memory_order_acquire)) events |= POLLOUT;
    /* Bounded turns interleave other connections and worker completions. */
    if (s->job && s->phase == PREPARING && s->prepared < s->job->count) {
        if (prepare_inputs(s)) return -1;
        if (s->prepared < s->job->count) events &= ~POLLIN;
        else events |= POLLIN;
    }
    if (events & (POLLIN | POLLHUP | POLLERR)) for (unsigned turn = 0; turn < 4; ++turn) {
        size_t before = s->frame.used;
        int allocated = s->frame.data != NULL;
        int ret = dcc_tcp_frame_read(s->socket, &s->frame, DCC_TCP_BYTES - retained);
        if (!allocated && s->frame.data) retained += s->frame.size;
        if (s->frame.used != before) s->active = now;
        if (ret <= 0) { if (ret < 0) return -1; break; }
        uint32_t type = s->frame.type & ~DCC_TCP_FLAGS;
        if (s->frame.fd_plus_one && type != DCC_TCP_BEGIN) return -1;
        if (type == DCC_TCP_BEGIN) {
            if (s->complete_queued && s->head) return -1;
            s->complete_queued = 0;
            ret = begin(s, &s->frame);
        } else if (type == DCC_TCP_INPUT) ret = receive_input(s, &s->frame);
        else ret = -1;
        if (ret == 1) break; /* The task borrows this frame until completion. */
        retained -= s->frame.size;
        dcc_tcp_frame_clear(&s->frame);
        if (ret) return -1;
        if (s->job && s->phase == PREPARING && s->prepared < s->job->count) break;
    }
    if (s->job && s->phase == PREPARING && s->prepared == s->job->count && !s->remaining) {
        if (!s->enqueued) {
            if (install_links(s)) return -1;
            dcc_tcp_ready_idle_ns();
            s->enqueued = 1; *ready_tail = s; ready_tail = &s->ready_next;
            dcc_tcp_ready_idle_ns();
        }
        dcc_tcp_dispatch_ready();
    }
    if (events & POLLOUT) for (unsigned turn = 0; turn < 4; ++turn) {
        if (!s->head && s->phase == RETURNING && output(s)) return -1;
        struct packet *p = s->head;
        if (!p) break;
        ssize_t n;
        if (p->sent < p->header_size)
            n = send(s->socket, p->data + p->sent, p->header_size - p->sent,
                     MSG_NOSIGNAL | MSG_DONTWAIT | (p->file_left ? MSG_MORE : 0));
        else {
            n = sendfile(s->socket, p->file, &p->file_offset, p->file_left);
            if (n > 0) p->file_left -= n;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) return -1;
        s->active = now;
        if (p->sent < p->header_size) p->sent += n;
        if (p->sent != p->header_size || p->file_left) continue;
        s->queued -= p->size;
        retained -= p->size;
        s->head = p->next;
        if (!s->head) s->tail = &s->head;
        free(p->data); free(p);
    }
    return s->failed ? -1 : 0;
}

void dcc_tcp_dispatch_ready(void)
{
    if (returning_bytes && retained > DCC_TCP_BYTES * 3 / 4) return;
    /* Once per parent poll turn, in ready order. A blocked link may be
     * bypassed by ordinary compiles, but never by a later link. */
    int link_waiting = 0;
    for (struct dcc_tcp_server *s = ready_head; s;) {
        struct dcc_tcp_server *next = s->ready_next;
        int link = s->job->kind == DCC_JOB_LINK;
        if (!link || !link_waiting) {
            s->run.operation = DCC_TCP_RUN;
            int input = s->job->kind == DCC_JOB_COMPILE && s->job->count == 1 ? s->job->inputs[0].fd : -1;
            int ret = s->dispatch->start(s, &s->run, s->metadata, input, link);
            if (ret < 0) { s->failed = 1; dequeue(s); }
            else if (ret) { dequeue(s); s->phase = RUNNING; }
            else if (!link) break;
            else link_waiting = 1;
        }
        s = next;
    }
}
