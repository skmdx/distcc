/* The helper owns its control socket and all invocation children. */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* group == 0 creates a private invocation group; otherwise join its owner. */
/* C/P carry command, completion peer and stdio; D adds a driver relay socket.
 * G/S are GCC driver/planner requests with an extra dependency journal. */
struct dcc_native_request { uint32_t operation, group; };
struct dcc_native_result { uint64_t status, peak_bytes, cpu_ns; };
#ifndef __cplusplus
int dcc_local_worker_key(const char *compiler, const char *library,
                        const char *const *omit_environment, char *key, size_t size);
int dcc_gcc_plan_execute(char **argv);
#endif
#ifdef __cplusplus
int dcc_native_worker(int control, int (*execute)(int command, bool preparing));
#endif
