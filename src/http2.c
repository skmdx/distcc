/* Bounded HTTP/2 to local-socket transport. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nghttp2/nghttp2.h>
#include "distcc.h"
#include "netutil.h"
#include "http2.h"
#include "trace.h"

#define STREAM_WINDOW (256 * 1024)
#define CONNECTION_WINDOW (8 * 1024 * 1024)
#define MAX_STREAMS 256
#define OUTPUT_SIZE (64 * 1024)

struct stream {
    struct stream *next;
    int32_t id;
    int fd, poll_index, ended, closed, eof, headers, link;
    int *status;
    size_t head, size;
    unsigned char *input;
};

struct dcc_h2 {
    int fd, server, failed, connecting;
    nghttp2_session *session;
    struct stream *streams;
    dcc_h2_accept_fn accept_job;
    void *context;
    size_t out_head, out_size;
    unsigned char output[OUTPUT_SIZE];
    /* mem_send's slice remains valid until the next mem_send call. */
    const uint8_t *pending;
    size_t pending_size;
};

#define NV(name, value) {(uint8_t *)(name), (uint8_t *)(value), sizeof(name)-1, sizeof(value)-1, NGHTTP2_NV_FLAG_NONE}

static struct stream *find_stream(struct dcc_h2 *c, int32_t id)
{
    struct stream *s;
    for (s = c->streams; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

static struct stream *new_stream(struct dcc_h2 *c, int32_t id)
{
    struct stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->id = id;
    s->fd = s->poll_index = -1;
    s->next = c->streams;
    c->streams = s;
    return s;
}

static int attach_socket(struct stream *s, int *other)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) return -1;
    s->input = malloc(STREAM_WINDOW);
    if (!s->input) { close(pair[0]); close(pair[1]); return -1; }
    s->fd = pair[0];
    dcc_set_nonblocking(s->fd);
    *other = pair[1];
    return 0;
}

static ssize_t read_data(nghttp2_session *session, int32_t id, uint8_t *buf,
                         size_t length, uint32_t *flags,
                         nghttp2_data_source *source, void *context)
{
    struct stream *s = source->ptr;
    ssize_t n;
    (void)session; (void)id; (void)context;
    if (s->fd < 0) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    do { n = read(s->fd, buf, length); } while (n < 0 && errno == EINTR);
    if (n > 0) return n;
    if (!n) { s->eof = 1; *flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return NGHTTP2_ERR_DEFERRED;
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
}

static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame,
                          void *context)
{
    struct dcc_h2 *c = context;
    if (c->server && frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        struct stream *s = new_stream(c, frame->hd.stream_id);
        if (!s) return NGHTTP2_ERR_CALLBACK_FAILURE;
        nghttp2_session_set_stream_user_data(session, s->id, s);
    }
    return 0;
}

static int header(nghttp2_session *session, const nghttp2_frame *frame,
                   const uint8_t *name, size_t namelen, const uint8_t *value,
                   size_t valuelen, uint8_t flags, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    (void)flags;
    if (!s) return 0;
    if (c->server) {
        if (namelen == 7 && !memcmp(name, ":method", 7) &&
            valuelen == 4 && !memcmp(value, "POST", 4)) s->headers |= 1;
        if (namelen == 5 && !memcmp(name, ":path", 5)) {
            if (valuelen == 11 && !memcmp(value, "/v1/compile", 11)) s->headers |= 2;
            if (valuelen == 8 && !memcmp(value, "/v1/link", 8)) { s->headers |= 2; s->link = 1; }
        }
    } else if (s->status && namelen == 7 && !memcmp(name, ":status", 7)) {
        if (valuelen == 3 && !memcmp(value, "200", 3)) *s->status = 200;
        else if (valuelen == 3 && !memcmp(value, "503", 3)) *s->status = 503;
        else *s->status = -1;
    }
    return 0;
}

static int receive_frame(nghttp2_session *session, const nghttp2_frame *frame,
                          void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!s) return 0;
    if (c->server && frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        int fd = -1, accepted = s->headers == 3 && !attach_socket(s, &fd) &&
                                  !c->accept_job(c->context, fd, s->link);
        nghttp2_nv ok[] = { NV(":status", "200"), NV("content-type", "application/x-distcc") };
        nghttp2_nv busy[] = { NV(":status", "503") };
        nghttp2_data_provider provider = { .source.ptr = s, .read_callback = read_data };
        if (!accepted && fd >= 0) close(fd);
        if (!accepted && s->fd >= 0) { close(s->fd); s->fd = -1; }
        if (!accepted) { free(s->input); s->input = NULL; }
        if (nghttp2_submit_response(session, s->id, accepted ? ok : busy,
                                    accepted ? 2 : 1, accepted ? &provider : NULL))
            return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) s->ended = 1;
    return 0;
}

static int receive_data(nghttp2_session *session, uint8_t flags, int32_t id,
                         const uint8_t *data, size_t length, void *context)
{
    struct stream *s = nghttp2_session_get_stream_user_data(session, id);
    size_t tail, first;
    (void)flags; (void)context;
    if (!s || s->fd < 0) {
        nghttp2_session_consume_connection(session, length);
        return 0;
    }
    if (length > STREAM_WINDOW - s->size) return NGHTTP2_ERR_CALLBACK_FAILURE;
    tail = (s->head + s->size) % STREAM_WINDOW;
    first = length < STREAM_WINDOW - tail ? length : STREAM_WINDOW - tail;
    memcpy(s->input + tail, data, first);
    memcpy(s->input, data + first, length - first);
    s->size += length;
    return 0;
}

static int close_stream(nghttp2_session *session, int32_t id, uint32_t error,
                         void *context)
{
    struct stream *s = nghttp2_session_get_stream_user_data(session, id);
    (void)context;
    if (!s) return 0;
    s->closed = 1;
    if (s->status && !*s->status) *s->status = -1;
    if (error) {
        if (s->fd >= 0) close(s->fd);
        s->fd = -1;
        nghttp2_session_consume_connection(session, s->size);
        s->size = 0;
    }
    return 0;
}

static int sent_frame(nghttp2_session *session, const nghttp2_frame *frame,
                       void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    /* A refused or malformed request need not finish uploading. Send the
     * complete response first, then release its remaining request half. */
    if (c->server && s && !s->ended &&
        (frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        return nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, s->id, NGHTTP2_NO_ERROR);
    return 0;
}

struct dcc_h2 *dcc_h2_new(int fd, dcc_h2_accept_fn accept_job, void *context)
{
    struct dcc_h2 *c = calloc(1, sizeof(*c));
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *option = NULL;
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, MAX_STREAMS },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, STREAM_WINDOW },
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 }
    };
    int ret, one = 1;
    if (!c) return NULL;
    c->fd = fd;
    c->server = accept_job != NULL;
    c->accept_job = accept_job;
    c->context = context;
    c->connecting = !c->server;
    if (nghttp2_session_callbacks_new(&callbacks) || nghttp2_option_new(&option)) goto failed;
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, receive_frame);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, receive_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, close_stream);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, sent_frame);
    nghttp2_option_set_no_auto_window_update(option, 1);
    ret = c->server ? nghttp2_session_server_new2(&c->session, callbacks, c, option)
                    : nghttp2_session_client_new2(&c->session, callbacks, c, option);
    if (ret || nghttp2_submit_settings(c->session, NGHTTP2_FLAG_NONE, settings,
                                        sizeof(settings) / sizeof(settings[0])) ||
        nghttp2_session_set_local_window_size(c->session, NGHTTP2_FLAG_NONE, 0, CONNECTION_WINDOW))
        goto failed;
    nghttp2_option_del(option);
    nghttp2_session_callbacks_del(callbacks);
    dcc_set_nonblocking(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return c;
failed:
    nghttp2_option_del(option);
    nghttp2_session_callbacks_del(callbacks);
    if (c->session) nghttp2_session_del(c->session);
    free(c);
    return NULL;
}

void dcc_h2_free(struct dcc_h2 *c)
{
    struct stream *s, *next;
    if (!c) return;
    for (s = c->streams; s; s = next) {
        next = s->next;
        if (s->status && !*s->status) *s->status = -1;
        if (s->fd >= 0) close(s->fd);
        free(s->input);
        free(s);
    }
    nghttp2_session_del(c->session);
    close(c->fd);
    free(c);
}

int dcc_h2_open(struct dcc_h2 *c, int link, int *fd, int *status)
{
    struct stream *s;
    nghttp2_nv headers[] = { NV(":method", "POST"), NV(":scheme", "http"),
        NV(":authority", "distccd"), NV(":path", "/v1/compile"),
        NV("content-type", "application/x-distcc") };
    nghttp2_data_provider provider = { .read_callback = read_data };
    if (c->failed || !nghttp2_session_check_request_allowed(c->session)) return -1;
    s = new_stream(c, 0);
    if (!s) return -1;
    if (attach_socket(s, fd)) { s->closed = 1; return -1; }
    if (link) { headers[3].value = (uint8_t *)"/v1/link"; headers[3].valuelen = 8; }
    provider.source.ptr = s;
    s->status = status;
    *status = 0;
    s->id = nghttp2_submit_request(c->session, NULL, headers, 5, &provider, s);
    if (s->id < 0) { close(*fd); *fd = -1; s->closed = 1; s->status = NULL; return -1; }
    return s->id;
}

void dcc_h2_cancel(struct dcc_h2 *c, int stream)
{
    struct stream *s = c ? find_stream(c, stream) : NULL;
    if (!s) return;
    s->status = NULL;
    if (!s->closed) nghttp2_submit_rst_stream(c->session, NGHTTP2_FLAG_NONE, stream, NGHTTP2_CANCEL);
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    nghttp2_session_consume_connection(c->session, s->size);
    s->size = 0;
}

/* Batch frames into a bounded write buffer. Neither a blocked receiver nor a
 * large result may make nghttp2 retain an unbounded application output queue. */
static int fill_output(struct dcc_h2 *c)
{
    if (c->out_size) return 0;
    c->out_head = 0;
    while (c->out_size < OUTPUT_SIZE) {
        size_t n;
        if (!c->pending_size) {
            ssize_t size = nghttp2_session_mem_send(c->session, &c->pending);
            if (size < 0) return -1;
            if (!size) break;
            c->pending_size = size;
        }
        n = c->pending_size < OUTPUT_SIZE - c->out_size ? c->pending_size : OUTPUT_SIZE - c->out_size;
        memcpy(c->output + c->out_size, c->pending, n);
        c->out_size += n;
        c->pending += n;
        c->pending_size -= n;
    }
    return 0;
}

static void reap_streams(struct dcc_h2 *c)
{
    struct stream **p = &c->streams;
    while (*p) {
        struct stream *s = *p;
        if (s->closed && !s->size) {
            *p = s->next;
            if (s->fd >= 0) close(s->fd);
            free(s->input);
            free(s);
        } else p = &s->next;
    }
}

int dcc_h2_prepare(struct dcc_h2 *c, struct pollfd *polls, int capacity)
{
    struct stream *s;
    int n = 1;
    if (capacity < 1) return -1;
    if (fill_output(c)) c->failed = 1;
    reap_streams(c);
    polls[0] = (struct pollfd){ c->fd, POLLIN | ((c->connecting || c->out_size) ? POLLOUT : 0), 0 };
    for (s = c->streams; s; s = s->next) {
        s->poll_index = -1;
        if (s->fd < 0) continue;
        if (n == capacity) return -1;
        s->poll_index = n;
        /* A readable local producer is not actionable while HTTP/2 or TCP
         * has no send capacity. Polling it then would spin until a peer
         * WINDOW_UPDATE or socket write wakes the connection. */
        int can_read = !s->eof && !s->closed && !c->out_size && !c->pending_size &&
            nghttp2_session_get_remote_window_size(c->session) > 0 &&
            nghttp2_session_get_stream_remote_window_size(c->session, s->id) > 0;
        short events = (can_read ? POLLIN : 0) | (s->size ? POLLOUT : 0);
        polls[n++] = (struct pollfd){ events ? s->fd : -1, events, 0 };
    }
    return n;
}

int dcc_h2_process(struct dcc_h2 *c, struct pollfd *polls)
{
    struct stream *s;
    unsigned char buffer[OUTPUT_SIZE];
    ssize_t n;
    if (c->failed) return -1;
    if (c->connecting && polls[0].revents) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) return -1;
        c->connecting = 0;
    }
    if (polls[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        n = recv(c->fd, buffer, sizeof(buffer), 0);
        if (!n || (n < 0 && errno != EAGAIN && errno != EINTR)) return -1;
        if (n > 0 && nghttp2_session_mem_recv(c->session, buffer, n) != n) return -1;
    }
    for (s = c->streams; s; s = s->next) {
        short events = s->poll_index < 0 ? 0 : polls[s->poll_index].revents;
        if (s->fd < 0) continue;
        if (s->size) {
            size_t size = s->size < STREAM_WINDOW - s->head ? s->size : STREAM_WINDOW - s->head;
            n = send(s->fd, s->input + s->head, size, MSG_NOSIGNAL);
            if (n > 0) {
                s->head = (s->head + n) % STREAM_WINDOW;
                s->size -= n;
                if (s->closed) nghttp2_session_consume_connection(c->session, n);
                else nghttp2_session_consume(c->session, s->id, n);
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                dcc_h2_cancel(c, s->id);
                continue;
            }
        }
        /* A completed request is not a disconnected client. Keep the worker
         * socket writable while it monitors cancellation during compilation. */
        if (!c->server && s->ended && !s->size) shutdown(s->fd, SHUT_WR);
        if (events & (POLLIN | POLLHUP | POLLERR))
            nghttp2_session_resume_data(c->session, s->id);
    }
    if (fill_output(c)) return -1;
    if (c->out_size && !c->connecting) {
        n = send(c->fd, c->output + c->out_head, c->out_size, MSG_NOSIGNAL);
        if (n > 0) { c->out_head += n; c->out_size -= n; }
        else if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
    }
    reap_streams(c);
    return 0;
}

int dcc_h2_idle(struct dcc_h2 *c)
{
    return !c->streams && !c->out_size && !c->pending_size;
}

int dcc_h2_active(struct dcc_h2 *c)
{
    struct stream *s;
    int count = 0;
    if (c) for (s = c->streams; s; s = s->next) count += s->fd >= 0;
    return count;
}
