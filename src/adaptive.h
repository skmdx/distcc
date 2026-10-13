/* Adaptive host selection.  SPDX-License-Identifier: GPL-2.0-or-later */
struct dcc_hostdef;
struct dcc_capacity {
    int cpus, jobs;
    double load, memory_pressure;
    /* Optional live metrics; -1 means unavailable, never idle. */
    int running, busy, queued, links, admission, available;
    int preparing, returning, capacity;
};
int dcc_parse_capacity(char *buf, struct dcc_capacity *cap);
int dcc_capacity_slots(const struct dcc_hostdef *host, const struct dcc_capacity *cap);
int dcc_host_slots(const struct dcc_hostdef *host);
double dcc_capacity_score(const struct dcc_hostdef *host,
                          const struct dcc_capacity *cap, int busy);
int dcc_lock_adaptive(struct dcc_hostdef *hosts,
                      struct dcc_hostdef **buildhost, int *cpu_lock_fd);
