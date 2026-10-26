/* Job metadata shared by the local FD IPC and prepared TCP transport.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_JOB_H
#define DCC_JOB_H
#include <stdint.h>
#include <stddef.h>
#define DCC_JOB_METADATA_MAX (16 * 1024 * 1024)
#define DCC_JOB_INPUT_MAX 65536
#define DCC_JOB_RESULT_COUNT 4
#define DCC_SYSTEM_HEADERS ".distcc-system"
enum dcc_job_kind { DCC_JOB_COMPILE, DCC_JOB_PUMP, DCC_JOB_LINK, DCC_JOB_RUST, DCC_JOB_THINLTO,
                    DCC_JOB_OFFLOAD_LINK, DCC_JOB_KIND_COUNT };
enum dcc_job_input_kind { DCC_JOB_FILE, DCC_JOB_SYMLINK, DCC_JOB_EMPTY,
                         DCC_JOB_READONLY };
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
static inline int dcc_job_file(const struct dcc_job_input *in)
{
    return in->kind == DCC_JOB_FILE || in->kind == DCC_JOB_READONLY;
}
static inline int dcc_job_tree(const struct dcc_job *job)
{
    return job->kind == DCC_JOB_RUST || job->kind == DCC_JOB_THINLTO ||
           job->kind == DCC_JOB_OFFLOAD_LINK;
}
static inline const char *dcc_job_helper(const struct dcc_job *job)
{
    switch (job->kind) {
    case DCC_JOB_RUST: return "distcc-rust";
    case DCC_JOB_THINLTO: return "distcc-thinlto";
    case DCC_JOB_OFFLOAD_LINK: return "distcc-offload-link";
    default: return "distcc-link";
    }
}
static inline int dcc_job_shared(const struct dcc_job *job, unsigned index)
{
    return dcc_job_tree(job) || job->inputs[index].kind == DCC_JOB_READONLY;
}
struct dcc_job_result {
    int status, mode;
    uint64_t prepare_ns, queue_ns, execute_ns; /* Server monotonic phase durations. */
    unsigned long long peak_bytes; /* Local execution telemetry, not wire data. */
    uint64_t size[DCC_JOB_RESULT_COUNT];
    int fd[DCC_JOB_RESULT_COUNT];
    /* Local unpublished destinations, never part of wire metadata. */
    char *temporary[DCC_JOB_RESULT_COUNT];
};
void dcc_job_free(struct dcc_job *);
struct dcc_job *dcc_job_describe(char **argv, const char *cpp, int cpp_fd, char **files);
void dcc_job_result_init(struct dcc_job_result *);
void dcc_job_result_free(struct dcc_job_result *);
/* No file payload is copied into metadata. */
int dcc_job_decode(int fd, struct dcc_job **);
unsigned char *dcc_job_encode_buffer(const struct dcc_job *, size_t *);
int dcc_job_decode_buffer(const void *, size_t, struct dcc_job **);
/* Anonymous storage for data which must outlive its producing process. */
int dcc_job_memfd(const char *name);
struct dcc_buffer;
/* Server compiler execution consumes an already materialized private tree.
 * Results own open FDs before the worker removes temporary pathnames. */
int dcc_execute_job(struct dcc_job *, const char *root, int cancel_fd, struct dcc_job_result *);
const char *dcc_job_tmp_top(const struct dcc_job *);
int dcc_file_transfer(const char *metadata, const char *snapshots, const char *output);
struct dcc_hostdef;
int dcc_compile_direct(struct dcc_hostdef *, struct dcc_job *, char **argv, const char *cpp, int cpp_fd, char **files, char **auxiliary,
                       const char *output, const char *deps, struct dcc_buffer *err,
                       struct dcc_buffer *out, int *status);
#endif
