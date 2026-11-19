/* Native Rust planner and isolated worker. */
#pragma once
#define DCC_RUST_ROOT "/__distcc_rust"
#define DCC_RUST_DIGEST_TABLE "rust-input-digests-v1"
#define DCC_RUST_OUT (DCC_RUST_ROOT "/out")
#define DCC_RUST_STDERR (DCC_RUST_ROOT "/stderr")
#define DCC_RUST_LINKERS (DCC_RUST_ROOT "/linkers")
#define DCC_RUST_SCRATCH (DCC_RUST_ROOT "/scratch")
#define DCC_RUST_LINK_PREFIX ".distcc-link-"
#define DCC_RUST_FINAL (DCC_RUST_ROOT "/final")
#define DCC_RUST_PRELOAD (DCC_RUST_ROOT "/native.so")
#define DCC_RUST_PROFILE (DCC_RUST_ROOT "/native")
#define DCC_RUST_COMPILER (DCC_RUST_ROOT "/compiler")
struct dcc_job;
struct dcc_job_result;
struct dcc_work_hint;
struct dcc_rust_artifacts;
#ifdef __cplusplus
extern "C" {
#endif
int dcc_rust_client(char **argv);
int dcc_rust_catalog_fd(void);
int dcc_rust_catalog_attach(int fd);
const char *dcc_rust_host(void);
int dcc_rust_capture(char **argv);
int dcc_rust_pipelined(const struct dcc_job *);
struct dcc_rust_artifacts *dcc_rust_watch(const char *root, int *watchfd);
int dcc_rust_next_metadata(struct dcc_rust_artifacts *, const char **name, const char **line, int *file);
void dcc_rust_watch_free(struct dcc_rust_artifacts *);
int dcc_rust_worker(const struct dcc_job *, const char *root, int cancel, const char *out, const char *err,
                    struct dcc_job_result *);
int dcc_transfer_prepared(struct dcc_job *, char **files, const char *output, const struct dcc_work_hint *,
                          int queued_fd, struct dcc_job_result *);
#ifdef __cplusplus
}
#endif
