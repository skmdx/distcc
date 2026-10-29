/* Read-only compiler filesystem requests. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_FRONTEND_FS_H
#define DCC_FRONTEND_FS_H
#include <stddef.h>
#include <stdint.h>
/* QUERY carries an operation u32 and the path. The reply byte stream starts
 * with errno/length u32. All integers use network order. File metadata is
 * dev, ino, mtime_ns, size, mode, uid, gid, ctime_ns. */
#define DCC_FS_METADATA 52
enum dcc_fs_operation { DCC_FS_STAT = 1, DCC_FS_READ, DCC_FS_REALPATH, DCC_FS_LIST };
struct dcc_job;
int dcc_frontend_fs_reply(const struct dcc_job *, const void *, size_t, uint64_t *);
int dcc_frontend_prepare(char **, struct dcc_job **);
#endif
