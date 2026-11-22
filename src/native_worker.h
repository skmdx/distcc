/* The helper owns its control socket and all invocation children. */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* group == 0 creates a private invocation group; otherwise join its owner. */
/* C/P/L/B carry command, completion peer and stdio; D adds a driver relay socket.
 * G/S are GCC driver/planner requests with an extra dependency journal. */
struct dcc_native_request { uint32_t operation, group; };
struct dcc_native_result { uint64_t status, peak_bytes, cpu_ns; };
#ifndef __cplusplus
enum dcc_compiler_context { DCC_WORKER_IMAGE, DCC_PLAN_ENVIRONMENT };
int dcc_local_worker_key(const char *compiler, const char *library,
                        enum dcc_compiler_context context, char *key, size_t size);
int dcc_gcc_plan_execute(char **argv);
#endif
#ifdef __cplusplus
int dcc_clang_native_worker(int fd);
int dcc_native_worker(int control, int (*execute)(int command, unsigned operation),
                      int (*plan)(int command, unsigned operation, int journal) = nullptr);
#endif
