/* Real TCP fragments, read-ahead and borrowed payload lifetimes. */
#include "tcp_client.c"
#include <netinet/in.h>
#include <netinet/tcp.h>

static void require(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}
static void connect_pair(int *pair)
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    require(listener >= 0 && !bind(listener, (struct sockaddr *)&address, size) &&
            !getsockname(listener, (struct sockaddr *)&address, &size) && !listen(listener, 1), "listener");
    pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(pair[0] >= 0 && !connect(pair[0], (struct sockaddr *)&address, size), "connect");
    pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    require(pair[1] >= 0, "accept");
    int one = 1;
    require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), "nodelay");
    close(listener);
}
static void send_bytes(int fd, const void *bytes, size_t length)
{
    require(send(fd, bytes, length, MSG_NOSIGNAL) == (ssize_t)length, "send bytes");
}
static int receive_ready(int fd, struct tcp_input *input, struct dcc_tcp_frame *frame)
{
    int result;
    while (!(result = read_remote(fd, input, frame))) {
        struct pollfd p = {fd, POLLIN, 0};
        require(poll(&p, 1, 1000) > 0, "receive timeout");
    }
    return result;
}
int main(void)
{
    int pair[2]; connect_pair(pair);
    struct tcp_input input = {.wire = malloc(DCC_TCP_CHUNK + 16U)};
    struct dcc_tcp_frame frame = {0};
    require(input.wire != NULL, "buffer allocation");
    unsigned char wire[13*16];
    for (unsigned i = 0; i < 13; ++i) {
        dcc_tcp_put32(wire+i*16, DCC_TCP_PROGRESS);
        dcc_tcp_put32(wire+i*16+4, 8);
        dcc_tcp_put64(wire+i*16+8, i);
    }
    /* More than the exchange loop's per-turn quota, plus a partial next header. */
    send_bytes(pair[0], wire, sizeof(wire)-13);
    for (unsigned i = 0; i < 12; ++i) {
        require(receive_ready(pair[1], &input, &frame) == 1 && frame.size == 8 &&
                frame.type == DCC_TCP_PROGRESS && dcc_tcp_get64(frame.data) == i, "coalesced frame");
        consume_remote(&input, &frame);
    }
    require(read_remote(pair[1], &input, &frame) == 0, "partial header complete");
    require(input.end-input.begin == frame.used, "partial header falsely ready");
    send_bytes(pair[0], wire+sizeof(wire)-13, 7);
    require(read_remote(pair[1], &input, &frame) == 0, "partial body complete");
    require(input.end-input.begin == frame.used, "partial body falsely ready");
    send_bytes(pair[0], wire+sizeof(wire)-6, 6);
    require(receive_ready(pair[1], &input, &frame) == 1 && dcc_tcp_get64(frame.data) == 12, "fragmented frame");
    consume_remote(&input, &frame);

    size_t length = DCC_TCP_CHUNK + 16U;
    unsigned char *large = malloc(length);
    require(large != NULL, "large allocation");
    dcc_tcp_put32(large, DCC_TCP_OUTPUT); dcc_tcp_put32(large+4, length-8);
    for (size_t i = 8; i < length; ++i) large[i] = (unsigned char)(i*29+i/997);
    /* Leave the next header behind a consumed frame to exercise compaction. */
    memcpy(wire+16, large, 3);
    send_bytes(pair[0], wire, 19);
    require(receive_ready(pair[1], &input, &frame) == 1, "prefix frame");
    consume_remote(&input, &frame);
    require(read_remote(pair[1], &input, &frame) == 0, "large partial header");
    for (size_t offset = 3; offset < length;) {
        size_t amount = length-offset > 4093 ? 4093 : length-offset;
        send_bytes(pair[0], large+offset, amount); offset += amount;
        int complete = read_remote(pair[1], &input, &frame);
        require(complete >= 0 && (!complete || offset == length), "large fragmented frame");
    }
    require(receive_ready(pair[1], &input, &frame) == 1 && frame.size == length-8 &&
            !memcmp(frame.data, large+8, length-8), "large payload");
    consume_remote(&input, &frame);
    require(!input.begin && !input.end, "empty buffer not reset");
    free(large);

    /* EOF may follow buffered complete frames but cannot complete a short body. */
    send_bytes(pair[0], wire, 18);
    require(!shutdown(pair[0], SHUT_WR), "shutdown");
    require(receive_ready(pair[1], &input, &frame) == 1, "frame before EOF lost");
    consume_remote(&input, &frame);
    require(receive_ready(pair[1], &input, &frame) == -1, "truncated EOF accepted");
    close(pair[0]); close(pair[1]);
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        connect_pair(pair); input.begin = input.end = 0; frame = (struct dcc_tcp_frame){0};
        dcc_tcp_put32(wire, DCC_TCP_PROGRESS); dcc_tcp_put32(wire+4, 8);
        dcc_tcp_put32(wire+16, invalid == 0 ? 0 : invalid == 1 ? DCC_TCP_PROGRESS | DCC_TCP_FINAL : DCC_TCP_OUTPUT);
        dcc_tcp_put32(wire+20, invalid == 2 ? DCC_TCP_CHUNK + 9U : 0);
        send_bytes(pair[0], wire, 24);
        require(receive_ready(pair[1], &input, &frame) == 1, "valid prefix rejected");
        consume_remote(&input, &frame);
        require(receive_ready(pair[1], &input, &frame) == -1, "invalid following header accepted");
        close(pair[0]); close(pair[1]);
    }
    free(input.wire);
    return 0;
}
