#include <config.h>
#include "fd_io.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
/* The sender retains ownership; the receiver owns every returned CLOEXEC fd. */
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
#endif
