/* Bounded compile-cost learning, in seconds on an idle speed=1 CPU. */
#ifndef DCC_WORKLOAD_H
#define DCC_WORKLOAD_H
#include <stdint.h>
#include <stddef.h>

#define DCC_WORK_RUST 7
#define DCC_WORK_THINLTO 8
#define DCC_WORK_OFFLOAD 9
#define DCC_WORK_OFFLOAD_LINK 10
#define DCC_WORK_RUST_CGU 11
#define DCC_WORK_CLASSES DCC_WORK_RUST_CGU
#define DCC_WORK_HISTORY 4096
#define DCC_WORK_WAYS 4
#define DCC_RUST_FEATURE_BYTES 2048
#define DCC_RUST_FEATURE_POOL_BYTES 8192
struct dcc_work_hint {
    uint64_t key, bytes;
    uint32_t kind, work_units; /* Language-specific estimate of computation independent of bytes. */
    uint64_t family; /* Source-content prior across command/configuration changes. */
    uint64_t scope_key;
    uint64_t memory_class; /* Same resource class as the serialized worker job. */
    uint64_t rust_compiler; /* rustc identity and host triple for a whole crate. */
    char cgu_backend[96]; /* LLVM version and registered target for a CGU. */
    char rust_host_features[DCC_RUST_FEATURE_BYTES]; /* Required only for host code in a whole crate. */
};
/* Ordinary jobs send only the fixed prefix on the local broker socket. */
#define DCC_WORK_HINT_BASE_SIZE offsetof(struct dcc_work_hint, rust_host_features)
static inline size_t dcc_work_hint_size(const struct dcc_work_hint *hint)
{
    return hint->rust_host_features[0] ? sizeof(*hint) : DCC_WORK_HINT_BASE_SIZE;
}
struct dcc_work_class {
    double weight, x, y, z;
    double xx, xy, yy, xz, yz;
    double fixed, input_rate, work_rate;
    double mass, mean; /* Unweighted execution-time EWMA for competing work. */
};
struct dcc_work_model {
    struct { uint64_t key, family, bytes; double seconds; int peer; } history[DCC_WORK_HISTORY];
    unsigned char next[DCC_WORK_HISTORY / DCC_WORK_WAYS];
    struct dcc_work_class classes[DCC_WORK_CLASSES];
};
struct dcc_source_features { uint64_t hash; uint32_t units, included_bytes; };
void dcc_work_describe(struct dcc_work_hint *, char **, const char *, const char *, int, int, struct dcc_source_features *);
int dcc_work_valid(const struct dcc_work_hint *);
double dcc_work_predict(const struct dcc_work_model *, const struct dcc_work_hint *);
int dcc_work_peer(const struct dcc_work_model *, const struct dcc_work_hint *);
void dcc_work_observe(struct dcc_work_model *, const struct dcc_work_hint *, double, int);
#endif
