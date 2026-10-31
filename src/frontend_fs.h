/* Read-only compiler filesystem requests. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_FRONTEND_FS_H
#define DCC_FRONTEND_FS_H
#include <stddef.h>
#include <stdint.h>
/* QUERY carries an operation u32 and the path. The reply byte stream starts
 * with errno/length u32. All integers use network order. File metadata is
 * dev, ino, mtime_ns, size, mode, uid, gid, ctime_ns. LIST entries expose
 * file types, not permission bits; STAT carries the complete metadata. */
#define DCC_FS_METADATA 52
enum dcc_fs_operation { DCC_FS_STAT = 1, DCC_FS_READ, DCC_FS_REALPATH, DCC_FS_LIST };
/* Compiler-to-daemon only: a requested CPU count replaces the path. Zero
 * releases borrowed CPUs. The reply contains the granted count. */
#define DCC_FS_CODEGEN 5
struct dcc_job;
/* An owned metadata prefix followed by an optional open file extent. No
 * temporary file is needed to concatenate them for the transport. */
struct dcc_frontend_reply {
    unsigned char *data;
    size_t size;
    int fd;
    uint64_t file_size;
};
int dcc_frontend_fs_reply(const struct dcc_job *, const void *, size_t, struct dcc_frontend_reply *);
void dcc_frontend_fs_clear(struct dcc_frontend_reply *);
int dcc_frontend_prepare(char **, struct dcc_job **);
#endif
