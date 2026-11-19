/* Native Rust planner and isolated worker. */
#pragma once
#include <sys/types.h>
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
struct dcc_rust_artifacts;
#ifdef __cplusplus
extern "C" {
#endif
int dcc_rust_client(char **argv);
int dcc_rust_catalog_fd(void);
int dcc_rust_catalog_attach(int fd);
const char *dcc_rust_host(void);
const char *dcc_rust_cgu_capabilities(void);
int dcc_spawn_rust_cgu(pid_t *, const char *abi, const char *tree, const char *output,
                       const char *stdout_path, const char *stderr_path);
int dcc_rust_submit_cgu(const char *version, const char *target, const char *phase);
int dcc_rust_capture(char **argv);
struct dcc_rust_artifacts *dcc_rust_watch(const char *root, int *watchfd);
int dcc_rust_next_metadata(struct dcc_rust_artifacts *, const char **name, const char **line, int *file);
void dcc_rust_watch_free(struct dcc_rust_artifacts *);
int dcc_rust_worker(const struct dcc_job *, const char *root, int cancel, const char *out, const char *err,
                    struct dcc_job_result *);
#ifdef __cplusplus
}
#endif
