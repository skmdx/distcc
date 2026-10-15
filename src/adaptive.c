/* distcc adaptive host selection. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "distcc.h"
#include "hosts.h"
#include "lock.h"
#include "clinet.h"
#include "netutil.h"
#include "trace.h"
#include "exitcode.h"
#include "adaptive.h"

/* Parse complete stats responses only. Missing, nonfinite or out-of-range
 * metrics cannot silently turn into a high-capacity host. */
static int read_capacity(int fd, struct dcc_capacity *cap)
{
    char buf[4096];
    size_t used = 0;
    ssize_t n;

    while (used < sizeof(buf) - 1) {
        n = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (!n)
            break;
        used += n;
    }
    buf[used] = '\0';
    return used == sizeof(buf) - 1 ? -1 : dcc_parse_capacity(buf, cap);
}

int dcc_parse_capacity(char *buf, struct dcc_capacity *cap)
{
    char *line, extra;
    int fields = 0;
    double value;
    *cap = (struct dcc_capacity){ .running = -1, .busy = -1, .queued = -1,
                                 .links = -1, .available = -1, .memory_pressure = -1,
                                 .preparing = -1, .returning = -1, .capacity = -1,
                                 .cpu_scale = -1, .cpu_age = -1 };
    if ((strncmp(buf, "HTTP/1.0 200 ", 13) &&
         strncmp(buf, "HTTP/1.1 200 ", 13)) ||
        !(line = strstr(buf, "\n<distccstats>\n")) ||
        !strstr(line, "\n</distccstats>"))
        return -1;

    for (line = strtok(line, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "dcc_cpus ", 9)) {
            if ((fields & 1) || sscanf(line + 9, "%lf %c", &value, &extra) != 1 ||
                !(value >= 1 && value <= 10000) || value != (int)value)
                return -1;
            cap->cpus = value;
            fields |= 1;
        } else if (!strncmp(line, "dcc_cores ", 10)) {
            if ((fields & 8192) || sscanf(line + 10, "%lf %c", &value, &extra) != 1 ||
                !(value >= 1 && value <= 10000) || value != (int)value)
                return -1;
            cap->cores = value;
            fields |= 8192;
        } else if (!strncmp(line, "dcc_core_scale ", 15)) {
            if ((fields & 32768) || sscanf(line + 15, "%lf %c", &value, &extra) != 1 ||
                !(value >= 0 && value <= 1)) return -1;
            cap->core_scale = value;
            fields |= 32768;
        } else if (!strncmp(line, "dcc_smt_gain ", 13)) {
            if ((fields & 16384) || sscanf(line + 13, "%lf %c", &value, &extra) != 1 ||
                !(value >= -1 && value <= 1)) return -1;
            cap->smt_gain = value;
            fields |= 16384;
        } else if (!strncmp(line, "dcc_cpu_scale ", 14)) {
            if ((fields & 65536) || sscanf(line + 14, "%lf %c", &value, &extra) != 1 ||
                !(value == -1 || (value > 0 && value <= 1))) return -1;
            cap->cpu_scale = value;
            fields |= 65536;
        } else if (!strncmp(line, "dcc_cpu_age ", 12)) {
            if ((fields & 131072) || sscanf(line + 12, "%lf %c", &value, &extra) != 1 ||
                !(value == -1 || (value >= 0 && value <= 1e9))) return -1;
            cap->cpu_age = value;
            fields |= 131072;
        } else if (!strncmp(line, "dcc_cpu_probe ", 14)) {
            if ((fields & 262144) || sscanf(line + 14, "%lf %c", &value, &extra) != 1 ||
                !(value == 0 || value == 1)) return -1;
            cap->cpu_probe = value;
            fields |= 262144;
        } else if (!strncmp(line, "dcc_max_kids ", 13)) {
            if ((fields & 2) || sscanf(line + 13, "%lf %c", &value, &extra) != 1 ||
                !(value >= 1 && value <= 10000) || value != (int)value)
                return -1;
            cap->jobs = value;
            fields |= 2;
        } else if (!strncmp(line, "dcc_load1 ", 10)) {
            if ((fields & 4) || sscanf(line + 10, "%lf %c", &value, &extra) != 1 ||
                !(value >= 0 && value <= 1e9))
                return -1;
            cap->load = value;
            fields |= 4;
        } else if (!strncmp(line, "dcc_memory_pressure ", 20)) {
            if ((fields & 4096) || sscanf(line + 20, "%lf %c", &value, &extra) != 1 ||
                !(value >= -1 && value <= 100)) return -1;
            cap->memory_pressure = value;
            fields |= 4096;
        } else {
            static const char *names[] = {"dcc_running", "dcc_busy", "dcc_queued",
                                          "dcc_links", "dcc_admission", "dcc_available",
                                          "dcc_preparing", "dcc_returning", "dcc_capacity"};
            int *values[] = {&cap->running, &cap->busy, &cap->queued,
                            &cap->links, &cap->admission, &cap->available,
                            &cap->preparing, &cap->returning, &cap->capacity};
            unsigned i;
            for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
                size_t len = strlen(names[i]);
                if (strncmp(line, names[i], len) || line[len] != ' ') continue;
                if ((fields & (8 << i)) || sscanf(line + len, "%lf %c", &value, &extra) != 1 ||
                    !(value >= -1 && value <= 1000000) || value != (int)value)
                    return -1;
                *values[i] = value;
                fields |= 8 << i;
            }
        }
    }
    const int required = 7 | 8192 | 16384 | 32768 | 65536 | 131072 | 262144;
    return (fields & required) == required && cap->cores <= cap->cpus &&
           ((cap->cpu_scale == -1 && cap->cpu_age == -1 && !cap->cpu_probe) ||
            (cap->cpu_scale > 0 && cap->cpu_age >= 0)) ? 0 : -1;
}

/* Isolate the existing resolver and socket code so the entire probe, including
 * DNS and a stalled HTTP peer, has a 250ms deadline. Always reap the worker. */
static int query_capacity(const struct dcc_hostdef *host, struct dcc_capacity *cap)
{
    int channel[2], fd, ret = -1, status;
    pid_t pid;
    fd_set fds;
    struct timeval timeout = {0, 250000};
    const char request[] = "GET / HTTP/1.0\r\n\r\n";

    if (pipe(channel))
        return -1;
    pid = fork();
    if (pid == 0) {
        close(channel[0]);
        if (!dcc_connect_by_name(host->hostname, host->stats_port, &fd)) {
            dcc_set_blocking(fd);
            if (!dcc_writex(fd, request, sizeof(request) - 1) &&
                !read_capacity(fd, cap)) {
                /* One small atomic pipe write, before closing the socket. */
                if (write(channel[1], cap, sizeof(*cap)) != sizeof(*cap))
                    _exit(1);
            }
            close(fd);
        }
        _exit(0);
    }
    close(channel[1]);
    if (pid > 0) {
        FD_ZERO(&fds);
        FD_SET(channel[0], &fds);
        if (select(channel[0] + 1, &fds, NULL, NULL, &timeout) > 0 &&
            read(channel[0], cap, sizeof(*cap)) == sizeof(*cap))
            ret = 0;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    close(channel[0]);
    return ret;
}

/* Reuse the per-user lock namespace for a one-second, cross-process cache.
 * Failed probes are cached too, but stale successful metrics are never used. */
static int cached_capacity(const struct dcc_hostdef *host, struct dcc_capacity *cap)
{
    struct dcc_hostdef endpoint = *host;
    char buf[384];
    long stamp;
    time_t now;
    int fd, valid, len;
    ssize_t n;

    endpoint.port = host->stats_port;
    if (dcc_lock_host("stats3", &endpoint, 0, 1, &fd))
        return -1;
    now = time(NULL);
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (sscanf(buf, "%ld %d %d %d %lf %d %d %d %d %d %lf %d %d %d %d %d %lf %lf %lf %lf %d", &stamp, &valid,
                   &cap->cpus, &cap->jobs, &cap->load, &cap->running, &cap->busy,
                   &cap->queued, &cap->links, &cap->admission, &cap->memory_pressure, &cap->available,
                   &cap->preparing, &cap->returning, &cap->capacity,
                   &cap->cores, &cap->core_scale, &cap->smt_gain,
                   &cap->cpu_scale, &cap->cpu_age, &cap->cpu_probe) == 21 &&
            stamp == (long)now &&
            (valid == 0 || (valid == 1 && cap->cpus > 0 &&
             cap->cpus <= 10000 && cap->jobs > 0 && cap->jobs <= 10000 &&
             cap->load >= 0 && cap->load <= 1e9 && cap->running >= -1 &&
             cap->running <= 1000000 && cap->busy >= -1 && cap->busy <= 1000000 &&
             cap->queued >= -1 && cap->queued <= 1000000 && cap->links >= -1 &&
             cap->links <= 1000000 && cap->admission >= -1 && cap->admission <= 1000000 &&
             cap->memory_pressure >= -1 && cap->memory_pressure <= 100 &&
             cap->available >= -1 && cap->available <= 1000000 &&
             cap->preparing >= -1 && cap->preparing <= 1000000 &&
             cap->returning >= -1 && cap->returning <= 1000000 &&
             cap->capacity >= -1 && cap->capacity <= 1000000 &&
             cap->cores >= 1 && cap->cores <= cap->cpus &&
             cap->core_scale >= 0 && cap->core_scale <= 1 &&
             cap->smt_gain >= -1 && cap->smt_gain <= 1 &&
             ((cap->cpu_scale == -1 && cap->cpu_age == -1 && !cap->cpu_probe) ||
              (cap->cpu_scale > 0 && cap->cpu_scale <= 1 &&
               cap->cpu_age >= 0 && cap->cpu_age <= 1e9 &&
               (cap->cpu_probe == 0 || cap->cpu_probe == 1)))))) {
            dcc_unlock(fd);
            return valid ? 0 : -1;
        }
    }
    memset(cap, 0, sizeof(*cap));
    valid = query_capacity(host, cap) == 0;
    if (!valid)
        rs_log_warning("adaptive stats unavailable for %s; using configured slots/speed",
                       host->hostname);
    len = snprintf(buf, sizeof(buf), "%ld %d %d %d %.17g %d %d %d %d %d %.2f %d %d %d %d %d %.17g %.17g %.17g %.17g %d\n",
                   (long)time(NULL), valid, cap->cpus, cap->jobs, cap->load,
                   cap->running, cap->busy, cap->queued, cap->links, cap->admission,
                   cap->memory_pressure, cap->available, cap->preparing, cap->returning,
                   cap->capacity, cap->cores, cap->core_scale, cap->smt_gain,
                   cap->cpu_scale, cap->cpu_age, cap->cpu_probe);
    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) ||
        write(fd, buf, len) != len)
        rs_log_warning("could not cache adaptive stats");
    dcc_unlock(fd);
    return valid ? 0 : -1;
}

/* The configured limit is a ceiling, not an estimate of CPU performance.
 * Without an explicit limit, stats-enabled hosts use their advertised CPUs. */
int dcc_host_slots(const struct dcc_hostdef *host)
{
    struct dcc_capacity cap;
    return dcc_capacity_jobs(host,
        host->is_up && host->n_slots > 0 && host->stats_port &&
        !cached_capacity(host, &cap) ? &cap : NULL);
}

int dcc_capacity_slots(const struct dcc_hostdef *host, const struct dcc_capacity *cap)
{
    int slots = host->is_up ? host->n_slots : 0;
    if (slots > 10000) slots = 10000;
    if (slots > 0 && cap) {
        if (!host->slots_specified || slots > cap->cpus) slots = cap->cpus;
        if (slots > cap->jobs) slots = cap->jobs;
    }
    return slots;
}

/* Build parallelism reflects sustainable concurrency, not momentary vacancy.
 * Keep admission slots separate so a temporarily zero capacity can recover. */
int dcc_capacity_jobs(const struct dcc_hostdef *host, const struct dcc_capacity *cap)
{
    int slots = dcc_capacity_slots(host, cap);
    if (cap && cap->capacity >= 0 && slots > cap->capacity)
        slots = cap->capacity;
    return slots;
}

/* Estimate per-job service time relative to an idle single-job compile. */
static double cpu_service(double demand, int limit, const struct dcc_capacity *cap)
{
    int cores = cap ? cap->cores : limit;
    double active = demand < limit ? demand : limit;
    double physical = cap ? cores * cap->core_scale : cores;
    if (physical < 1) physical = 1;
    /* Interpolate measured throughput from one job to one job/core, then
     * through the SMT range. Negative SMT gain represents measured losses
     * from sharing execution resources or reducing sustained clock speed. */
    double capacity = active <= 1 ? active : active <= cores ?
                      1 + (active - 1) * (physical - 1) / (cores - 1) :
                      physical + (active - cores) * cap->smt_gain * cap->core_scale;
    if (capacity < 1) capacity = 1;
    return demand > capacity && capacity > 0 ? demand / capacity : 1;
}

/* Global occupancy overlaps local leases: take the maximum, never add both
 * counts. Admission on the server closes the race between brokers. */
double dcc_capacity_score(const struct dcc_hostdef *host,
                          const struct dcc_capacity *cap, int busy)
{
    int slots = dcc_capacity_slots(host, cap), occupied = busy;
    int parallelism = cap ? (cap->cpus < cap->jobs ? cap->cpus : cap->jobs) : slots;
    /* Physical cores provide independent execution. Additional SMT threads
     * add only part of a core's throughput; beyond admission, work queues. */
    double service = 1, demand;
    if (cap) {
        int remote = (cap->busy > 0 ? cap->busy : 0) + (cap->queued > 0 ? cap->queued : 0);
        if (remote > occupied) occupied = remote;
        if (cap->capacity >= 0 && cap->capacity < parallelism) parallelism = cap->capacity;
    }
    /* A client's /LIMIT restricts its own leases, not other clients' CPUs. */
    demand = cpu_service(occupied + 1.0, parallelism > 0 ? parallelism : 1, cap);
    if (demand > service) service = demand;
    if (cap) {
        /* Runnable load includes compiler work: take the larger contention
         * estimate, never add it to reservations or multiply both penalties. */
        demand = cpu_service((cap->running >= 0 ? cap->running : cap->load) + 1,
                             cap->cpus, cap);
        if (demand > service) service = demand;
        if (cap->memory_pressure > 0) service *= 1 + cap->memory_pressure / 100;
        /* Apply after the contention floor so even one job can slow down.
         * The ratio is relative to the calibrated clock at the observed load. */
        if (cap->cpu_scale > 0) service /= cap->cpu_scale;
    }
    return service / (host->speed ? host->speed : 1);
}

struct candidate {
    struct dcc_hostdef *host;
    int slots;
    struct dcc_capacity metrics;
    const struct dcc_capacity *cap;
};

int dcc_lock_adaptive(struct dcc_hostdef *hosts,
                      struct dcc_hostdef **buildhost, int *cpu_lock_fd)
{
    struct dcc_hostdef *h;
    struct candidate *candidates;
    int count = 0, i, slot, busy, first, fd, candidate_fd, scheduler_fd;
    int best_fd = -1, best_slot = 0, ret = EXIT_BUSY;
    double score, best_score = 0, best_fill = 0;

    for (h = hosts; h; h = h->next)
        count++;
    candidates = calloc(count, sizeof(*candidates));
    if (!candidates)
        return EXIT_OUT_OF_MEMORY;

    for (h = hosts, i = 0; h; h = h->next, i++) {
        struct candidate *c = &candidates[i];
        c->host = h;
        c->cap = h->is_up && h->n_slots > 0 && h->stats_port &&
                 !cached_capacity(h, &c->metrics) ? &c->metrics : NULL;
        c->slots = dcc_capacity_slots(h, c->cap);
    }

    /* Serialize only slot inspection/reservation, never network probes.
     * Existing cpu locks continue to enforce limits against legacy clients. */
    ret = dcc_lock_host("schedule", dcc_hostdef_local, 0, 1, &scheduler_fd);
    if (ret)
        goto done;
    ret = EXIT_BUSY;
    for (i = 0; i < count; i++) {
        struct candidate *c = &candidates[i];
        int previous;
        /* fcntl locks belong to the process: reopening/closing a duplicate
         * endpoint's slot would release our retained reservation. First wins. */
        for (previous = 0; previous < i; previous++) {
            struct dcc_hostdef *p = candidates[previous].host;
            if (p->mode == c->host->mode &&
                (p->mode == DCC_MODE_LOCAL ||
                 (!strcmp(p->hostname, c->host->hostname) &&
                  (p->mode != DCC_MODE_TCP || p->port == c->host->port))))
                break;
        }
        if (previous < i)
            continue;
        if (c->cap && (c->cap->available == 0 || c->cap->capacity == 0 ||
                       c->cap->memory_pressure >= 10)) continue;
        busy = 0;
        first = -1;
        candidate_fd = -1;
        for (slot = 0; slot < c->slots; slot++) {
            int locked = dcc_lock_host("cpu", c->host, slot, 0, &fd);
            if (locked == EXIT_BUSY) {
                busy++;
            } else if (locked) {
                ret = locked;
                if (candidate_fd >= 0)
                    dcc_unlock(candidate_fd);
                goto unlock;
            } else if (first < 0) {
                first = slot;
                candidate_fd = fd;
            } else {
                dcc_unlock(fd);
            }
        }
        if (first < 0)
            continue;
        score = dcc_capacity_score(c->host, c->cap, busy);
        if (c->cap && c->cap->cpu_probe && !busy && !c->cap->busy && !c->cap->queued)
            score = 0;
        if (best_fd < 0 || score < best_score ||
            (score == best_score && (busy + 1.0) / c->slots < best_fill)) {
            if (best_fd >= 0)
                dcc_unlock(best_fd);
            best_fd = candidate_fd;
            best_score = score;
            best_fill = (busy + 1.0) / c->slots;
            best_slot = first;
            *buildhost = c->host;
        } else {
            dcc_unlock(candidate_fd);
        }
    }
    if (best_fd >= 0) {
        *cpu_lock_fd = best_fd;
        dcc_note_state_slot(best_slot,
                           (*buildhost)->mode == DCC_MODE_LOCAL ? DCC_LOCAL : DCC_REMOTE);
        rs_trace("adaptive selected %s slot %d score %.6g",
                 (*buildhost)->hostdef_string, best_slot, best_score);
        best_fd = -1;
        ret = 0;
    }
unlock:
    if (best_fd >= 0)
        dcc_unlock(best_fd);
    dcc_unlock(scheduler_fd);
done:
    free(candidates);
    return ret;
}
