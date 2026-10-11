/* Adaptive host selection.  SPDX-License-Identifier: GPL-2.0-or-later */
struct dcc_hostdef;
double dcc_host_capacity(const struct dcc_hostdef *host, int *slots);
int dcc_lock_adaptive(struct dcc_hostdef *hosts,
                      struct dcc_hostdef **buildhost, int *cpu_lock_fd);
