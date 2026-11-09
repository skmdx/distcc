/* Read-only compiler filesystem requests. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_FRONTEND_FS_H
#define DCC_FRONTEND_FS_H
#include <stddef.h>
#include <stdint.h>
/* QUERY carries an operation u32 and the path. The reply byte stream starts
 * with errno/length u32. All integers use network order. File metadata is
 * dev, ino, mtime_ns, size, mode, uid, gid, ctime_ns. LIST entries expose
 * file types, not permission bits; STAT carries the complete metadata.
 * LSTAT carries a NUL-terminated path, a previous directory version (or zeros),
 * and sorted, unique NUL-terminated child names to prefetch. Directory replies
 * add flags/count u32 before their entries. CACHED replies omit unchanged names;
 * child metadata is always fresh. STABLE permits publishing the name snapshot
 * after matching the directory version both before and after the lookup.
 * DIRECTORIES contains length-u32/LSTAT-query pairs; its reply contains the
 * corresponding ordinary errno/length/body replies, in request order. */
#define DCC_FS_METADATA 52
#define DCC_FS_QUERY_MAX 65536
#define DCC_FS_NO_METADATA UINT32_MAX
#define DCC_FS_DIRECTORY_CACHED 1U
#define DCC_FS_DIRECTORY_STABLE 2U
enum dcc_fs_operation { DCC_FS_STAT = 1, DCC_FS_READ, DCC_FS_REALPATH, DCC_FS_LIST,
                       DCC_FS_CODEGEN, DCC_FS_READLINK = 7, DCC_FS_LSTAT = 8,
                       DCC_FS_DIRECTORIES, DCC_FS_STDIN };
/* STDIN requests use "/" as their operand and return the same metadata/body
 * layout as READ. Only a job's explicitly captured stdin is accessible. */
/* Compiler-to-daemon only: a requested CPU count replaces the path. Zero
 * releases borrowed CPUs. The reply contains the granted count. */
struct dcc_job;
int dcc_frontend_fs_add_roots(struct dcc_job *job, const char *paths);
int dcc_frontend_fs_add_root(struct dcc_job *job, const char *path);
/* An owned metadata prefix followed by an optional open file extent. No
 * temporary file is needed to concatenate them for the transport. Initialize
 * with {.fd = -1}; reset releases the extent but retains metadata storage. */
struct dcc_frontend_reply {
    unsigned char *data;
    size_t size, capacity;
    int fd;
    uint64_t file_size;
};
int dcc_frontend_fs_reply(struct dcc_job *, const void *, size_t, struct dcc_frontend_reply *);
void dcc_frontend_fs_reset(struct dcc_frontend_reply *);
void dcc_frontend_fs_clear(struct dcc_frontend_reply *);
int dcc_frontend_prepare(char **, struct dcc_job **);
int dcc_gcc_frontend_prepare(char **, const char *, struct dcc_job **);
#endif
