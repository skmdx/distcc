/* Reuse loaded Clang libraries within one daemon worker. */
#ifndef DCC_CLANG_WORKER_H
#define DCC_CLANG_WORKER_H
#include <stdint.h>
int dcc_clang_worker(char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns);
#endif
