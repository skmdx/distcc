/* Live CPU capacity relative to the calibrated contention curve. */
#ifndef DCC_CPUPERF_H
#define DCC_CPUPERF_H

struct dcc_cpu_perf {
    double scale, age, frequency, reference, active;
    unsigned long samples;
    int probe;
};

/* Owned by the daemon parent; workers and clients never sample the CPU. */
int dcc_cpu_perf_poll(int busy, int timeout);
void dcc_cpu_perf_get(struct dcc_cpu_perf *perf);
int dcc_cpu_perf_limit(int limit);
void dcc_cpu_perf_started(void);
void dcc_cpu_perf_child(void);

#endif
