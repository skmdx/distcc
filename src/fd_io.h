#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Chunk large input sets within Linux's per-message SCM_RIGHTS limit. */
#define DCC_IPC_FDS_MAX 64
int dcc_send_fds(int, const void *, size_t, const int *, unsigned);
ssize_t dcc_recv_fds(int, void *, size_t, int *, unsigned *);
#ifdef __cplusplus
}
#endif
