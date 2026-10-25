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
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <xxhash.h>
#include "distcc.h"
#include "exitcode.h"
#include "trace.h"
#include "file_cache.h"
#include "resource.h"
#include "cpuperf.h"
#include "daemon.h"
#include "tcp_job.h"
#include "tcp_async.h"

enum phase { PREPARING, RUNNING, RETURNING };
struct packet {
    struct packet *next;
    unsigned char *data;
    size_t size, sent, header_size, file_left;
    off_t file_offset;
    int file, rights;
};
struct upload;
struct input {
    struct dcc_tcp_server *server;
    struct input *next;
    struct upload *upload;
    char *path;
    unsigned index;
    int wanted, storage_needed;
};
struct upload {
    struct upload *next;
    unsigned char digest[8];
    uint64_t size;
    struct input *owner, *waiters;
    int ready;
};
struct dcc_tcp_server {
    int socket, failed, metadata, input_fd, local;
    enum phase phase;
    time_t active;
    uint64_t begun_at, ready_at, started_at;
    /* The reader owns frame; the decoder exclusively owns decoding. */
    struct dcc_tcp_frame frame, decoding;
    struct packet *head, **tail;
    size_t queued;
    const struct dcc_tcp_dispatch *dispatch;
    struct dcc_job *job;
    struct dcc_tcp_run run;
    struct input *inputs;
    unsigned remaining, input_index, output_index, prepared;
    uint64_t input_offset, output_offset, reserved, input_bytes, result_bytes;
    uint64_t input_pending; /* File bytes not yet materialized in the private tree. */
    int storage_reserved;
    unsigned storage_group;
    int result_pending;
    XXH3_state_t *hash;
    struct dcc_job_result result;
    unsigned char *buffer;
    struct dcc_tcp_server *ready_next;
    int enqueued;
    struct dcc_tcp_work work;
    struct packet *encoded;
    size_t encoding_bytes;
    int input_error;
    unsigned cache_index, borrowers;
    int cache_hit;
    struct input *copy_source;
};
/* The parent event loop alone owns these. Children abandon inherited FDs. */
static struct upload *uploads[256];
static uint64_t retained;
static uint64_t storage_pending[2];
static uint64_t returning_bytes;
static unsigned waiting_results;
static struct dcc_tcp_server *ready_head, **ready_tail = &ready_head;
static uint64_t idle_ns, idle_at;
static unsigned idle_slots;
static uint64_t input_prefetch_bytes, output_overlaps;
uint64_t dcc_tcp_input_prefetch_bytes(void) { return input_prefetch_bytes; }
uint64_t dcc_tcp_output_overlaps(void) { return output_overlaps; }
static void completed_input(struct input *);
static void unhash(struct upload *);
static void input_complete(void *);
static void output_complete(void *);
static void copy_input(void *);
static void copy_complete(void *);
static int prepare_inputs(struct dcc_tcp_server *);
static void reserve_input(struct input *in)
{
    if (in->storage_needed) return;
    struct dcc_tcp_server *s = in->server;
    uint64_t size = s->job->inputs[in->index].plain_size;
    in->storage_needed = 1;
    s->input_pending += size;
    if (s->storage_reserved) storage_pending[s->storage_group] += size;
}

static uint64_t monotonic_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}
static int copy_ready(const struct input *in)
{
    return !in->server->work.run && (!in->storage_needed || in->server->storage_reserved);
}

int dcc_tcp_server_pending(void)
{
    for (unsigned i = 0; i < 256; ++i)
        for (struct upload *u = uploads[i]; u; u = u->next)
            if (u->ready && (!u->waiters || copy_ready(u->waiters))) return 1;
    return 0;
}

void dcc_tcp_server_collect(void)
{
    /* One snapshot per turn. The source remains PREPARING until borrowers
     * own their private copy or read-only link. */
    for (unsigned i = 0; i < 256; ++i) for (struct upload *u = uploads[i]; u; u = u->next) {
        if (!u->ready) continue;
        struct input *w = u->waiters;
        if (w) {
            struct dcc_tcp_server *s = w->server;
            if (!copy_ready(w)) continue;
            s->cache_index = w->index; s->copy_source = u->owner;
            ++u->owner->server->borrowers;
            if (dcc_tcp_async_submit(&s->work, copy_input, copy_complete, s)) {
                --u->owner->server->borrowers;
                s->copy_source = NULL; s->failed = 1;
            }
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
    do {
        dcc_tcp_async_stop();
        dcc_tcp_async_drain();
    } while (dcc_tcp_async_pending());
}

uint64_t dcc_tcp_ready_idle_ns(void)
{
    struct dcc_server_load load;
    int eligible = 0, available = 0;
    if (ready_head) {
        dcc_prefork_status(&load);
        for (struct dcc_tcp_server *s = ready_head; s; s = s->ready_next)
            if (s->job->kind != DCC_JOB_LINK || !load.links) { eligible = 1; break; }
        if (eligible) available = dcc_cpu_perf_limit(dcc_resource_slots(dcc_max_kids)) - load.busy;
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
    reserve_input(in);
    /* Preparation batches all wanted inputs. A promoted waiter must not
     * also send an early NEED that the final batch would repeat. */
    if (in->server->prepared < in->server->job->count || !in->server->storage_reserved) return;
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
            u->owner = u->waiters;
            u->waiters = u->waiters->next;
            u->owner->next = NULL;
            if (u->ready && in->server->job->kind == u->owner->server->job->kind &&
                !link(in->path, u->owner->path)) u->owner->wanted = 0;
            else { u->ready = 0; need(u->owner); }
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
struct tree_cleanup {
    struct dcc_tcp_work work;
    uint64_t bytes;
    char root[PATH_MAX];
    struct dcc_job *job;
};
static void prune_inputs(const struct dcc_job *job)
{
    uint64_t seen = 0;
    if (job) for (unsigned i = 0; i < job->count; ++i) {
        uint64_t shard = UINT64_C(1) << (job->inputs[i].digest[0] % DCC_FILE_CACHE_SHARDS);
        if (dcc_job_shared(job, i) && job->inputs[i].cached && !(seen & shard)) {
            seen |= shard;
            dcc_file_cache_prune(job->inputs[i].digest, dcc_job_tmp_top(job));
        }
    }
}
static void cleanup_tree(void *argument)
{
    struct tree_cleanup *tree = argument;
    remove_tree(AT_FDCWD, tree->root);
    prune_inputs(tree->job);
}
static void cleanup_complete(void *argument)
{
    struct tree_cleanup *tree = argument;
    retained -= tree->bytes;
    dcc_job_free(tree->job);
    free(tree);
}
static void release_inputs(struct dcc_tcp_server *s)
{
    if (s->storage_reserved) storage_pending[s->storage_group] -= s->input_pending;
    s->storage_reserved = 0;
    s->storage_group = 0;
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
    uint64_t bytes = s->input_bytes;
    if (s->run.root[0]) {
        struct tree_cleanup *tree = calloc(1, sizeof(*tree));
        if (tree) {
            strcpy(tree->root, s->run.root);
            tree->bytes = bytes;
            tree->job = s->job;
            if (!dcc_tcp_async_submit(&tree->work, cleanup_tree, cleanup_complete, tree)) {
                bytes = 0;
                s->job = NULL;
            }
            else { free(tree); tree = NULL; }
        }
        /* Allocation/thread creation failure must still reclaim the private tree. */
        if (!tree) {
            remove_tree(AT_FDCWD, s->run.root);
            prune_inputs(s->job);
        }
    }
    s->run.root[0] = 0;
    free(s->inputs); s->inputs = NULL;
    dcc_job_free(s->job); s->job = NULL;
    retained -= bytes;
    s->reserved -= s->input_bytes;
    s->input_bytes = 0;
    s->input_pending = 0;
}
static void reset_job(struct dcc_tcp_server *s)
{
    dequeue(s);
    release_inputs(s);
    memset(&s->run, 0, sizeof(s->run));
    dcc_job_result_free(&s->result);
    retained -= s->reserved;
    returning_bytes -= s->result_bytes;
    if (s->result_pending) --waiting_results;
    s->result_pending = 0;
    s->result_bytes = 0;
    s->reserved = 0;
    s->remaining = s->output_index = s->prepared = 0;
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
    s->local = domain == AF_UNIX;
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
    dcc_tcp_frame_clear(&s->decoding);
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
    if (s->work.run || s->borrowers) {
        close(s->socket); s->socket = -1;
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
    retained -= DCC_TCP_CHUNK + (s->frame.data ? s->frame.size : 0) +
                (s->decoding.data ? s->decoding.size : 0);
    dcc_tcp_frame_clear(&s->frame);
    dcc_tcp_frame_clear(&s->decoding);
    free(s->buffer); XXH3_freeState(s->hash); free(s);
}
static int path_order(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static void copy_input(void *argument)
{
    struct dcc_tcp_server *s = argument;
    int shared = dcc_job_shared(s->job, s->cache_index) &&
                 dcc_job_shared(s->copy_source->server->job, s->copy_source->index);
    s->cache_hit = dcc_file_cache_copy(NULL, s->copy_source->path, s->inputs[s->cache_index].path, shared);
}
static void copy_complete(void *argument)
{
    struct dcc_tcp_server *s = argument;
    struct dcc_tcp_server *owner = s->copy_source->server;
    struct input *in = &s->inputs[s->cache_index];
    s->copy_source = NULL;
    detach(in);
    if (s->cache_hit) completed_input(in);
    else s->failed = 1;
    --owner->borrowers;
    if (owner != s && owner->socket < 0) dcc_tcp_server_free(owner);
    if (s->socket < 0) dcc_tcp_server_free(s);
}
static int prepare(struct dcc_tcp_server *s)
{
    char **paths = calloc(s->job->count ? s->job->count : 1, sizeof(*paths));
    const char *tmp = dcc_job_tmp_top(s->job);
    if (!paths || !tmp) { free(paths); return -1; }
    int length = snprintf(s->run.root, sizeof(s->run.root), "%s/distcc-tcp-XXXXXX", tmp);
    if (length < 0 || (size_t)length >= sizeof(s->run.root) || !mkdtemp(s->run.root)) {
        s->run.root[0] = 0; free(paths); return -1;
    }
    char *canonical = realpath(s->run.root, NULL);
    if (!canonical || strlen(canonical) >= sizeof(s->run.root)) { free(canonical); free(paths); return -1; }
    strcpy(s->run.root, canonical); free(canonical);
    const char *regular;
    struct stat location, normal;
    if (dcc_get_tmp_top(&regular) || stat(regular, &normal) || stat(s->run.root, &location)) {
        free(paths); return -1;
    }
    /* The two work roots share reservations when they share a filesystem. */
    s->storage_group = location.st_dev != normal.st_dev;
    s->inputs = calloc(s->job->count ? s->job->count : 1, sizeof(*s->inputs));
    if (!s->inputs) { free(paths); return -1; }
    if (!dcc_job_tree(s->job)) {
        storage_pending[s->storage_group] += s->input_pending;
        s->storage_reserved = 1;
    }
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

/* Keep metadata while waiting for file space; a busy worker must not
 * truncate a valid request merely because another input tree is live. */
static int input_space(const struct dcc_tcp_server *s)
{
    if (!s->input_pending) return 1;
    const char *directory = dcc_job_tmp_top(s->job);
    struct statvfs space;
    if (!directory || statvfs(directory, &space)) return -1;
    uint64_t available = (uint64_t)space.f_bavail * space.f_frsize;
    uint64_t pending = storage_pending[s->storage_group];
    return pending <= available && s->input_pending <= available - pending;
}
static int prepare_available(struct dcc_tcp_server *s)
{
    int available = dcc_job_tree(s->job) ? 1 : input_space(s);
    if (available <= 0 || waiting_results) return available < 0 ? -1 : 0;
    /* Inputs live in files, not the framing heap. Reserve unwritten bytes
     * against their filesystem; completed files are reflected by statvfs. */
    return prepare(s) ? -1 : 1;
}
static void input_stored(struct input *in)
{
    if (!in->storage_needed) return;
    uint64_t size = in->server->job->inputs[in->index].plain_size;
    in->server->input_pending -= size;
    if (in->server->storage_reserved) storage_pending[in->server->storage_group] -= size;
    in->storage_needed = 0;
}
static int input_missing(struct dcc_tcp_server *s, unsigned i)
{
    struct input *in = &s->inputs[i];
    const struct dcc_job_input *description = &s->job->inputs[i];
    ++s->remaining;
    if (!description->cached) { reserve_input(in); in->wanted = 1; return 0; }
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
    if (u->owner == in || s->storage_group != u->owner->server->storage_group ||
        !dcc_job_shared(s->job, i) ||
        !dcc_job_shared(u->owner->server->job, u->owner->index)) reserve_input(in);
    return 0;
}
static void lookup_input(void *argument)
{
    struct dcc_tcp_server *s = argument;
    const struct dcc_job_input *in = &s->job->inputs[s->prepared];
    const char *path = s->inputs[s->prepared].path;
    s->cache_hit = dcc_file_cache_get(in->digest, path, dcc_job_shared(s->job, s->prepared),
                                     dcc_job_tmp_top(s->job));
    if (s->cache_hit) {
        struct stat st;
        if (stat(path, &st) || (uint64_t)st.st_size != in->plain_size) s->cache_hit = -1;
    }
}
static void lookup_complete(void *argument)
{
    struct dcc_tcp_server *s = argument;
    if (s->cache_hit < 0 || (!s->cache_hit && input_missing(s, s->prepared))) {
        rs_trace("TCP input %u cache lookup failed", s->prepared);
        s->failed = 1;
    }
    if (s->cache_hit > 0) input_stored(&s->inputs[s->prepared]);
    ++s->prepared;
    if (s->prepared == s->job->count && prepare_inputs(s)) s->failed = 1;
    if (s->socket < 0) dcc_tcp_server_free(s);
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
        if (!dcc_job_file(description)) continue;
        if (description->fd >= 0) continue;
        in->storage_needed = !dcc_job_shared(s->job, i);
        if (description->cached)
            return dcc_tcp_async_submit(&s->work, lookup_input, lookup_complete, s) ? -1 : 0;
        if (input_missing(s, i)) return -1;
    }
    if (s->prepared != s->job->count) return 0;
    if (!s->storage_reserved) {
        int available = input_space(s);
        if (available <= 0) return available < 0 ? -1 : 0;
        storage_pending[s->storage_group] += s->input_pending;
        s->storage_reserved = 1;
    }
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
    s->begun_at = monotonic_ns();
    s->metadata = dcc_job_memfd("distcc-tcp-manifest");
    if (s->metadata < 0 || dcc_writex(s->metadata, f->data + 8, f->size - 8) ||
        dcc_job_decode_buffer(f->data + 8, f->size - 8, &s->job)) return -1;
    if (f->fd_plus_one) {
        struct stat st;
        int fd = f->fd_plus_one - 1;
        int seals = fcntl(fd, F_GET_SEALS);
        int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
        if (s->job->kind != DCC_JOB_COMPILE || s->job->count != 1 ||
            s->job->inputs[0].kind != DCC_JOB_FILE || s->job->inputs[0].cached ||
            s->job->inputs[0].encoding != DCC_JOB_RAW || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size != s->job->inputs[0].plain_size ||
            (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY || seals < 0 ||
            (seals & required_seals) != required_seals) return -1;
        s->job->inputs[0].fd = fd; f->fd_plus_one = 0;
        ++local_input_jobs;
        local_input_bytes += st.st_size;
    }
    /* Include the encoded manifest, decoded strings and input bookkeeping,
     * in addition to the incoming frame already charged by the parser. */
    uint64_t metadata_bytes = 3 * f->size + s->job->count * sizeof(struct input);
    uint64_t amount = 0;
    for (unsigned i = 0; i < s->job->count; ++i) {
        if (s->job->inputs[i].fd >= 0 || dcc_job_shared(s->job, i)) continue;
        uint64_t size = s->job->inputs[i].plain_size;
        if (size > UINT64_MAX - amount) return -1;
        amount += size;
    }
    if (metadata_bytes > DCC_TCP_BYTES - retained) return -1;
    retained += metadata_bytes;
    s->reserved = s->input_bytes = metadata_bytes;
    s->input_pending = dcc_job_tree(s->job) ? 0 : amount;
    return prepare_available(s) < 0 ? -1 : 0;
}
static void completed_input(struct input *in)
{
    input_stored(in);
    in->wanted = 0; --in->server->remaining;
}
static void decode_input(void *argument)
{
    struct dcc_tcp_server *s = argument;
    unsigned index;
    size_t length;
    const void *data;
    s->input_error = dcc_tcp_unpack(&s->decoding, &index, s->buffer, &data, &length);
    if (s->input_error) return;
    s->input_error = dcc_writex(s->input_fd, data, length);
    if (!s->input_error && s->job->inputs[index].cached)
        s->input_error = XXH3_64bits_update(s->hash, data, length) != XXH_OK;
    if (!s->input_error && s->job->inputs[index].cached && (s->decoding.type & DCC_TCP_FINAL)) {
        XXH64_canonical_t digest;
        XXH64_canonicalFromHash(&digest, XXH3_64bits_digest(s->hash));
        if (s->input_offset + length != s->job->inputs[index].plain_size ||
            memcmp(digest.digest, s->job->inputs[index].digest, 8)) s->input_error = -1;
        else dcc_file_cache_put(s->job->inputs[index].digest, s->inputs[index].path,
                               dcc_job_shared(s->job, index), dcc_job_tmp_top(s->job));
    }
}
static int finish_input(struct dcc_tcp_server *s)
{
    const struct dcc_tcp_frame *f = &s->decoding;
    unsigned index = dcc_tcp_get32(f->data);
    size_t length = dcc_tcp_get32(f->data + 4);
    struct input *in = &s->inputs[index];
    const struct dcc_job_input *description = &s->job->inputs[index];
    if (s->input_error) return -1;
    s->input_offset += length;
    if (!(f->type & DCC_TCP_FINAL)) return s->input_offset < description->plain_size ? 0 : -1;
    if (s->input_offset != description->plain_size) return -1;
    close(s->input_fd); s->input_fd = -1;
    input_stored(in);
    if (in->upload) {
        in->wanted = 0;
        in->upload->ready = 1;
    } else completed_input(in);
    return 0;
}
static int complete_input(struct dcc_tcp_server *s)
{
    int ret = finish_input(s);
    retained -= s->decoding.size;
    dcc_tcp_frame_clear(&s->decoding);
    return ret;
}
static void input_complete(void *argument)
{
    struct dcc_tcp_server *s = argument;
    if (complete_input(s)) s->failed = 1;
    if (s->socket < 0) dcc_tcp_server_free(s);
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
    int compressed = f->type & DCC_TCP_COMPRESSED;
    s->decoding = s->frame;
    memset(&s->frame, 0, sizeof(s->frame));
    /* RAW already points into the receive frame: avoid a thread handoff for
     * a bounded write. Offload expansion, where independent jobs can use CPUs. */
    if ((length >= 65536 && compressed) ||
        (s->job->inputs[index].cached && (s->decoding.type & DCC_TCP_FINAL)))
        return dcc_tcp_async_submit(&s->work, decode_input, input_complete, s) ? -1 : 1;
    decode_input(s);
    return complete_input(s);
}
static int install_links(struct dcc_tcp_server *s)
{
    for (unsigned i = 0; i < s->job->count; ++i) {
        struct input *in = &s->inputs[i];
        const struct dcc_job_input *d = &s->job->inputs[i];
        if (dcc_job_file(d)) continue;
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
    /* A result exists already. Keep its FDs while asynchronous input cleanup
     * frees the budget; never discard completed work for transient pressure.
     * One connection buffer, the result header and one output chunk must fit. */
    if (bytes > DCC_TCP_BYTES - 2 * DCC_TCP_CHUNK - 88U) goto fail;
    returning_bytes += bytes; s->result_bytes = bytes;
    s->result.status = r->status; s->result.mode = r->mode;
    s->result.prepare_ns = s->ready_at - s->begun_at;
    s->result.queue_ns = s->started_at - s->ready_at;
    s->result.execute_ns = monotonic_ns() - s->started_at;
    s->result_pending = 1; ++waiting_results;
    s->phase = RETURNING;
    s->active = time(NULL);
    return;
fail:
    for (unsigned i = 0; i < count; ++i) if (fds[i] >= 0) close(fds[i]);
    dcc_tcp_server_lost(s);
}
static int result_available(struct dcc_tcp_server *s)
{
    return s->result_bytes + DCC_TCP_CHUNK + 88U <= DCC_TCP_BYTES - retained;
}
static int publish_result(struct dcc_tcp_server *s)
{
    retained += s->result_bytes; s->reserved += s->result_bytes;
    s->result_pending = 0; --waiting_results;
    unsigned char description[64];
    dcc_tcp_put32(description, s->result.status); dcc_tcp_put32(description + 4, s->result.mode);
    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i)
        dcc_tcp_put64(description + 8 + i * 8, s->result.size[i]);
    dcc_tcp_put64(description + 40, s->result.prepare_ns);
    dcc_tcp_put64(description + 48, s->result.queue_ns);
    dcc_tcp_put64(description + 56, s->result.execute_ns);
    return queue(s, DCC_TCP_RESULT, description, sizeof(description));
}
static void encode_output(void *argument)
{
    struct dcc_tcp_server *s = argument;
    unsigned i = s->output_index;
    uint64_t left = s->result.size[i] - s->output_offset;
    size_t amount = left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK;
    struct packet *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->data = dcc_tcp_file_chunk(DCC_TCP_OUTPUT, i, s->result.fd[i],
                                s->output_offset, amount, amount == left, 1, &p->size);
    if (!p->data) {
        free(p->data); free(p); return;
    }
    p->header_size = p->size;
    p->file = -1;
    if (p->size == 16) {
        p->file = s->result.fd[i]; p->file_left = amount; p->file_offset = s->output_offset;
    }
    unsigned char *compact = realloc(p->data, p->size);
    if (!compact) { free(p->data); free(p); return; }
    p->data = compact;
    s->encoded = p;
}
static int accept_output(struct dcc_tcp_server *s)
{
    struct packet *p = s->encoded; s->encoded = NULL;
    retained -= s->encoding_bytes; s->encoding_bytes = 0;
    if (!p) return -1;
    /* At most two packets, including the one being encoded. Result FDs
     * outlive both the sender and the encoder. */
    *s->tail = p; s->tail = &p->next;
    s->queued += p->size; retained += p->size;
    s->output_offset += dcc_tcp_get32(p->data + 12);
    return 0;
}
static void output_complete(void *argument)
{
    struct dcc_tcp_server *s = argument;
    if (accept_output(s)) s->failed = 1;
    if (s->socket < 0) dcc_tcp_server_free(s);
}
static int output_available(struct dcc_tcp_server *s)
{
    for (unsigned i = s->output_index; i < DCC_JOB_RESULT_COUNT; ++i) {
        uint64_t left = s->result.size[i] - (i == s->output_index ? s->output_offset : 0);
        if (!left) continue;
        size_t amount = s->local ? 12 : (left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK) + 16;
        return amount <= DCC_TCP_BYTES - retained;
    }
    return 1;
}
static int output(struct dcc_tcp_server *s)
{
    if (s->work.run) return 0;
    if (s->head && s->head->next) return 0;
    if (!output_available(s)) return 0;
    while (s->output_index < DCC_JOB_RESULT_COUNT) {
        unsigned i = s->output_index;
        uint64_t left = s->result.size[i] - s->output_offset;
        if (!left) { ++s->output_index; s->output_offset = 0; continue; }
        if (s->local) {
            unsigned char index[4];
            dcc_tcp_put32(index, i);
            if (queue(s, DCC_TCP_RESULT_FD, index, sizeof(index))) return -1;
            struct packet *p = s->head;
            while (p->next) p = p->next;
            p->file = s->result.fd[i]; p->rights = 1;
            s->output_offset = s->result.size[i];
            return 0;
        }
        size_t amount = left < DCC_TCP_CHUNK ? left : DCC_TCP_CHUNK;
        s->encoding_bytes = amount + 16; retained += s->encoding_bytes;
        if (amount >= 65536) {
            if (dcc_tcp_async_submit(&s->work, encode_output, output_complete, s)) return -1;
            if (s->head && (dcc_tcp_get32(s->head->data) & ~DCC_TCP_FLAGS) == DCC_TCP_OUTPUT)
                ++output_overlaps;
            return 0;
        }
        encode_output(s);
        return accept_output(s);
    }
    if (!s->head) {
        unsigned char id[8];
        dcc_tcp_put64(id, s->run.id);
        /* Reclaim job resources before exposing the reusable boundary. */
        reset_job(s);
        return queue(s, DCC_TCP_COMPLETE, id, sizeof(id));
    }
    return 0;
}
int dcc_tcp_server_poll(struct dcc_tcp_server *s, struct pollfd *p)
{
    p->fd = s->socket;
    int pending = s->work.run != NULL;
    if (s->job && s->phase == PREPARING && !s->inputs) {
        p->events = POLLRDHUP;
        p->revents = 0;
        return !waiting_results && (dcc_job_tree(s->job) || input_space(s) != 0);
    }
    if (s->job && s->prepared == s->job->count && !s->storage_reserved && s->phase == PREPARING) {
        p->events = POLLRDHUP;
        p->revents = 0;
        return !pending && input_space(s) != 0;
    }
    if (s->result_pending) {
        p->events = POLLRDHUP;
        p->revents = 0;
        return result_available(s);
    }
    p->events = POLLIN | (s->head ? POLLOUT : 0);
    if (pending && s->work.run != encode_output) p->events = POLLRDHUP;
    if (pending && s->work.run == decode_input) {
        int buffered = s->frame.data && s->frame.used == s->frame.size + 8;
        p->events = POLLRDHUP | (buffered ? 0 : POLLIN);
    }
    int preparing = s->job && s->phase == PREPARING && s->prepared < s->job->count;
    if (preparing) p->events = POLLRDHUP;
    p->revents = 0;
    int buffered = s->frame.data && s->frame.used == s->frame.size + 8;
    return !pending && (preparing || buffered ||
        (s->phase == RETURNING && output_available(s) && (!s->head ||
            (s->output_index < DCC_JOB_RESULT_COUNT && !s->head->next))));
}
int dcc_tcp_server_process(struct dcc_tcp_server *s, short events)
{
    time_t now = time(NULL);
    if (s->failed || now - s->active >= (s->job || s->phase == RETURNING ? dcc_get_io_timeout() : 30)) return -1;
    /* Once inputs are owned, silence is compiler/queue time, not a stalled
     * upload. Keep both transport deadlines alive without relaxing them for
     * unresponsive peers. No heartbeat is queued behind pending output. */
    if (s->job && (s->phase == RUNNING || s->enqueued) && !s->head &&
        now - s->active >= (dcc_get_io_timeout() > 1 ? dcc_get_io_timeout() / 2 : 1)) {
        unsigned char id[8];
        dcc_tcp_put64(id, s->run.id);
        if (queue(s, DCC_TCP_PROGRESS, id, sizeof(id))) return -1;
    }
    if (s->work.run == decode_input) {
        if (events & (POLLHUP | POLLERR | POLLRDHUP)) return -1;
        if (events & POLLIN) {
            size_t before = s->frame.used;
            int allocated = s->frame.data != NULL;
            size_t available = DCC_TCP_BYTES - retained;
            if (available > DCC_TCP_CHUNK + 8U) available = DCC_TCP_CHUNK + 8U;
            int ret = dcc_tcp_frame_read(s->socket, &s->frame, available);
            if (!allocated && s->frame.data) retained += s->frame.size;
            input_prefetch_bytes += s->frame.used - before;
            if (s->frame.used != before) s->active = now;
            if (ret < 0) return -1;
        }
        return 0;
    }
    if (s->work.run && s->work.run != encode_output)
        return events & (POLLHUP | POLLERR | POLLRDHUP) ? -1 : 0;
    if (s->result_pending) {
        if (events & (POLLHUP | POLLERR | POLLRDHUP)) return -1;
        if (!result_available(s)) return 0;
        if (publish_result(s)) return -1;
    }
    if (s->job && s->phase == PREPARING && !s->inputs) {
        if (events & (POLLHUP | POLLERR | POLLRDHUP)) return -1;
        int admitted = prepare_available(s);
        if (admitted <= 0) return admitted;
    }
    /* Bounded turns interleave other connections and worker completions. */
    if (s->job && s->phase == PREPARING && (s->prepared < s->job->count || !s->storage_reserved)) {
        if (prepare_inputs(s)) return -1;
        if (s->work.run) return 0;
        if (!s->storage_reserved && s->prepared == s->job->count) return 0;
        if (s->prepared < s->job->count) events &= ~POLLIN;
    }
    int buffered = s->frame.data && s->frame.used == s->frame.size + 8;
    if (buffered || (events & (POLLIN | POLLHUP | POLLERR))) for (unsigned turn = 0; turn < 4; ++turn) {
        size_t before = s->frame.used;
        int allocated = s->frame.data != NULL;
        int ret = dcc_tcp_frame_read(s->socket, &s->frame, DCC_TCP_BYTES - retained);
        if (!allocated && s->frame.data) retained += s->frame.size;
        if (s->frame.used != before) s->active = now;
        if (ret <= 0) {
            if (ret < 0) {
                rs_trace("TCP frame read failed with %llu bytes retained", (unsigned long long)retained);
                return -1;
            }
            break;
        }
        uint32_t type = s->frame.type & ~DCC_TCP_FLAGS;
        if (s->frame.fd_plus_one && type != DCC_TCP_BEGIN) return -1;
        if (type == DCC_TCP_BEGIN) {
            if (!s->job && s->head) return -1;
            ret = begin(s, &s->frame);
        } else if (type == DCC_TCP_INPUT) ret = receive_input(s, &s->frame);
        else ret = -1;
        if (ret == 1) break; /* The task borrows this frame until completion. */
        retained -= s->frame.size;
        dcc_tcp_frame_clear(&s->frame);
        if (ret) {
            rs_trace("TCP frame %x rejected with %llu bytes retained", type, (unsigned long long)retained);
            return -1;
        }
        if (s->job && s->phase == PREPARING && s->prepared < s->job->count) break;
    }
    if (s->job && s->inputs && s->storage_reserved && s->phase == PREPARING &&
        s->prepared == s->job->count && !s->remaining) {
        if (!s->enqueued) {
            if (install_links(s)) return -1;
            s->ready_at = monotonic_ns();
            dcc_tcp_ready_idle_ns();
            s->enqueued = 1; *ready_tail = s; ready_tail = &s->ready_next;
            dcc_tcp_ready_idle_ns();
        }
    }
    if (s->phase == RETURNING && output(s)) return -1;
    if (events & POLLOUT) for (unsigned turn = 0; turn < 4; ++turn) {
        struct packet *p = s->head;
        if (!p) break;
        ssize_t n;
        if (p->rights) {
            struct iovec iov = {p->data + p->sent, p->header_size - p->sent};
            union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control = {0};
            struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                .msg_control = control.data, .msg_controllen = sizeof(control)};
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &p->file, sizeof(int));
            n = sendmsg(s->socket, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) p->rights = 0;
        } else if (p->sent < p->header_size)
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
    if (waiting_results) return;
    if (returning_bytes && retained > DCC_TCP_BYTES * 3 / 4) return;
    struct dcc_server_load load;
    dcc_prefork_status(&load);
    int slots = dcc_cpu_perf_limit(dcc_max_kids) - load.busy;
    if (slots <= 0) return;
    /* Once per parent poll turn. Admission is workload-specific: a memory
     * calibration or large reservation must not block smaller known jobs.
     * Preserve ordering between links, which share one execution lane. */
    int link_waiting = 0;
    for (struct dcc_tcp_server *s = ready_head; s;) {
        struct dcc_tcp_server *next = s->ready_next;
        int link = s->job->kind == DCC_JOB_LINK;
        if (!link || !link_waiting) {
            s->run.operation = DCC_TCP_RUN;
            int input = s->job->kind == DCC_JOB_COMPILE && s->job->count == 1 ? s->job->inputs[0].fd : -1;
            int ret = s->dispatch->start(s, &s->run, s->metadata, input, s->job);
            if (ret < 0) { s->failed = 1; dequeue(s); }
            else if (ret == EXIT_BUSY) {
                /* All inputs arrived, but the measured reservation no longer
                 * fits. Keep unstarted work movable instead of pinning it
                 * behind an unrelated long compiler. RETRY is a reusable
                 * transaction boundary; no compiler or output has run. */
                unsigned char id[8];
                dcc_tcp_put64(id, s->run.id);
                reset_job(s);
                if (queue(s, DCC_TCP_RETRY, id, sizeof(id))) s->failed = 1;
            }
            else if (ret) {
                dequeue(s); s->phase = RUNNING; s->started_at = monotonic_ns();
                if (!--slots) break;
            }
            else if (link) link_waiting = 1;
        }
        s = next;
    }
}
