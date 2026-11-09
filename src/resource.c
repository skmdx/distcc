/* Resource-aware admission.  SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#ifdef __linux__
#include <sched.h>
#include <sys/inotify.h>
#endif
#include "resource.h"
#include "job.h"

static double pressure = -1;
static int processors = 1;
static unsigned long long available = ULLONG_MAX, budget = ULLONG_MAX;

#ifdef __linux__
/* These procfs/cgroup records are small. Read a complete snapshot without
 * allocating a stdio stream and its buffer for every admission sample. */
static int read_text(const char *path, char *data, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < capacity - 1) {
        ssize_t n = read(fd, data + used, capacity - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (!n) { data[used] = 0; close(fd); return 0; }
        used += n;
    }
    close(fd);
    errno = EOVERFLOW;
    return -1;
}

static unsigned long long read_bytes(const char *group, const char *name)
{
    char path[PATH_MAX], data[64];
    unsigned long long value = ULLONG_MAX;
    if (snprintf(path, sizeof(path), "%s/%s", group, name) >= (int)sizeof(path))
        return value;
    if (!read_text(path, data, sizeof(data)) && sscanf(data, "%llu", &value) != 1)
        value = ULLONG_MAX;
    return value;
}

static unsigned long long memory_used(const char *group, unsigned long long *backing)
{
    unsigned long long used = read_bytes(group, "memory.current"), value, reclaimable = 0;
    char path[PATH_MAX], name[64], data[16384];
    if (used == ULLONG_MAX ||
        snprintf(path, sizeof(path), "%s/memory.stat", group) >= (int)sizeof(path))
        return used;
    if (!read_text(path, data, sizeof(data))) {
        char *cursor = data, *line;
        while ((line = strsep(&cursor, "\n"))) {
            if (sscanf(line, "%63s %llu", name, &value) != 2) continue;
            if (!strcmp(name, "inactive_file")) reclaimable = value;
            else if (backing && !strcmp(name, "shmem")) *backing = value;
        }
    }
    /* Completed jobs leave clean page cache charged to the service. Counting
     * inactive file pages as live jobs could stop admission indefinitely. */
    return used > reclaimable ? used - reclaimable : 0;
}

static void read_pressure(const char *path)
{
    double value;
    char data[512];
    if (read_text(path, data, sizeof(data))) return;
    if (sscanf(data, "some avg10=%lf", &value) == 1 && value >= 0 &&
        value <= 100 && value > pressure) pressure = value;
}

static void sample_resources(void)
{
    static time_t sampled;
    static char group[PATH_MAX];
    static int located;
    char path[PATH_MAX], data[16384];
    unsigned long long own = 0, backing = 0, value;
    unsigned long long own_sample = ULLONG_MAX;
    time_t now = time(NULL);
    if (sampled == now) return;
    sampled = now;
    pressure = -1;
    available = budget = ULLONG_MAX;
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    processors = online > 0 && online < INT_MAX ? (int)online : 1;
    cpu_set_t allowed;
    if (!sched_getaffinity(0, sizeof(allowed), &allowed) && CPU_COUNT(&allowed))
        processors = CPU_COUNT(&allowed);
    if (!located) {
        located = 1;
        if (!read_text("/proc/self/cgroup", data, sizeof(data))) {
            char *cursor = data, *line;
            while ((line = strsep(&cursor, "\n"))) {
                if (strncmp(line, "0::/", 4)) continue;
                if (strlen(line + 3) + sizeof("/sys/fs/cgroup") <= sizeof(group)) {
                    strcpy(group, "/sys/fs/cgroup");
                    strcat(group, line + 3);
                }
                break;
            }
        }
    }
    if (group[0]) {
        own_sample = memory_used(group, &backing);
        if (own_sample != ULLONG_MAX) own = own_sample;
    }
    if (!read_text("/proc/meminfo", data, sizeof(data))) {
        char *cursor = data, *line;
        while ((line = strsep(&cursor, "\n")))
            if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
                available = value * 1024;
                budget = available + own;
                break;
            }
    }
    read_pressure("/proc/pressure/memory");
    if (group[0]) {
        strcpy(path, group);
        for (;;) {
            char name[PATH_MAX], *slash;
            if (snprintf(name, sizeof(name), "%s/cpu.max", path) < (int)sizeof(name)) {
                unsigned long long quota, period;
                if (!read_text(name, data, sizeof(data))) {
                    if (sscanf(data, "%llu %llu", &quota, &period) == 2 && quota && period) {
                        unsigned long long capacity = quota / period + (quota % period != 0);
                        if (capacity < (unsigned)processors) processors = (int)capacity;
                    }
                }
            }
            unsigned long long limit = read_bytes(path, "memory.max");
            unsigned long long used = limit == ULLONG_MAX ? ULLONG_MAX :
                                      !strcmp(path, group) ? own_sample : memory_used(path, NULL);
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
    /* Anonymous input/output files remain charged independently of compiler
     * working sets. Their actual shared pages must not become job capacity. */
    budget = budget > backing ? budget - backing : 0;
}
#else
static void sample_resources(void)
{
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    processors = online > 0 && online < INT_MAX ? (int)online : 1;
}
#endif

int dcc_resource_cpus(void)
{
    sample_resources();
    return processors;
}

double dcc_memory_pressure(void)
{
    sample_resources();
    return pressure;
}

static unsigned long long peak_job;
static unsigned samples;
static unsigned long long learned_job;
/* Keep two consecutive CPU-sized cohorts. A single small job cannot erase
 * an expensive observation, but an earlier package cannot reserve memory
 * forever after a whole new workload has completed. Class zero also tracks
 * the recent demand used to admit previously unseen classes. */
struct memory_history {
    unsigned long long current, previous;
    unsigned completed;
};
static struct memory_history class_memory[1 + DCC_JOB_KIND_COUNT * DCC_MEMORY_BUCKETS];

static unsigned long long recent_peak(const struct memory_history *history)
{
    return history->current > history->previous ? history->current : history->previous;
}

static void observe_memory(struct memory_history *history,
                           unsigned long long peak, int success)
{
    if (success && history->completed >= (unsigned)processors) {
        history->previous = history->current;
        history->current = 0;
        history->completed = 0;
    }
    if (peak > history->current) history->current = peak;
    if (success) ++history->completed;
}

static int memory_learning(void)
{
    const char *setting = getenv("DISTCC_MEMORY_LEARNING");
    return setting && !strcmp(setting, "1");
}

unsigned dcc_resource_bucket(unsigned long long bytes)
{
    unsigned bucket = 0;
    while (bytes >>= 1) ++bucket;
    return bucket;
}

unsigned long long dcc_resource_input_bytes(const struct dcc_job *job)
{
    /* Lazy frontends read sources on demand. Uploaded auxiliary profiles must
     * not move them out of the class advertised to the scheduling broker. */
    if (dcc_job_frontend(job)) return 0;
    unsigned long long bytes = 0;
    for (unsigned i = 0; i < job->count; ++i) {
        /* Shared code-generation data (for example PGO profiles) does not
         * describe source complexity. Including one large common profile
         * collapses tiny and expensive compilations into the same class.
         * Storage remains charged through the cgroup's actual backing pages. */
        if ((job->kind == DCC_JOB_COMPILE || job->kind == DCC_JOB_PUMP) &&
            job->inputs[i].kind == DCC_JOB_READONLY) continue;
        unsigned long long size = job->inputs[i].plain_size;
        bytes = size <= ULLONG_MAX - bytes ? bytes + size : ULLONG_MAX;
    }
    return bytes;
}

unsigned dcc_resource_class(const struct dcc_job *job)
{
    return 1 + (dcc_job_frontend(job) ? DCC_JOB_FRONTEND : job->kind) * DCC_MEMORY_BUCKETS +
           dcc_resource_bucket(dcc_resource_input_bytes(job));
}

int dcc_resource_known(unsigned memory_class)
{
    return !memory_learning() ||
        (memory_class < sizeof(class_memory) / sizeof(*class_memory) &&
         recent_peak(&class_memory[memory_class]));
}

static unsigned long long measured_job_bytes(unsigned long long peak)
{
    const unsigned long long overhead = 64ULL * 1024 * 1024;
    return peak <= (ULLONG_MAX - overhead) / 5 * 4 ?
        peak + peak / 4 + overhead : ULLONG_MAX;
}

void dcc_resource_observe(unsigned memory_class, unsigned long long peak_bytes, int success)
{
    if (peak_bytes > peak_job) peak_job = peak_bytes;
    if (!peak_bytes) return;
    sample_resources();
    observe_memory(&class_memory[0], peak_bytes, success);
    if (!success) return;
    if (samples < UINT_MAX) ++samples;
    if (memory_class && memory_class < sizeof(class_memory) / sizeof(*class_memory))
        observe_memory(&class_memory[memory_class], peak_bytes, success);
    else memory_class = 0;
    peak_bytes = recent_peak(&class_memory[memory_class]);
    /* Advertise the current workload, not a percentile still dominated by a
     * previous package. Admission separately charges every active class. */
    learned_job = measured_job_bytes(peak_bytes);
    if (learned_job < 256ULL * 1024 * 1024) learned_job = 256ULL * 1024 * 1024;
}

unsigned dcc_resource_samples(void) { return samples; }
unsigned long long dcc_resource_peak_bytes(void) { return peak_job; }

static unsigned long long configured_job_bytes(void)
{
    const char *setting = getenv("DISTCC_JOB_MEMORY");
    char *end;
    unsigned long long job = 1024;
    if (setting && *setting) {
        unsigned long long requested = strtoull(setting, &end, 10);
        if (!*end && requested >= 1 && requested <= 1048576) job = requested;
    }
    return job * 1024 * 1024;
}

unsigned long long dcc_resource_job_bytes(void)
{
    return memory_learning() && samples ? learned_job : configured_job_bytes();
}

unsigned long long dcc_resource_estimate(unsigned memory_class)
{
    if (memory_learning() && dcc_resource_known(memory_class)) {
        unsigned long long estimate = measured_job_bytes(recent_peak(&class_memory[memory_class]));
        return estimate < 256ULL * 1024 * 1024 ? 256ULL * 1024 * 1024 : estimate;
    }
    /* Unseen work retains the configured floor after a lightweight cohort. */
    unsigned long long estimate = configured_job_bytes();
    unsigned long long observed = measured_job_bytes(recent_peak(&class_memory[0]));
    if (memory_learning() && observed > estimate) estimate = observed;
    return estimate;
}

unsigned long long dcc_resource_spare_bytes(void)
{
    /* Each active job now carries its own class reservation. Charging the
     * lifetime maximum again would penalize unrelated small jobs indefinitely. */
    return dcc_resource_job_bytes() / 4;
}

static int resource_slots(int maximum, unsigned long long job,
                          unsigned long long spare, int learning)
{
    static int pressure_blocked;
    unsigned long long slots;
    sample_resources();
    if (pressure >= 10) pressure_blocked = 1;
    else if (pressure < 5) pressure_blocked = 0;
    const char *learn = getenv("DISTCC_MEMORY_LEARNING");
    if (!pressure_blocked && learning && learn && !strcmp(learn, "1") && samples &&
        (budget < job || budget - job < spare)) {
        /* The estimate already includes measurement headroom. A separate
         * host reserve must not strand one otherwise fitting measured job. */
        return maximum > 0 && available >= job && budget >= job ? 1 : 0;
    }
    /* PSI is reactive; keep headroom and reserve a budget before the burst.
     * Existing jobs finish normally when the admissible limit falls. */
    if (pressure_blocked || available < job || available - job < spare ||
        budget < job || budget - job < spare)
        return 0;
    slots = (budget - spare) / job;
    return slots < (unsigned)maximum ? (int)slots : maximum;
}

int dcc_resource_slots(int maximum)
{
    return resource_slots(maximum, dcc_resource_job_bytes(), dcc_resource_spare_bytes(), 1);
}

int dcc_resource_local_slots(int maximum)
{
    const char *setting = getenv("DISTCC_JOB_MEMORY");
    unsigned long long job = setting && *setting ? configured_job_bytes() : 256ULL * 1024 * 1024;
    return resource_slots(maximum, job, job / 4, 0);
}

int dcc_resource_class_slots(int maximum, unsigned memory_class,
                             unsigned long long reserved)
{
    if (maximum <= 0 || !dcc_resource_admit(memory_class, reserved)) return 0;
    unsigned long long job = dcc_resource_estimate(memory_class);
    unsigned long long spare = reserved ? dcc_resource_spare_bytes() : 0;
    unsigned long long free = budget - reserved;
    if (available < free) free = available;
    unsigned long long slots = (free - spare) / job;
    return slots < (unsigned)maximum ? (int)slots : maximum;
}

unsigned long long dcc_resource_headroom(unsigned long long reserved)
{
    sample_resources();
    unsigned long long free = budget > reserved ? budget - reserved : 0;
    if (available < free) free = available;
    /* Brokers use this only after projecting additional reservations, when
     * the host spare applies even if the observed worker was idle. */
    unsigned long long spare = dcc_resource_spare_bytes();
    return free > spare ? free - spare : 0;
}

int dcc_resource_admit(unsigned memory_class, unsigned long long reserved)
{
    unsigned long long job = dcc_resource_estimate(memory_class);
    /* One calibrated job already includes its own measurement headroom. An
     * extra host reserve must not prevent an otherwise fitting idle probe. */
    unsigned long long spare = reserved ? dcc_resource_spare_bytes() : 0;
    if (!resource_slots(1, job, spare, 0)) return 0;
    return reserved <= budget - job - spare;
}

static int analysis_memory_path(char path[PATH_MAX])
{
    const char *port = getenv("INCLUDE_SERVER_PORT");
    return port && snprintf(path, PATH_MAX, "%s.memory", port) < PATH_MAX;
}

int dcc_resource_analysis_slots(int maximum)
{
    char path[PATH_MAX];
    uint64_t peak = 0;
    /* The build-scoped pool publishes its observed resident+frontend peak.
     * Compilation and ordinary preprocessing continue using their own model. */
    if (analysis_memory_path(path)) {
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            struct flock lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
            if (fcntl(fd, F_SETLKW, &lock) || read(fd, &peak, sizeof(peak)) != sizeof(peak))
                peak = 0;
            close(fd);
        }
    }
    if (!peak) {
        /* One real request calibrates demand before enabling parallel scans.
         * Reserve the available budget; a full-compilation estimate must not
         * prevent smaller native analysis from ever collecting a sample. */
        sample_resources();
        unsigned long long initial = available < budget ? available : budget;
        return initial ? resource_slots(1, initial, 0, 0) : 0;
    }
    unsigned long long job = measured_job_bytes(peak);
    int slots = resource_slots(maximum, job, peak, 0);
    /* Persistent caches can leave room for one measured scan but not a second
     * peak as reserve. Keep the per-job margin and serialize that case. */
    return slots ? slots : resource_slots(1, job, 0, 0);
}

void dcc_resource_analysis_wait(int occupied, unsigned timeout_ms)
{
    struct pollfd ready = {.fd = -1, .events = POLLIN};
#ifdef __linux__
    char path[PATH_MAX];
    if (analysis_memory_path(path)) {
        ready.fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (ready.fd >= 0 && inotify_add_watch(ready.fd, path, IN_MODIFY | IN_DELETE_SELF) < 0) {
            close(ready.fd);
            ready.fd = -1;
        }
    }
#endif
    /* Install the watch before rechecking admission: calibration may finish
     * between releasing the rejected lease and starting this wait. The
     * timeout still covers unrelated changes in the client's memory budget. */
    if (occupied > dcc_resource_analysis_slots(dcc_resource_cpus()))
        poll(&ready, 1, timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);
    if (ready.fd >= 0) close(ready.fd);
}
