/* Pooled, single-job TCP connections and descriptor-only execution IPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_TCP_JOB_H
#define DCC_TCP_JOB_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/types.h>
#include "job.h"

/* 'T' distinguishes this protocol on the existing ACL-protected listener.
 * One framing layer; flags never introduce an inner RPC stream. */
enum dcc_tcp_type {
    DCC_TCP_BEGIN = 0x54000200, DCC_TCP_NEED, DCC_TCP_INPUT,
    DCC_TCP_RESULT, DCC_TCP_OUTPUT, DCC_TCP_COMPLETE, DCC_TCP_RESULT_FD, DCC_TCP_RETRY,
    DCC_TCP_PROGRESS, DCC_TCP_FS_QUERY, DCC_TCP_FS_REPLY
};
#define DCC_TCP_COMPRESSED 0x10000U
#define DCC_TCP_FINAL      0x20000U
#define DCC_TCP_FLAGS (DCC_TCP_COMPRESSED | DCC_TCP_FINAL)
#define DCC_TCP_CHUNK (256 * 1024)
#define DCC_TCP_BYTES (512ULL * 1024 * 1024)
#define DCC_TCP_RESULT_SIZE 72
/* The parser buffers one frame, never a whole file. */
struct dcc_tcp_frame {
    unsigned char header[8], *data;
    size_t used, size;
    uint32_t type;
    int fd_plus_one; /* Zero-initializable ownership of an optional local FD. */
};
int dcc_tcp_local_address(int port, struct sockaddr_un *);
int dcc_tcp_local_listener(int port);
uint32_t dcc_tcp_get32(const void *);
uint64_t dcc_tcp_get64(const void *);
void dcc_tcp_put32(void *, uint32_t);
void dcc_tcp_put64(void *, uint64_t);
void dcc_tcp_frame_clear(struct dcc_tcp_frame *);
/* 1 complete, 0 would block, -1 malformed/closed. Caller clears after use. */
int dcc_tcp_frame_read(int, struct dcc_tcp_frame *, size_t);
/* Encode at most one chunk; raw input and precompressed pump blocks share
 * this framing. Returned allocation includes the complete 8-byte header. */
unsigned char *dcc_tcp_frame_make(uint32_t, const void *, size_t, size_t *);
int dcc_tcp_unpack(const struct dcc_tcp_frame *, unsigned *, void *, const void **, size_t *);
/* Caller supplies CHUNK+16 bytes. RAW returns only its 16-byte header. */
size_t dcc_tcp_encode(uint32_t, unsigned, const void *, size_t, int, void *);
/* Immutable extents use mmap/sendfile; mutable files retain a private read buffer. */
unsigned char *dcc_tcp_file_chunk(uint32_t, unsigned, int, off_t, size_t, int, int, size_t *);

#define DCC_TCP_RUN 128
struct dcc_tcp_run { uint32_t operation; uint64_t id; char root[PATH_MAX]; };
struct dcc_tcp_finished {
    uint64_t id;
    uint64_t peak_bytes;
    uint64_t cpu_ns;
    uint32_t error, retiring, status, mode, present;
    uint64_t size[DCC_JOB_RESULT_COUNT];
};
int dcc_tcp_worker(int, const struct dcc_tcp_run *, int, int, int);

struct dcc_tcp_server;
struct dcc_tcp_dispatch {
    /* 1 started, 0 CPU lane busy, EXIT_BUSY memory admission changed, -1 failed. */
    int (*start)(struct dcc_tcp_server *, const struct dcc_tcp_run *, int, int, const struct dcc_job *);
    void (*cancel)(struct dcc_tcp_server *);
    unsigned (*codegen)(struct dcc_tcp_server *, unsigned);
};
struct dcc_tcp_server *dcc_tcp_server_new(int, const struct dcc_tcp_dispatch *);
void dcc_tcp_server_free(struct dcc_tcp_server *);
void dcc_tcp_server_abandon(struct dcc_tcp_server *);
void dcc_tcp_server_collect(void);
void dcc_tcp_server_quiesce(void);
int dcc_tcp_server_pending(void);
int dcc_tcp_server_poll(struct dcc_tcp_server *, struct pollfd *);
int dcc_tcp_server_process(struct dcc_tcp_server *, short);
void dcc_tcp_server_fs_poll(struct dcc_tcp_server *, struct pollfd *);
int dcc_tcp_server_fs_process(struct dcc_tcp_server *, short);
struct dcc_server_load;
void dcc_tcp_server_load(struct dcc_tcp_server *, struct dcc_server_load *);
void dcc_tcp_dispatch_ready(void);
/* Cumulative unused admissible slot time while an eligible job is ready. */
uint64_t dcc_tcp_ready_idle_ns(void);
uint64_t dcc_tcp_local_input_jobs(void);
uint64_t dcc_tcp_input_prefetch_bytes(void);
uint64_t dcc_tcp_output_overlaps(void);
uint64_t dcc_tcp_local_input_bytes(void);
void dcc_tcp_server_finished(struct dcc_tcp_server *, const struct dcc_tcp_finished *, int *, unsigned);
void dcc_tcp_server_lost(struct dcc_tcp_server *);
struct dcc_hostdef;
int dcc_tcp_exchange(struct dcc_hostdef *, struct dcc_job *, char **, const char *,
                     const char *, const char *, struct dcc_job_result *);
#endif
