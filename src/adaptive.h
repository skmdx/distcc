/* Adaptive host selection.  SPDX-License-Identifier: GPL-2.0-or-later */
struct dcc_hostdef;
struct dcc_capacity { int cpus, jobs; double load; };
int dcc_parse_capacity(char *buf, struct dcc_capacity *cap);
double dcc_capacity_value(const struct dcc_hostdef *host,
                          const struct dcc_capacity *cap, int *slots);
double dcc_host_capacity(const struct dcc_hostdef *host, int *slots);
int dcc_lock_adaptive(struct dcc_hostdef *hosts,
                      struct dcc_hostdef **buildhost, int *cpu_lock_fd);
