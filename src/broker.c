/* Local scheduling broker. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <ifaddrs.h>

#include "distcc.h"
#include "hosts.h"
#include "adaptive.h"
#include "broker.h"
#include "resource.h"
#include <netinet/tcp.h>
#include "exitcode.h"
#include "trace.h"
#include "util.h"
#include "tcp_job.h"
#include "workload.h"

#ifdef __linux__

/* This is local IPC between matching versions, never a network protocol.
 * The control connection is a lease: EOF releases the reserved slot. Bulk
 * compiler traffic goes directly over the descriptor sent with the reply.
 * Only an explicit RETURN with a descriptor can populate the idle pool. */
#define BROKER_VERSION 2
#define MAX_HOSTS 128
#define MAX_CLIENTS 256
enum { ACQUIRE = 1, JOBS = 2, CPP_DONE = 3, MEMO_GET = 4, MEMO_PUT = 5,
       ACQUIRE_POOL = 6, RETURN = 7, ACQUIRE_LINK = 8, ACQUIRE_CPP = 9,
       ACQUIRE_READY = 10, FAILED = 11, LOCAL_CPP = 12, LOCAL_ANALYSIS = 13, DEFER = 14,
       TRY_LOCAL = 15 };
#define MAX_MEMOS 32
/* The caller can compile its original command locally. Backend-only clients
 * still require a connected worker socket, even on this machine. */
#define CLIENT_COMPILE (1u << 16)
struct request { uint32_t version, command; };
struct feedback { uint64_t prepare_ns, queue_ns, execute_ns; };
struct reply { int32_t status, slots; char host[1024]; };

struct peer {
    struct dcc_hostdef *host;
    struct sockaddr_storage address;
    socklen_t address_len;
    struct dcc_capacity metrics;
    double valid_until, retry_at, deadline;
    int busy, stats_fd, stats_reading;
    uint64_t serial, probe_serial, observed_serial;
    int completed;
    double full_until, link_until, probe_after;
    int pool_supported;
    int admission_supported;
    int local, direct;
    size_t used;
    char stats[DCC_STATS_SIZE];
    /* Seconds/byte for prepared network input; seconds/job otherwise. */
    double prepare_cost[DCC_WORK_CLASSES];
    double mean_cost;
};
enum client_state { REQUEST, WAITING, CONNECTING, HELLO_SEND, HELLO_READ,
                    RESERVE_SEND, RESERVE_READ, ACTIVE, MEMO_OWNER, MEMO_WAIT };
struct client {
    int fd, net, peer, cpp_reserved, direct, can_compile_locally;
    uint64_t serial;
    enum client_state state;
    uint32_t command;
    double deadline;
    int memo;
    int pooled;
    int reused;
    int admission;
    size_t used;
    char hello[12];
    struct dcc_work_hint hint;
    double cost, ready_at, run_at, factor, waiting_since;
};
struct memo { char *key, *value; double used; };
struct idle_connection { int fd, peer; double expires; };
struct broker {
    struct peer peers[MAX_HOSTS];
    struct client clients[MAX_CLIENTS];
    int count, cpp_busy, cpp_limit, analysis_busy;
    uint64_t direct_completed, direct_prepared_completed;
    double refresh_at, first_probe_until;
    struct memo memos[MAX_MEMOS];
    struct idle_connection pool[MAX_CLIENTS];
    struct dcc_work_model model;
};

static int unix_address(const char *path, struct sockaddr_un *address);

int dcc_broker_memo_get(const char *key, char **value)
{
    struct sockaddr_un address;
    struct request request = { BROKER_VERSION, MEMO_GET };
    struct iovec parts[2] = {{ &request, sizeof(request) }, { (void *)key, strlen(key) + 1 }};
    struct msghdr msg = { .msg_iov = parts, .msg_iovlen = 2 };
    struct timeval timeout = { 5, 0 };
    char *buffer;
    int fd;
    ssize_t size;
    *value = NULL;
    if (strlen(key) >= DCC_MEMO_LIMIT || unix_address(getenv("DISTCC_BROKER"), &address))
        return -1;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    buffer = malloc(DCC_MEMO_LIMIT);
    if (!buffer || connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        sendmsg(fd, &msg, MSG_NOSIGNAL) != (ssize_t)(sizeof(request) + strlen(key) + 1))
        goto failed;
    size = recv(fd, buffer, DCC_MEMO_LIMIT, MSG_TRUNC);
    if (size == 1 && !buffer[0]) { free(buffer); return fd; }
    if (size > 1 && size <= DCC_MEMO_LIMIT && !buffer[size - 1] &&
        strlen(buffer) == (size_t)size - 1) {
        *value = buffer;
        close(fd);
        return -1;
    }
failed:
    free(buffer);
    close(fd);
    return -1;
}

void dcc_broker_memo_put(int lease, const char *value)
{
    struct request request = { BROKER_VERSION, MEMO_PUT };
    struct iovec parts[2] = {{ &request, sizeof(request) }, { (void *)value, value ? strlen(value) + 1 : 0 }};
    struct msghdr msg = { .msg_iov = parts, .msg_iovlen = 2 };
    if (lease < 0) return;
    if (value && *value && strlen(value) < DCC_MEMO_LIMIT)
        sendmsg(lease, &msg, MSG_NOSIGNAL);
    close(lease);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void free_hosts(struct dcc_hostdef *host)
{
    while (host) {
        struct dcc_hostdef *next = host->next;
        dcc_free_hostdef(host);
        host = next;
    }
}

static int unix_address(const char *path, struct sockaddr_un *address)
{
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address->sun_path)) {
        errno = EINVAL;
        return -1;
    }
    strcpy(address->sun_path, path);
    return 0;
}

static int send_reply(int fd, const struct reply *reply, int net)
{
    return dcc_send_fd(fd, reply, sizeof(*reply), net);
}

static int broker_request_once(uint32_t command, struct reply *reply, int *net,
                               const struct dcc_work_hint *hint, double deadline, int *queued_fd)
{
    struct sockaddr_un address;
    struct { struct request header; struct dcc_work_hint hint; } request = {{ BROKER_VERSION, command }, {0}};
    size_t request_size = sizeof(request.header);
    if (hint) { request.hint = *hint; request_size = sizeof(request); }
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))];
    } control;
    struct iovec iov = { reply, sizeof(*reply) };
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    int fd, error, passcred = 1, authenticated = 0, received_fds = 0;
    ssize_t n = -1;
    *net = -1;
    if (unix_address(getenv("DISTCC_BROKER"), &address))
        return -1;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    /* Authenticate the reply sender, not the listener's creator. A systemd
     * listener belongs to root, whose UID may be unmapped in a build userns. */
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)))
        goto failed;
    if (deadline) {
        struct timeval timeout;
        double remaining = deadline - now_seconds();
        if (remaining <= 0) { errno = ETIMEDOUT; goto failed; }
        timeout.tv_sec = (time_t)remaining;
        timeout.tv_usec = (remaining - timeout.tv_sec) * 1000000 + 1;
        if (timeout.tv_usec >= 1000000) {
            timeout.tv_sec++;
            timeout.tv_usec = 0;
        }
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)))
            goto failed;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)))
        goto failed;
    ssize_t sent = send(fd, &request, request_size, MSG_NOSIGNAL);
    if (sent != (ssize_t)request_size &&
        errno != EPIPE && errno != ECONNRESET)
        goto failed;
    /* A planner may start its next ranked job once this request is queued.
     * Do not wait for admission: a memory-limited job must not prevent a
     * later job from using a different available worker. */
    if (sent == (ssize_t)request_size && queued_fd && *queued_fd >= 0) {
        ssize_t written;
        do { written = write(*queued_fd, "Q", 1); } while (written < 0 && errno == EINTR);
        if (written != 1) {
            if (errno == EPIPE) errno = ECANCELED; /* Planner exit is not a broker outage. */
            goto failed;
        }
        close(*queued_fd);
        *queued_fd = -1;
    }
    /* A rejected client can receive an error even if the broker closed before
     * send(). Read that reply so an access denial is not retried as an outage. */
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    do {
        n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    for (cmsg = CMSG_FIRSTHDR(&msg); n >= 0 && cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET)
            continue;
        if (cmsg->cmsg_type == SCM_CREDENTIALS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred credentials;
            memcpy(&credentials, CMSG_DATA(cmsg), sizeof(credentials));
            authenticated = geteuid() == 0 || credentials.uid == geteuid() || credentials.uid == 0;
        } else if (cmsg->cmsg_type == SCM_RIGHTS) {
            size_t offset, size = cmsg->cmsg_len - CMSG_LEN(0);
            for (offset = 0; offset + sizeof(int) <= size; offset += sizeof(int)) {
                int received;
                memcpy(&received, (char *)CMSG_DATA(cmsg) + offset, sizeof(received));
                if (!received_fds++) *net = received;
                else close(received);
            }
        }
    }
    if (n <= 0) {
        if (!n) errno = ECONNRESET;
        goto failed;
    }
    if (!authenticated) {
        errno = EACCES;
        goto failed;
    }
    if (n != sizeof(*reply) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        received_fds > 1 || !memchr(reply->host, '\0', sizeof(reply->host))) {
        errno = EPROTO;
        goto failed;
    }
    if (reply->status) {
        /* Admission can be temporarily unavailable while failed workers
         * restart or remain in backoff. No job has been sent on this lease. */
        errno = reply->status == EXIT_ACCESS_DENIED ? EACCES :
                reply->status == EXIT_CONNECT_FAILED ? EHOSTUNREACH : EPROTO;
        goto failed;
    }
    int can_compile_locally = command & CLIENT_COMPILE;
    command &= ~CLIENT_COMPILE;
    int direct = (reply->slots & DCC_BROKER_DIRECT) && can_compile_locally &&
                 (command == ACQUIRE_CPP || command == ACQUIRE_READY);
    if (((command == ACQUIRE || command == ACQUIRE_POOL || command == ACQUIRE_CPP ||
          command == ACQUIRE_READY) && !direct && *net < 0) ||
        (direct && *net >= 0) ||
        ((command == JOBS || command == LOCAL_CPP || command == LOCAL_ANALYSIS ||
          command == TRY_LOCAL) && *net >= 0)) {
        errno = EPROTO;
        goto failed;
    }
    return fd;
failed:
    error = errno;
    if (*net >= 0) {
        close(*net);
        *net = -1;
    }
    close(fd);
    errno = error;
    return -1;
}

static int broker_request(uint32_t command, struct reply *reply, int *net,
                          const struct dcc_work_hint *hint, int *queued_fd)
{
    double deadline = 0;
    int delay = 100, fd;
    const char *setting = getenv("DISTCC_RECONNECT_TIMEOUT");
    int timeout = setting ? atoi(setting) : 90;
    /* Closing a failed control socket releases its lease. No compiler request
     * has been sent yet, so reconnecting cannot replay a completed job. */
    while ((fd = broker_request_once(command, reply, net, hint, deadline, queued_fd)) < 0) {
        int error = errno;
        if (error != ENOENT && error != ECONNREFUSED &&
            error != ECONNRESET && error != EPIPE &&
            !(error == EHOSTUNREACH && command != JOBS))
            break;
        if (!deadline) deadline = now_seconds() + timeout;
        double remaining = deadline - now_seconds();
        if (remaining <= 0) break;
        poll(NULL, 0, remaining * 1000 < delay ? (int)(remaining * 1000) : delay);
        errno = error;
        if (delay < 1000) delay *= 2;
    }
    if (fd < 0)
        rs_log_error("local broker request failed (command=%u): %s", command, strerror(errno));
    return fd;
}

int dcc_broker_local_cpp(int *lease, int *occupied, int analysis)
{
    struct reply reply;
    int net;
    *lease = broker_request(analysis ? LOCAL_ANALYSIS : LOCAL_CPP, &reply, &net, NULL, NULL);
    if (*lease >= 0) *occupied = reply.slots;
    return *lease < 0 ? EXIT_CONNECT_FAILED : 0;
}

int dcc_broker_try_local(const struct dcc_work_hint *hint)
{
    struct reply reply;
    int net, lease = broker_request(TRY_LOCAL, &reply, &net, hint, NULL);
    if (lease >= 0 && !reply.slots) { close(lease); lease = -1; }
    return lease;
}

void dcc_broker_local_done(int lease, double elapsed, int status)
{
    struct { struct request header; struct feedback timing; } request = {{ BROKER_VERSION, RETURN }, {0}};
    if (!status && elapsed > 0) request.timing.execute_ns = elapsed * 1e9;
    send(lease, &request, sizeof(request), MSG_NOSIGNAL);
    close(lease);
}

int dcc_broker_acquire(struct dcc_hostdef **host, int *lease, int cpp_ready,
                       const struct dcc_work_hint *hint, int *queued_fd)
{
    struct reply reply;
    struct dcc_hostdef *local = NULL;
    int count, net, fd;
    /* Preserve the client's --localslots and --localslots_cpp settings. */
    if (dcc_get_hostlist(&local, &count))
        return EXIT_BAD_HOSTSPEC;
    free_hosts(local);
    int remote_cpp = getenv("INCLUDE_SERVER_PORT") != NULL;
    fd = broker_request(CLIENT_COMPILE | (remote_cpp ? ACQUIRE_CPP : cpp_ready ? ACQUIRE_READY : ACQUIRE_POOL),
                        &reply, &net, hint, queued_fd);
    if (fd < 0)
        return EXIT_CONNECT_FAILED;
    *host = NULL;
    count = 0;
    if (dcc_parse_hosts(reply.host, "broker", host, &count, NULL) ||
        count != 1 || (*host)->mode != DCC_MODE_TCP) {
        free_hosts(*host);
        *host = NULL;
        close(net);
        close(fd);
        return EXIT_BAD_HOSTSPEC;
    }
    (*host)->broker_managed = 1;
    if (reply.slots & DCC_BROKER_DIRECT) (*host)->mode = DCC_MODE_LOCAL;
    if (remote_cpp || cpp_ready) {
        (*host)->cpp_where = remote_cpp ? DCC_CPP_ON_SERVER : DCC_CPP_ON_CLIENT;
        if ((*host)->compr == DCC_COMPRESS_NONE)
            (*host)->compr = DCC_COMPRESS_LZO1X;
        dcc_get_protover_from_features((*host)->compr, (*host)->cpp_where,
                                       &(*host)->protover);
    }
    (*host)->broker_fd = net;
    (*host)->broker_lease = fd;
    (*host)->broker_pooled = !!(reply.slots & DCC_BROKER_POOLED);
    *lease = fd;
    return 0;
}

void dcc_broker_return(const struct dcc_hostdef *host, int fd, const struct dcc_job_result *result)
{
    struct { struct request header; struct feedback timing; } request = {{ BROKER_VERSION, RETURN }, {0}};
    if (result && !result->status)
        request.timing = (struct feedback){result->prepare_ns, result->queue_ns, result->execute_ns};
    if (host->broker_pooled)
        dcc_send_fd(host->broker_lease, &request, sizeof(request), fd);
}

void dcc_broker_failed(const struct dcc_hostdef *host)
{
    struct request request = { BROKER_VERSION, FAILED };
    if (host->broker_managed)
        send(host->broker_lease, &request, sizeof(request), MSG_NOSIGNAL);
}

void dcc_broker_defer(const struct dcc_hostdef *host)
{
    struct request request = { BROKER_VERSION, DEFER };
    if (host->broker_pooled)
        dcc_send_fd(host->broker_lease, &request, sizeof(request), host->broker_fd);
}

int dcc_broker_jobs(char **capacity)
{
    struct reply reply;
    int net, fd = broker_request(JOBS, &reply, &net, NULL, NULL);
    if (fd < 0)
        return -1;
    close(fd);
    if (capacity && !(*capacity = strdup(reply.host)))
        return -1;
    return reply.slots;
}

void dcc_broker_cpp_done(const struct dcc_hostdef *host)
{
    struct request request = { BROKER_VERSION, CPP_DONE };
    if (host->broker_managed &&
        send(host->broker_lease, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request))
        rs_log_warning("could not release broker preprocessing reservation");
}

static int connect_peer(const struct peer *peer, int port)
{
    struct sockaddr_storage address = peer->address;
    int fd;
    struct sockaddr_un local;
    if (peer->local && peer->host->tcp_jobs && port == peer->host->port &&
        !dcc_tcp_local_address(port, &local)) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            if (!connect(fd, (struct sockaddr *)&local, sizeof(local))) return fd;
            close(fd);
        }
    }
    if (!peer->address_len)
        return -1;
    if (address.ss_family == AF_INET)
        ((struct sockaddr_in *)&address)->sin_port = htons(port);
    else
        ((struct sockaddr_in6 *)&address)->sin6_port = htons(port);
    fd = socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && peer->host->tcp_jobs && port == peer->host->port) {
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
            close(fd);
            return -1;
        }
    }
    if (fd >= 0 && connect(fd, (struct sockaddr *)&address, peer->address_len) &&
        errno != EINPROGRESS) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static int connected(int fd)
{
    int error;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length)) return -1;
    if (error) { errno = error; return -1; }
    return 0;
}

static void finish_probe(struct peer *peer, double now, int complete)
{
    struct dcc_capacity metrics;
    peer->stats[peer->used] = '\0';
    if (complete && !dcc_parse_capacity(peer->stats, &metrics)) {
        peer->metrics = metrics;
        peer->observed_serial = peer->probe_serial;
        peer->completed = 0;
        peer->valid_until = now + 5;
        peer->admission_supported = peer->metrics.admission == 1;
    }
    close(peer->stats_fd);
    peer->stats_fd = -1;
}

static void probe_event(struct peer *peer, double now)
{
    static const char request[] = "GET / HTTP/1.0\r\n\r\n";
    ssize_t n;
    if (!peer->stats_reading) {
        if (connected(peer->stats_fd) ||
            send(peer->stats_fd, request, sizeof(request) - 1, MSG_NOSIGNAL) !=
            sizeof(request) - 1) {
            finish_probe(peer, now, 0);
            return;
        }
        peer->stats_reading = 1;
    }
    n = recv(peer->stats_fd, peer->stats + peer->used,
             sizeof(peer->stats) - 1 - peer->used, 0);
    if (n > 0) {
        peer->used += n;
        if (peer->used == sizeof(peer->stats) - 1)
            finish_probe(peer, now, 0);
    } else if (!n || (errno != EAGAIN && errno != EINTR)) {
        finish_probe(peer, now, !n);
    }
}

static void release_cpp(struct broker *broker, struct client *client)
{
    broker->cpp_busy -= client->cpp_reserved;
    if (client->command == LOCAL_ANALYSIS)
        broker->analysis_busy -= client->cpp_reserved;
    client->cpp_reserved = 0;
}

static void release_assignment(struct broker *broker, struct client *client)
{
    release_cpp(broker, client);
    if (client->peer >= 0) {
        struct peer *p = &broker->peers[client->peer];
        p->busy--;
        if (client->direct) p->direct--;
    }
    if (client->net >= 0) close(client->net);
    client->net = client->peer = -1;
}

static void release_client(struct broker *broker, struct client *client)
{
    if (client->state == MEMO_OWNER && !broker->memos[client->memo].value) {
        int i;
        free(broker->memos[client->memo].key);
        broker->memos[client->memo].key = NULL;
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *waiter = &broker->clients[i];
            if (waiter->fd >= 0 && waiter->state == MEMO_WAIT && waiter->memo == client->memo)
                release_client(broker, waiter);
        }
    }
    release_assignment(broker, client);
    close(client->fd);
    client->fd = client->net = client->peer = -1;
}

static void memo_request(struct broker *broker, struct client *client,
                         unsigned command, const char *payload, double now)
{
    int i, slot = -1;
    struct memo *memo;
    if (command == MEMO_GET && client->state == REQUEST) {
        for (i = 0; i < MAX_MEMOS; i++) {
            memo = &broker->memos[i];
            if (memo->key && !strcmp(memo->key, payload)) {
                memo->used = now;
                if (memo->value) {
                    send(client->fd, memo->value, strlen(memo->value) + 1, MSG_NOSIGNAL);
                    release_client(broker, client);
                } else {
                    client->state = MEMO_WAIT;
                    client->memo = i;
                    client->deadline = now + 5;
                }
                return;
            }
            if ((!memo->key || memo->value) &&
                (slot < 0 || memo->used < broker->memos[slot].used)) slot = i;
        }
        if (slot >= 0) {
            memo = &broker->memos[slot];
            free(memo->key);
            free(memo->value);
            memo->key = strdup(payload);
            memo->value = NULL;
            memo->used = now;
            if (memo->key && send(client->fd, "", 1, MSG_NOSIGNAL) == 1) {
                client->memo = slot;
                client->state = MEMO_OWNER;
                client->deadline = now + 5;
                return;
            }
            free(memo->key);
            memo->key = NULL;
        }
    } else if (command == MEMO_PUT && client->state == MEMO_OWNER) {
        memo = &broker->memos[client->memo];
        memo->value = strdup(payload);
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *waiter = &broker->clients[i];
            if (waiter->fd >= 0 && waiter->state == MEMO_WAIT && waiter->memo == client->memo) {
                if (memo->value)
                    send(waiter->fd, payload, strlen(payload) + 1, MSG_NOSIGNAL);
                release_client(broker, waiter);
            }
        }
    }
    release_client(broker, client);
}

static int pool_take(struct broker *broker, int peer, double now)
{
    int i;
    for (i = MAX_CLIENTS - 1; i >= 0; --i) {
        struct idle_connection *idle = &broker->pool[i];
        char byte;
        if (idle->fd < 0) continue;
        if (now >= idle->expires || recv(idle->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) != -1 || errno != EAGAIN) {
            close(idle->fd);
            idle->fd = -1;
        } else if (idle->peer == peer) {
            int fd = idle->fd;
            idle->fd = -1;
            return fd;
        }
    }
    return -1;
}

static void pool_store(struct broker *broker, struct client *client, int fd, double now)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    struct peer *peer = &broker->peers[client->peer];
    int i, type, valid = 0;
    socklen_t type_length = sizeof(type);
    if (!getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) && type == SOCK_STREAM &&
        !getpeername(fd, (struct sockaddr *)&address, &length)) {
        if (address.ss_family == AF_UNIX && peer->local && peer->host->tcp_jobs) {
            struct sockaddr_un local;
            valid = !dcc_tcp_local_address(peer->host->port, &local) &&
                length <= sizeof(local) && length > offsetof(struct sockaddr_un, sun_path) &&
                !memcmp(&address, &local, length);
        } else if (address.ss_family == peer->address.ss_family && address.ss_family == AF_INET) {
            struct sockaddr_in *a = (void *)&address, *b = (void *)&peer->address;
            valid = a->sin_port == htons(peer->host->port) && a->sin_addr.s_addr == b->sin_addr.s_addr;
        } else if (address.ss_family == peer->address.ss_family && address.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (void *)&address, *b = (void *)&peer->address;
            valid = a->sin6_port == htons(peer->host->port) && a->sin6_scope_id == b->sin6_scope_id &&
                    !memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr));
        }
    }
    if (valid) {
        for (i = 0; i < MAX_CLIENTS; ++i)
            if (broker->pool[i].fd < 0) {
                broker->pool[i] = (struct idle_connection){ fd, client->peer, now + 10 };
                return;
            }
    }
    close(fd);
}

static void deliver_connection(struct broker *broker, struct client *client)
{
    struct reply reply = { .slots = client->direct ?
        (client->command == TRY_LOCAL ? 1 : DCC_BROKER_DIRECT) :
        client->pooled ? DCC_BROKER_POOLED : 0 };
    strcpy(reply.host, broker->peers[client->peer].host->hostdef_string);
    if (send_reply(client->fd, &reply, client->net)) {
        release_client(broker, client);
        return;
    }
    if (client->net >= 0) close(client->net);
    client->net = -1;
    client->state = ACTIVE;
}

/* Service time while executing, excluding the queue predicted below. Replace
 * this daemon's runnable compilers with the projected execution concurrency;
 * external CPU demand remains in the existing contention calculation. */
static double execution_factor(const struct peer *peer, int work, int recovering, int capacity)
{
    struct dcc_capacity cap = peer->metrics;
    if (cap.busy >= 0) cap.busy += peer->direct;
    cap.capacity = capacity;
    if (recovering) cap.cpu_scale = 1;
    int parallel = cap.cpus < cap.jobs ? cap.cpus : cap.jobs;
    if (cap.capacity >= 0 && cap.capacity < parallel) parallel = cap.capacity;
    if (parallel < 1) parallel = 1;
    int executing = work < parallel ? work + 1 : parallel;
    if (cap.running >= 0) {
        cap.running -= cap.busy > 0 ? cap.busy : 0;
        if (cap.running < 0) cap.running = 0;
        cap.running += executing - 1;
        if (cap.running < 0) cap.running = 0;
    } else {
        cap.load -= cap.busy > 0 ? cap.busy : 0;
        if (cap.load < 0) cap.load = 0;
        cap.load += executing - 1;
    }
    cap.busy = executing - 1;
    cap.queued = 0;
    return dcc_capacity_score(peer->host, &cap, executing - 1);
}

static int assigned_order(const void *a, const void *b)
{
    const struct client *left = *(const struct client *const *)a;
    const struct client *right = *(const struct client *const *)b;
    return (left->serial > right->serial) - (left->serial < right->serial);
}

static int time_order(const void *a, const void *b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

/* A client lease and a worker CPU are separate resources. Waiting jobs must
 * forecast both gates, without treating a client /LIMIT as global CPU capacity. */
static double lease_ready(double *releases, int count, int inflight)
{
    if (count < inflight) return 0;
    qsort(releases, count, sizeof(*releases), time_order);
    return releases[count - inflight];
}

struct prediction { double start, prepare, factor, finish; };
/* Rebuilt once per dispatch pass. A waiting job reserves a forecast position,
 * not a server slot: later jobs must not all wait for the same fast CPU. */
struct dispatch_batch {
    unsigned count;
    struct { int peer; double cost, prepare; } planned[MAX_CLIENTS];
};
static int bulk_input(const struct peer *peer, const struct dcc_work_hint *hint)
{
    /* Local inputs normally pass a sealed FD. Pump's logical tree includes
     * cached headers, so its size does not predict uploaded byte volume. */
    return !peer->local && (hint->kind <= 3 || hint->kind == DCC_WORK_OFFLOAD);
}

static struct prediction predict(struct broker *broker, const struct client *client,
                                  int peer_index, int work, double cost, double now,
                                  const struct dispatch_batch *batch, int recovering, int capacity, int inflight)
{
    struct peer *peer = &broker->peers[peer_index];
    const struct dcc_capacity *cap = &peer->metrics;
    int waiting = 0;
    for (unsigned i = 0; i < batch->count; ++i) waiting += batch->planned[i].peer == peer_index;
    struct prediction result = {.factor = execution_factor(peer, work + waiting, recovering, capacity)};
    int bulk = bulk_input(peer, &client->hint);
    double prepare = peer->prepare_cost[client->hint.kind - 1];
    if (!prepare) prepare = bulk ? 1.0 / (64 * 1048576) : peer->local ? .001 : .005;
    result.prepare = bulk ? client->hint.bytes * prepare + .005 : prepare;
    int lanes = cap->cpus < cap->jobs ? cap->cpus : cap->jobs;
    if (capacity >= 0 && capacity < lanes) lanes = capacity;
    if (lanes < 1) lanes = 1;
    if (lanes > MAX_CLIENTS) lanes = MAX_CLIENTS;
    const struct client *assigned[MAX_CLIENTS];
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        const struct client *c = &broker->clients[i];
        if (c->fd >= 0 && c->peer == peer_index && c->cost > 0 && c->factor > 0)
            assigned[count++] = c;
    }
    qsort(assigned, count, sizeof(*assigned), assigned_order);
    /* Other brokers are represented by the projected global occupancy. Their
     * work is never added again to this broker's known jobs. */
    int unknown = work > count ? work - count : 0;
    int rounds = unknown / lanes;
    double average = (peer->mean_cost > 0 ? peer->mean_cost : 1) * result.factor;
    double free_at[MAX_CLIENTS];
    double remaining[MAX_CLIENTS], ready_at[MAX_CLIENTS], releases[MAX_CLIENTS];
    int leases = 0;
    for (int i = 0; i < lanes; ++i)
        free_at[i] = (rounds + (i < unknown % lanes)) * average;
    for (int i = 0; i < count; ++i) {
        const struct client *c = assigned[i];
        double elapsed = now > c->run_at ? now - c->run_at : 0;
        /* Completed work belongs to the rate at which it was scheduled.
         * Apply the new contention estimate only to the remaining work;
         * repricing the past can falsely turn a recovering job into an
         * overrun, or invent unfinished work when contention increases. */
        double left = (c->cost - elapsed / c->factor) * result.factor;
        /* A missed forecast no longer establishes when this lane will free.
         * Use the observed service age as the conservative residual estimate;
         * it grows while the job overruns instead of promising an imminent
         * finish forever and starving an idle worker. */
        if (left <= 0) left = elapsed;
        remaining[i] = left;
        ready_at[i] = c->ready_at > now ? c->ready_at - now : 0;
        releases[leases++] = ready_at[i] + left;
    }
    for (int i = 0; i < count; ++i) {
        /* Statistics identify execution counts, not individual returning
         * leases. Retain the longest possible residuals up to that count;
         * never invent extra CPU work from results still being transferred. */
        int rank = 0;
        if (count > work)
            for (int j = 0; j < count && rank < work; ++j)
                rank += remaining[j] > remaining[i] ||
                        (remaining[j] == remaining[i] && j < i);
        if (rank >= work) continue;
        int first = 0;
        for (int j = 1; j < lanes; ++j) if (free_at[j] < free_at[first]) first = j;
        if (free_at[first] < ready_at[i]) free_at[first] = ready_at[i];
        free_at[first] += remaining[i];
        releases[i] = free_at[first];
    }
    while (leases < peer->busy && leases < MAX_CLIENTS) releases[leases++] = average;
    for (unsigned i = 0; i < batch->count; ++i) {
        if (batch->planned[i].peer != peer_index) continue;
        int first = 0;
        for (int j = 1; j < lanes; ++j) if (free_at[j] < free_at[first]) first = j;
        double ready = lease_ready(releases, leases, inflight) + batch->planned[i].prepare;
        if (free_at[first] < ready) free_at[first] = ready;
        free_at[first] += batch->planned[i].cost * result.factor;
        releases[leases++] = free_at[first];
    }
    result.start = free_at[0];
    for (int i = 1; i < lanes; ++i) if (free_at[i] < result.start) result.start = free_at[i];
    double ready = lease_ready(releases, leases, inflight) + result.prepare;
    if (result.start < ready) result.start = ready;
    result.finish = result.start + cost * result.factor;
    return result;
}

static void observe(struct broker *broker, struct client *client, const struct feedback *timing, double now)
{
    if (!timing->execute_ns || client->factor <= 0 ||
        timing->execute_ns > 86400ULL * 1000000000 ||
        timing->prepare_ns > 86400ULL * 1000000000 ||
        timing->queue_ns > 86400ULL * 1000000000) return;
    struct peer *peer = &broker->peers[client->peer];
    double cost = timing->execute_ns / 1e9 / client->factor;
    /* Both direct compilation and remote jobs train the input-cost model. */
    dcc_work_observe(&broker->model, &client->hint, cost);
    peer->mean_cost = peer->mean_cost > 0 ? .875 * peer->mean_cost + .125 * cost : cost;
    int bulk = bulk_input(peer, &client->hint);
    /* Tiny transfers cannot measure bandwidth reliably. Include fast samples
     * as well as slow ones, so a transient delay cannot poison the estimate. */
    if (timing->prepare_ns && (!bulk || client->hint.bytes >= DCC_TCP_CHUNK)) {
        double sample = timing->prepare_ns / 1e9;
        if (bulk) sample /= client->hint.bytes;
        double *previous = &peer->prepare_cost[client->hint.kind - 1];
        double prior = *previous > 0 ? *previous : bulk ? 1.0 / (64 * 1048576) : peer->local ? .001 : .005;
        *previous = .875 * prior + .125 * sample;
    }
    rs_trace("job cost: key=%llx kind=%u bytes=%llu units=%u predicted=%.6f factor=%.6f "
             "execute=%.6f prepare=%.6f queue=%.6f normalized=%.6f host=%s now=%.6f client=%td request=%.6f",
             (unsigned long long)client->hint.key, client->hint.kind,
             (unsigned long long)client->hint.bytes, client->hint.work_units,
             client->cost, client->factor, timing->execute_ns / 1e9,
             timing->prepare_ns / 1e9, timing->queue_ns / 1e9, cost, peer->host->hostname,
             now, client - broker->clients, client->waiting_since);
}

static unsigned long long memory_estimate(const struct dcc_capacity *cap,
                                         const struct dcc_work_hint *hint)
{
    if (hint->kind < 1 || hint->kind > DCC_WORK_CLASSES) return cap->job_memory;
    unsigned kind = hint->kind == DCC_WORK_OFFLOAD_LINK ? 4 : hint->kind == DCC_WORK_THINLTO ? 3 : hint->kind == DCC_WORK_RUST ? 2 :
                    hint->kind >= 4 && hint->kind <= 6 ? 1 : 0;
    return cap->memory_estimates[kind][dcc_resource_bucket(hint->bytes)];
}

static int cpp_limit(const struct broker *broker)
{
    if (broker->cpp_limit) return broker->cpp_limit;
    int limit = dcc_resource_cpus();
    for (int i = 0; i < broker->count; ++i) {
        const struct peer *p = &broker->peers[i];
        if (!p->local) continue;
        const struct dcc_capacity *cap = &p->metrics;
        int slots = cap->cpus > 0 ? cap->cpus : limit;
        /* Worker memory admission belongs to the worker's cgroup. The client
         * checks its own memory budget before starting this frontend. */
        int busy = p->busy;
        if (cap->cpus > 0 && cap->busy >= 0 && cap->busy + p->direct > busy)
            busy = cap->busy + p->direct;
        if (slots - busy < limit) limit = slots - busy;
    }
    return limit > 0 ? limit : 0;
}

static int dispatch(struct broker *broker, struct client *client, double now, struct dispatch_batch *batch)
{
    struct reply reply = {0};
    if (client->command == LOCAL_CPP || client->command == LOCAL_ANALYSIS) {
        int limit = client->command == LOCAL_ANALYSIS ? dcc_resource_cpus() : cpp_limit(broker);
        if (broker->cpp_busy < limit) {
            /* Memory calibration belongs to the analyzer pool. IR producers
             * still consume CPU here, but are not resident analyzer workers. */
            reply.slots = (client->command == LOCAL_ANALYSIS ?
                           broker->analysis_busy : broker->cpp_busy) + 1;
            if (send_reply(client->fd, &reply, -1)) {
                release_client(broker, client);
            } else {
                broker->cpp_busy++;
                broker->analysis_busy += client->command == LOCAL_ANALYSIS;
                client->cpp_reserved = 1;
                client->state = ACTIVE;
            }
        }
        return 0;
    }
    int i, slots, available = 0, ready_hosts = 0;
    double work_rate = 0, min_rate = 0;
    struct selection {
        int peer, probe, wait, idle;
        double score, fill;
        struct prediction forecast;
    } best = {.peer = -1}, ready = {.peer = -1};
    int direct = client->command == TRY_LOCAL;
    double cost = direct ? 0 : dcc_work_predict(&broker->model, &client->hint);
    double ready_finish = 0;
    int link = client->command == ACQUIRE_LINK;
    int link_backend = client->hint.kind == DCC_WORK_THINLTO ||
                       client->hint.kind == DCC_WORK_OFFLOAD_LINK;
    int local_cpp = !link && !direct && client->command != ACQUIRE_CPP && client->command != ACQUIRE_READY;
    int recent[MAX_HOSTS] = {0}, recent_links[MAX_HOSTS] = {0};
    unsigned long long headroom[MAX_HOSTS], pending[MAX_HOSTS] = {0}, new_memory[MAX_HOSTS] = {0};
    for (i = 0; i < MAX_CLIENTS; ++i) {
        const struct client *c = &broker->clients[i];
        if (c->fd < 0 || c->peer < 0) continue;
        unsigned long long bytes = memory_estimate(&broker->peers[c->peer].metrics, &c->hint);
        pending[c->peer] = bytes > UINT64_MAX - pending[c->peer] ? UINT64_MAX : pending[c->peer] + bytes;
        if (!c->direct && c->serial > broker->peers[c->peer].observed_serial) {
            recent[c->peer]++;
            recent_links[c->peer] += c->command == ACQUIRE_LINK;
            new_memory[c->peer] = bytes > UINT64_MAX - new_memory[c->peer] ? UINT64_MAX : new_memory[c->peer] + bytes;
        }
    }
    for (i = 0; i < broker->count; ++i) {
        const struct peer *p = &broker->peers[i];
        const struct dcc_capacity *cap = &p->metrics;
        /* A refreshed idle snapshot must not erase leases whose upload has
         * not arrived yet. Running jobs already have server reservations;
         * returning jobs no longer need a compiler working set. */
        unsigned long long unseen = 0;
        if (cap->busy >= 0 && cap->returning >= 0 && p->busy > cap->busy + cap->returning &&
            pending[i] > cap->memory_reserved)
            unseen = pending[i] - cap->memory_reserved;
        if (new_memory[i] > unseen) unseen = new_memory[i];
        headroom[i] = cap->memory_headroom > unseen ? cap->memory_headroom - unseen : 0;
    }
    for (i = 0; i < broker->count; i++) {
        struct peer *p = &broker->peers[i];
        struct dcc_capacity observed = p->metrics;
        if (observed.busy >= 0) observed.busy += p->direct;
        const struct dcc_capacity *cap = p->valid_until > now ? &observed : NULL;
        struct dcc_capacity projected;
        if (cap && p->host->tcp_jobs && client->hint.kind >= 1 && client->hint.kind <= DCC_WORK_CLASSES) {
            projected = *cap;
            const int *capacities = client->hint.kind == DCC_WORK_OFFLOAD_LINK ? cap->offload_capacity :
                                    client->hint.kind == DCC_WORK_THINLTO ? cap->thinlto_capacity :
                                    client->hint.kind == DCC_WORK_RUST ? cap->rust_capacity :
                                    client->hint.kind >= 4 && client->hint.kind <= 6 ?
                                    cap->pump_capacity : cap->compile_capacity;
            int limit = capacities[dcc_resource_bucket(client->hint.bytes)];
            /* The snapshot's class capacity assumes no subsequent jobs have
             * reserved memory. Charge their own measured classes: one large
             * lease can consume several small-job memory slots. CPU counts
             * alone otherwise admit a burst that stalls after upload. */
            if (headroom[i] < cap->memory_headroom && cap->busy >= 0 && limit >= 0) {
                int occupied = cap->busy + recent[i];
                int leased = p->busy - (cap->returning > 0 ? cap->returning : 0);
                if (leased > occupied) occupied = leased;
                unsigned long long estimate = memory_estimate(cap, &client->hint);
                unsigned long long extra = headroom[i] / estimate;
                if (limit > occupied && extra < (unsigned)(limit - occupied))
                    limit = occupied + (int)extra;
            }
            if (limit >= 0) projected.capacity = limit;
            cap = &projected;
        }
        slots = dcc_capacity_slots(p->host, cap);
        if (direct && p->local && !broker->cpp_limit &&
            broker->cpp_busy >= cpp_limit(broker)) continue;
        if (!p->address_len || p->retry_at > now || slots <= 0)
            continue;
        available++;
        if (client->command == JOBS) {
            int jobs = dcc_capacity_parallelism(p->host, cap);
            reply.slots += jobs;
            if (jobs > 0) {
                struct dcc_capacity full;
                if (cap) {
                    full = *cap;
                    /* Grain spans the whole link. Transient runnable work
                     * (including its planner) must not reshuffle partitions
                     * on every invocation. Placement still uses live load. */
                    full.running = full.busy = jobs - 1;
                    full.capacity = jobs;
                    full.queued = 0;
                }
                double factor = dcc_capacity_score(p->host, cap ? &full : NULL, jobs - 1);
                double rate = 1 / factor;
                work_rate += jobs * rate;
                if (!min_rate || rate < min_rate) min_rate = rate;
            }
            continue;
        }
        if (p->valid_until < 0) continue; /* Admission was refused: refresh first. */
        int staged = p->host->tcp_jobs;
        int predicted = staged && cap && cost > 0 && !link;
        int blocked = 0;
        if (cap && (cap->capacity == 0 || cap->memory_pressure >= 10)) continue;
        if (cap && cap->available == 0 && !staged) continue;
        if (p->full_until > now || (link && p->link_until > now)) continue;
        int inflight = slots + (staged ? (slots < DCC_TCP_PIPELINE ? slots : DCC_TCP_PIPELINE) : 0);
        int work = p->busy;
        if (staged && cap && cap->busy >= 0 && cap->queued >= 0 && cap->preparing >= 0) {
            int limit = cap->cpus < cap->jobs ? cap->cpus : cap->jobs;
            if (cap->capacity >= 0 && cap->capacity < limit) limit = cap->capacity;
            int pipeline = limit < DCC_TCP_PIPELINE ? limit : DCC_TCP_PIPELINE;
            /* Project the global snapshot, not local leases plus the same
             * server queue. Requests dispatched during/after the probe are
             * conservatively charged until the next successful snapshot.
             * Returning jobs in that snapshot consume no compiler capacity. */
            /* A RETURN proves execution has ended without another IPC or
             * network round trip. First charge completions against jobs that
             * were already returning in the snapshot, never against CPU work. */
            int finished = p->completed - (cap->returning > 0 ? cap->returning : 0);
            if (finished < 0) finished = 0;
            int cpu = cap->busy + cap->queued;
            int preparing = cap->preparing;
            if (finished > cpu) {
                preparing -= finished - cpu;
                if (preparing < 0) preparing = 0;
            }
            cpu = finished < cpu ? cpu - finished : 0;
            work = cpu + recent[i];
            /* A lease can precede the worker's first input byte. A fresh idle
             * snapshot must not erase that work and repeatedly refill the
             * same worker while the client prepares its request. */
            int returning = cap->returning - p->completed;
            if (returning < 0) returning = 0;
            int leased = p->busy - returning;
            if (link && (cap->links > 0 || recent_links[i])) continue;
            blocked = work + preparing >= limit + pipeline || leased >= limit + pipeline;
            if (blocked && !predicted) continue;
            /* Input preparation overlaps execution; only excess preparation
             * beyond the bounded pipeline adds CPU queueing delay. */
            if (preparing > pipeline) work += preparing - pipeline;
            if (leased > work) work = leased;
            if (returning > 0)
                inflight += returning < slots ? returning : slots;
            projected = *cap;
            projected.busy = cpu;
            projected.queued = 0;
            cap = &projected;
        }
        if (p->busy < inflight || predicted) {
            /* A recovery trial uses the host's calibrated speed, not a zero
             * completion time. An uncompetitive host need not delay this job
             * just to refresh stale frequency telemetry. */
            int probe = !link && cap && cap->cpu_probe && work == 0 && now >= p->probe_after;
            double score = probe ? execution_factor(p, work, 1, cap->capacity) :
                                   dcc_capacity_score(p->host, cap, work);
            struct prediction forecast = {0};
            if ((client->hint.key || direct) && cap)
                forecast.factor = execution_factor(p, work, probe, cap->capacity);
            int wait = 0;
            if (predicted) {
                forecast = predict(broker, client, i, work, cost, now, batch, probe, cap->capacity, inflight);
                score = forecast.finish;
                wait = forecast.start > forecast.prepare + .02;
            } else if (cost > 0) score *= cost;
            if (staged && cap && client->hint.key && !link) {
                /* Keep a ready job movable until an execution lane is actually
                 * available. Near-zero predicted residuals are not completion
                 * evidence: uploading then would strand it behind an overrun
                 * while a different worker becomes free. Input preparation
                 * and returning results still overlap running compilers.
                 * Cold work has no duration forecast, but still consumes a
                 * CPU lane: the input pipeline is not an execution queue. */
                int lanes = cap->cpus < cap->jobs ? cap->cpus : cap->jobs;
                if (cap->capacity >= 0 && cap->capacity < lanes) lanes = cap->capacity;
                wait |= blocked || work >= lanes || p->busy >= inflight;
            }
            /* Charge the throughput displaced by another compiler on every
             * worker, including existing compiles on a remote SMT core.
             * Minimizing this job's latency alone packs the fastest worker's
             * SMT lanes while independent physical cores remain unused. */
            if (staged && cap && !link && !link_backend && !direct && (predicted ||
                (!blocked && work < dcc_capacity_jobs(p->host, cap)))) {
                const struct dcc_capacity *observed = &p->metrics;
                double external = (observed->running >= 0 ? observed->running : observed->load)
                                  - (observed->busy > 0 ? observed->busy : 0) - p->direct;
                int placement = work;
                if (wait) {
                    int lanes = cap->cpus < cap->jobs ? cap->cpus : cap->jobs;
                    if (cap->capacity >= 0 && cap->capacity < lanes) lanes = cap->capacity;
                    /* A queued choice still displaces CPU work when its next
                     * lane opens. Apply the same placement policy then as now;
                     * otherwise we wait for a host that we reject on release. */
                    if (placement >= lanes) placement = lanes > 0 ? lanes - 1 : 0;
                }
                double demand = placement + (external > 0 ? external : 0);
                /* Include the CPU work displaced by this compile, notably
                 * client preprocessing on a machine that also runs a worker. */
                double before = demand ? demand / execution_factor(p, placement - 1, probe, cap->capacity) : 0;
                double gain = (demand + 1) / execution_factor(p, placement, probe, cap->capacity) - before;
                /* At saturation there is no positive marginal throughput.
                 * Charge aggregate service demand instead of dropping the
                 * penalty or dividing by numerical noise. This finite bound
                 * also lets the least costly host progress when all are busy. */
                double penalty = (demand + 1) * execution_factor(p, placement, probe, cap->capacity);
                if (gain > 0 && gain * penalty > 1) penalty = 1 / gain;
                double throughput = forecast.start + (cost > 0 ? cost : 1) * penalty;
                if (!predicted || throughput > score) score = throughput;
            }
            double fill = (work + 1.0) / slots;
            if (link && cap && cap->links > 0) score *= 1 + cap->links;
            rs_trace("candidate host=%s work=%d capacity=%d cost=%.3f factor=%.3f prepare=%.3f start=%.3f score=%.3f wait=%d "
                     "finish=%.6f blocked=%d busy=%d preparing=%d queued=%d leases=%d planned=%u "
                     "now=%.6f client=%td request=%.6f key=%llx",
                     p->host->hostname, work, cap ? cap->capacity : -1, cost,
                     forecast.factor, forecast.prepare, forecast.start, score, wait,
                     forecast.finish, blocked, cap ? cap->busy : -1, cap ? cap->preparing : -1,
                     cap ? cap->queued : -1, p->busy, batch->count,
                     now, client - broker->clients, client->waiting_since,
                     (unsigned long long)client->hint.key);
            /* Without any input description there is no completion estimate
             * with which to justify waiting instead of using an idle CPU. */
            int idle = !cost && staged && cap && !blocked && work < dcc_capacity_jobs(p->host, cap);
            struct selection candidate = {i, probe, wait, idle, score, fill, forecast};
            if (best.peer < 0 || idle > best.idle || (idle == best.idle &&
                (score < best.score || (score == best.score && fill < best.fill)))) best = candidate;
            if (predicted && !wait) {
                ready_hosts++;
                if (ready.peer < 0 || forecast.finish < ready_finish)
                    ready_finish = forecast.finish;
                if (ready.peer < 0 || score < ready.score ||
                    (score == ready.score && fill < ready.fill)) ready = candidate;
            }
        }
    }
    /* Throughput displacement ranks immediate placements, but is not elapsed
     * time. Waiting must actually finish sooner than an available placement;
     * comparing a queued latency with an immediate throughput penalty can
     * otherwise strand idle CPUs for an entire running job. Once waiting is
     * rejected, preserve throughput ranking among the available workers;
     * choosing their lowest latency again would discard the contention cost. */
    /* Link backends share a completion barrier. Keep available lanes working:
     * waiting for a faster worker concentrates later jobs there and leaves
     * capacity idle even with learned costs. Rank the available workers by
     * completion time, and retain queued work until any lane actually opens. */
    if (best.wait && ready.peer >= 0 &&
        (link_backend || best.forecast.finish >= ready_finish))
        best = ready;
    if (direct && !(best.peer >= 0 && best.idle && !best.wait &&
                    broker->peers[best.peer].local && broker->peers[best.peer].host->tcp_jobs)) {
        reply.slots = 0;
        send_reply(client->fd, &reply, -1);
        release_client(broker, client);
    } else if (client->command == JOBS || !available) {
        if (client->command == JOBS)
            snprintf(reply.host, sizeof(reply.host),
                     "{\"jobs\":%d,\"work_rate\":%.9g,\"min_rate\":%.9g,\"direct_completed\":%llu,"
                     "\"direct_prepared_completed\":%llu}",
                     reply.slots, work_rate, min_rate, (unsigned long long)broker->direct_completed,
                     (unsigned long long)broker->direct_prepared_completed);
        if (!available && client->command != JOBS) {
            for (i = 0; i < broker->count; i++) {
                struct peer *p = &broker->peers[i];
                rs_log_warning("unavailable worker %s: address=%u up=%d slots=%d cpus=%d jobs=%d retry=%.3fs",
                               p->host->hostname, (unsigned)p->address_len, p->host->is_up,
                               p->host->n_slots, p->metrics.cpus, p->metrics.jobs,
                               p->retry_at > now ? p->retry_at - now : 0);
            }
        }
        reply.status = available ? 0 : EXIT_CONNECT_FAILED;
        send_reply(client->fd, &reply, -1);
        release_client(broker, client);
    } else if (best.peer >= 0 && best.wait) {
        rs_trace("waiting for host=%s cost=%.3f score=%.3f finish=%.6f ready=%d ready_finish=%.6f "
                 "now=%.6f client=%td request=%.6f key=%llx",
                 broker->peers[best.peer].host->hostname, cost, best.score, best.forecast.finish,
                 ready_hosts, ready_finish, now, client - broker->clients, client->waiting_since,
                 (unsigned long long)client->hint.key);
        unsigned index = batch->count++;
        batch->planned[index].peer = best.peer;
        batch->planned[index].cost = cost;
        batch->planned[index].prepare = best.forecast.prepare;
        double delay = best.forecast.start - best.forecast.prepare - .02;
        if (delay < .05) delay = .05;
        if (delay > 1) delay = 1;
        client->deadline = now + delay;
    } else if (best.peer >= 0 && (!local_cpp || broker->cpp_busy < cpp_limit(broker))) {
        struct peer *p = &broker->peers[best.peer];
        rs_trace("assigning host=%s cost=%.3f score=%.3f broker_wait=%.6f key=%llx units=%u "
                 "now=%.6f client=%td request=%.6f",
                 p->host->hostname, cost, best.score, now - client->waiting_since,
                 (unsigned long long)client->hint.key, client->hint.work_units,
                 now, client - broker->clients, client->waiting_since);
        client->direct = direct || (client->can_compile_locally && p->local && p->host->tcp_jobs &&
            p->valid_until > now && p->metrics.busy >= 0 &&
            (client->command == ACQUIRE_CPP || client->command == ACQUIRE_READY));
        if (!client->direct) {
            client->admission = !p->host->tcp_jobs && client->command != ACQUIRE &&
                                p->admission_supported && p->pool_supported >= 0;
            client->net = client->command != ACQUIRE ? pool_take(broker, best.peer, now) : -1;
            client->pooled = client->net >= 0;
            client->reused = client->pooled;
            if (p->host->tcp_jobs) client->pooled = 1;
            if (client->net < 0) client->net = connect_peer(p, p->host->port);
            if (client->net < 0) {
                rs_log_warning("cannot connect to %s: %s", p->host->hostname, strerror(errno));
                p->retry_at = now + 5;
                return 1;
            }
        }
        client->peer = best.peer;
        client->cost = direct ? dcc_work_predict(&broker->model, &client->hint) : cost;
        if (direct && !client->cost) client->cost = p->mean_cost;
        client->factor = best.forecast.factor;
        client->ready_at = now + (client->direct ? 0 : best.forecast.prepare);
        client->run_at = now + (client->direct ? 0 : best.forecast.start);
        if (best.probe) p->probe_after = now + 10;
        client->serial = ++p->serial;
        client->state = CONNECTING;
        client->deadline = now + 3;
        p->busy++;
        p->direct += client->direct;
        if (client->direct)
            rs_trace("direct local compile host=%s prepared=%d client=%td", p->host->hostname,
                     client->command != TRY_LOCAL, client - broker->clients);
        broker->cpp_busy += local_cpp;
        client->cpp_reserved = local_cpp;
        client->used = 0;
        if (client->direct || client->reused) {
            if (client->admission) client->state = RESERVE_SEND;
            else deliver_connection(broker, client);
        }
    }
    return 0;
}

struct waiting_job { struct client *client; double cost, age; };

static int waiting_order(const void *a, const void *b)
{
    const struct waiting_job *left = a, *right = b;
    int lc = left->client->command == LOCAL_CPP || left->client->command == LOCAL_ANALYSIS || left->client->command == JOBS;
    int rc = right->client->command == LOCAL_CPP || right->client->command == LOCAL_ANALYSIS || right->client->command == JOBS;
    if (lc != rc) return rc - lc;
    /* Large jobs have the greatest execution penalty on a slower worker.
     * Aging gives sustained arrivals a bounded opportunity to overtake. */
    int la = left->age >= 1, ra = right->age >= 1;
    if (la != ra) return ra - la;
    if (la) return (right->age > left->age) - (right->age < left->age);
    if (left->cost != right->cost)
        return (right->cost > left->cost) - (right->cost < left->cost);
    return (right->age > left->age) - (right->age < left->age);
}

static void connection_event(struct broker *broker, struct client *client,
                             double now, int timed_out)
{
    struct peer *peer = &broker->peers[client->peer];
    if (timed_out || connected(client->net)) {
        rs_log_warning("connection to %s failed: %s", peer->host->hostname,
                       timed_out ? "connection timeout" : strerror(errno));
        peer->retry_at = now + 5;
        release_assignment(broker, client);
        client->state = WAITING;
        return;
    }
    if (!peer->host->tcp_jobs && client->command != ACQUIRE && peer->pool_supported >= 0) {
        client->state = client->admission ? RESERVE_SEND : HELLO_SEND;
        client->used = 0;
        client->deadline = now + 3;
    } else deliver_connection(broker, client);
}

static void hello_event(struct broker *broker, struct client *client, int timed_out)
{
    static const char hello[] = "DIST00000005";
    ssize_t n = -1;
    if (!timed_out) {
        if (client->state == HELLO_SEND)
            n = send(client->net, hello + client->used, 12 - client->used, MSG_NOSIGNAL);
        else n = recv(client->net, client->hello + client->used, 12 - client->used, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n > 0) {
            client->used += n;
            if (client->used < 12) return;
            if (client->state == HELLO_SEND) {
                client->state = HELLO_READ;
                client->used = 0;
                return;
            }
            if (!memcmp(client->hello, "POOL00000001", 12)) {
                broker->peers[client->peer].pool_supported = 1;
                client->pooled = 1;
                deliver_connection(broker, client);
                return;
            }
        }
    }
    /* No job was sent. Reconnect using the original protocol for old peers. */
    broker->peers[client->peer].pool_supported = -1;
    release_assignment(broker, client);
    client->state = WAITING;
}

/* Admission is one short exchange on the reusable connection. A refusal is
 * never a compile failure: try another worker before giving the FD to a client. */
static void reserve_event(struct broker *broker, struct client *client, double now, int timed_out)
{
    struct peer *peer = &broker->peers[client->peer];
    const char *request = client->command == ACQUIRE_LINK ? "SLOT00000002" : "SLOT00000001";
    ssize_t n = -1;
    if (!timed_out) {
        if (client->state == RESERVE_SEND)
            n = send(client->net, request + client->used, 12 - client->used, MSG_NOSIGNAL);
        else n = recv(client->net, client->hello + client->used, 12 - client->used, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n > 0) {
            client->used += n;
            if (client->used < 12) return;
            if (client->state == RESERVE_SEND) {
                client->state = RESERVE_READ;
                client->used = 0;
                return;
            }
            if (!memcmp(client->hello, "GRNT00000001", 12)) {
                client->pooled = 1;
                peer->pool_supported = 1;
                deliver_connection(broker, client);
                return;
            }
            if (!memcmp(client->hello, "BUSY00000000", 12)) {
                if (client->command == ACQUIRE_LINK) peer->link_until = now + .05;
                else peer->full_until = now + .05;
                pool_store(broker, client, client->net, now);
                client->net = -1;
                release_assignment(broker, client);
                client->state = WAITING;
                return;
            }
        }
    }
    /* A restarted/older daemon may no longer support advertised admission. */
    peer->valid_until = 0;
    peer->admission_supported = 0;
    peer->retry_at = now + 1;
    release_assignment(broker, client);
    client->state = WAITING;
}

static volatile sig_atomic_t stopping;
static void stop_broker(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

/* Socket activation lets a sandboxed compiler request connections from a
 * service outside its network namespace. No compiler children are spawned. */
static int listener(const char *path, int *lock_fd, int *inherited)
{
    struct sockaddr_un address, actual;
    const char *pid = getenv("LISTEN_PID"), *fds = getenv("LISTEN_FDS");
    char lock_path[sizeof(address.sun_path) + 6];
    struct stat st;
    int fd, type;
    socklen_t size;
    *lock_fd = -1;
    *inherited = 0;
    if (unix_address(path, &address))
        return -1;
    if (pid && fds && atol(pid) == getpid() && !strcmp(fds, "1")) {
        size = sizeof(type);
        if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET)
            return -1;
        memset(&actual, 0, sizeof(actual));
        size = sizeof(actual);
        if (getsockname(3, (struct sockaddr *)&actual, &size) ||
            actual.sun_family != AF_UNIX || strcmp(actual.sun_path, path))
            return -1;
        *inherited = 1;
        fd = 3;
    } else {
        snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
        *lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (*lock_fd < 0 || flock(*lock_fd, LOCK_EX | LOCK_NB))
            return -1;
        if (!lstat(path, &st)) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || unlink(path))
                return -1;
        } else if (errno != ENOENT) {
            return -1;
        }
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
            chmod(path, 0600) || listen(fd, MAX_CLIENTS)) {
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int initialize(struct broker *broker, struct dcc_hostdef *hosts)
{
    struct dcc_hostdef *host;
    int i;
    broker->cpp_limit = dcc_hostdef_local_cpp->slots_specified ? dcc_hostdef_local_cpp->n_slots : 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        broker->clients[i].fd = broker->clients[i].net = broker->clients[i].peer = -1;
    for (host = hosts; host; host = host->next) {
        struct addrinfo hints = {0}, *addresses = NULL;
        struct peer *peer;
        for (i = 0; i < broker->count; i++)
            if (host->port == broker->peers[i].host->port &&
                !strcmp(host->hostname, broker->peers[i].host->hostname))
                break;
        if (i < broker->count)
            continue;
        if (broker->count == MAX_HOSTS || host->mode != DCC_MODE_TCP ||
            strlen(host->hostdef_string) >= sizeof(((struct reply *)0)->host))
            return -1;
        peer = &broker->peers[broker->count++];
        peer->host = host;
        peer->stats_fd = -1;
#ifdef HAVE_GSSAPI
        if (host->authenticate) peer->pool_supported = -1;
#endif
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (!getaddrinfo(host->hostname, NULL, &hints, &addresses)) {
            if (addresses->ai_addrlen <= sizeof(peer->address)) {
                memcpy(&peer->address, addresses->ai_addr, addresses->ai_addrlen);
                peer->address_len = addresses->ai_addrlen;
                struct ifaddrs *interfaces;
                if (!getifaddrs(&interfaces)) {
                    for (struct ifaddrs *a = interfaces; a; a = a->ifa_next) {
                        if (!a->ifa_addr || a->ifa_addr->sa_family != peer->address.ss_family) continue;
                        if (peer->address.ss_family == AF_INET)
                            peer->local |= ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr ==
                                           ((struct sockaddr_in *)&peer->address)->sin_addr.s_addr;
                        else if (peer->address.ss_family == AF_INET6)
                            peer->local |= !memcmp(&((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr,
                                &((struct sockaddr_in6 *)&peer->address)->sin6_addr, sizeof(struct in6_addr));
                    }
                    freeifaddrs(interfaces);
                }
            }
            freeaddrinfo(addresses);
        }
    }
    return broker->count ? 0 : -1;
}

static void accept_client(struct broker *broker, int listener_fd)
{
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    int i, fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size)) {
        if (credentials.uid != geteuid() && credentials.uid != 0) {
            struct reply reply = { .status = EXIT_ACCESS_DENIED };
            send_reply(fd, &reply, -1);
            close(fd);
            return;
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *client = &broker->clients[i];
            if (client->fd < 0) {
                *client = (struct client){ .fd = fd, .net = -1, .peer = -1,
                    .state = REQUEST, .deadline = now_seconds() + 3 };
                return;
            }
        }
    }
    close(fd);
}

int dcc_broker_serve(const char *path)
{
    if (dcc_getenv_bool("DISTCC_VERBOSE", 0)) {
        rs_remove_all_loggers();
        rs_trace_set_level(RS_LOG_DEBUG);
        rs_add_logger(rs_logger_file, RS_LOG_DEBUG, NULL, STDERR_FILENO);
    }
    struct broker *broker = calloc(1, sizeof(*broker));
    struct dcc_hostdef *hosts = NULL;
    int count, fd = -1, lock_fd = -1, inherited = 0, ret = EXIT_BAD_ARGUMENTS;
    sigset_t blocked, previous;
    int mask_set = 0;
    const char *setting = getenv("DISTCC_BROKER_IDLE");
    char *end;
    long idle = setting ? strtol(setting, &end, 10) : 60;
    const char *backoff_setting = getenv("DISTCC_BACKOFF_PERIOD");
    int backoff = backoff_setting ? atoi(backoff_setting) : 60;
    double idle_deadline = 0;
    if (!broker)
        return EXIT_OUT_OF_MEMORY;
    for (count = 0; count < MAX_CLIENTS; ++count) broker->pool[count].fd = -1;
    if ((setting && (*end || !*setting)) || idle < 1 || idle > 86400 ||
        dcc_get_hostlist(&hosts, &count) || initialize(broker, hosts))
        goto done;
    fd = listener(path, &lock_fd, &inherited);
    if (fd < 0) {
        rs_log_error("cannot listen on broker socket: %s", strerror(errno));
        goto done;
    }
    signal(SIGTERM, stop_broker);
    signal(SIGINT, stop_broker);
    signal(SIGPIPE, SIG_IGN);
    /* Atomically unblock termination only while waiting. A signal between
     * the loop condition and poll must not leave shutdown asleep until idle. */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) goto done;
    mask_set = 1;
    ret = 0;
    while (!stopping) {
        struct dispatch_batch batch = {0};
        struct pollfd polls[1 + MAX_HOSTS + 2 * MAX_CLIENTS];
        int stats_index[MAX_HOSTS], control_index[MAX_CLIENTS], net_index[MAX_CLIENTS];
        int i, n = 1, clients = 0, probes = 0, timeout;
        struct timespec wait_time;
        double now = now_seconds();
        double wake_at = now + 60;
        polls[0] = (struct pollfd){ fd, POLLIN, 0 };
        for (i = 0; i < MAX_CLIENTS; ++i) {
            struct idle_connection *entry = &broker->pool[i];
            if (entry->fd < 0) continue;
            if (now >= entry->expires) { close(entry->fd); entry->fd = -1; }
            else if (entry->expires < wake_at) wake_at = entry->expires;
        }
        for (i = 0; i < MAX_CLIENTS; i++)
            clients += broker->clients[i].fd >= 0;
        /* An idle period starts after the last client leaves, including
         * leases held through long compiles and requests that time out. */
        if (clients) {
            idle_deadline = 0;
        } else {
            if (!idle_deadline) idle_deadline = now + idle;
            if (now >= idle_deadline) break;
            if (idle_deadline < wake_at) wake_at = idle_deadline;
        }
        if (clients && now >= broker->refresh_at) {
            int fresh = 0;
            for (i = 0; i < broker->count; i++)
                fresh |= broker->peers[i].valid_until > now;
            if (!fresh)
                broker->first_probe_until = now + .25;
            broker->refresh_at = now + 1;
            for (i = 0; i < broker->count; i++) {
                struct peer *p = &broker->peers[i];
                if (p->host->stats_port && p->stats_fd < 0) {
                    p->probe_serial = p->serial;
                    p->stats_fd = connect_peer(p, p->host->stats_port);
                    p->deadline = now + .25;
                    p->used = p->stats_reading = 0;
                }
            }
        }
        if (clients && broker->refresh_at < wake_at)
            wake_at = broker->refresh_at;
        for (i = 0; i < broker->count; i++) {
            struct peer *p = &broker->peers[i];
            if (clients && p->full_until > now && p->full_until < wake_at) wake_at = p->full_until;
            if (clients && p->link_until > now && p->link_until < wake_at) wake_at = p->link_until;
            if (p->stats_fd >= 0 && now >= p->deadline)
                finish_probe(p, now, 0);
            stats_index[i] = -1;
            if (p->stats_fd >= 0) {
                if (p->deadline < wake_at)
                    wake_at = p->deadline;
                probes++;
                stats_index[i] = n;
                polls[n++] = (struct pollfd){ p->stats_fd,
                    p->stats_reading ? POLLIN : POLLOUT, 0 };
            }
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = &broker->clients[i];
            control_index[i] = net_index[i] = -1;
            if (c->fd < 0)
                continue;
            if ((c->state == REQUEST || c->state == MEMO_OWNER || c->state == MEMO_WAIT) && now >= c->deadline) {
                release_client(broker, c);
                continue;
            }
            if (c->state == CONNECTING && now >= c->deadline)
                connection_event(broker, c, now, 1);
            if ((c->state == HELLO_SEND || c->state == HELLO_READ) && now >= c->deadline)
                hello_event(broker, c, 1);
            if ((c->state == RESERVE_SEND || c->state == RESERVE_READ) && now >= c->deadline)
                reserve_event(broker, c, now, 1);
        }
        struct waiting_job waiting[MAX_CLIENTS];
        int count_waiting = 0;
        for (i = 0; i < MAX_CLIENTS; ++i) {
            struct client *c = &broker->clients[i];
            if (c->fd >= 0 && c->state == WAITING &&
                (c->command == LOCAL_CPP || c->command == LOCAL_ANALYSIS || !probes || now >= broker->first_probe_until))
                waiting[count_waiting++] = (struct waiting_job){c,
                    dcc_work_predict(&broker->model, &c->hint), now - c->waiting_since};
        }
        qsort(waiting, count_waiting, sizeof(*waiting), waiting_order);
        for (i = 0; i < count_waiting; ++i)
            if (dispatch(broker, waiting[i].client, now, &batch))
                wake_at = now;
        for (i = 0; i < MAX_CLIENTS; ++i) {
            struct client *c = &broker->clients[i];
            if (c->fd < 0)
                continue;
            if (c->state == WAITING && c->deadline > now && c->deadline < wake_at)
                wake_at = c->deadline;
            if ((c->state == REQUEST || c->state == CONNECTING || c->state == HELLO_SEND || c->state == HELLO_READ ||
                 c->state == RESERVE_SEND || c->state == RESERVE_READ ||
                 c->state == MEMO_OWNER || c->state == MEMO_WAIT) && c->deadline < wake_at)
                wake_at = c->deadline;
            control_index[i] = n;
            polls[n++] = (struct pollfd){ c->fd, POLLIN, 0 };
            if (c->state == CONNECTING || c->state == HELLO_SEND || c->state == HELLO_READ ||
                c->state == RESERVE_SEND || c->state == RESERVE_READ) {
                net_index[i] = n;
                polls[n++] = (struct pollfd){ c->net,
                    c->state == HELLO_READ || c->state == RESERVE_READ ? POLLIN : POLLOUT, 0 };
            }
        }
        timeout = wake_at > now ? (int)((wake_at - now) * 1000) + 1 : 0;
        wait_time = (struct timespec){ timeout / 1000, (timeout % 1000) * 1000000L };
        if (ppoll(polls, n, &wait_time, &previous) < 0) {
            if (errno == EINTR)
                continue;
            ret = EXIT_IO_ERROR;
            break;
        }
        now = now_seconds();
        for (i = 0; i < broker->count; i++)
            if (stats_index[i] >= 0 && polls[stats_index[i]].revents)
                probe_event(&broker->peers[i], now);
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = &broker->clients[i];
            if (control_index[i] < 0)
                continue;
            if (polls[control_index[i]].revents) {
                struct { struct request header; char payload[DCC_MEMO_LIMIT]; } packet = {0};
                int returned;
                ssize_t length = dcc_recv_fd(c->fd, &packet, sizeof(packet), &returned);
                struct request request = packet.header;
                int can_compile_locally = request.command & CLIENT_COMPILE;
                request.command &= ~CLIENT_COMPILE;
                if (can_compile_locally && (c->state != REQUEST ||
                    (request.command != ACQUIRE_POOL && request.command != ACQUIRE_CPP &&
                     request.command != ACQUIRE_READY))) {
                    if (returned >= 0) close(returned);
                    release_client(broker, c);
                    continue;
                }
                if (returned < 0 && c->state == ACTIVE && c->direct &&
                    length == sizeof(request) + sizeof(struct feedback) &&
                    request.version == BROKER_VERSION && request.command == RETURN) {
                    struct feedback timing;
                    memcpy(&timing, packet.payload, sizeof(timing));
                    observe(broker, c, &timing, now);
                    if (timing.execute_ns) {
                        broker->direct_completed++;
                        broker->direct_prepared_completed += c->command != TRY_LOCAL;
                    }
                    release_client(broker, c);
                    continue;
                }
                if (returned < 0 && c->state == ACTIVE && c->peer >= 0 && length == sizeof(request) &&
                    request.version == BROKER_VERSION && request.command == FAILED) {
                    if (c->reused) {
                        /* An idle socket can die after the pool liveness
                         * check. Discard the peer's idle pool and try a fresh
                         * connection before quarantining a healthy worker. */
                        int j;
                        for (j = 0; j < MAX_CLIENTS; j++) {
                            struct idle_connection *entry = &broker->pool[j];
                            if (entry->fd >= 0 && entry->peer == c->peer) {
                                close(entry->fd);
                                entry->fd = -1;
                            }
                        }
                    } else {
                        broker->peers[c->peer].retry_at = now + (backoff > 0 ? backoff : 0);
                    }
                    release_client(broker, c);
                    continue;
                }
                if (returned >= 0) {
                    if (length == sizeof(request) && request.version == BROKER_VERSION &&
                        request.command == DEFER && c->state == ACTIVE && c->pooled) {
                        struct peer *p = &broker->peers[c->peer];
                        /* This socket completed no CPU work. Reuse it without
                         * training execution cost or quarantining the worker,
                         * and discard any capacity probe from before refusal. */
                        if (p->stats_fd >= 0) close(p->stats_fd);
                        p->stats_fd = -1;
                        if (p->host->stats_port) p->valid_until = -1;
                        else p->full_until = now + 1;
                        broker->refresh_at = 0;
                        rs_trace("rescheduling unstarted job from host=%s", p->host->hostname);
                        pool_store(broker, c, returned, now);
                    } else if (length == sizeof(request) + sizeof(struct feedback) && request.version == BROKER_VERSION &&
                        request.command == RETURN && c->state == ACTIVE && c->pooled) {
                        struct peer *p = &broker->peers[c->peer];
                        struct feedback timing;
                        memcpy(&timing, packet.payload, sizeof(timing));
                        observe(broker, c, &timing, now);
                        if (c->serial <= p->observed_serial) p->completed++;
                        pool_store(broker, c, returned, now);
                    } else close(returned);
                    release_client(broker, c);
                    continue;
                }
                if (length > (ssize_t)sizeof(request) && length <= (ssize_t)sizeof(packet) &&
                    request.version == BROKER_VERSION &&
                    (request.command == MEMO_GET || request.command == MEMO_PUT) &&
                    packet.payload[0] && !packet.payload[length - sizeof(request) - 1] &&
                    strlen(packet.payload) == (size_t)length - sizeof(request) - 1) {
                    memo_request(broker, c, request.command, packet.payload, now);
                    continue;
                }
                if (c->state == ACTIVE && length == sizeof(request) &&
                    request.version == BROKER_VERSION && request.command == CPP_DONE &&
                    (c->cpp_reserved || c->command == ACQUIRE_LINK || c->command == ACQUIRE_CPP ||
                     c->command == ACQUIRE_READY)) {
                    release_cpp(broker, c);
                    continue;
                }
                int compile = request.command == ACQUIRE_POOL || request.command == ACQUIRE_CPP ||
                              request.command == ACQUIRE_READY || request.command == TRY_LOCAL;
                if (c->state != REQUEST || length != (ssize_t)(sizeof(request) +
                        (compile ? sizeof(struct dcc_work_hint) : 0)) ||
                    request.version != BROKER_VERSION ||
                    (request.command != ACQUIRE && request.command != ACQUIRE_POOL &&
                     request.command != ACQUIRE_LINK && request.command != ACQUIRE_CPP &&
                     request.command != ACQUIRE_READY && request.command != LOCAL_CPP && request.command != LOCAL_ANALYSIS &&
                     request.command != JOBS && request.command != TRY_LOCAL)) {
                    release_client(broker, c);
                    continue;
                }
                if (compile) {
                    memcpy(&c->hint, packet.payload, sizeof(c->hint));
                    if (!dcc_work_valid(&c->hint)) { release_client(broker, c); continue; }
                }
                c->command = request.command;
                c->can_compile_locally = can_compile_locally != 0;
                c->state = WAITING;
                c->waiting_since = now;
            }
            if (net_index[i] >= 0 && polls[net_index[i]].revents) {
                if (c->state == CONNECTING) connection_event(broker, c, now, 0);
                else if (c->state == HELLO_SEND || c->state == HELLO_READ) hello_event(broker, c, 0);
                else if (c->state == RESERVE_SEND || c->state == RESERVE_READ) reserve_event(broker, c, now, 0);
            }
        }
        if (polls[0].revents)
            accept_client(broker, fd);
    }
    for (count = 0; count < MAX_CLIENTS; count++)
        if (broker->clients[count].fd >= 0)
            release_client(broker, &broker->clients[count]);
    for (count = 0; count < broker->count; count++)
        if (broker->peers[count].stats_fd >= 0)
            close(broker->peers[count].stats_fd);
done:
    if (mask_set) sigprocmask(SIG_SETMASK, &previous, NULL);
    for (count = 0; count < MAX_CLIENTS; ++count)
        if (broker->pool[count].fd >= 0) close(broker->pool[count].fd);
    for (count = 0; count < MAX_MEMOS; count++) {
        free(broker->memos[count].key);
        free(broker->memos[count].value);
    }
    if (fd >= 0) {
        close(fd);
        if (!inherited)
            unlink(path);
    }
    if (lock_fd >= 0)
        close(lock_fd);
    free_hosts(hosts);
    free(broker);
    return ret;
}

#else
int dcc_broker_local_cpp(int *lease, int *occupied, int analysis)
{
    (void)analysis;
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
int dcc_broker_memo_get(const char *key, char **value)
{
    (void)key;
    *value = NULL;
    return -1;
}
void dcc_broker_memo_put(int lease, const char *value)
{
    (void)lease;
    (void)value;
}
int dcc_broker_serve(const char *path)
{
    (void)path;
    rs_log_error("the local broker requires Linux");
    return EXIT_BAD_ARGUMENTS;
}
int dcc_broker_acquire(struct dcc_hostdef **host, int *lease, int cpp_ready,
                       const struct dcc_work_hint *hint, int *queued_fd)
{
    (void)queued_fd;
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
void dcc_broker_cpp_done(const struct dcc_hostdef *host)
{
    (void)host;
}
#endif
