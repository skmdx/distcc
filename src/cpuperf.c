/* Live CPU capacity relative to the calibrated contention curve.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "distcc.h"
#include "trace.h"
#include "cpuperf.h"

#ifdef __linux__
#include <sched.h>

static struct {
    int initialized, enabled, have_counters, daemon_busy;
    cpu_set_t allowed;
    struct { int fd; unsigned long long busy, total; } cpu[CPU_SETSIZE];
    double baseline[3], sampled_at, next_sample, valid_at, recovery_until, probe_after;
    struct dcc_cpu_perf perf;
} cpu_perf;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void initialize(void)
{
    if (cpu_perf.initialized) return;
    cpu_perf.initialized = 1;
    cpu_perf.perf.scale = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) cpu_perf.cpu[i].fd = -1;
    const char *value = getenv("DISTCC_CPU_FREQ_REF");
    if (!value) return;
    char extra;
    double *f = cpu_perf.baseline;
    if (sscanf(value, "%lf,%lf,%lf%c", &f[0], &f[1], &f[2], &extra) != 3 ||
        !(f[0] >= 1000 && f[0] <= 10000000 && f[1] >= 1000 && f[1] <= 10000000 &&
          f[2] >= 1000 && f[2] <= 10000000)) {
        rs_log_warning("DISTCC_CPU_FREQ_REF requires three frequencies in kHz (1, physical, logical jobs)");
        return;
    }
    cpu_perf.enabled = 1;
}

static double frequency_reference(double active)
{
    int logical, physical;
    double scale, gain;
    dcc_cpu_topology(&logical, &physical, &scale, &gain);
    const double *f = cpu_perf.baseline;
    if (active <= 1) return f[0];
    if (active <= physical)
        return f[0] + (f[1] - f[0]) * (active - 1) / (physical - 1);
    if (active >= logical) return f[2];
    return f[1] + (f[2] - f[1]) * (active - physical) / (logical - physical);
}

/* /proc/stat is one bounded CPU inventory, not a process scan. Frequency
 * files are opened only when the allowed CPU set changes. */
static int sample(double now)
{
    /* A long idle gap cannot pair an old CPU interval with today's clock.
     * Use the same freshness horizon as recovery admission below. */
    if (now - cpu_perf.sampled_at >= 10) cpu_perf.have_counters = 0;
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) return 0;
    if (!CPU_EQUAL(&allowed, &cpu_perf.allowed)) {
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (cpu_perf.cpu[i].fd >= 0) close(cpu_perf.cpu[i].fd);
            cpu_perf.cpu[i].fd = -1;
            if (CPU_ISSET(i, &allowed)) {
                char path[128];
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_avg_freq", i);
                cpu_perf.cpu[i].fd = open(path, O_RDONLY | O_CLOEXEC);
            }
        }
        cpu_perf.allowed = allowed;
        cpu_perf.have_counters = 0;
        cpu_perf.perf.scale = -1;
        cpu_perf.valid_at = 0;
    }
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return 0;
    char line[512];
    double weighted = 0, observed_busy = 0, busy_ticks = 0, active = 0;
    while (fgets(line, sizeof(line), file)) {
        int cpu;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 9)
            continue;
        if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) continue;
        unsigned long long busy = user + nice + system + irq + softirq;
        unsigned long long total = busy + idle + iowait + steal;
        if (cpu_perf.have_counters && busy >= cpu_perf.cpu[cpu].busy && total > cpu_perf.cpu[cpu].total) {
            double db = busy - cpu_perf.cpu[cpu].busy;
            double dt = total - cpu_perf.cpu[cpu].total;
            if (db <= dt) {
                active += db / dt;
                busy_ticks += db;
                char text[64], extra;
                int fd = cpu_perf.cpu[cpu].fd;
                ssize_t size = db > 0 && fd >= 0 ? pread(fd, text, sizeof(text) - 1, 0) : -1;
                double frequency;
                if (size > 0) {
                    text[size] = 0;
                    if (sscanf(text, "%lf %c", &frequency, &extra) == 1 &&
                        frequency >= 1000 && frequency <= 10000000) {
                        weighted += db * frequency;
                        observed_busy += db;
                    }
                }
            }
        }
        cpu_perf.cpu[cpu].busy = busy;
        cpu_perf.cpu[cpu].total = total;
    }
    /* The lowest calibration point is one continuously busy CPU. Sparse
     * housekeeping on an idle machine observes its power-saving clock, not
     * the capacity it can provide to a compiler. Do not train on that state. */
    int valid = !ferror(file) && cpu_perf.have_counters && active >= 1 &&
                observed_busy >= busy_ticks * .8;
    fclose(file);
    if (valid) {
        double reference = frequency_reference(active);
        double frequency = weighted / observed_busy;
        double observed = frequency / reference;
        if (observed > 1) observed = 1;
        double *scale = &cpu_perf.perf.scale;
        if (*scale < 0) *scale = observed;
        else {
            /* Only busy intervals advance the filter. Symmetric smoothing
             * tracks sustained performance without turning alternating high
             * and low frequency samples into a persistent throttle penalty. */
            double dt = now - cpu_perf.sampled_at;
            *scale += (observed - *scale) * dt / (2 + dt);
        }
        cpu_perf.perf.frequency = frequency;
        cpu_perf.perf.reference = reference;
        cpu_perf.perf.active = active;
        cpu_perf.perf.samples++;
        if (cpu_perf.valid_at && now - cpu_perf.valid_at >= 10 && now < cpu_perf.recovery_until)
            cpu_perf.recovery_until = now + 2;
        cpu_perf.valid_at = now;
    }
    cpu_perf.sampled_at = now;
    cpu_perf.have_counters = 1;
    return valid;
}
#endif

void dcc_cpu_perf_refresh(void)
{
#ifdef __linux__
    initialize();
    if (!cpu_perf.enabled) return;
    double now = monotonic_seconds();
    if (now >= cpu_perf.next_sample) {
        sample(now);
        cpu_perf.next_sample = now + .5;
    }
#endif
}

int dcc_cpu_perf_poll(int busy, int timeout)
{
#ifdef __linux__
    initialize();
    if (!cpu_perf.enabled) return timeout;
    cpu_perf.daemon_busy = busy;
    if (!busy) return timeout;
    dcc_cpu_perf_refresh();
    double now = monotonic_seconds();
    double remaining = cpu_perf.next_sample - now;
    int next = remaining > 0 ? (int)(remaining * 1000) + 1 : 500;
    if (next < timeout) timeout = next;
#else
    (void)busy;
#endif
    return timeout;
}

void dcc_cpu_perf_get(struct dcc_cpu_perf *perf)
{
    *perf = (struct dcc_cpu_perf){ .scale = -1, .age = -1 };
#ifdef __linux__
    initialize();
    if (!cpu_perf.enabled || !cpu_perf.valid_at) return;
    double now = monotonic_seconds();
    *perf = cpu_perf.perf;
    perf->age = now - cpu_perf.valid_at;
    perf->probe = !cpu_perf.daemon_busy && perf->scale < .98 && perf->age >= 10 &&
                  now >= cpu_perf.probe_after;
#endif
}

int dcc_cpu_perf_limit(int limit)
{
#ifdef __linux__
    struct dcc_cpu_perf perf;
    dcc_cpu_perf_get(&perf);
    /* Keep the global recovery trial at one compiler until a fresh sample
     * has had time to reach clients through their one-second stats cycle. */
    if (limit > 1 && (perf.probe || monotonic_seconds() < cpu_perf.recovery_until)) return 1;
#endif
    return limit;
}

void dcc_cpu_perf_started(void)
{
#ifdef __linux__
    struct dcc_cpu_perf perf;
    dcc_cpu_perf_get(&perf);
    if (perf.probe) {
        double now = monotonic_seconds();
        cpu_perf.recovery_until = now + 2;
        cpu_perf.probe_after = now + 10;
    }
#endif
}

void dcc_cpu_perf_child(void)
{
#ifdef __linux__
    if (!cpu_perf.initialized) return;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (cpu_perf.cpu[i].fd >= 0) {
            close(cpu_perf.cpu[i].fd);
            cpu_perf.cpu[i].fd = -1;
        }
#endif
}
