/* Same-version local IPC. The control connection owns the CPU lease.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_BROKER_PROTOCOL_H
#define DCC_BROKER_PROTOCOL_H
#include <stdint.h>
#define BROKER_VERSION 2
enum { ACQUIRE = 1, JOBS = 2, CPP_DONE = 3, MEMO_GET = 4, MEMO_PUT = 5,
       ACQUIRE_POOL = 6, RETURN = 7, ACQUIRE_LINK = 8, ACQUIRE_CPP = 9,
       ACQUIRE_READY = 10, FAILED = 11, LOCAL_CPP = 12, LOCAL_ANALYSIS = 13, DEFER = 14,
       TRY_LOCAL = 15, WORKER_GET = 16, WORKER_PUT = 17 };
/* Backend-only clients need a worker socket even on this machine. */
#define CLIENT_COMPILE (1u << 16)
struct request { uint32_t version, command; };
struct feedback { uint64_t prepare_ns, queue_ns, execute_ns, cpu_ns; };
struct reply { int32_t status, slots; char host[1024]; };
#endif
