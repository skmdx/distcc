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

#include "distcc.h"
#include "hosts.h"
#include "adaptive.h"
#include "broker.h"
#include "exitcode.h"
#include "trace.h"

#ifdef __linux__

/* This is local IPC between matching versions, never a network protocol.
 * The control connection is a lease: EOF releases the reserved slot. Bulk
 * compiler traffic goes directly over the descriptor sent with the reply. */
#define BROKER_VERSION 2
#define MAX_HOSTS 128
#define MAX_CLIENTS 256
enum { ACQUIRE = 1, JOBS = 2, CPP_DONE = 3 };
struct request { uint32_t version, command; };
struct reply { int32_t status, slots; char host[1024]; };

struct peer {
    struct dcc_hostdef *host;
    struct sockaddr_storage address;
    socklen_t address_len;
    struct dcc_capacity metrics;
    double valid_until, retry_at, deadline;
    int busy, stats_fd, stats_reading;
    size_t used;
    char stats[4096];
};
enum client_state { REQUEST, WAITING, CONNECTING, ACTIVE };
struct client {
    int fd, net, peer, cpp_reserved;
    enum client_state state;
    uint32_t command;
    double deadline;
};
struct broker {
    struct peer peers[MAX_HOSTS];
    struct client clients[MAX_CLIENTS];
    int count, cpp_busy, cpp_limit;
    double refresh_at, first_probe_until;
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
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec iov = { (void *)reply, sizeof(*reply) };
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (net >= 0) {
        struct cmsghdr *cmsg;
        memset(&control, 0, sizeof(control));
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &net, sizeof(net));
    }
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == sizeof(*reply) ? 0 : -1;
}

static int broker_request(uint32_t command, struct reply *reply, int *net)
{
    struct sockaddr_un address;
    struct request request = { BROKER_VERSION, command };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec iov = { reply, sizeof(*reply) };
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    int fd;
    ssize_t n;
    *net = -1;
    if (unix_address(getenv("DISTCC_BROKER"), &address))
        return -1;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        send(fd, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request))
        goto failed;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    do {
        n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    for (cmsg = CMSG_FIRSTHDR(&msg); n >= 0 && cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(net, CMSG_DATA(cmsg), sizeof(*net));
    }
    if (n != sizeof(*reply) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        reply->status || !memchr(reply->host, '\0', sizeof(reply->host)) ||
        (command == ACQUIRE && *net < 0) || (command == JOBS && *net >= 0))
        goto failed;
    return fd;
failed:
    if (*net >= 0) {
        close(*net);
        *net = -1;
    }
    close(fd);
    rs_log_error("local broker request failed");
    return -1;
}

int dcc_broker_acquire(struct dcc_hostdef **host, int *lease)
{
    struct reply reply;
    struct dcc_hostdef *local = NULL;
    int count, net, fd;
    /* Preserve the client's --localslots and --localslots_cpp settings. */
    if (dcc_get_hostlist(&local, &count))
        return EXIT_BAD_HOSTSPEC;
    free_hosts(local);
    fd = broker_request(ACQUIRE, &reply, &net);
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
    (*host)->broker_fd = net;
    (*host)->broker_lease = fd;
    *lease = fd;
    return 0;
}

int dcc_broker_jobs(void)
{
    struct reply reply;
    int net, fd = broker_request(JOBS, &reply, &net);
    if (fd < 0)
        return -1;
    close(fd);
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
    if (!peer->address_len)
        return -1;
    if (address.ss_family == AF_INET)
        ((struct sockaddr_in *)&address)->sin_port = htons(port);
    else
        ((struct sockaddr_in6 *)&address)->sin6_port = htons(port);
    fd = socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error ? -1 : 0;
}

static void finish_probe(struct peer *peer, double now, int complete)
{
    peer->stats[peer->used] = '\0';
    if (complete && !dcc_parse_capacity(peer->stats, &peer->metrics))
        peer->valid_until = now + 5;
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

static void release_client(struct broker *broker, struct client *client)
{
    broker->cpp_busy -= client->cpp_reserved;
    client->cpp_reserved = 0;
    if (client->peer >= 0)
        broker->peers[client->peer].busy--;
    if (client->net >= 0)
        close(client->net);
    close(client->fd);
    client->fd = client->net = client->peer = -1;
}

static int dispatch(struct broker *broker, struct client *client, double now)
{
    struct reply reply = {0};
    int i, slots, best = -1, available = 0;
    double best_score = 0;
    for (i = 0; i < broker->count; i++) {
        struct peer *p = &broker->peers[i];
        double capacity = dcc_capacity_value(p->host,
            p->valid_until > now ? &p->metrics : NULL, &slots);
        if (!p->address_len || p->retry_at > now || capacity <= 0)
            continue;
        available++;
        reply.slots += slots;
        if (p->busy < slots) {
            double score = (p->busy + 1) / capacity;
            if (best < 0 || score < best_score) {
                best = i;
                best_score = score;
            }
        }
    }
    if (client->command == JOBS || !available) {
        reply.status = available ? 0 : EXIT_CONNECT_FAILED;
        send_reply(client->fd, &reply, -1);
        release_client(broker, client);
    } else if (best >= 0 && broker->cpp_busy < broker->cpp_limit) {
        struct peer *p = &broker->peers[best];
        client->net = connect_peer(p, p->host->port);
        if (client->net < 0) {
            p->retry_at = now + 5;
            return 1;
        }
        client->peer = best;
        client->state = CONNECTING;
        client->deadline = now + 3;
        p->busy++;
        broker->cpp_busy++;
        client->cpp_reserved = 1;
    }
    return 0;
}

static void connection_event(struct broker *broker, struct client *client,
                             double now, int timed_out)
{
    struct peer *peer = &broker->peers[client->peer];
    struct reply reply = {0};
    if (timed_out || connected(client->net)) {
        peer->retry_at = now + 5;
        peer->busy--;
        broker->cpp_busy--;
        client->cpp_reserved = 0;
        close(client->net);
        client->net = client->peer = -1;
        client->state = WAITING;
        return;
    }
    strcpy(reply.host, peer->host->hostdef_string);
    /* Preserve O_NONBLOCK: the client's IO helpers implement deadlines. */
    if (send_reply(client->fd, &reply, client->net)) {
        release_client(broker, client);
        return;
    }
    close(client->net);
    client->net = -1;
    client->state = ACTIVE;
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
    broker->cpp_limit = dcc_hostdef_local_cpp->n_slots;
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
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (!getaddrinfo(host->hostname, NULL, &hints, &addresses)) {
            if (addresses->ai_addrlen <= sizeof(peer->address)) {
                memcpy(&peer->address, addresses->ai_addr, addresses->ai_addrlen);
                peer->address_len = addresses->ai_addrlen;
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
    if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) &&
        credentials.uid == geteuid()) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *client = &broker->clients[i];
            if (client->fd < 0) {
                client->fd = fd;
                client->state = REQUEST;
                client->deadline = now_seconds() + 3;
                return;
            }
        }
    }
    close(fd);
}

int dcc_broker_serve(const char *path)
{
    struct broker *broker = calloc(1, sizeof(*broker));
    struct dcc_hostdef *hosts = NULL;
    int count, fd = -1, lock_fd = -1, inherited = 0, ret = EXIT_BAD_ARGUMENTS;
    const char *setting = getenv("DISTCC_BROKER_IDLE");
    char *end;
    long idle = setting ? strtol(setting, &end, 10) : 60;
    double last_request = now_seconds();
    if (!broker)
        return EXIT_OUT_OF_MEMORY;
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
    ret = 0;
    while (!stopping) {
        struct pollfd polls[1 + MAX_HOSTS + 2 * MAX_CLIENTS];
        int stats_index[MAX_HOSTS], control_index[MAX_CLIENTS], net_index[MAX_CLIENTS];
        int i, n = 1, clients = 0, probes = 0, timeout;
        double now = now_seconds();
        double wake_at = now + 60;
        polls[0] = (struct pollfd){ fd, POLLIN, 0 };
        for (i = 0; i < MAX_CLIENTS; i++)
            clients += broker->clients[i].fd >= 0;
        if (!clients && now - last_request >= idle)
            break;
        if (!clients)
            wake_at = last_request + idle;
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
            if (c->state == REQUEST && now >= c->deadline) {
                release_client(broker, c);
                continue;
            }
            if (c->state == CONNECTING && now >= c->deadline)
                connection_event(broker, c, now, 1);
            if (c->state == WAITING && (!probes || now >= broker->first_probe_until) &&
                dispatch(broker, c, now))
                wake_at = now;
            if (c->fd < 0)
                continue;
            if ((c->state == REQUEST || c->state == CONNECTING) && c->deadline < wake_at)
                wake_at = c->deadline;
            control_index[i] = n;
            polls[n++] = (struct pollfd){ c->fd, POLLIN, 0 };
            if (c->state == CONNECTING) {
                net_index[i] = n;
                polls[n++] = (struct pollfd){ c->net, POLLOUT, 0 };
            }
        }
        timeout = wake_at > now ? (int)((wake_at - now) * 1000) + 1 : 0;
        if (poll(polls, n, timeout) < 0) {
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
                struct request request;
                ssize_t length = recv(c->fd, &request, sizeof(request), MSG_TRUNC);
                if (c->state == ACTIVE && length == sizeof(request) &&
                    request.version == BROKER_VERSION && request.command == CPP_DONE &&
                    c->cpp_reserved) {
                    broker->cpp_busy--;
                    c->cpp_reserved = 0;
                    continue;
                }
                if (c->state != REQUEST || length != sizeof(request) ||
                    request.version != BROKER_VERSION ||
                    (request.command != ACQUIRE && request.command != JOBS)) {
                    release_client(broker, c);
                    continue;
                }
                c->command = request.command;
                c->state = WAITING;
                last_request = now;
            }
            if (net_index[i] >= 0 && polls[net_index[i]].revents)
                connection_event(broker, c, now, 0);
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
int dcc_broker_serve(const char *path)
{
    (void)path;
    rs_log_error("the local broker requires Linux");
    return EXIT_BAD_ARGUMENTS;
}
int dcc_broker_acquire(struct dcc_hostdef **host, int *lease)
{
    (void)host;
    (void)lease;
    return EXIT_CONNECT_FAILED;
}
int dcc_broker_jobs(void)
{
    return -1;
}
void dcc_broker_cpp_done(const struct dcc_hostdef *host)
{
    (void)host;
}
#endif
