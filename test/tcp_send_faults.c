/* Exercise the real client while writes are short, blocked or disconnected.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tcp_job.h"

static void mark(unsigned bit, char event)
{
    static unsigned marked;
    if (marked & bit) return;
    marked |= bit;
    const char *path = getenv("DISTCC_TEST_SEND_LOG");
    int fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600) : -1;
    if (fd >= 0) { (void)write(fd, &event, 1); close(fd); }
}

ssize_t send(int fd, const void *data, size_t size, int flags)
{
    static ssize_t (*original)(int, const void *, size_t, int);
    static size_t remaining, transferred;
    static unsigned calls, type;
    if (!original) original = dlsym(RTLD_NEXT, "send");
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (!(flags & MSG_DONTWAIT) || getsockname(fd, (struct sockaddr *)&address, &length) ||
        (address.ss_family != AF_INET && address.ss_family != AF_INET6))
        return original(fd, data, size, flags);
    if (!remaining) {
        if (size < 8) abort();
        uint32_t header[2];
        memcpy(header, data, sizeof(header));
        type = ntohl(header[0]) & ~DCC_TCP_FLAGS;
        remaining = 8U + ntohl(header[1]);
        transferred = 0;
    }
    if (type == DCC_TCP_FS_REPLY && transferred && getenv("DISTCC_TEST_SEND_DROP")) {
        mark(4, 'D'); shutdown(fd, SHUT_RDWR); errno = ECONNRESET; return -1;
    }
    if (++calls % 3 == 1) { mark(1, 'B'); errno = EAGAIN; return -1; }
    size_t limited = size > 4093 ? 4093 : size;
    ssize_t result = original(fd, data, limited, flags);
    if (result > 0) {
        if ((size_t)result > remaining) abort();
        remaining -= result; transferred += result;
        if ((size_t)result < size) mark(2, 'S');
    }
    return result;
}
