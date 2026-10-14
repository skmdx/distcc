/* Bounded compile-cost learning, in seconds on an idle speed=1 CPU. */
#ifndef DCC_WORKLOAD_H
#define DCC_WORKLOAD_H
#include <stdint.h>

#define DCC_WORK_CLASSES 6
#define DCC_WORK_HISTORY 4096
struct dcc_work_hint {
    uint64_t key, bytes;
    uint32_t kind, reserved; /* 1..3: preprocessed C/C++/asm; 4..6: source. */
};
struct dcc_work_model {
    struct { uint64_t key, bytes; double seconds; } history[DCC_WORK_HISTORY];
    struct { double bytes, seconds; } classes[DCC_WORK_CLASSES];
};
void dcc_work_describe(struct dcc_work_hint *, char **, const char *, const char *, int, int);
int dcc_work_valid(const struct dcc_work_hint *);
double dcc_work_predict(const struct dcc_work_model *, const struct dcc_work_hint *);
void dcc_work_observe(struct dcc_work_model *, const struct dcc_work_hint *, double);
#endif
