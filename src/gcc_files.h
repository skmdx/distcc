/* Job-private native compiler filesystem. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_GCC_FILES_H
#define DCC_GCC_FILES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The caller is a dedicated job child. Its mount namespace cannot be reused
 * by the persistent worker. The service is a child in the same process group. */
void *dcc_gcc_files_start(int channel, const char *mountpoint, const char *identity, const char *job_root);
int dcc_gcc_files_stop(void *, uint64_t *cpu_ns, unsigned long long *peak_bytes);
#ifdef __cplusplus
}
#endif
#endif
