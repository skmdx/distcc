/* Exercise the production FD transport over real TCP, with short writes.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include "http2.h"
#include "distcc.h"
#include "job.h"

const char *rs_program_name = "h_http2";

#define COUNT 12
#define BYTES (1024 * 1024 + 7919)
struct transfer { int fd, number, ended, closed, error; uint64_t size; };
static struct transfer received[COUNT], sent[COUNT];
static uint64_t receive_limit = UINT64_MAX;
static unsigned char pattern(unsigned i) { return (i * 31 + i / 271) & 255; }
static int temporary(void)
{
    int fd = memfd_create("h2-body", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(fd >= 0);
    return fd;
}
static int headers(struct dcc_h2 *c, int id, const struct dcc_h2_field *fields,
                    size_t count, void *context)
{
    struct transfer *t = context;
    const char *path = dcc_h2_field(fields, count, ":path");
    if (path) {
        int number = -1;
        assert(sscanf(path, "/echo/%d", &number) == 1 && number >= 0 && number < COUNT);
        t = &received[number];
        t->number = number;
        t->size = sent[number].size;
        dcc_h2_set_context(c, id, t);
    } else assert(!strcmp(dcc_h2_field(fields, count, ":status"), "200"));
    t->fd = temporary();
    assert(!dcc_h2_sink(c, id, t->fd, path && receive_limit < t->size ? receive_limit : t->size));
    return 0;
}
static int end(struct dcc_h2 *c, int id, void *context)
{
    struct transfer *t = context;
    unsigned char data[7919];
    uint64_t offset = 0;
    while (offset < t->size) {
        size_t amount = t->size - offset < sizeof(data) ? t->size - offset : sizeof(data), i;
        assert(pread(t->fd, data, amount, offset) == (ssize_t)amount);
        for (i = 0; i < amount; ++i) assert(data[i] == pattern(offset + i));
        offset += amount;
    }
    t->ended = 1;
    if (t == &received[t->number]) {
        struct dcc_h2_body body = { .fd = t->fd, .length = t->size };
        assert(!dcc_h2_response(c, id, 200, NULL, 0, &body));
        /* The source survives immediate caller close and pathname removal. */
        close(t->fd);
        t->fd = -1;
    }
    return 0;
}
static void closed(struct dcc_h2 *c, int id, unsigned error, void *context)
{
    struct transfer *t = context;
    (void)c; (void)id;
    if (!t) return;
    assert(!t->closed);
    t->closed = 1;
    t->error = error;
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
}
static void connect_pair(int fds[2])
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t length = sizeof(addr);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), small = 1024;
    assert(listener >= 0 && !bind(listener, (struct sockaddr *)&addr, sizeof(addr)));
    assert(!getsockname(listener, (struct sockaddr *)&addr, &length) && !listen(listener, 1));
    fds[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fds[0] >= 0 && !connect(fds[0], (struct sockaddr *)&addr, length));
    fds[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    assert(fds[1] >= 0);
    close(listener);
    assert(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
    assert(!setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
}
static void failure_case(int kind)
{
    const struct dcc_h2_events events = { headers, end, closed };
    int sockets[2], source = temporary(), id, failed = 0;
    struct dcc_h2 *client, *server;
    struct pollfd polls[2];
    unsigned char data[7919];
    size_t offset = 0;
    while (offset < BYTES) {
        size_t amount = BYTES - offset < sizeof(data) ? BYTES - offset : sizeof(data), i;
        for (i = 0; i < amount; ++i) data[i] = pattern(offset + i);
        assert(write(source, data, amount) == (ssize_t)amount);
        offset += amount;
    }
    sent[0] = (struct transfer){.fd = -1, .size = BYTES};
    received[0] = (struct transfer){.fd = -1};
    receive_limit = kind == 0 ? 4 : UINT64_MAX;
    connect_pair(sockets);
    client = dcc_h2_new(sockets[0], 0, &events, NULL);
    server = dcc_h2_new(sockets[1], 1, &events, NULL);
    assert(client && server);
    struct dcc_h2_body body = {.fd = source, .length = BYTES};
    id = dcc_h2_request(client, "POST", "/echo/0", NULL, 0, &body, &sent[0]);
    assert(id > 0);
    if (kind == 1) assert(!ftruncate(source, 0));
    if (kind == 2) {
        /* Send until the small TCP buffer/window stalls inside the upload. */
        polls[0] = (struct pollfd){sockets[0], POLLOUT, POLLOUT};
        assert(!dcc_h2_process(client, polls));
        assert(!sent[0].closed);
        dcc_h2_cancel(client, id);
    }
    if (kind == 3) {
        dcc_h2_free(server);
        server = NULL;
    }
    while (!sent[0].closed && !failed) {
        assert(dcc_h2_prepare(client, &polls[0], 1) == 1);
        if (server) assert(dcc_h2_prepare(server, &polls[1], 1) == 1);
        assert(poll(polls, server ? 2 : 1, 1000) >= 0);
        failed = dcc_h2_process(client, &polls[0]);
        if (!failed && server) failed = dcc_h2_process(server, &polls[1]);
    }
    if (kind == 2) assert(!failed); /* Local cancellation preserves framing. */
    dcc_h2_free(client);
    dcc_h2_free(server);
    assert(sent[0].closed && sent[0].error && !sent[0].ended);
    close(source);
    receive_limit = UINT64_MAX;
}
static void compression(void)
{
    unsigned char input[65536];
    struct dcc_buffer result = {0};
    uint64_t wire;
    uint32_t random = 1701;
    int fd;
    for (int kind = 0; kind < 2; ++kind) {
        for (unsigned i = 0; i < sizeof(input); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            input[i] = kind ? random : 42;
        }
        fd = temporary();
        assert(write(fd, input, sizeof(input)) == sizeof(input));
        assert(dcc_job_compress(&fd, sizeof(input), &wire) == (kind ? DCC_JOB_RAW : DCC_JOB_LZ4));
        if (!kind) {
            assert(wire < sizeof(input));
            assert(!dcc_job_expand(fd, wire, sizeof(input), -1, &result));
            assert(result.size == sizeof(input) && !memcmp(result.data, input, sizeof(input)));
            dcc_free_buffer(&result);
            assert(dcc_job_expand(fd, wire, sizeof(input) - 1, -1, &result));
            assert(dcc_job_expand(fd, wire, sizeof(input) + 1, -1, &result));
            assert(dcc_job_expand(fd, wire - 1, sizeof(input), -1, &result));
            assert(dcc_job_expand(fd, wire + 1, sizeof(input), -1, &result));
            /* A real truncated representation, not merely wrong metadata. */
            int truncated = temporary();
            unsigned char bytes[1024];
            assert(wire < sizeof(bytes));
            assert(pread(fd, bytes, wire, 0) == (ssize_t)wire);
            assert(write(truncated, bytes, wire - 1) == (ssize_t)wire - 1);
            assert(dcc_job_expand(truncated, wire - 1, sizeof(input), -1, &result));
            close(truncated);
        } else {
            unsigned char raw[sizeof(input)];
            assert(wire == sizeof(input));
            assert(pread(fd, raw, wire, 0) == (ssize_t)wire && !memcmp(raw, input, sizeof(input)));
        }
        close(fd);
    }
}
int main(void)
{
    const struct dcc_h2_events events = { headers, end, closed };
    struct dcc_h2 *client, *server;
    int sockets[2], i, source = temporary();
    unsigned char data[BYTES];
    struct pollfd polls[2];
    signal(SIGPIPE, SIG_IGN);
    alarm(30);
    compression();
    for (i = 0; i < BYTES; ++i) data[i] = pattern(i);
    assert(write(source, data, sizeof(data)) == sizeof(data));
    assert(!fcntl(source, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
    connect_pair(sockets);
    client = dcc_h2_new(sockets[0], 0, &events, NULL);
    server = dcc_h2_new(sockets[1], 1, &events, NULL);
    assert(client && server);
    for (i = 0; i < COUNT; ++i) {
        char path[40];
        struct dcc_h2_body body = { .fd = source, .length = i == 0 ? 0 : BYTES };
        if (i == 1) { body.fd = -1; body.data = data; body.length = 113; }
        sent[i] = (struct transfer){ .number = i, .fd = -1, .size = body.length };
        received[i].fd = -1;
        snprintf(path, sizeof(path), "/echo/%d", i);
        assert(dcc_h2_request(client, "POST", path, NULL, 0, &body, &sent[i]) > 0);
    }
    close(source);
    while (!dcc_h2_idle(client) || !dcc_h2_idle(server)) {
        assert(dcc_h2_prepare(client, &polls[0], 1) == 1);
        assert(dcc_h2_prepare(server, &polls[1], 1) == 1);
        assert(poll(polls, 2, 1000) >= 0);
        assert(!dcc_h2_process(client, &polls[0]));
        assert(!dcc_h2_process(server, &polls[1]));
    }
    for (i = 0; i < COUNT; ++i) {
        assert(sent[i].ended && sent[i].closed && !sent[i].error);
        assert(received[i].ended && received[i].closed && !received[i].error);
    }
    dcc_h2_free(client);
    dcc_h2_free(server);
    for (i = 0; i < 4; ++i) failure_case(i);
    puts("HTTP/2: multiplexed file/memory bodies, short writes, FD lifetime, limits, truncation, cancellation, peer loss: OK");
    return 0;
}
