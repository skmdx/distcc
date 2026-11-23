/* Per-process broker control transport. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "distcc.h"
#include "broker.h"
#include "broker_protocol.h"
#include "broker_session.h"

#ifdef __linux__
#include <pthread.h>
#include <poll.h>
#include <limits.h>
#include <time.h>
static pthread_once_t cleanup_once = PTHREAD_ONCE_INIT;
static pthread_key_t cleanup_key;
static int cleanup_error;
static void close_thread_session(void *socket) { close((intptr_t)socket - 1); }
static void init_cleanup(void) { cleanup_error = pthread_key_create(&cleanup_key, close_thread_session); }

static _Thread_local struct {
    pid_t pid;
    int fd;
    struct sockaddr_un address;
    /* A disconnected lease remains reserved until its caller drops the handle. */
    enum { FREE, LIVE, LOST } tags[DCC_BROKER_CLIENT_MAX];
} session = {.fd = -1};

static void dcc_session_lost(void)
{
    if (session.fd >= 0) {
        pthread_setspecific(cleanup_key, NULL);
        close(session.fd);
    }
    session.fd = -1;
    for (unsigned i = 0; i < DCC_BROKER_CLIENT_MAX; ++i)
        if (session.tags[i] == LIVE) session.tags[i] = LOST;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct reconnect { double deadline; int delay; };
static int reconnect_wait(struct reconnect *retry, int error, int allow_unreachable)
{
    if (error != ENOENT && error != ECONNREFUSED && error != ECONNRESET &&
        error != EPIPE && !(allow_unreachable && error == EHOSTUNREACH)) return 0;
    if (!retry->deadline) {
        const char *setting = getenv("DISTCC_RECONNECT_TIMEOUT");
        retry->deadline = now_seconds() + (setting ? atoi(setting) : 90);
        retry->delay = 100;
    }
    double remaining = retry->deadline - now_seconds();
    if (remaining <= 0) return 0;
    struct pollfd delay = {-1, 0, 0};
    if (dcc_poll_cancelable(&delay, remaining * 1000 < retry->delay ? (int)(remaining * 1000) : retry->delay) < 0 &&
        errno == ECANCELED) return 0;
    errno = error;
    if (retry->delay < 1000) retry->delay *= 2;
    return 1;
}

static int dcc_session_begin(unsigned *tag, double deadline, int io_timeout)
{
    const char *path = getenv("DISTCC_BROKER");
    if (!path || !*path || strlen(path) >= sizeof(session.address.sun_path)) { errno = EINVAL; return -1; }
    if (session.pid != getpid() || strcmp(path, session.address.sun_path)) {
        dcc_session_lost();
        session.pid = getpid();
        session.address.sun_family = AF_UNIX;
        strcpy(session.address.sun_path, path);
    }
    if (session.fd < 0) {
        pthread_once(&cleanup_once, init_cleanup);
        if (cleanup_error) { errno = cleanup_error; return -1; }
        session.fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (session.fd < 0) return -1;
        int error = pthread_setspecific(cleanup_key, (void *)(intptr_t)(session.fd + 1));
        if (error) { close(session.fd); session.fd = -1; errno = error; return -1; }
        int one = 1;
        if (setsockopt(session.fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) ||
            connect(session.fd, (struct sockaddr *)&session.address, sizeof(session.address))) {
            error = errno;
            dcc_session_lost(); errno = error; return -1;
        }
    }
    struct timeval timeout = {.tv_sec = io_timeout};
    if (deadline) {
        double left = deadline - now_seconds();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        if (!io_timeout || left < io_timeout) {
            timeout.tv_sec = (time_t)left;
            timeout.tv_usec = (suseconds_t)((left - timeout.tv_sec) * 1000000);
            if (!timeout.tv_sec && !timeout.tv_usec) timeout.tv_usec = 1;
        }
    }
    if (setsockopt(session.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(session.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) return -1;
    for (unsigned i = 0; i < DCC_BROKER_CLIENT_MAX; ++i) {
        if (session.tags[i] != FREE) continue;
        session.tags[i] = LIVE;
        *tag = i + 1;
        return session.fd;
    }
    errno = EMFILE;
    return -1;
}

void dcc_session_finish(int lease)
{
    unsigned i = (unsigned)lease - DCC_BROKER_LEASE_BASE - 1;
    if (i < DCC_BROKER_CLIENT_MAX) session.tags[i] = FREE;
}

static ssize_t receive_reply(int fd, void *buffer, size_t size, int *net)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))]; } control;
    struct iovec part = {buffer, size};
    struct msghdr msg = {.msg_iov = &part, .msg_iovlen = 1,
                         .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    int authenticated = 0, received_fds = 0;
    ssize_t n;
    struct cmsghdr *cmsg;
    *net = -1;
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
            size_t offset, bytes = cmsg->cmsg_len - CMSG_LEN(0);
            for (offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
                int received;
                memcpy(&received, (char *)CMSG_DATA(cmsg) + offset, sizeof(received));
                if (!received_fds++) *net = received;
                else close(received);
            }
        }
    }
    if (n > 0 && authenticated && received_fds <= 1 && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) return n;
    if (*net >= 0) { close(*net); *net = -1; }
    if (n >= 0) errno = !n ? ECONNRESET : !authenticated ? EACCES : EPROTO;
    return -1;
}

static int mark_queued(int *queued_fd)
{
    if (!queued_fd || *queued_fd < 0) return 0;
    ssize_t n;
    do { n = write(*queued_fd, "Q", 1); } while (n < 0 && errno == EINTR);
    if (n != 1) {
        if (errno == EPIPE) errno = ECANCELED;
        return -1;
    }
    close(*queued_fd);
    *queued_fd = -1;
    return 0;
}

int dcc_session_call(struct broker_request_hdr *header, struct iovec *parts, size_t count,
                     struct iovec reply, int *descriptor, size_t *received, int *queued_fd,
                     int io_timeout, dcc_reply_decoder decode)
{
    struct reconnect retry = {0};
    struct msghdr msg = {.msg_iov = parts, .msg_iovlen = count};
    *descriptor = -1;
    do {
        unsigned tag;
        int fd = dcc_session_begin(&tag, retry.deadline, io_timeout);
        if (fd < 0) continue;
        int lease = DCC_BROKER_LEASE_BASE + tag;
        header->tag = tag;
        ssize_t sent;
        do { sent = sendmsg(fd, &msg, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
        /* A denied peer can close immediately after queuing its error. */
        int read_reply = sent < 0 ? errno == EPIPE || errno == ECONNRESET : mark_queued(queued_fd) == 0;
        if (read_reply && dcc_client_cancel_fd >= 0) {
            struct pollfd p = {fd, POLLIN, 0};
            double end = io_timeout ? now_seconds() + io_timeout : 0;
            if (retry.deadline && (!end || retry.deadline < end)) end = retry.deadline;
            int ready;
            do {
                double ms = end ? (end - now_seconds()) * 1000 : 0;
                int timeout = !end ? -1 : ms <= 0 ? 0 : ms >= INT_MAX ? INT_MAX : (int)ms + 1;
                ready = dcc_poll_cancelable(&p, timeout);
            } while (ready < 0 && errno == EINTR);
            if (!ready) errno = ETIMEDOUT;
            read_reply = ready > 0;
        }
        if (read_reply) {
            ssize_t n = receive_reply(fd, reply.iov_base, reply.iov_len, descriptor);
            if (n >= 0) {
                int error = decode(header->command, header->flags, reply.iov_base, n, *descriptor);
                if (!error) {
                    if (received) *received = n;
                    return lease;
                }
                if (*descriptor >= 0) { close(*descriptor); *descriptor = -1; }
                errno = error;
            }
        }
        int error = errno;
        dcc_session_lost();
        dcc_session_finish(lease);
        errno = error;
    } while (reconnect_wait(&retry, errno, header->command != JOBS));
    return -1;
}

void dcc_session_send_lease(int lease, unsigned command, const struct iovec *payload,
                            size_t count, int descriptor, enum dcc_lease_end end)
{
    unsigned i = (unsigned)lease - DCC_BROKER_LEASE_BASE - 1;
    if (i >= DCC_BROKER_CLIENT_MAX) return;
    if (session.pid == getpid() && session.tags[i] == LIVE) {
        struct broker_request_hdr header = {BROKER_VERSION, command, 0, i + 1};
        struct iovec parts[1 + count];
        parts[0] = (struct iovec){&header, sizeof(header)};
        if (count) memcpy(parts + 1, payload, count * sizeof(*parts));
        struct msghdr msg = {.msg_iov = parts, .msg_iovlen = count + 1};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
        if (descriptor >= 0) {
            msg.msg_control = control.bytes;
            msg.msg_controllen = sizeof(control.bytes);
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &descriptor, sizeof(descriptor));
        }
        ssize_t n;
        do { n = sendmsg(session.fd, &msg, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        if (n < 0) dcc_session_lost();
        else if (end == RETIRE_LEASE) session.tags[i] = LOST;
    }
    if (end == FINISH_LEASE) dcc_session_finish(lease);
}

void dcc_broker_release(int lease)
{
    dcc_session_send_lease(lease, RELEASE, NULL, 0, -1, FINISH_LEASE);
}
#else
void dcc_broker_release(int lease) { (void)lease; }
#endif
