/* Reuse loaded Clang libraries across daemon execution workers. */
#ifndef DCC_CLANG_WORKER_H
#define DCC_CLANG_WORKER_H
#include <stdint.h>
#include <sys/types.h>
#include "native_worker.h"
int dcc_clang_pool_get(const char *compiler);
int dcc_clang_pool_reaped(pid_t pid);
void dcc_clang_pool_abandon(void);
void dcc_clang_pool_shutdown(void);
int dcc_clang_worker(int session, char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns);
#endif
