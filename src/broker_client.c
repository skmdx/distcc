/* Client-side RPCs on the local scheduling broker. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "distcc.h"
#include "hosts.h"
#include "broker.h"
#include "exitcode.h"
#include "trace.h"

#ifdef __linux__
#include <sys/socket.h>
#include <xxhash.h>
#include "broker_session.h"
#include "where.h"
#include "util.h"
#include "tcp_job.h"
#include "workload.h"
#include "plan_memo.h"

static int decode_memo(unsigned command, unsigned flags, void *data, size_t size, int descriptor)
{
    (void)flags;
    const char *value = data;
    if (descriptor >= 0)
        return command == WORKER_GET && size == 2 && !memcmp(value, "W", 2) ? 0 : EPROTO;
    if (size == 1 && !value[0]) return 0;
    if (command != WORKER_GET && ((size > 1 && size <= DCC_MEMO_LIMIT && !value[size-1]) ||
        (size > 1 + sizeof(uint32_t) && value[size-1] == 1))) return 0;
    return EPROTO;
}

static int memo_get(const char *key, char **value, size_t *length, int *worker, unsigned command,
                    int *verified, const uint8_t *digest)
{
    if (verified) *verified = 0;
    if (value) *value = NULL;
    if (worker) *worker = -1;
    size_t key_size = strlen(key) + 1;
    if (key_size > DCC_MEMO_LIMIT) { errno = EINVAL; return -1; }
    struct memo_refresh_request request = {.header = {BROKER_VERSION, command, 0, 0}};
    if (digest) memcpy(request.stale_digest, digest, sizeof(request.stale_digest));
    struct iovec parts[] = {{&request, digest ? sizeof(request) : sizeof(request.header)}, {(void *)key, key_size}};
    char short_reply[2];
    size_t capacity = worker ? sizeof(short_reply) : DCC_MEMO_LIMIT + sizeof(uint32_t);
    char *buffer = worker ? short_reply : malloc(capacity);
    if (!buffer) return -1;
    size_t size;
    int descriptor;
    int lease = dcc_session_call(&request.header, parts, 2, (struct iovec){buffer, capacity},
                                 &descriptor, &size, NULL, dcc_get_io_timeout(), decode_memo);
    if (lease < 0 || (size == 1 && !buffer[0])) {
        if (!worker) free(buffer);
        return lease;
    }
    dcc_session_finish(lease);
    if (worker) { *worker = descriptor; return -1; }
    if (buffer[size-1] == 1) {
        uint32_t count;
        size -= sizeof(count);
        memcpy(&count, buffer + size - 1, sizeof(count));
        if (verified && getuid() == geteuid() && getgid() == getegid() &&
            count <= DCC_PLAN_JOURNAL_LIMIT / sizeof(struct dcc_plan_dependency))
            *verified = count;
        buffer[size-1] = 0;
    }
    if (length) *length = size - 1;
    if (value) *value = buffer;
    else free(buffer);
    return -1;
}

int dcc_broker_memo_get(const char *key, char **value, size_t *length, int *verified) { return memo_get(key, value, length, NULL, MEMO_GET, verified, NULL); }
int dcc_broker_worker_get(const char *key, int *worker) { return memo_get(key, NULL, NULL, worker, WORKER_GET, NULL, NULL); }

int dcc_broker_memo_refresh(const char *key, const char *stale, size_t stale_length,
                            char **value, size_t *length, int *verified)
{
    XXH128_canonical_t digest;
    XXH128_canonicalFromHash(&digest, XXH3_128bits(stale, stale_length));
    return memo_get(key, value, length, NULL, MEMO_REFRESH, verified, digest.digest);
}

void dcc_broker_worker_put(int lease, int worker)
{
    if (worker < 0) dcc_broker_release(lease);
    else dcc_session_send_lease(lease, WORKER_PUT, NULL, 0, worker, FINISH_LEASE);
}

void dcc_broker_memo_put(int lease, const char *value, size_t length)
{
    if (!value || !length || length >= DCC_MEMO_LIMIT) { dcc_broker_release(lease); return; }
    struct iovec parts[] = {{(void *)value, length}, {(void *)"", 1}};
    dcc_session_send_lease(lease, MEMO_PUT, parts, 2, -1, FINISH_LEASE);
}

static void free_hosts(struct dcc_hostdef *host)
{
    while (host) {
        struct dcc_hostdef *next = host->next;
        dcc_free_hostdef(host);
        host = next;
    }
}

static int decode_schedule(unsigned command, unsigned flags, void *data, size_t size, int descriptor)
{
    const struct reply *reply = data;
    int acquire = command == ACQUIRE || command == ACQUIRE_POOL || command == ACQUIRE_LINK ||
                  command == ACQUIRE_CPP || command == ACQUIRE_READY;
    if (size != sizeof(*reply) ||
        ((acquire || command == JOBS) && !memchr(reply->host, 0, sizeof(reply->host)))) return EPROTO;
    if (reply->status)
        return reply->status == EXIT_ACCESS_DENIED ? EACCES :
               reply->status == EXIT_CONNECT_FAILED ? EHOSTUNREACH :
               reply->status == EXIT_LOCAL_COMPILE ? EXDEV : EPROTO;
    int direct = reply->flags & DCC_BROKER_DIRECT;
    if (direct && (!(flags & CLIENT_COMPILE) || (command != ACQUIRE_CPP && command != ACQUIRE_READY)))
        return EPROTO;
    return (acquire && !direct) == (descriptor >= 0) ? 0 : EPROTO;
}

static int broker_request(uint8_t command, uint8_t flags, struct reply *reply, int *net,
                          const struct dcc_work_hint *hint, int *queued_fd, uint32_t local_limit)
{
    struct broker_request_hdr header = {BROKER_VERSION, command, flags, 0};
    struct iovec parts[] = {{&header, sizeof(header)}, {0}};
    if (hint) parts[1] = (struct iovec){(void *)hint, dcc_work_hint_size(hint)};
    else if (command == LOCAL_CPP)
        parts[1] = (struct iovec){&local_limit, sizeof(local_limit)};
    int fd = dcc_session_call(&header, parts, 2, (struct iovec){reply, sizeof(*reply)},
                              net, NULL, queued_fd, 0, decode_schedule);
    if (fd < 0)
        rs_log_error("local broker request failed (command=%u): %s", command, strerror(errno));
    return fd;
}

int dcc_broker_local_cpp(int *lease, int *occupied, unsigned limit)
{
    struct reply reply;
    int net;
    *lease = broker_request(LOCAL_CPP, 0, &reply, &net, NULL, NULL, limit);
    if (*lease >= 0) *occupied = reply.slots;
    return *lease < 0 ? EXIT_CONNECT_FAILED : 0;
}

int dcc_broker_try_local(const struct dcc_work_hint *hint)
{
    struct reply reply;
    int net, lease = broker_request(TRY_LOCAL, 0, &reply, &net, hint, NULL, 0);
    if (lease >= 0 && !reply.slots) { dcc_session_finish(lease); lease = -1; }
    return lease;
}

void dcc_broker_local_done(int lease, double elapsed, uint64_t cpu_ns, int status)
{
    struct feedback timing = {0};
    if (!status && elapsed > 0) {
        timing.execute_ns = elapsed * 1e9;
        timing.cpu_ns = cpu_ns;
    }
    struct iovec part = {&timing, sizeof(timing)};
    dcc_session_send_lease(lease, RETURN, &part, 1, -1, FINISH_LEASE);
}

int dcc_broker_acquire(struct dcc_hostdef **host, int *lease, int cpp_ready, int can_compile_locally,
                       const struct dcc_work_hint *hint, int *queued_fd)
{
    struct reply reply;
    int count, net, fd;
    /* Preserve the client's --localslots and --localslots_cpp settings. */
    if (dcc_read_localslots_configuration())
        return EXIT_BAD_HOSTSPEC;
    fd = broker_request(cpp_ready ? ACQUIRE_READY : ACQUIRE_POOL,
                        can_compile_locally ? CLIENT_COMPILE : 0,
                        &reply, &net, hint, queued_fd, 0);
    if (fd < 0)
        return errno == EXDEV ? EXIT_LOCAL_COMPILE : EXIT_CONNECT_FAILED;
    *host = NULL;
    count = 0;
    if (dcc_parse_hosts(reply.host, "broker", host, &count, NULL) ||
        count != 1 || (*host)->mode != DCC_MODE_TCP) {
        free_hosts(*host);
        *host = NULL;
        if (net >= 0) close(net);
        dcc_broker_release(fd);
        return EXIT_BAD_HOSTSPEC;
    }
    (*host)->broker_managed = 1;
    (*host)->broker_cpp_reserved = !cpp_ready;
    if (reply.flags & DCC_BROKER_DIRECT) (*host)->mode = DCC_MODE_LOCAL;
    (*host)->broker_fd = net;
    (*host)->broker_lease = fd;
    (*host)->broker_pooled = !!(reply.flags & DCC_BROKER_POOLED);
    *lease = fd;
    return 0;
}

void dcc_broker_return(const struct dcc_hostdef *host, int fd, const struct dcc_job_result *result)
{
    if (!host->broker_pooled) return;
    struct feedback timing = {0};
    struct dcc_load_report load = {0};
    if (result) {
        load = result->load;
        if (!result->status)
            timing = (struct feedback){result->prepare_ns, result->queue_ns, result->execute_ns, result->cpu_ns};
    }
    struct iovec parts[] = {{&timing, sizeof(timing)}, {&load, sizeof(load)}};
    dcc_session_send_lease(host->broker_lease, RETURN, parts, 2, fd, RETIRE_LEASE);
}

void dcc_broker_failed(const struct dcc_hostdef *host)
{
    if (host->broker_managed)
        dcc_session_send_lease(host->broker_lease, FAILED, NULL, 0, -1, RETIRE_LEASE);
}

void dcc_broker_defer(const struct dcc_hostdef *host)
{
    if (host->broker_pooled)
        dcc_session_send_lease(host->broker_lease, DEFER, NULL, 0, host->broker_fd, RETIRE_LEASE);
}
int dcc_broker_jobs(char **capacity)
{
    struct reply reply;
    int net, fd = broker_request(JOBS, 0, &reply, &net, NULL, NULL, 0);
    if (fd < 0)
        return -1;
    dcc_session_finish(fd);
    if (capacity && !(*capacity = strdup(reply.host)))
        return -1;
    return reply.slots;
}

int dcc_broker_cgu_capacity(const struct dcc_work_hint *hint, int *native)
{
    struct reply reply;
    int net, fd = broker_request(CGU_CAPACITY, 0, &reply, &net, hint, NULL, 0);
    if (fd < 0) return -1;
    *native = reply.native_peers;
    dcc_session_finish(fd);
    return reply.slots;
}

void dcc_broker_cpp_done(const struct dcc_hostdef *host)
{
    if (host->broker_managed && host->broker_cpp_reserved)
        dcc_session_send_lease(host->broker_lease, CPP_DONE, NULL, 0, -1, KEEP_LEASE);
}
#else
int dcc_broker_worker_get(const char *key, int *worker)
{
    (void)key;
    *worker = -1;
    return -1;
}
void dcc_broker_worker_put(int lease, int worker)
{
    (void)lease;
    (void)worker;
}
int dcc_broker_local_cpp(int *lease, int *occupied, unsigned limit)
{
    (void)limit;
    (void)occupied;
    *lease = -1;
    return EXIT_CONNECT_FAILED;
}
void dcc_broker_failed(const struct dcc_hostdef *host) { (void)host; }
void dcc_broker_defer(const struct dcc_hostdef *host) { (void)host; }
void dcc_broker_return(const struct dcc_hostdef *host, int fd, const struct dcc_job_result *result)
{
    (void)host;
    (void)fd;
    (void)result;
}
int dcc_broker_memo_get(const char *key, char **value, size_t *length, int *verified)
{
    (void)key;
    if (value) *value = NULL;
    if (verified) *verified = 0;
    if (length) *length = 0;
    return -1;
}
int dcc_broker_memo_refresh(const char *key, const char *stale, size_t stale_length, char **value, size_t *length, int *verified)
{
    (void)stale_length;
    (void)stale;
    return dcc_broker_memo_get(key,value,length,verified);
}
void dcc_broker_memo_put(int lease, const char *value, size_t length)
{
    (void)lease;
    (void)length;
    (void)value;
}
int dcc_broker_try_local(const struct dcc_work_hint *hint) { (void)hint; return -1; }
void dcc_broker_local_done(int lease, double elapsed, uint64_t cpu_ns, int status)
{
    (void)lease; (void)elapsed; (void)cpu_ns; (void)status;
}
int dcc_broker_acquire(struct dcc_hostdef **host, int *lease, int cpp_ready, int can_compile_locally,
                       const struct dcc_work_hint *hint, int *queued_fd)
{
    (void)queued_fd;
    (void)can_compile_locally;
    (void)hint;
    (void)cpp_ready;
    (void)host;
    (void)lease;
    return EXIT_CONNECT_FAILED;
}
int dcc_broker_jobs(char **capacity)
{
    (void)capacity;
    return -1;
}
int dcc_broker_cgu_capacity(const struct dcc_work_hint *hint, int *native) { (void)hint; *native = 0; return 0; }
void dcc_broker_cpp_done(const struct dcc_hostdef *host)
{
    (void)host;
}
#endif
