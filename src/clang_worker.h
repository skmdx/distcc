/* Reuse loaded Clang libraries across daemon execution workers. */
#ifndef DCC_CLANG_WORKER_H
#define DCC_CLANG_WORKER_H
#include <stdint.h>
#include <sys/types.h>
/* group == 0 creates a private invocation group; otherwise join the owning
 * execution worker's group so cancellation retains the same job boundary. */
struct dcc_clang_request { uint32_t operation, group; };
/* Native worker result: one completed invocation, including its descendants. */
struct dcc_clang_result { uint64_t status, peak_bytes, cpu_ns; };
int dcc_clang_pool_get(const char *compiler);
int dcc_clang_pool_reaped(pid_t pid);
void dcc_clang_pool_abandon(void);
void dcc_clang_pool_shutdown(void);
int dcc_clang_worker(int session, char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns);
#endif
