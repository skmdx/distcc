/* Job metadata shared by the local FD IPC and HTTP representations.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_JOB_H
#define DCC_JOB_H
#include <stdint.h>
#include <stddef.h>
#define DCC_JOB_METADATA_MAX (16 * 1024 * 1024)
#define DCC_JOB_INPUT_MAX 65536
#define DCC_JOB_RESULT_COUNT 4
#define DCC_JOB_CONTENT_ENCODING "x-distcc-lz4-stream"
enum dcc_job_kind { DCC_JOB_COMPILE, DCC_JOB_PUMP, DCC_JOB_LINK };
enum dcc_job_input_kind { DCC_JOB_FILE, DCC_JOB_SYMLINK, DCC_JOB_EMPTY };
enum dcc_job_encoding { DCC_JOB_RAW, DCC_JOB_LZ4 };
enum dcc_job_artifact { DCC_JOB_STDERR, DCC_JOB_STDOUT, DCC_JOB_OBJECT, DCC_JOB_DEPS };
struct dcc_job_input {
    char *path, *target;
    enum dcc_job_input_kind kind;
    enum dcc_job_encoding encoding;
    uint64_t wire_size, plain_size;
    unsigned char digest[8];
    int cached;
    /* Owned local reference, never serialized. Open on demand to bound FD use. */
    int fd;
};
struct dcc_job {
    enum dcc_job_kind kind;
    char *cwd;
    char **argv;
    unsigned count;
    struct dcc_job_input *inputs;
};
struct dcc_job_result {
    int status, mode;
    uint64_t size[DCC_JOB_RESULT_COUNT];
    uint64_t wire_size[DCC_JOB_RESULT_COUNT];
    unsigned compressed;
    int fd[DCC_JOB_RESULT_COUNT];
};
void dcc_job_free(struct dcc_job *);
void dcc_job_result_init(struct dcc_job_result *);
void dcc_job_result_free(struct dcc_job_result *);
/* These return an owned sealed metadata memfd, or -1. No file payload is copied
 * into metadata; HTTP file bodies and SCM_RIGHTS carry it independently. */
int dcc_job_encode(const struct dcc_job *);
int dcc_job_decode(int fd, struct dcc_job **);
int dcc_job_result_encode(const struct dcc_job_result *);
int dcc_job_result_decode(int fd, struct dcc_job_result *);
/* Anonymous storage for data which must outlive its producing process. */
int dcc_job_memfd(const char *name);
struct dcc_buffer;
/* Replace an owned raw FD only when compression shrinks it. Returns encoding,
 * or -1 on failure. Payload work stays outside the connection event loop. */
int dcc_job_compress(int *fd, uint64_t plain, uint64_t *wire);
int dcc_job_expand(int fd, uint64_t wire, uint64_t plain, int output,
                   struct dcc_buffer *buffer);
/* Server compiler execution consumes an already materialized private tree.
 * Results own open FDs before the worker removes temporary pathnames. */
int dcc_execute_job(struct dcc_job *, const char *root, int cancel_fd, struct dcc_job_result *);
struct dcc_hostdef;
int dcc_compile_direct(struct dcc_hostdef *, char **argv, const char *cpp, char **files,
                       const char *output, const char *deps, struct dcc_buffer *err,
                       struct dcc_buffer *out, int *status);
#endif
