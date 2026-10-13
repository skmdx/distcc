/* Resource-aware admission.  SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "resource.h"

static double pressure = -1;
static unsigned long long available = ULLONG_MAX, budget = ULLONG_MAX;

#ifdef __linux__
static unsigned long long read_bytes(const char *group, const char *name)
{
    char path[PATH_MAX];
    unsigned long long value = ULLONG_MAX;
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/%s", group, name) >= (int)sizeof(path))
        return value;
    file = fopen(path, "r");
    if (file) {
        if (fscanf(file, "%llu", &value) != 1) value = ULLONG_MAX;
        fclose(file);
    }
    return value;
}

static unsigned long long memory_used(const char *group)
{
    unsigned long long used = read_bytes(group, "memory.current"), value, reclaimable = 0;
    char path[PATH_MAX], name[64];
    FILE *file;
    if (used == ULLONG_MAX ||
        snprintf(path, sizeof(path), "%s/memory.stat", group) >= (int)sizeof(path))
        return used;
    file = fopen(path, "r");
    if (file) {
        while (fscanf(file, "%63s %llu", name, &value) == 2)
            if (!strcmp(name, "inactive_file")) reclaimable = value;
        fclose(file);
    }
    /* Completed jobs leave clean page cache charged to the service. Counting
     * inactive file pages as live jobs could stop admission indefinitely. */
    return used > reclaimable ? used - reclaimable : 0;
}

static void read_pressure(const char *path)
{
    double value;
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (fscanf(file, "some avg10=%lf", &value) == 1 && value >= 0 &&
        value <= 100 && value > pressure) pressure = value;
    fclose(file);
}

static void sample_resources(void)
{
    static time_t sampled;
    static char group[PATH_MAX];
    static int located;
    char path[PATH_MAX], line[PATH_MAX];
    unsigned long long own = 0, value;
    FILE *file;
    time_t now = time(NULL);
    if (sampled == now) return;
    sampled = now;
    pressure = -1;
    available = budget = ULLONG_MAX;
    if (!located) {
        located = 1;
        file = fopen("/proc/self/cgroup", "r");
        if (file) {
            while (fgets(line, sizeof(line), file)) {
                if (strncmp(line, "0::/", 4)) continue;
                line[strcspn(line, "\n")] = 0;
                if (strlen(line + 3) + sizeof("/sys/fs/cgroup") <= sizeof(group)) {
                    strcpy(group, "/sys/fs/cgroup");
                    strcat(group, line + 3);
                }
                break;
            }
            fclose(file);
        }
    }
    if (group[0]) {
        value = memory_used(group);
        if (value != ULLONG_MAX) own = value;
    }
    file = fopen("/proc/meminfo", "r");
    if (file) {
        while (fgets(line, sizeof(line), file))
            if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
                available = value * 1024;
                budget = available + own;
                break;
            }
        fclose(file);
    }
    read_pressure("/proc/pressure/memory");
    if (group[0]) {
        strcpy(path, group);
        for (;;) {
            char name[PATH_MAX], *slash;
            unsigned long long limit = read_bytes(path, "memory.max");
            unsigned long long used = memory_used(path);
            if (limit != ULLONG_MAX && used != ULLONG_MAX) {
                unsigned long long free = limit > used ? limit - used : 0;
                if (free < available) available = free;
                if (free + own < budget) budget = free + own;
            }
            if (snprintf(name, sizeof(name), "%s/memory.pressure", path) < (int)sizeof(name))
                read_pressure(name);
            slash = strrchr(path, '/');
            if (!slash || slash <= path + strlen("/sys/fs/cgroup")) break;
            *slash = 0;
        }
    }
}
#else
static void sample_resources(void) {}
#endif

double dcc_memory_pressure(void)
{
    sample_resources();
    return pressure;
}

static unsigned long long peak_job;
static unsigned samples;

void dcc_resource_observe(unsigned long long peak_bytes, int success)
{
    /* Retain the high-water mark until daemon restart. Small later jobs must
     * not erase evidence of a large compiler working set. */
    if (peak_bytes > peak_job) peak_job = peak_bytes;
    if (peak_bytes && success && samples < UINT_MAX) ++samples;
}

unsigned dcc_resource_samples(void) { return samples; }
unsigned long long dcc_resource_peak_bytes(void) { return peak_job; }

unsigned long long dcc_resource_job_bytes(void)
{
    const char *setting = getenv("DISTCC_JOB_MEMORY");
    char *end;
    unsigned long long job = 1024;
    if (setting && *setting) {
        unsigned long long requested = strtoull(setting, &end, 10);
        if (!*end && requested >= 1 && requested <= 1048576) job = requested;
    }
    job *= 1024 * 1024;
    const char *learn = getenv("DISTCC_MEMORY_LEARNING");
    if (learn && !strcmp(learn, "1")) {
        /* Child RSS excludes daemon/input storage and simultaneous peaks.
         * Reserve twice the largest observation plus 64 MiB, never less
         * than 256 MiB. The cgroup/headroom/PSI checks remain authoritative. */
        unsigned long long measured = peak_job <= (ULLONG_MAX - 64ULL * 1024 * 1024) / 2 ?
            peak_job * 2 + 64ULL * 1024 * 1024 : ULLONG_MAX;
        if (samples >= 8 || measured > job)
            job = measured > 256ULL * 1024 * 1024 ? measured : 256ULL * 1024 * 1024;
    }
    return job;
}

int dcc_resource_slots(int maximum)
{
    static int pressure_blocked;
    unsigned long long job = dcc_resource_job_bytes(), spare, slots;
    spare = job / 4;
    sample_resources();
    if (pressure >= 10) pressure_blocked = 1;
    else if (pressure < 5) pressure_blocked = 0;
    /* PSI is reactive; keep headroom and reserve a budget before the burst.
     * Existing jobs finish normally when the admissible limit falls. */
    if (pressure_blocked || available < job || available - job < spare ||
        budget < job || budget - job < spare)
        return 0;
    slots = (budget - spare) / job;
    return slots < (unsigned)maximum ? (int)slots : maximum;
}
