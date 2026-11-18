/* Per-process broker control transport. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "broker.h"
#include "broker_protocol.h"
#include "broker_session.h"

#ifdef __linux__
static struct {
    pid_t pid;
    int fd, credentials;
    struct sockaddr_un address;
    /* A disconnected lease remains reserved until its caller drops the handle. */
    enum { FREE, LIVE, LOST } tags[DCC_BROKER_CLIENT_MAX];
} session = {.fd = -1};

void dcc_session_lost(void)
{
    if (session.fd >= 0) close(session.fd);
    session.fd = -1;
    for (unsigned i = 0; i < DCC_BROKER_CLIENT_MAX; ++i)
        if (session.tags[i] == LIVE) session.tags[i] = LOST;
}

int dcc_session_begin(int credentials, unsigned *tag)
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
        session.fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (session.fd < 0) return -1;
        if (connect(session.fd, (struct sockaddr *)&session.address, sizeof(session.address))) {
            int error = errno;
            dcc_session_lost(); errno = error; return -1;
        }
        session.credentials = 0;
    }
    if (session.credentials != credentials) {
        if (setsockopt(session.fd, SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials))) return -1;
        session.credentials = credentials;
    }
    for (unsigned i = 0; i < DCC_BROKER_CLIENT_MAX; ++i) {
        if (session.tags[i] != FREE) continue;
        session.tags[i] = LIVE;
        *tag = i + 1;
        return session.fd;
    }
    errno = EMFILE;
    return -1;
}

int dcc_session_fd(int lease)
{
    unsigned i = (unsigned)lease - DCC_BROKER_LEASE_BASE - 1;
    return i < DCC_BROKER_CLIENT_MAX && session.pid == getpid() && session.tags[i] == LIVE ? session.fd : -1;
}

void dcc_session_retire(int lease)
{
    unsigned i = (unsigned)lease - DCC_BROKER_LEASE_BASE - 1;
    if (i < DCC_BROKER_CLIENT_MAX && session.tags[i] == LIVE) session.tags[i] = LOST;
}

void dcc_session_finish(int lease)
{
    unsigned i = (unsigned)lease - DCC_BROKER_LEASE_BASE - 1;
    if (i < DCC_BROKER_CLIENT_MAX) session.tags[i] = FREE;
}

void dcc_broker_release(int lease)
{
    int fd = dcc_session_fd(lease);
    if (fd >= 0) {
        unsigned tag = lease - DCC_BROKER_LEASE_BASE;
        struct request request = {BROKER_VERSION, RELEASE | (tag << BROKER_TAG_SHIFT)};
        if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request)) dcc_session_lost();
    }
    dcc_session_finish(lease);
}
#else
void dcc_broker_release(int lease) { (void)lease; }
#endif
