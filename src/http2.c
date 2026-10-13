/* Direct HTTP/2 file and buffer transport. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nghttp2/nghttp2.h>
#include "distcc.h"
#include "netutil.h"
#include "http2.h"

#define STREAM_WINDOW (256 * 1024)
#define CONNECTION_WINDOW (8 * 1024 * 1024)
#define FRAME_SIZE (64 * 1024)
#define IO_BUDGET (16 * FRAME_SIZE)
#define MAX_FIELDS 32

struct stream {
    struct stream *next;
    int32_t id;
    int source, sink, closed, responded, ended, cancelled;
    uint64_t offset, remaining, received, limit, expected;
    int has_length, headers_done;
    unsigned error;
    time_t activity;
    unsigned char *memory;
    size_t memory_offset, fields_count, fields_size;
    struct dcc_h2_field fields[MAX_FIELDS];
    void *context;
};
struct dcc_h2 {
    int fd, server, failed, connecting, count;
    nghttp2_session *session;
    struct stream *streams;
    struct dcc_h2_events events;
    void *context;
    size_t budget;
    /* Retry the exact partial DATA frame after WOULD_BLOCK. No other frame
     * may interleave, and its source stays pinned until the frame completes. */
    struct stream *sending;
    size_t header_sent, body_sent, body_length;
};

static struct stream *find_stream(struct dcc_h2 *c, int id)
{
    struct stream *s;
    for (s = c->streams; s; s = s->next) if (s->id == id) return s;
    return NULL;
}
static void free_stream(struct stream *s)
{
    size_t i;
    if (s->source >= 0) close(s->source);
    if (s->sink >= 0) close(s->sink);
    for (i = 0; i < s->fields_count; ++i) {
        free((void *)s->fields[i].name);
        free((void *)s->fields[i].value);
    }
    free(s->memory);
    free(s);
}
static struct stream *new_stream(struct dcc_h2 *c, int id, void *context)
{
    struct stream *s;
    if (c->count >= DCC_H2_MAX_STREAMS || !(s = calloc(1, sizeof(*s)))) return NULL;
    s->id = id;
    s->activity = time(NULL);
    s->source = s->sink = -1;
    s->context = context;
    s->next = c->streams;
    c->streams = s;
    ++c->count;
    return s;
}
static int source_body(struct stream *s, const struct dcc_h2_body *body)
{
    struct stat st;
    if (!body || !body->length) return 0;
    if (body->length > INT64_MAX || body->offset > INT64_MAX - body->length) return -1;
    if (body->fd >= 0) {
        if (fstat(body->fd, &st) || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size < body->offset + body->length) return -1;
        s->source = fcntl(body->fd, F_DUPFD_CLOEXEC, 0);
        if (s->source < 0) return -1;
        s->offset = body->offset;
    } else {
        if (!body->data || body->offset || body->length > DCC_H2_HEADER_LIMIT) return -1;
        if (!(s->memory = malloc(body->length))) return -1;
        memcpy(s->memory, body->data, body->length);
    }
    s->remaining = body->length;
    return 0;
}
static ssize_t read_data(nghttp2_session *session, int32_t id, uint8_t *buf,
                         size_t length, uint32_t *flags,
                         nghttp2_data_source *source, void *context)
{
    struct stream *s = source->ptr;
    (void)session; (void)id; (void)buf; (void)context;
    if (length > s->remaining) length = s->remaining;
    *flags |= NGHTTP2_DATA_FLAG_NO_COPY;
    if (length == s->remaining) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return length;
}
static ssize_t read_length(nghttp2_session *session, uint8_t type, int32_t id,
                           int32_t connection_window, int32_t stream_window,
                           uint32_t maximum, void *context)
{
    /* nghttp2 clamps this to both flow-control windows and the peer limit. */
    (void)session; (void)type; (void)id; (void)connection_window;
    (void)stream_window; (void)maximum; (void)context;
    return FRAME_SIZE;
}
static ssize_t send_bytes(nghttp2_session *session, const uint8_t *data,
                          size_t length, int flags, void *context)
{
    struct dcc_h2 *c = context;
    ssize_t n;
    (void)session;
    if (!c->budget) return NGHTTP2_ERR_WOULDBLOCK;
    if (length > c->budget) length = c->budget;
    do { n = send(c->fd, data, length, MSG_NOSIGNAL | flags); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NGHTTP2_ERR_WOULDBLOCK;
    if (n <= 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    c->budget -= n;
    return n;
}
static int send_data(nghttp2_session *session, nghttp2_frame *frame,
                     const uint8_t *header, size_t length,
                     nghttp2_data_source *source, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = source->ptr;
    ssize_t n;
    if (frame->data.padlen) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (!c->sending) {
        c->sending = s;
        c->header_sent = c->body_sent = 0;
        c->body_length = length;
    }
    if (c->sending != s || c->body_length != length) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (c->header_sent < 9) {
        n = send_bytes(session, header + c->header_sent, 9 - c->header_sent, MSG_MORE, c);
        if (n < 0) return n;
        c->header_sent += n;
        if (c->header_sent != 9) return NGHTTP2_ERR_WOULDBLOCK;
    }
    while (c->body_sent < length) {
        size_t amount = length - c->body_sent;
        if (!c->budget) return NGHTTP2_ERR_WOULDBLOCK;
        if (amount > c->budget) amount = c->budget;
        if (s->source >= 0) {
            off_t offset = s->offset + c->body_sent;
            do { n = sendfile(c->fd, s->source, &offset, amount); } while (n < 0 && errno == EINTR);
        } else {
            do { n = send(c->fd, s->memory + s->memory_offset + c->body_sent,
                           amount, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NGHTTP2_ERR_WOULDBLOCK;
        /* Source truncation after the header requires connection failure:
         * emitting RST_STREAM inside this frame would corrupt the wire. */
        if (n <= 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
        c->body_sent += n;
        s->activity = time(NULL);
        c->budget -= n;
    }
    s->offset += length;
    s->memory_offset += length;
    s->remaining -= length;
    c->sending = NULL;
    return 0;
}
static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = find_stream(c, frame->hd.stream_id);
    if (c->server && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        s = new_stream(c, frame->hd.stream_id, c->context);
        if (!s) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_REFUSED_STREAM);
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        nghttp2_session_set_stream_user_data(session, s->id, s);
    }
    /* Result metadata is a representation of its own, not late trailers. */
    return s && s->headers_done ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE : 0;
}
static int header_field(nghttp2_session *session, const nghttp2_frame *frame,
                        const uint8_t *name, size_t namelen, const uint8_t *value,
                        size_t valuelen, uint8_t flags, void *context)
{
    struct stream *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    char *n, *v;
    (void)context; (void)flags;
    if (!s) return 0;
    if (s->fields_count == MAX_FIELDS || namelen + valuelen > DCC_H2_HEADER_LIMIT - s->fields_size ||
        memchr(name, 0, namelen) || memchr(value, 0, valuelen)) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    if (!(n = strndup((const char *)name, namelen))) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (!(v = strndup((const char *)value, valuelen))) { free(n); return NGHTTP2_ERR_CALLBACK_FAILURE; }
    s->fields[s->fields_count++] = (struct dcc_h2_field){ n, v };
    s->fields_size += namelen + valuelen;
    if (!strcmp(n, "content-length")) {
        uint64_t size = 0;
        size_t i;
        if (s->has_length || !valuelen) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        for (i = 0; i < valuelen; ++i) {
            if (value[i] < '0' || value[i] > '9' || size > ((uint64_t)INT64_MAX - (value[i] - '0')) / 10)
                return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            size = size * 10 + value[i] - '0';
        }
        s->expected = size;
        s->has_length = 1;
    }
    return 0;
}
static int received_frame(nghttp2_session *session, const nghttp2_frame *frame, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!s) return 0;
    if (frame->hd.type == NGHTTP2_HEADERS) {
        s->activity = time(NULL);
        s->headers_done = 1;
        if (c->events.headers && c->events.headers(c, s->id, s->fields, s->fields_count, s->context)) {
            dcc_h2_cancel(c, s->id);
            return 0;
        }
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if ((s->has_length && s->received != s->expected) || s->cancelled) dcc_h2_cancel(c, s->id);
        else {
            s->ended = 1;
            if (c->events.end && c->events.end(c, s->id, s->context)) dcc_h2_cancel(c, s->id);
        }
    }
    return 0;
}
static int receive_data(nghttp2_session *session, uint8_t flags, int32_t id,
                         const uint8_t *data, size_t length, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, id);
    size_t done = 0;
    (void)flags;
    if (!s || s->cancelled) {
        nghttp2_session_consume_connection(session, length);
        return 0;
    }
    if (s->sink < 0 || length > s->limit - s->received ||
        (s->has_length && length > s->expected - s->received)) goto failed;
    while (done < length) {
        ssize_t n = pwrite(s->sink, data + done, length - done, s->received + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        done += n;
    }
    s->received += length;
    s->activity = time(NULL);
    return nghttp2_session_consume(session, id, length);
failed:
    nghttp2_session_consume_connection(session, length);
    dcc_h2_cancel(c, id);
    return 0;
}
static int closed_stream(nghttp2_session *session, int32_t id, uint32_t error, void *context)
{
    struct dcc_h2 *c = context;
    struct stream *s = nghttp2_session_get_stream_user_data(session, id);
    if (s) {
        s->closed = 1;
        s->error = error ? error : (!s->ended || s->cancelled ? NGHTTP2_CANCEL : 0);
        if (c->sending == s) c->failed = 1;
    }
    return 0;
}
static void reap_streams(struct dcc_h2 *c)
{
    struct stream **p = &c->streams;
    while (*p) {
        struct stream *s = *p;
        if (s->cancelled && !s->closed && c->sending != s)
            nghttp2_submit_rst_stream(c->session, NGHTTP2_FLAG_NONE, s->id, NGHTTP2_CANCEL);
        if (s->closed && c->sending != s) {
            *p = s->next;
            --c->count;
            if (c->events.closed) c->events.closed(c, s->id, s->error, s->context);
            free_stream(s);
            p = &c->streams;
        } else p = &s->next;
    }
}
struct dcc_h2 *dcc_h2_new(int fd, int server, const struct dcc_h2_events *events, void *context)
{
    struct dcc_h2 *c = calloc(1, sizeof(*c));
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *option = NULL;
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, DCC_H2_MAX_STREAMS },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, STREAM_WINDOW },
        { NGHTTP2_SETTINGS_MAX_FRAME_SIZE, FRAME_SIZE },
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 }
    };
    int ret, one = 1;
    if (!c) return NULL;
    c->fd = fd;
    c->server = server;
    c->events = *events;
    c->context = context;
    c->connecting = !server;
    if (nghttp2_session_callbacks_new(&callbacks) || nghttp2_option_new(&option)) goto failed;
    nghttp2_session_callbacks_set_send_callback(callbacks, send_bytes);
    nghttp2_session_callbacks_set_send_data_callback(callbacks, send_data);
    nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, read_length);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, header_field);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, received_frame);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, receive_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closed_stream);
    nghttp2_option_set_no_auto_window_update(option, 1);
    ret = server ? nghttp2_session_server_new2(&c->session, callbacks, c, option)
                 : nghttp2_session_client_new2(&c->session, callbacks, c, option);
    if (ret || nghttp2_submit_settings(c->session, NGHTTP2_FLAG_NONE, settings,
                                      sizeof(settings) / sizeof(settings[0])) ||
        nghttp2_session_set_local_window_size(c->session, NGHTTP2_FLAG_NONE, 0, CONNECTION_WINDOW)) goto failed;
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
    struct stream *s;
    if (!c) return;
    c->failed = 1;
    while ((s = c->streams)) {
        c->streams = s->next;
        --c->count;
        if (c->events.closed)
            c->events.closed(c, s->id, s->closed ? s->error : NGHTTP2_INTERNAL_ERROR, s->context);
        free_stream(s);
    }
    nghttp2_session_del(c->session);
    close(c->fd);
    free(c);
}
static void fields_nv(nghttp2_nv *out, const struct dcc_h2_field *fields, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        out[i] = (nghttp2_nv){ (uint8_t *)fields[i].name, (uint8_t *)fields[i].value,
            strlen(fields[i].name), strlen(fields[i].value), NGHTTP2_NV_FLAG_NONE };
}
int dcc_h2_request(struct dcc_h2 *c, const char *method, const char *path,
                   const struct dcc_h2_field *fields, size_t count,
                   const struct dcc_h2_body *body, void *context)
{
    struct stream *s;
    nghttp2_nv headers[MAX_FIELDS];
    char length[32];
    struct dcc_h2_field initial[] = { {":method", method}, {":scheme", "http"},
        {":authority", "distccd"}, {":path", path}, {"content-length", length} };
    nghttp2_data_provider provider = { .read_callback = read_data };
    if (count > MAX_FIELDS - 5 || c->server || c->failed ||
        !nghttp2_session_check_request_allowed(c->session)) return -1;
    if (!(s = new_stream(c, 0, context))) return -1;
    if (source_body(s, body)) goto failed;
    snprintf(length, sizeof(length), "%llu", (unsigned long long)s->remaining);
    fields_nv(headers, initial, 5);
    fields_nv(headers + 5, fields, count);
    provider.source.ptr = s;
    s->id = nghttp2_submit_request(c->session, NULL, headers, count + 5,
                                   s->remaining ? &provider : NULL, s);
    if (s->id < 0) goto failed;
    return s->id;
failed:
    c->streams = s->next;
    --c->count;
    free_stream(s);
    return -1;
}
int dcc_h2_response(struct dcc_h2 *c, int id, int status,
                    const struct dcc_h2_field *fields, size_t count, const struct dcc_h2_body *body)
{
    struct stream *s = find_stream(c, id);
    nghttp2_nv headers[MAX_FIELDS];
    char code[4], length[32];
    struct dcc_h2_field initial[] = { {":status", code}, {"content-length", length} };
    nghttp2_data_provider provider = { .read_callback = read_data };
    if (!c->server || !s || s->closed || s->responded || count > MAX_FIELDS - 2 ||
        status < 200 || status > 599 || source_body(s, body)) return -1;
    snprintf(code, sizeof(code), "%d", status);
    snprintf(length, sizeof(length), "%llu", (unsigned long long)s->remaining);
    fields_nv(headers, initial, 2);
    fields_nv(headers + 2, fields, count);
    provider.source.ptr = s;
    if (status == 204) {
        if (s->remaining) return -1;
        fields_nv(headers + 1, fields, count);
    }
    if (nghttp2_submit_response(c->session, id, headers, count + (status == 204 ? 1 : 2),
                                 s->remaining ? &provider : NULL)) return -1;
    s->responded = 1;
    return 0;
}
int dcc_h2_sink(struct dcc_h2 *c, int id, int fd, uint64_t limit)
{
    struct stream *s = find_stream(c, id);
    struct stat st;
    if (!s || s->sink >= 0 || s->received || limit > INT64_MAX ||
        fstat(fd, &st) || !S_ISREG(st.st_mode)) return -1;
    if ((s->sink = fcntl(fd, F_DUPFD_CLOEXEC, 0)) < 0) return -1;
    s->limit = limit;
    return 0;
}
void *dcc_h2_context(struct dcc_h2 *c, int id)
{
    struct stream *s = find_stream(c, id);
    return s ? s->context : NULL;
}
void *dcc_h2_owner(struct dcc_h2 *c) { return c->context; }
void dcc_h2_set_context(struct dcc_h2 *c, int id, void *context)
{
    struct stream *s = find_stream(c, id);
    if (s) s->context = context;
}
const char *dcc_h2_field(const struct dcc_h2_field *fields, size_t count, const char *name)
{
    size_t i;
    const char *value = NULL;
    for (i = 0; i < count; ++i) if (!strcmp(fields[i].name, name)) {
        if (value) return NULL;
        value = fields[i].value;
    }
    return value;
}
void dcc_h2_cancel(struct dcc_h2 *c, int id)
{
    struct stream *s = c ? find_stream(c, id) : NULL;
    if (!s || s->closed) return;
    s->cancelled = 1;
    if (c->sending != s) nghttp2_submit_rst_stream(c->session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
}
int dcc_h2_prepare(struct dcc_h2 *c, struct pollfd *polls, int capacity)
{
    if (capacity < 1) return -1;
    reap_streams(c);
    polls[0] = (struct pollfd){ c->fd, POLLIN | ((c->connecting || c->failed ||
        nghttp2_session_want_write(c->session)) ? POLLOUT : 0), 0 };
    return 1;
}
int dcc_h2_process(struct dcc_h2 *c, struct pollfd *polls)
{
    unsigned char buffer[FRAME_SIZE];
    ssize_t n;
    if (c->failed) return -1;
    if (c->connecting && polls[0].revents) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) return -1;
        c->connecting = 0;
    }
    if (polls[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        size_t received = 0;
        do {
            n = recv(c->fd, buffer, sizeof(buffer), 0);
            if (!n || (n < 0 && errno != EAGAIN && errno != EINTR)) return -1;
            if (n < 0) break;
            if (nghttp2_session_mem_recv(c->session, buffer, n) != n || c->failed) return -1;
            received += n;
        } while (received < IO_BUDGET);
    }
    if (!c->connecting && (polls[0].revents & POLLOUT)) {
        c->budget = IO_BUDGET;
        if (nghttp2_session_send(c->session) || c->failed) return -1;
    }
    reap_streams(c);
    return 0;
}
int dcc_h2_idle(struct dcc_h2 *c)
{
    return !c->streams && !nghttp2_session_want_write(c->session);
}
int dcc_h2_active(struct dcc_h2 *c) { return c ? c->count : 0; }
time_t dcc_h2_activity(struct dcc_h2 *c, int id)
{
    struct stream *s = find_stream(c, id);
    return s ? s->activity : 0;
}
