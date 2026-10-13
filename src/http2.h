/* Direct HTTP messages. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_HTTP2_H
#define DCC_HTTP2_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#define DCC_H2_PIPELINE 4
#define DCC_H2_MAX_STREAMS 256
#define DCC_H2_HEADER_LIMIT 16384
struct dcc_h2;
struct dcc_h2_field { const char *name, *value; };
/* fd >= 0: immutable regular file range; otherwise a small memory body.
 * Submission duplicates fd or copies data; the caller retains its reference.
 * File contents must remain immutable until stream close, even after unlink. */
struct dcc_h2_body {
    int fd;
    const void *data;
    uint64_t offset, length;
};
struct dcc_h2_events {
    /* Install a sink before returning if a body is expected. Nonzero resets
     * this stream. Callbacks must not recursively drive/free the connection. */
    int (*headers)(struct dcc_h2 *, int, const struct dcc_h2_field *, size_t, void *);
    int (*end)(struct dcc_h2 *, int, void *);
    /* Exactly once, including connection loss. Zero means the complete HTTP
     * exchange, not compiler success. */
    void (*closed)(struct dcc_h2 *, int, unsigned error, void *);
};
struct dcc_h2 *dcc_h2_new(int fd, int server, const struct dcc_h2_events *, void *);
void dcc_h2_free(struct dcc_h2 *);
int dcc_h2_request(struct dcc_h2 *, const char *method, const char *path,
                   const struct dcc_h2_field *, size_t, const struct dcc_h2_body *, void *);
int dcc_h2_response(struct dcc_h2 *, int stream, int status,
                    const struct dcc_h2_field *, size_t, const struct dcc_h2_body *);
/* Duplicates fd. Uses explicit offsets, never the shared file position.
 * Only bounded, prepared storage belongs in the network event loop.
 * A stream without a sink accepts an empty body only. */
int dcc_h2_sink(struct dcc_h2 *, int stream, int fd, uint64_t limit);
void *dcc_h2_context(struct dcc_h2 *, int stream);
void *dcc_h2_owner(struct dcc_h2 *);
void dcc_h2_set_context(struct dcc_h2 *, int stream, void *);
const char *dcc_h2_field(const struct dcc_h2_field *, size_t, const char *name);
void dcc_h2_cancel(struct dcc_h2 *, int stream);
int dcc_h2_prepare(struct dcc_h2 *, struct pollfd *, int capacity);
int dcc_h2_process(struct dcc_h2 *, struct pollfd *);
int dcc_h2_idle(struct dcc_h2 *);
int dcc_h2_active(struct dcc_h2 *);
time_t dcc_h2_activity(struct dcc_h2 *, int stream);
#endif
