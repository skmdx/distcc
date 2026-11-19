/* Adaptive host selection.  SPDX-License-Identifier: GPL-2.0-or-later */
#include "resource.h"
#define DCC_STATS_SIZE 32768
struct dcc_hostdef;
struct dcc_capacity {
    int cpus, cores, jobs;
    char rust_host[96];
    char llvm_cgu[1024]; /* LLVM version followed by space-separated registered target names. */
    double core_scale, smt_gain;
    double cpu_scale, cpu_age;
    int cpu_probe, live_report;
    unsigned long long report_stamp;
    double load, memory_pressure;
    /* Optional live metrics; -1 means unavailable, never idle. */
    int running, busy, queued, links, admission, available;
    int preparing, returning, capacity, suspended;
    int compile_capacity[DCC_MEMORY_BUCKETS];
    int pump_capacity[DCC_MEMORY_BUCKETS];
    int rust_capacity[DCC_MEMORY_BUCKETS];
    int thinlto_capacity[DCC_MEMORY_BUCKETS];
    int offload_capacity[DCC_MEMORY_BUCKETS];
    /* Frontend jobs fetch inputs lazily and have no uploaded input tree. */
    int frontend_capacity[DCC_MEMORY_BUCKETS];
    int cgu_capacity[DCC_MEMORY_BUCKETS];
    unsigned long long memory_headroom, memory_reserved, job_memory;
    unsigned long long memory_estimates[7][DCC_MEMORY_BUCKETS];
};
struct dcc_load_report;
int dcc_capacity_update(struct dcc_capacity *, const struct dcc_load_report *, int owned);
int dcc_parse_capacity(char *buf, struct dcc_capacity *cap);
int dcc_capacity_slots(const struct dcc_hostdef *host, const struct dcc_capacity *cap);
int dcc_capacity_jobs(const struct dcc_hostdef *host, const struct dcc_capacity *cap);
int dcc_capacity_parallelism(const struct dcc_hostdef *host, const struct dcc_capacity *cap);
int dcc_host_slots(const struct dcc_hostdef *host);
double dcc_capacity_score(const struct dcc_hostdef *host,
                          const struct dcc_capacity *cap, int busy);
int dcc_lock_adaptive(struct dcc_hostdef *hosts,
                      struct dcc_hostdef **buildhost, int *cpu_lock_fd);
