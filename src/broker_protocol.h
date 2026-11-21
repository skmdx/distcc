/* Same-version local IPC. The control connection owns the CPU lease.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_BROKER_PROTOCOL_H
#define DCC_BROKER_PROTOCOL_H
#include <stdint.h>
#define BROKER_VERSION 9
#define DCC_BROKER_CLIENT_MAX 256
enum { ACQUIRE = 1, JOBS = 2, CPP_DONE = 3, MEMO_GET = 4, MEMO_PUT = 5,
       ACQUIRE_POOL = 6, RETURN = 7, ACQUIRE_LINK = 8, ACQUIRE_CPP = 9,
       ACQUIRE_READY = 10, FAILED = 11, LOCAL_CPP = 12, LOCAL_ANALYSIS = 13, DEFER = 14,
       TRY_LOCAL = 15, WORKER_GET = 16, WORKER_PUT = 17, MEMO_REFRESH = 18, RELEASE = 19,
       CGU_CAPACITY = 20 };
/* A nonzero tag identifies a logical lease within one process's session. */
enum { CLIENT_COMPILE = 1 };
enum { DCC_BROKER_POOLED = 1, DCC_BROKER_DIRECT = 2, DCC_BROKER_COLD_FAMILY = 4 };
struct broker_request_hdr { uint16_t version; uint8_t command, flags; uint32_t tag; };
#ifdef __cplusplus
static_assert(sizeof(broker_request_hdr) == 8, "broker IPC header");
#else
_Static_assert(sizeof(struct broker_request_hdr) == 8, "broker IPC header");
#endif
struct memo_refresh_request { struct broker_request_hdr header; uint8_t stale_digest[16]; };
struct feedback { uint64_t prepare_ns, queue_ns, execute_ns, cpu_ns; };
struct reply { int32_t status, slots; uint32_t flags; union { char host[1024]; int32_t native_peers; }; };
#endif
