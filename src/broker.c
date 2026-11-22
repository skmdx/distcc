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
#include "where.h"

#include "broker.h"
#include "broker_protocol.h"

#include <netinet/tcp.h>
#include "exitcode.h"
#include "trace.h"
#include "util.h"
#include "tcp_job.h"
#include "workload.h"
#include "plan_memo.h"

#ifdef __linux__
#include "broker_schedule.h"
#include "broker_memo.h"
#include "epoll_set.h"
struct idle_connection { int fd, peer, next, previous; double expires; uint32_t generation; };
struct session {
    struct memo_session memo;
    struct client *jobs;
    int denied;
    double deadline, preflight;
    unsigned kind;
};
enum { LISTENER, STATS, CONTROL = STATS + MAX_HOSTS, NETWORK = CONTROL + MAX_CLIENTS,
       IDLE_SOCKET = NETWORK + MAX_CLIENTS, WATCH_COUNT = IDLE_SOCKET + MAX_CLIENTS };
struct broker {
    int ep, free_idle, dirty;
    struct dcc_watch watches[WATCH_COUNT];
    struct session sessions[MAX_CLIENTS];
    struct broker_schedule schedule;
    struct dcc_memo_store *memos;
    double refresh_at, first_probe_until;
    struct idle_connection pool[MAX_CLIENTS];
};

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

static socklen_t peer_endpoint(const struct peer *peer, int family, int port,
                               struct sockaddr_storage *address)
{
    if (family == AF_UNIX) {
        if (!peer->local || !peer->host->tcp_jobs || port != peer->host->port ||
            dcc_tcp_local_address(port, (struct sockaddr_un *)address)) return 0;
        return sizeof(struct sockaddr_un);
    }
    if (!peer->address_len || family != peer->address.ss_family) return 0;
    *address = peer->address;
    if (family == AF_INET) ((struct sockaddr_in *)address)->sin_port = htons(port);
    else if (family == AF_INET6) ((struct sockaddr_in6 *)address)->sin6_port = htons(port);
    else return 0;
    return peer->address_len;
}

static int same_address(const struct sockaddr *a, const struct sockaddr *b, int endpoint)
{
    if (!a || a->sa_family != b->sa_family) return 0;
    switch (a->sa_family) {
    case AF_UNIX:
        return !strncmp(((const struct sockaddr_un *)a)->sun_path, ((const struct sockaddr_un *)b)->sun_path,
                        sizeof(((struct sockaddr_un *)a)->sun_path));
    case AF_INET: {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_addr.s_addr == y->sin_addr.s_addr && (!endpoint || x->sin_port == y->sin_port);
    }
    case AF_INET6: {
        const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
        return !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) &&
               (!endpoint || (x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id));
    }
    default: return 0;
    }
}

static int connect_peer(const struct peer *peer, int port)
{
    struct sockaddr_storage address;
    int fd;
    socklen_t len = peer_endpoint(peer, AF_UNIX, port, &address);
    if (len) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            if (!connect(fd, (struct sockaddr *)&address, len)) return fd;
            close(fd);
        }
    }
    len = peer_endpoint(peer, peer->address.ss_family, port, &address);
    if (!len) return -1;
    fd = socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && peer->host->tcp_jobs && port == peer->host->port) {
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
            close(fd);
            return -1;
        }
    }
    if (fd >= 0 && connect(fd, (struct sockaddr *)&address, len) &&
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

static void stop_probe(struct broker *broker, struct peer *peer)
{
    if (peer->stats_fd < 0) return;
    dcc_watch_drop(broker->ep, &broker->watches[STATS + (peer - broker->schedule.peers)]);
    close(peer->stats_fd);
    peer->stats_fd = -1;
}

static void commit_peer_capacity(struct peer *peer, const struct dcc_capacity *metrics,
                                 uint64_t serial, uint64_t stamp, double now)
{
    peer->metrics = *metrics;
    peer->observed_serial = serial;
    peer->completed = 0;
    peer->valid_until = now + 5;
    peer->admission_supported = metrics->admission == 1;
    if (stamp && peer->host->tcp_jobs) {
        peer->report_stamp = stamp;
        peer->report_until = peer->valid_until;
    }
}

static void finish_probe(struct broker *broker, struct peer *peer, double now, int complete, int cpp)
{
    struct dcc_capacity metrics;
    peer->stats[peer->used] = '\0';
    if (complete && !dcc_parse_capacity(peer->stats, &metrics)) {
        /* Direct compilers and input producers appear in system load, not
         * daemon occupancy. Remove them at this snapshot; using
         * the later live lease count would turn completed jobs into external
         * CPU demand until the next probe. Project live leases at selection. */
        int owned = peer->direct + (peer->local ? cpp : 0);
        rs_trace("capacity sample host=%s running=%d direct=%d cpp=%d now=%.6f",
                 peer->host->hostname, metrics.running, peer->direct, peer->local ? cpp : 0, now);
        if (metrics.running >= 0)
            metrics.running = metrics.running > owned ? metrics.running - owned : 0;
        commit_peer_capacity(peer, &metrics, peer->probe_serial, metrics.report_stamp, now);
    } else if (peer->valid_until <= now && peer->deadline < broker->refresh_at) {
        /* Share a failed sample until the next regular probe. Concurrent
         * capacity queries must not serialize timeouts on the same peer. */
        peer->deadline = broker->refresh_at;
    }
    stop_probe(broker, peer);
}

static void probe_event(struct broker *broker, struct peer *peer, double now, int cpp)
{
    static const char request[] = "GET / HTTP/1.0\r\n\r\n";
    ssize_t n;
    if (!peer->stats_reading) {
        if (connected(peer->stats_fd) ||
            send(peer->stats_fd, request, sizeof(request) - 1, MSG_NOSIGNAL) !=
            sizeof(request) - 1) {
            finish_probe(broker, peer, now, 0, cpp);
            return;
        }
        peer->stats_reading = 1;
    }
    n = recv(peer->stats_fd, peer->stats + peer->used,
             sizeof(peer->stats) - 1 - peer->used, 0);
    if (n > 0) {
        peer->used += n;
        if (peer->used == sizeof(peer->stats) - 1)
            finish_probe(broker, peer, now, 0, cpp);
    } else if (!n || (errno != EAGAIN && errno != EINTR)) {
        finish_probe(broker, peer, now, !n, cpp);
    }
}

static void release_cpp(struct broker *broker, struct client *client)
{
    if (client->cpp_reserved && client->command == LOCAL_CPP)
        rs_trace("input preparation command=%u wait=%.6f execute=%.6f client=%td request=%.6f",
                 client->command, client->run_at - client->waiting_since,
                 now_seconds() - client->run_at, client - broker->schedule.clients, client->waiting_since);
    if (client->cpp_reserved) broker->dirty = 1;
    broker->schedule.cpp_busy -= client->cpp_reserved;
    client->cpp_reserved = 0;
}

static void release_assignment(struct broker *broker, struct client *client)
{
    release_cpp(broker, client);
    if (client->peer >= 0) {
        struct peer *p = &broker->schedule.peers[client->peer];
        p->busy--;
        broker->dirty = 1;
        if (client->direct) p->direct--;
    }
    dcc_watch_drop(broker->ep, &broker->watches[NETWORK + (client - broker->schedule.clients)]);
    if (client->net >= 0) close(client->net);
    client->net = client->peer = -1;
}

static void close_session(struct broker *broker, int id);

static void release_client(struct broker *broker, struct client *client)
{
    release_assignment(broker, client);
    struct client **p = &broker->sessions[client->session].jobs;
    while (*p != client) p = &(*p)->session_next;
    *p = client->session_next;
    client->fd = client->net = client->peer = -1;
    if (!client->tag) {
        broker->sessions[client->session].memo.closed = 1;
        close_session(broker, client->session);
    }
}

static int pool_remove(struct broker *broker, int i)
{
    struct idle_connection *entry = &broker->pool[i];
    if (entry->previous) broker->pool[entry->previous - 1].next = entry->next;
    else broker->schedule.peers[entry->peer].idle_head = entry->next;
    if (entry->next) broker->pool[entry->next - 1].previous = entry->previous;
    int fd = entry->fd;
    entry->fd = -1;
    entry->next = broker->free_idle;
    broker->free_idle = i + 1;
    return fd;
}
static int pool_take(struct broker *broker, int peer)
{
    int head = broker->schedule.peers[peer].idle_head;
    return head ? pool_remove(broker, head - 1) : -1;
}

static void pool_store(struct broker *broker, struct client *client, int fd, double now)
{
    struct sockaddr_storage address = {0};
    socklen_t length = sizeof(address);
    struct peer *peer = &broker->schedule.peers[client->peer];
    int i, type, valid = 0;
    socklen_t type_length = sizeof(type);
    if (!getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) && type == SOCK_STREAM &&
        !getpeername(fd, (struct sockaddr *)&address, &length)) {
        struct sockaddr_storage expected;
        valid = length >= sizeof(sa_family_t) &&
                peer_endpoint(peer, address.ss_family, peer->host->port, &expected) &&
                same_address((struct sockaddr *)&address, (struct sockaddr *)&expected, 1);
    }
    if (valid && broker->free_idle) {
        i = broker->free_idle - 1;
        uint32_t generation = broker->pool[i].generation + 1;
        struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
                                    .data.u64 = ((uint64_t)(IDLE_SOCKET + i) << 32) | generation};
        if (!epoll_ctl(broker->ep, EPOLL_CTL_ADD, fd, &event) ||
            (errno == EEXIST && !epoll_ctl(broker->ep, EPOLL_CTL_MOD, fd, &event))) {
            broker->free_idle = broker->pool[i].next;
            if (peer->idle_head) broker->pool[peer->idle_head - 1].previous = i + 1;
            broker->pool[i] = (struct idle_connection){fd, client->peer, peer->idle_head, 0, now + 10, generation};
            peer->idle_head = i + 1;
            return;
        }
    }
    close(fd);
}

static void deliver_connection(struct broker *broker, struct client *client)
{
    struct reply reply = {.slots = client->command == TRY_LOCAL,
                          .flags = client->direct && client->command != TRY_LOCAL ? DCC_BROKER_DIRECT :
                                   client->pooled ? DCC_BROKER_POOLED : 0};
    struct peer *peer = &broker->schedule.peers[client->peer];
    strcpy(reply.host, peer->host->hostdef_string);
    dcc_watch_drop(broker->ep, &broker->watches[NETWORK + (client - broker->schedule.clients)]);
    if (send_reply(client->fd, &reply, client->net)) {
        release_client(broker, client);
        return;
    }
    if (client->net >= 0) close(client->net);
    client->net = -1;
    client->state = ACTIVE;
}

static void connection_event(struct broker *broker, struct client *client,
                             double now, int timed_out)
{
    struct peer *peer = &broker->schedule.peers[client->peer];
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

/* Advance exactly one nonblocking write/read exchange. */
enum token_result { TOKEN_PENDING, TOKEN_RECEIVED, TOKEN_FAILED };
static enum token_result step_token_exchange(struct client *client, const char *token,
                                              enum client_state reading, int timed_out)
{
    if (timed_out) return TOKEN_FAILED;
    int read_reply = client->state == reading;
    size_t left = sizeof(client->hello) - client->used;
    ssize_t n = read_reply ? recv(client->net, client->hello + client->used, left, 0) :
                             send(client->net, token + client->used, left, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return TOKEN_PENDING;
    if (n <= 0) return TOKEN_FAILED;
    client->used += n;
    if (client->used < sizeof(client->hello)) return TOKEN_PENDING;
    client->used = 0;
    client->state = reading;
    return read_reply ? TOKEN_RECEIVED : TOKEN_PENDING;
}

static void token_event(struct broker *broker, struct client *client, double now, int timed_out)
{
    struct peer *peer = &broker->schedule.peers[client->peer];
    int hello = client->state == HELLO_SEND || client->state == HELLO_READ;
    const char *request = hello ? "DIST00000005" :
                          client->command == ACQUIRE_LINK ? "SLOT00000002" : "SLOT00000001";
    enum token_result result = step_token_exchange(client, request, hello ? HELLO_READ : RESERVE_READ, timed_out);
    if (result == TOKEN_PENDING) return;
    if (result == TOKEN_RECEIVED && !memcmp(client->hello, hello ? "POOL00000001" : "GRNT00000001", 12)) {
        peer->pool_supported = client->pooled = 1;
        deliver_connection(broker, client);
        return;
    }
    if (hello) peer->pool_supported = -1;
    else if (result == TOKEN_RECEIVED && !memcmp(client->hello, "BUSY00000000", 12)) {
        if (client->command == ACQUIRE_LINK) peer->link_until = now + .05;
        else peer->full_until = now + .05;
        dcc_watch_drop(broker->ep, &broker->watches[NETWORK + (client - broker->schedule.clients)]);
        pool_store(broker, client, client->net, now);
        client->net = -1;
    } else {
        peer->valid_until = 0;
        peer->admission_supported = 0;
        peer->retry_at = now + 1;
    }
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
    broker->schedule.cpp_limit = dcc_hostdef_local_cpp->slots_specified ? dcc_hostdef_local_cpp->n_slots : 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        broker->schedule.clients[i].fd = broker->schedule.clients[i].net = broker->schedule.clients[i].peer = -1;
    for (host = hosts; host; host = host->next) {
        struct addrinfo hints = {0}, *addresses = NULL;
        struct peer *peer;
        for (i = 0; i < broker->schedule.count; i++)
            if (host->port == broker->schedule.peers[i].host->port &&
                !strcmp(host->hostname, broker->schedule.peers[i].host->hostname))
                break;
        if (i < broker->schedule.count)
            continue;
        if (broker->schedule.count == MAX_HOSTS || host->mode != DCC_MODE_TCP ||
            strlen(host->hostdef_string) >= sizeof(((struct reply *)0)->host))
            return -1;
        peer = &broker->schedule.peers[broker->schedule.count++];
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
                    for (struct ifaddrs *a = interfaces; a; a = a->ifa_next)
                        peer->local |= same_address(a->ifa_addr, (struct sockaddr *)&peer->address, 0);
                    freeifaddrs(interfaces);
                }
            }
            freeaddrinfo(addresses);
        }
    }
    return broker->schedule.count ? 0 : -1;
}

static void accept_client(struct broker *broker, int listener_fd)
{
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        struct session *session = &broker->sessions[i];
        if (session->memo.fd >= 0) continue;
        struct ucred credentials;
        socklen_t size = sizeof(credentials);
        int fd;
        do { fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); }
        while (fd < 0 && errno == EINTR);
        if (fd < 0) return;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size)) {
            close(fd);
            --i;
            continue;
        }
        *session = (struct session){.memo = {.fd = fd, .peer = credentials},
                                    .denied = credentials.uid != geteuid() && credentials.uid != 0,
                                    .deadline = now_seconds() + 3};
    }
}

static void close_session(struct broker *broker, int id)
{
    struct session *session = &broker->sessions[id];
    int fd = session->memo.fd;
    if (fd < 0) return;
    session->memo.fd = -1;
    dcc_watch_drop(broker->ep, &broker->watches[CONTROL + id]);
    dcc_memo_disconnect(broker->memos, &session->memo);
    while (session->jobs) release_client(broker, session->jobs);
    if (session->memo.closed) {
        /* Closing with unread packets resets the peer before it can read
         * its final reply. Stop new sends, then discard queued records. */
        shutdown(fd, SHUT_RD);
        ssize_t n;
        do { n = recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT); }
        while (n > 0 || (n < 0 && errno == EINTR));
    }
    close(fd);
}

static struct client *find_job(struct session *session, unsigned tag)
{
    for (struct client *c = session->jobs; c; c = c->session_next)
        if (c->tag == tag) return c;
    return NULL;
}

static struct client *new_job(struct broker *broker, int id, unsigned tag)
{
    struct session *session = &broker->sessions[id];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        struct client *c = &broker->schedule.clients[i];
        if (c->fd >= 0) continue;
        *c = (struct client){.fd = session->memo.fd, .net = -1, .peer = -1, .state = WAITING,
                             .session = id, .tag = tag, .session_next = session->jobs};
        session->jobs = c;
        return c;
    }
    return NULL;
}

struct packet {
    struct broker_request_hdr header;
    uint8_t data[sizeof(((struct memo_refresh_request *)0)->stale_digest) + DCC_MEMO_LIMIT];
};
struct decoded_request {
    struct dcc_work_hint hint;
    struct feedback timing;
    const struct dcc_load_report *report;
    const char *value;
    const uint8_t *digest;
    size_t size;
    uint32_t limit;
};

static int decode_request(const struct packet *packet, size_t size, int fd,
                          const struct client *job, int memo, struct decoded_request *out)
{
    const struct broker_request_hdr *hdr = &packet->header;
    unsigned op = hdr->command;
    if (hdr->version != BROKER_VERSION || (hdr->flags & ~CLIENT_COMPILE)) return 0;
    if (hdr->flags && op != ACQUIRE_CPP && op != ACQUIRE_READY && op != ACQUIRE_POOL) return 0;
    const uint8_t *data = packet->data;
    if (op == RELEASE) return !size && fd < 0;
    if (op == WORKER_PUT) return memo && !size && fd >= 0;
    if (op == MEMO_PUT || op == MEMO_GET || op == WORKER_GET || op == MEMO_REFRESH) {
        if (job || (op == MEMO_PUT) != memo || fd >= 0) return 0;
        if (op == MEMO_REFRESH) {
            size_t n = sizeof(((struct memo_refresh_request *)0)->stale_digest);
            if (size < n) return 0;
            out->digest = data;
            data += n; size -= n;
        }
        out->value = (const char *)data;
        out->size = size;
        return size > 1 && size <= DCC_MEMO_LIMIT && !data[size-1] &&
               (op == MEMO_PUT || !memchr(data, 0, size-1));
    }
    if (job || memo) {
        if (!job || job->state != ACTIVE) return 0;
        switch (op) {
        case FAILED: return job->peer >= 0 && !size && fd < 0;
        case DEFER: return job->pooled && !size && fd >= 0;
        case CPP_DONE: return job->cpp_reserved && !size && fd < 0;
        case RETURN:
            if (fd < 0 ? !job->direct : !job->pooled) return 0;
            if (size != sizeof(out->timing)) {
                if (fd < 0 || size != sizeof(out->timing) + sizeof(*out->report)) return 0;
                out->report = (const void *)(data + sizeof(out->timing));
            }
            memcpy(&out->timing, data, sizeof(out->timing));
            return 1;
        default: return 0;
        }
    }
    if (fd >= 0) return 0;
    switch (op) {
    case ACQUIRE: case ACQUIRE_LINK: case JOBS: return !size;
    case LOCAL_CPP:
        if (size != sizeof(out->limit)) return 0;
        memcpy(&out->limit, data, sizeof(out->limit));
        return out->limit && out->limit <= INT_MAX;
    case ACQUIRE_POOL: case ACQUIRE_READY: case ACQUIRE_CPP: case TRY_LOCAL: case CGU_CAPACITY:
        if (size != DCC_WORK_HINT_BASE_SIZE && size != sizeof(out->hint)) return 0;
        memcpy(&out->hint, data, size);
        return size == dcc_work_hint_size(&out->hint) && dcc_work_valid(&out->hint, op == TRY_LOCAL) &&
               (op != CGU_CAPACITY || out->hint.kind == DCC_WORK_RUST_CGU);
    default: return 0;
    }
}

static void handle_request(struct broker *broker, int id, double now, int backoff)
{
    struct session *session = &broker->sessions[id];
    if (session->memo.closed) {
        close_session(broker, id);
        return;
    }
    struct packet packet;
    int returned;
    ssize_t n = dcc_recv_fd(session->memo.fd, &packet, sizeof(packet), &returned);
    if (n < (ssize_t)sizeof(packet.header)) goto invalid;
    session->deadline = 0;
    if (session->denied) {
        struct reply denied = {.status = EXIT_ACCESS_DENIED};
        send_reply(session->memo.fd, &denied, -1);
        goto invalid;
    }
    unsigned tag = packet.header.tag, op = packet.header.command;
    struct client *c = find_job(session, tag);
    struct decoded_request request = {0};
    if (!decode_request(&packet, n - sizeof(packet.header), returned, c, dcc_memo_has(&session->memo, tag), &request))
        goto invalid;
    switch (op) {
    case RELEASE:
        if (c) release_client(broker, c);
        else if (dcc_memo_release(broker->memos, &session->memo, tag) && session->memo.closed)
            close_session(broker, id);
        return;
    case MEMO_GET: case MEMO_REFRESH: case WORKER_GET: case MEMO_PUT: case WORKER_PUT:
        if (dcc_memo_request(broker->memos, &session->memo, tag, op, request.value, request.size,
                             request.digest, returned)) goto invalid;
        if (session->memo.closed) close_session(broker, id);
        return;
    case CPP_DONE: release_cpp(broker, c); return;
    case FAILED: {
        struct peer *p = &broker->schedule.peers[c->peer];
        if (c->reused) {
            while (p->idle_head) close(pool_take(broker, c->peer));
        } else p->retry_at = now + (backoff > 0 ? backoff : 0);
        break;
    }
    case DEFER: {
        struct peer *p = &broker->schedule.peers[c->peer];
        stop_probe(broker, p);
        if (p->host->stats_port) p->valid_until = -1;
        else p->full_until = now + 1;
        p->report_until = 0;
        broker->refresh_at = 0;
        rs_trace("rescheduling unstarted job from host=%s client=%td request=%.6f",
                 p->host->hostname, c - broker->schedule.clients, c->waiting_since);
        pool_store(broker, c, returned, now);
        break;
    }
    case RETURN: {
        observe(&broker->schedule, c, &request.timing, now);
        if (c->direct) {
            if (request.timing.execute_ns) {
                broker->schedule.direct_completed++;
                broker->schedule.direct_prepared_completed += c->command != TRY_LOCAL;
            }
        } else {
            struct peer *p = &broker->schedule.peers[c->peer];
            const struct dcc_load_report *report = request.report;
            if (report && p->metrics.jobs && dcc_tcp_get64(report->stamp) > p->report_stamp &&
                !dcc_capacity_update(&p->metrics, report, p->direct + (p->local ? broker->schedule.cpp_busy : 0))) {
                stop_probe(broker, p);
                commit_peer_capacity(p, &p->metrics, c->serial, dcc_tcp_get64(report->stamp), now);
            } else if (c->serial <= p->observed_serial) p->completed++;
            pool_store(broker, c, returned, now);
        }
        break;
    }
    default:
        c = new_job(broker, id, tag);
        if (!c) goto invalid;
        c->hint = request.hint;
        c->local_limit = request.limit;
        if (tag && op == TRY_LOCAL && c->hint.key) {
            session->preflight = now;
            session->kind = c->hint.kind;
        } else if (tag && session->preflight &&
                   (op == ACQUIRE_POOL || op == ACQUIRE_CPP || op == ACQUIRE_READY)) {
            observe_local_prepare(&broker->schedule, session->kind, now - session->preflight);
            session->preflight = 0;
        }
        c->command = op;
        c->can_compile_locally = !!(packet.header.flags & CLIENT_COMPILE);
        broker->dirty = 1;
        c->waiting_since = now;
        return;
    }
    release_client(broker, c);
    return;
invalid:
    if (returned >= 0) close(returned);
    close_session(broker, id);
}

static int dispatch(struct broker *broker, struct client *client, double now, struct dispatch_batch *batch)
{
    struct reply reply = {0};
    struct placement choice;
    switch (schedule_request(&broker->schedule, client, now, batch, &reply, &choice)) {
    case ANSWER:
        send_reply(client->fd, &reply, -1);
        release_client(broker, client);
        return 0;
    case LOCAL_SLOT:
        if (send_reply(client->fd, &reply, -1)) release_client(broker, client);
        return 0;
    case HOLD_JOB: return 0;
    case ASSIGN_JOB: break;
    }
    struct peer *p = &broker->schedule.peers[choice.peer];
    if (!choice.direct) {
        client->admission = !p->host->tcp_jobs && client->command != ACQUIRE &&
                            p->admission_supported && p->pool_supported >= 0;
        client->net = client->command != ACQUIRE ? pool_take(broker, choice.peer) : -1;
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
    schedule_assign(&broker->schedule, client, batch, &choice, now);
    client->used = 0;
    if (choice.direct || client->reused) {
        if (client->admission) client->state = RESERVE_SEND;
        else deliver_connection(broker, client);
    }
    return 0;
}

static int step_pool_and_probes(struct broker *broker, int clients, double capacity_request,
                                double now, double *next)
{
    double wake_at = *next;
    int i, probes = 0;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        struct idle_connection *entry = &broker->pool[i];
        if (entry->fd < 0) continue;
        if (now >= entry->expires) close(pool_remove(broker, i));
        else if (entry->expires < wake_at) wake_at = entry->expires;
    }
    int periodic = clients && now >= broker->refresh_at;
    if (periodic) {
        int fresh = 0;
        for (i = 0; i < broker->schedule.count; i++)
            fresh |= broker->schedule.peers[i].valid_until > now;
        if (!fresh)
            broker->first_probe_until = now + .25;
        broker->refresh_at = now + 1;
    }
    if (clients && broker->refresh_at < wake_at)
        wake_at = broker->refresh_at;
    for (i = 0; i < broker->schedule.count; i++) {
        struct peer *p = &broker->schedule.peers[i];
        if ((periodic || (p->valid_until <= now && p->deadline < capacity_request)) &&
            p->address_len && p->host->stats_port && p->stats_fd < 0 && now >= p->report_until) {
            p->probe_serial = p->serial;
            p->stats_fd = connect_peer(p, p->host->stats_port);
            p->deadline = now + .25;
            p->used = p->stats_reading = 0;
        }
        if (clients && p->full_until > now && p->full_until < wake_at) wake_at = p->full_until;
        if (clients && p->link_until > now && p->link_until < wake_at) wake_at = p->link_until;
        if (dcc_watch_set(broker->ep, &broker->watches[STATS+i], p->stats_fd,
                          p->stats_reading ? EPOLLIN : EPOLLOUT, STATS+i)) return -1;
        if (p->stats_fd >= 0) {
            if (p->deadline < wake_at)
                wake_at = p->deadline;
            probes++;
        }
    }
    *next = wake_at;
    return probes;
}

static void dispatch_waiting_batch(struct broker *broker, double now, int pending, double *next)
{
    if (!broker->dirty) return;
    struct dispatch_batch batch = {0};
    struct waiting_job waiting[MAX_CLIENTS];
    int n = schedule_batch(&broker->schedule, &batch, waiting, now, pending);
    broker->dirty = 0;
    for (int i = 0; i < n; ++i)
        if (dispatch(broker, waiting[i].client, now, &batch)) {
            broker->dirty = 1;
            *next = now;
        }
}

static int arm_watches(struct broker *broker, double now, double *next)
{
    double wake_at = *next;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        struct client *c = &broker->schedule.clients[i];
        struct session *session = &broker->sessions[i];
        if (session->memo.fd >= 0 && session->deadline && session->deadline < wake_at)
            wake_at = session->deadline;
        if (dcc_watch_set(broker->ep, &broker->watches[CONTROL+i], session->memo.fd, EPOLLIN, CONTROL+i))
            return -1;
        if (dcc_watch_set(broker->ep, &broker->watches[NETWORK+i], c->net,
                c->state == HELLO_READ || c->state == RESERVE_READ ? EPOLLIN : EPOLLOUT, NETWORK+i))
            return -1;
        if (c->fd < 0) continue;
        if (c->state == WAITING && c->deadline > now && c->deadline < wake_at)
            wake_at = c->deadline;
        if ((c->state == CONNECTING || c->state == HELLO_SEND || c->state == HELLO_READ ||
             c->state == RESERVE_SEND || c->state == RESERVE_READ) && c->deadline < wake_at)
            wake_at = c->deadline;
    }
    *next = wake_at;
    return 0;
}

static void service_ready_events(struct broker *broker, int fd, int backoff,
                                 const struct epoll_event *events, int n)
{
    int i;
    if (!n) broker->dirty = 1;
    for (int j = 0; j < n; ++j) {
        if (events[j].data.u64 == DCC_PLAN_WATCH_EVENT) {
            dcc_memo_drain(broker->memos);
            continue;
        }
        unsigned token = events[j].data.u64 >> 32;
        if (token >= IDLE_SOCKET) {
            int slot = token - IDLE_SOCKET;
            if (broker->pool[slot].fd >= 0 && broker->pool[slot].generation == (uint32_t)events[j].data.u64)
                close(pool_remove(broker, slot));
        } else dcc_watch_ready(&broker->watches[token], events[j]);
    }
    double now = now_seconds();
    /* A delayed/interrupted poll does not make queued data stale. Service
     * readiness before expiry, including statistics and connection setup;
     * otherwise scheduler delay can reset a valid local worker request or
     * discard an already-arrived capacity reply. */
    for (i = 0; i < broker->schedule.count; i++) {
        struct peer *p = &broker->schedule.peers[i];
        if (p->stats_fd < 0) continue;
        if (broker->watches[STATS+i].ready || now >= p->deadline) broker->dirty = 1;
        if (broker->watches[STATS+i].ready)
            probe_event(broker, p, now, broker->schedule.cpp_busy);
        else if (now >= p->deadline)
            finish_probe(broker, p, now, 0, broker->schedule.cpp_busy);
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct session *session = &broker->sessions[i];
        if (session->memo.fd < 0) continue;
        if (!broker->watches[CONTROL+i].ready && session->deadline && now >= session->deadline) {
            close_session(broker, i); continue;
        }
        if (broker->watches[CONTROL+i].ready) handle_request(broker, i, now, backoff);
    }
    for (i = 0; i < MAX_CLIENTS; ++i) {
        struct client *c = &broker->schedule.clients[i];
        if (c->fd < 0 || c->net < 0) continue;
        int ready = broker->watches[NETWORK+i].ready && broker->watches[NETWORK+i].fd_plus_one == c->net + 1;
        if (!ready && now < c->deadline) continue;
        broker->dirty = 1;
        if (c->state == CONNECTING) connection_event(broker, c, now, !ready);
        else token_event(broker, c, now, !ready);
    }
    if (broker->watches[LISTENER].ready) accept_client(broker, fd);
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
    broker->ep = epoll_create1(EPOLL_CLOEXEC);
    if (broker->ep < 0) { free(broker); return EXIT_IO_ERROR; }
    broker->memos = dcc_memo_store_new(broker->ep);
    if (!broker->memos) { close(broker->ep); free(broker); return EXIT_OUT_OF_MEMORY; }
    for (count = MAX_CLIENTS; count--;) {
        broker->pool[count].fd = broker->sessions[count].memo.fd = -1;
        broker->pool[count].next = broker->free_idle;
        broker->free_idle = count + 1;
    }
    broker->dirty = 1;
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
        int i, clients = 0;
        double capacity_request = 0;
        double now = now_seconds();
        double wake_at = now + 60;
        for (i = 0; i < MAX_CLIENTS; i++) {
            clients += broker->sessions[i].memo.fd >= 0;
            const struct client *c = &broker->schedule.clients[i];
            if (c->fd >= 0 && c->state == WAITING && (c->command == JOBS || c->command == CGU_CAPACITY) &&
                c->waiting_since > capacity_request)
                capacity_request = c->waiting_since;
        }
        /* A full broker leaves connections in the kernel backlog. Do not
         * spin on listener readiness until a live client releases capacity. */
        if (dcc_watch_set(broker->ep, &broker->watches[LISTENER], clients == MAX_CLIENTS ? -1 : fd,
                          EPOLLIN, LISTENER)) { ret = EXIT_IO_ERROR; break; }
        /* An idle period starts after the last client leaves, including
         * leases held through long compiles and requests that time out. */
        if (clients) {
            idle_deadline = 0;
        } else {
            if (!idle_deadline) idle_deadline = now + idle;
            if (now >= idle_deadline) break;
            if (idle_deadline < wake_at) wake_at = idle_deadline;
        }
        int probes = step_pool_and_probes(broker, clients, capacity_request, now, &wake_at);
        if (probes < 0) { ret = EXIT_IO_ERROR; break; }
        dispatch_waiting_batch(broker, now, probes && now < broker->first_probe_until, &wake_at);
        if (arm_watches(broker, now, &wake_at)) { ret = EXIT_IO_ERROR; break; }
        int timeout = wake_at > now ? (int)((wake_at - now) * 1000) + 1 : 0;
        struct epoll_event events[128];
        int n = epoll_pwait(broker->ep, events, 128, timeout, &previous);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ret = EXIT_IO_ERROR;
            break;
        }
        service_ready_events(broker, fd, backoff, events, n);
    }
    for (count = 0; count < MAX_CLIENTS; ++count)
        if (broker->sessions[count].memo.fd >= 0) close_session(broker, count);
    for (count = 0; count < broker->schedule.count; count++)
        if (broker->schedule.peers[count].stats_fd >= 0)
            close(broker->schedule.peers[count].stats_fd);
done:
    if (mask_set) sigprocmask(SIG_SETMASK, &previous, NULL);
    for (count = 0; count < MAX_CLIENTS; ++count)
        if (broker->pool[count].fd >= 0) close(broker->pool[count].fd);
    if (fd >= 0) {
        close(fd);
        if (!inherited)
            unlink(path);
    }
    if (lock_fd >= 0)
        close(lock_fd);
    free_hosts(hosts);
    dcc_memo_store_free(broker->memos);
    close(broker->ep);
    free(broker);
    return ret;
}

#else
int dcc_broker_serve(const char *path)
{
    (void)path;
    rs_log_error("the local broker requires Linux");
    return EXIT_BAD_ARGUMENTS;
}
#endif
