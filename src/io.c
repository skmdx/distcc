/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


/**
 * @file
 *
 * Common low-level IO utilities.
 *
 * This code is not meant to know about our protocol, only to provide
 * a more comfortable layer on top of Unix IO.
 *
 * @todo Perhaps write things out using writev() to reduce the number
 * of system calls, and the risk of small packets when not using
 * TCP_CORK.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <limits.h>
#include <poll.h>
#include <time.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"



#ifdef __linux__
/* Local SOCK_SEQPACKET control messages carry at most one descriptor. The
 * sender retains ownership; the receiver owns the returned CLOEXEC fd. */
int dcc_send_fds(int sock, const void *data, size_t size, const int *fds, unsigned count)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(DCC_IPC_FDS_MAX * sizeof(int))]; } control = {0};
    struct iovec iov = { (void *)data, size };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t n;
    if (count > DCC_IPC_FDS_MAX) { errno = EINVAL; return -1; }
    if (count) {
        struct cmsghdr *cmsg;
        msg.msg_control = control.bytes;
        msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    }
    do { n = sendmsg(sock, &msg, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == (ssize_t)size ? 0 : -1;
}

ssize_t dcc_recv_fds(int sock, void *data, size_t size, int *fds, unsigned *count)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE((DCC_IPC_FDS_MAX + 1) * sizeof(int))]; } control;
    struct iovec iov = { data, size };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *cmsg;
    ssize_t n;
    int bad = 0;
    unsigned capacity = *count;
    *count = 0;
    do { n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC | MSG_TRUNC); } while (n < 0 && errno == EINTR);
    if (n < 0) return n;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t i, amount = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (i = 0; i < amount; ++i) {
                int received;
                memcpy(&received, (char *)CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                if (*count < capacity && *count < DCC_IPC_FDS_MAX) fds[(*count)++] = received;
                else { close(received); bad = 1; }
            }
        } else bad = 1;
    }
    if (bad || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        while (*count) close(fds[--*count]);
        errno = EPROTO;
        return -1;
    }
    return n;
}

int dcc_send_fd(int sock, const void *data, size_t size, int fd)
{
    return dcc_send_fds(sock, data, size, &fd, fd >= 0 ? 1 : 0);
}

ssize_t dcc_recv_fd(int sock, void *data, size_t size, int *fd)
{
    unsigned count = 1;
    *fd = -1;
    ssize_t n = dcc_recv_fds(sock, data, size, fd, &count);
    if (!count) *fd = -1;
    return n;
}
#endif

int dcc_get_io_timeout(void)
{
    /** Timeout for all IO other than opening connections.  Much longer,
     * because compiling files can take a long time. **/
    static const int default_dcc_io_timeout = 300; /* seconds */
    static int current_timeout = 0;

    if (current_timeout > 0)
        return current_timeout;

    const char *user_timeout = getenv("DISTCC_IO_TIMEOUT");
    if (user_timeout) {
        int parsed_user_timeout = atoi(user_timeout);
        if (parsed_user_timeout <= 0) {
          rs_log_error("Bad DISTCC_IO_TIMEOUT value: %s", user_timeout);
          exit(EXIT_BAD_ARGUMENTS);
        }
        current_timeout = parsed_user_timeout;
    } else {
        current_timeout = default_dcc_io_timeout;
    }
    return current_timeout;
}

/* Return poll's ready/timeout/error result. Signals do not renew the budget. */
int dcc_poll_fd(int fd, short events, int timeout)
{
    if (fd < 0 || timeout < 0) {
        errno = EINVAL;
        return -1;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    int64_t remaining = (int64_t)timeout * 1000000000;
    const int64_t end = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + remaining;
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int64_t ms = (remaining + 999999) / 1000000;
        int ready = poll(&p, 1, ms > INT_MAX ? INT_MAX : (int)ms);
        if (ready > 0 && (p.revents & POLLNVAL)) {
            errno = EBADF;
            return -1;
        }
        if (ready > 0 || (ready < 0 && errno != EINTR))
            return ready;
        if (!ready && ms <= INT_MAX) return 0;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        remaining = end - ((int64_t)now.tv_sec * 1000000000 + now.tv_nsec);
        if (remaining <= 0) return 0;
    }
}

static int wait_io(int fd, short events, int timeout)
{
    int ready = dcc_poll_fd(fd, events, timeout);
    if (ready > 0) return 0;  /* read/write reports EOF or a socket error. */
    if (ready < 0) rs_log_error("poll failed: %s", strerror(errno));
    else rs_log_error("IO timeout");
    return EXIT_IO_ERROR;
}

int dcc_select_for_read(int fd, int timeout)
{
    return wait_io(fd, POLLIN, timeout);
}

int dcc_select_for_write(int fd, int timeout)
{
    return wait_io(fd, POLLOUT, timeout);
}



/**
 * Read exactly @p len bytes from a file.
 **/
static struct {
    int fd;
    size_t head, size;
    char data[65536];
} read_buffer = { .fd = -1 };

void dcc_buffer_input(int fd)
{
    read_buffer.fd = fd;
    read_buffer.head = read_buffer.size = 0;
}

int dcc_readx(int fd, void *buf, size_t len)
{
    ssize_t r;
    int ret;

    while (len > 0) {
        int buffered = fd == read_buffer.fd && len < sizeof(read_buffer.data);
        if (fd == read_buffer.fd && read_buffer.size) {
            size_t n = len < read_buffer.size ? len : read_buffer.size;
            memcpy(buf, read_buffer.data + read_buffer.head, n);
            read_buffer.head += n;
            read_buffer.size -= n;
            buf = (char *)buf + n;
            len -= n;
            continue;
        }
        r = read(fd, buffered ? read_buffer.data : buf,
                    buffered ? sizeof(read_buffer.data) : len);

        if (r == -1 && errno == EAGAIN) {
            if ((ret = dcc_select_for_read(fd, dcc_get_io_timeout())))
                return ret;
            else
                continue;
        } else if (r == -1 && errno == EINTR) {
            continue;
        } else if (r == -1) {
            rs_log_error("failed to read: %s", strerror(errno));
            return EXIT_IO_ERROR;
        } else if (r == 0) {
            rs_log_error("unexpected eof on fd%d", fd);
            return EXIT_TRUNCATED;
        } else {
            if (buffered) {
                read_buffer.head = 0;
                read_buffer.size = r;
                continue;
            }
            buf = &((char *) buf)[r];
            len -= r;
        }
    }

    return 0;
}


/**
 * Write bytes to an fd.  Keep writing until we're all done or something goes
 * wrong.
 *
 * @returns 0 or exit code.
 **/
int dcc_writex(int fd, const void *buf, size_t len)
{
    ssize_t r;
    int ret;

    while (len > 0) {
        r = write(fd, buf, len);

        if (r == -1 && errno == EAGAIN) {
            if ((ret = dcc_select_for_write(fd, dcc_get_io_timeout())))
                return ret;
            else
                continue;
        } else if (r == -1 && errno == EINTR) {
            continue;
        } else if (r == -1) {
            rs_log_error("failed to write: %s", strerror(errno));
            return EXIT_IO_ERROR;
        } else {
            buf = &((char *) buf)[r];
            len -= r;
        }
    }

    return 0;
}

/**
 * Stick a TCP cork in the socket.  It's not clear that this will help
 * performance, but it might.
 *
 * This is a no-op if we don't think this platform has corks.
 **/
int tcp_cork_sock(int POSSIBLY_UNUSED(fd), int POSSIBLY_UNUSED(corked))
{
#if defined(TCP_CORK) && defined(SOL_TCP)
    if (!dcc_getenv_bool("DISTCC_TCP_CORK", 1) ||
        !(not_sd_is_socket(fd, AF_INET, SOCK_STREAM, 0) > 0 ||
          not_sd_is_socket(fd, AF_INET6, SOCK_STREAM, 0) > 0))
        return 0;

    if (setsockopt(fd, SOL_TCP, TCP_CORK, &corked, sizeof corked) == -1) {
        if (errno == ENOSYS || errno == ENOTSUP) {
            if (corked)
                rs_trace("no corks allowed on fd%d", fd);
            /* no need to complain about not uncorking */
        } else {
            rs_log_warning("setsockopt(corked=%d) failed: %s",
                           corked, strerror(errno));
            /* continue anyhow */
        }
    }
#endif /* def TCP_CORK */
    return 0;
}

int dcc_close(int fd)
{
    if (read_buffer.fd == fd) dcc_buffer_input(-1);
    if (close(fd) != 0) {
        rs_log_error("failed to close fd%d: %s", fd, strerror(errno));
        return EXIT_IO_ERROR;
    }
    return 0;
}
