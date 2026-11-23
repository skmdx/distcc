/* Reuse loaded Clang libraries across daemon execution workers. */
#ifndef DCC_CLANG_WORKER_H
#define DCC_CLANG_WORKER_H
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "native_worker.h"
struct dcc_job;
int dcc_clang_pool_get(const char *compiler, const char *lto_helper);
int dcc_lto_helper(const struct dcc_job *job, char *path);
int dcc_spawn_lto(pid_t *pid, int channel, const char *helper, const char *compiler);
int dcc_wait_compiler_fd(int fd, short events, int cancel, const struct timespec *started);
int dcc_compiler_result(int fd, int cancel, const struct timespec *started,
                        struct dcc_native_result *result);
int dcc_clang_pool_reaped(pid_t pid);
void dcc_clang_pool_shutdown(void);
int dcc_clang_worker(int session, char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns);
#endif
