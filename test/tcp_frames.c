/* Exercise the wire reader's real nonblocking I/O and storage ownership. */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "tcp_job.h"

static void require(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
static int receive(int *pair, struct dcc_tcp_frame *frame, unsigned type,
                   const unsigned char *data, size_t length, size_t limit, int rights)
{
    unsigned char header[8];
    dcc_tcp_put32(header, type); dcc_tcp_put32(header + 4, length);
    size_t sent = 0;
    int result = 0;
    while (!result) {
        const unsigned char *part = sent < 8 ? header + sent : data + sent - 8;
        size_t amount = sent < 8 ? 1 : length + 8 - sent;
        if (amount > 4093) amount = 4093;
        if (amount) {
            struct iovec iov = {(void *)part, amount};
            union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control;
            struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
            if (!sent && rights >= 0) {
                message.msg_control = control.data; message.msg_controllen = sizeof(control);
                struct cmsghdr *c = CMSG_FIRSTHDR(&message);
                c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(c), &rights, sizeof(rights));
            }
            ssize_t count = sendmsg(pair[0], &message, MSG_DONTWAIT | MSG_NOSIGNAL);
            require(count > 0 || (count < 0 && errno == EAGAIN), "send fragment");
            if (count > 0) sent += count;
        }
        result = dcc_tcp_frame_read(pair[1], frame, limit);
        require(result <= 0 || sent == length + 8, "premature complete frame");
        require(result != 0 || sent < length + 8, "complete frame still pending");
    }
    return result;
}

int main(void)
{
    int pair[2];
    require(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "socketpair");
    size_t large = DCC_TCP_CHUNK + 4096;
    unsigned char *data = malloc(large);
    require(data != NULL, "payload allocation");
    for (size_t i = 0; i < large; ++i) data[i] = (unsigned char)(i * 29 + i / 997);
    struct dcc_tcp_frame frame = {0}, decoding = {0};
    require(dcc_tcp_frame_read(pair[1], &frame, large) == 0, "empty nonblocking read");
    require(receive(pair, &frame, DCC_TCP_FS_REPLY, data, DCC_TCP_CHUNK + 8U, large, -1) == 1,
            "fragmented maximum chunk");
    require(frame.size == DCC_TCP_CHUNK + 8U && !memcmp(frame.data, data, frame.size), "chunk contents");
    unsigned char *storage = frame.data;
    size_t capacity = frame.capacity;
    dcc_tcp_frame_next(&frame);
    require(frame.data == storage && frame.capacity == capacity && !frame.used && !frame.type,
            "bounded storage was not reused");
    require(!memcmp(storage, data, capacity), "consume invalidated a borrowed reply");
    for (unsigned i = 0; i < 30; ++i) {
        size_t size = i % 3 ? 37 : 0;
        require(receive(pair, &frame, DCC_TCP_FS_QUERY, data, size, 65536, -1) == 1, "small frame");
        require(frame.data == storage && frame.size == size && !memcmp(frame.data, data, size), "reuse contents");
        dcc_tcp_frame_next(&frame);
    }
    int original = open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(original >= 0, "rights source");
    require(receive(pair, &frame, DCC_TCP_BEGIN, data, 32, large, original) == 1, "rights frame");
    int borrowed = frame.fd_plus_one - 1;
    require(borrowed >= 0 && (fcntl(borrowed, F_GETFD) & FD_CLOEXEC), "rights ownership");
    dcc_tcp_frame_next(&frame);
    require(fcntl(borrowed, F_GETFD) < 0 && errno == EBADF, "consume leaked descriptor");
    require(fcntl(original, F_GETFD) >= 0, "consume closed sender descriptor");
    close(original);

    require(receive(pair, &frame, DCC_TCP_INPUT, data, 53, large, -1) == 1, "decoder frame");
    decoding = frame; frame = (struct dcc_tcp_frame){0};
    require(receive(pair, &frame, DCC_TCP_INPUT, data + 53, 83, large, -1) == 1, "prefetched frame");
    require(decoding.data != frame.data && !memcmp(decoding.data, data, 53), "prefetch overwrote decoder");
    dcc_tcp_frame_next(&decoding);
    struct dcc_tcp_frame spare = decoding; decoding = frame; frame = spare;
    require(frame.data == storage && !frame.used && !memcmp(decoding.data, data + 53, 83), "decoder swap");
    dcc_tcp_frame_next(&decoding);
    dcc_tcp_frame_clear(&decoding);

    require(receive(pair, &frame, DCC_TCP_BEGIN, data, large, large, -1) == 1, "large manifest");
    require(frame.capacity >= large, "manifest allocation");
    dcc_tcp_frame_next(&frame);
    require(!frame.data && !frame.capacity, "oversized metadata retained");
    require(receive(pair, &frame, DCC_TCP_FS_QUERY, data, 64, large, -1) == 1, "new storage");
    dcc_tcp_frame_next(&frame);
    require(receive(pair, &frame, DCC_TCP_FS_QUERY, data, 64, 32, -1) == -1, "reused storage bypassed limit");
    dcc_tcp_frame_clear(&frame);
    close(pair[0]); close(pair[1]); free(data);
    return 0;
}
