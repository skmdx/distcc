/* distcc adaptive host selection. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "distcc.h"
#include "hosts.h"
#include "lock.h"
#include "clinet.h"
#include "netutil.h"
#include "trace.h"
#include "exitcode.h"
#include "adaptive.h"

/* All phases of one HTTP probe share the same monotonic deadline. */
static int64_t milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int wait_socket(int fd, short events, int64_t deadline)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) return -1;
        int ret = poll(&p, 1, remaining);
        if (ret < 0 && errno == EINTR) continue;
        return ret > 0 ? 0 : -1;
    }
}
static int capacity_at(const struct sockaddr *address, socklen_t size,
                       int64_t deadline, struct dcc_capacity *cap)
{
    static const char query[] = "GET / HTTP/1.0\r\n\r\n";
    int fd = socket(address->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int ret = -1, error = 0;
    socklen_t len = sizeof(error);
    if (connect(fd, address, size) && (errno != EINPROGRESS ||
        wait_socket(fd, POLLOUT, deadline) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) ||
        error)) goto done;
    for (size_t sent = 0; sent < sizeof(query)-1;) {
        if (wait_socket(fd, POLLOUT, deadline)) goto done;
        ssize_t n = send(fd, query + sent, sizeof(query)-1-sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) goto done;
        sent += n;
    }
    char buf[DCC_STATS_SIZE];
    size_t used = 0;
    while (used < sizeof(buf)-1 && !wait_socket(fd, POLLIN, deadline)) {
        ssize_t n = read(fd, buf + used, sizeof(buf)-1-used);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 0) break;
        if (!n) { buf[used] = 0; ret = dcc_parse_capacity(buf, cap); break; }
        used += n;
    }
done:
    close(fd);
    return ret;
}

#ifdef __linux__
#include "tcp_job.h"
int dcc_capacity_update(struct dcc_capacity *cap, const struct dcc_load_report *r, int owned)
{
    unsigned id = dcc_tcp_get32(r->memory_class);
    uint64_t estimate = dcc_tcp_get64(r->estimate), typical = dcc_tcp_get64(r->job_memory);
    if (!id || id > DCC_JOB_KIND_COUNT * DCC_MEMORY_BUCKETS || !estimate || !typical ||
        dcc_tcp_get32(r->capacity) > 10000 || dcc_tcp_get32(r->busy) > 10000 ||
        dcc_tcp_get32(r->queued) > 10000 || dcc_tcp_get32(r->preparing) > 10000 ||
        dcc_tcp_get32(r->returning) > 10000 || dcc_tcp_get32(r->suspended) > 10000 ||
        dcc_tcp_get32(r->links) > 10000) return -1;
    int32_t running = dcc_tcp_get32(r->running), scale = dcc_tcp_get32(r->cpu_scale);
    int32_t pressure = dcc_tcp_get32(r->pressure);
    if (running < -1 || running > 1000000 || (scale != -1000 && (scale < 0 || scale > 1000)) ||
        (pressure != -1000 && (pressure < 0 || pressure > 100000))) return -1;
    int kind = (id - 1) / DCC_MEMORY_BUCKETS, bucket = (id - 1) % DCC_MEMORY_BUCKETS;
    unsigned kinds[] = {DCC_JOB_COMPILE, DCC_JOB_PUMP, DCC_JOB_RUST, DCC_JOB_THINLTO,
                        DCC_JOB_OFFLOAD_LINK, DCC_JOB_FRONTEND, DCC_JOB_RUST_CGU};
    cap->live_report = 1;
    cap->memory_headroom = dcc_tcp_get64(r->headroom);
    cap->memory_reserved = dcc_tcp_get64(r->reserved);
    cap->job_memory = typical;
    cap->capacity = dcc_tcp_get32(r->capacity);
    cap->busy = dcc_tcp_get32(r->busy);
    cap->queued = dcc_tcp_get32(r->queued);
    cap->preparing = dcc_tcp_get32(r->preparing);
    cap->returning = dcc_tcp_get32(r->returning);
    cap->suspended = dcc_tcp_get32(r->suspended);
    cap->links = dcc_tcp_get32(r->links);
    cap->running = running;
    if (cap->running >= 0) cap->running = cap->running > owned ? cap->running - owned : 0;
    cap->cpu_scale = scale / 1000.0;
    uint32_t cpu_state = dcc_tcp_get32(r->cpu_state), age = cpu_state & INT32_MAX;
    cap->cpu_age = age ? (age - 1) / 1000.0 : -1;
    cap->cpu_probe = cpu_state >> 31;
    cap->memory_pressure = pressure / 1000.0;
    cap->available = cap->capacity > cap->busy ? cap->capacity - cap->busy : 0;
    for (unsigned i = 0; i < sizeof(kinds)/sizeof(*kinds); ++i) {
        if (kinds[i] == (unsigned)kind) {
            cap->memory_estimates[i][bucket] = estimate;
            break;
        }
    }
    return 0;
}
#endif

int dcc_parse_capacity(char *buf, struct dcc_capacity *cap)
{
    char *line, extra;
    const unsigned long long cgu_capacity_field = 1ULL << 37, cgu_memory_field = 1ULL << 38;
    unsigned long long fields = 0;
    double value;
    *cap = (struct dcc_capacity){ .running = -1, .busy = -1, .queued = -1,
                                 .links = -1, .available = -1, .memory_pressure = -1,
                                 .preparing = -1, .returning = -1, .capacity = -1, .suspended = -1,
                                 .cpu_scale = -1, .cpu_age = -1 };
    if ((strncmp(buf, "HTTP/1.0 200 ", 13) &&
         strncmp(buf, "HTTP/1.1 200 ", 13)) ||
        !(line = strstr(buf, "\n<distccstats>\n")) ||
        !strstr(line, "\n</distccstats>"))
        return -1;

    for (line = strtok(line, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "dcc_completion_clock_ns ", 24)) {
            if (cap->report_stamp || sscanf(line + 24, "%llu %c", &cap->report_stamp, &extra) != 1)
                return -1;
        } else if (!strncmp(line, "dcc_rust_host ", 14)) {
            if (sscanf(line+14,"%95s %c",cap->rust_host,&extra)!=1) return -1;
        } else if (!strncmp(line, "dcc_llvm_cgu ", 13)) {
            if (strlen(line + 13) >= sizeof(cap->llvm_cgu)) return -1;
            strcpy(cap->llvm_cgu, line + 13);
        } else if (!strncmp(line, "dcc_cpus ", 9)) {
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
        } else if (!strncmp(line, "dcc_compile_capacity ", 21) ||
                   !strncmp(line, "dcc_pump_capacity ", 18) ||
                   !strncmp(line, "dcc_rust_capacity ", 18) ||
                   !strncmp(line, "dcc_thinlto_capacity ", 21) ||
                   !strncmp(line, "dcc_offload_capacity ", 21) || !strncmp(line, "dcc_frontend_capacity ", 22) ||
                   !strncmp(line, "dcc_cgu_capacity ", 17)) {
            int cgu = !strncmp(line, "dcc_cgu_capacity ", 17);
            int frontend = !strncmp(line, "dcc_frontend_capacity ", 22);
            int pump = !strncmp(line, "dcc_pump_capacity ", 18);
            int rust = !strncmp(line, "dcc_rust_capacity ", 18);
            int thinlto = !strncmp(line, "dcc_thinlto_capacity ", 21);
            int offload = !strncmp(line, "dcc_offload_capacity ", 21);
            unsigned long long field = frontend ? 1ULL << 34 : offload ? 1ULL << 32 : thinlto ? 536870912 : rust ? 2097152 : pump ? 1048576 : 524288;
            int *capacity = frontend ? cap->frontend_capacity : offload ? cap->offload_capacity : thinlto ? cap->thinlto_capacity : rust ? cap->rust_capacity : pump ? cap->pump_capacity : cap->compile_capacity;
            if (cgu) { field = cgu_capacity_field; capacity = cap->cgu_capacity; }
            char *item = strchr(line, ' ') + 1, *end;
            if (fields & field) return -1;
            for (unsigned i = 0; i < DCC_MEMORY_BUCKETS; ++i) {
                if ((*item < '0' || *item > '9') && strncmp(item, "-1", 2)) return -1;
                errno = 0;
                long slots = strtol(item, &end, 10);
                if (errno || slots < -1 || slots > 10000 ||
                    *end != (i + 1 == DCC_MEMORY_BUCKETS ? '\0' : ',')) return -1;
                capacity[i] = slots;
                item = end + 1;
            }
            fields |= field;
        } else if (!strncmp(line, "dcc_memory_headroom_bytes ", 26) ||
                   !strncmp(line, "dcc_memory_reserved_bytes ", 26) ||
                   !strncmp(line, "dcc_job_memory_bytes ", 21) ||
                   !strncmp(line, "dcc_compile_memory ", 19) ||
                   !strncmp(line, "dcc_pump_memory ", 16) ||
                   !strncmp(line, "dcc_rust_memory ", 16) ||
                   !strncmp(line, "dcc_thinlto_memory ", 19) ||
                   !strncmp(line, "dcc_offload_memory ", 19) ||
                   !strncmp(line, "dcc_frontend_memory ", 20) || !strncmp(line, "dcc_cgu_memory ", 15)) {
            const char *names[] = {"dcc_memory_headroom_bytes", "dcc_memory_reserved_bytes", "dcc_job_memory_bytes",
                                  "dcc_compile_memory", "dcc_pump_memory", "dcc_rust_memory", "dcc_thinlto_memory", "dcc_offload_memory", "dcc_frontend_memory", "dcc_cgu_memory"};
            unsigned long long *values[] = {&cap->memory_headroom, &cap->memory_reserved, &cap->job_memory,
                cap->memory_estimates[0], cap->memory_estimates[1], cap->memory_estimates[2], cap->memory_estimates[3], cap->memory_estimates[4], cap->memory_estimates[5], cap->memory_estimates[6]};
            unsigned row = 0;
            while (strncmp(line, names[row], strlen(names[row]))) row++;
            unsigned count = row < 3 ? 1 : DCC_MEMORY_BUCKETS;
            unsigned long long field = row == 8 ? 1ULL << 35 : row == 7 ? 1ULL << 33 : 4194304ULL << row;
            if (row == 9) field = cgu_memory_field;
            char *item = line + strlen(names[row]) + 1, *end;
            if (fields & field) return -1;
            for (unsigned i = 0; i < count; ++i) {
                if (*item < '0' || *item > '9') return -1;
                errno = 0;
                unsigned long long bytes = strtoull(item, &end, 10);
                if (errno || (row >= 2 && !bytes) ||
                    *end != (i + 1 == count ? '\0' : ',')) return -1;
                values[row][i] = bytes;
                item = end + 1;
            }
            fields |= field;
        } else if (!strncmp(line, "dcc_memory_pressure ", 20)) {
            if ((fields & 4096) || sscanf(line + 20, "%lf %c", &value, &extra) != 1 ||
                !(value >= -1 && value <= 100)) return -1;
            cap->memory_pressure = value;
            fields |= 4096;
        } else {
            static const char *names[] = {"dcc_running", "dcc_busy", "dcc_queued",
                                          "dcc_links", "dcc_admission", "dcc_available",
                                          "dcc_preparing", "dcc_returning", "dcc_capacity", "dcc_suspended"};
            int *values[] = {&cap->running, &cap->busy, &cap->queued,
                            &cap->links, &cap->admission, &cap->available,
                            &cap->preparing, &cap->returning, &cap->capacity, &cap->suspended};
            unsigned i;
            for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
                size_t len = strlen(names[i]);
                if (strncmp(line, names[i], len) || line[len] != ' ') continue;
                unsigned long long field = i == 9 ? 1ULL << 36 : 8ULL << i;
                if ((fields & field) || sscanf(line + len, "%lf %c", &value, &extra) != 1 ||
                    !(value >= -1 && value <= 1000000) || value != (int)value)
                    return -1;
                *values[i] = value;
                fields |= field;
            }
        }
    }
    const unsigned long long required = 7 | 8192 | 16384 | 32768 | 65536 | 131072 | 262144 |
        524288 | 1048576 | 2097152 | 536870912 | (127 * 4194304) | (1ULL << 32) | (1ULL << 33) |
        (1ULL << 34) | (1ULL << 35);
    const unsigned long long cgu_fields = cgu_capacity_field | cgu_memory_field;
    if (*cap->llvm_cgu && strcmp(cap->llvm_cgu, "-") && (fields & cgu_fields) != cgu_fields) return -1;
    return (fields & required) == required && cap->cores <= cap->cpus &&
           ((cap->cpu_scale == -1 && cap->cpu_age == -1 && !cap->cpu_probe) ||
            (cap->cpu_scale > 0 && cap->cpu_age >= 0)) ? 0 : -1;
}

/* NSS can block past the deadline. Only hostname probes need a resolver
 * thread. It owns its request and socket even if the caller has timed out. */
struct name_query { int fd, port; char name[]; };
static void *resolve_capacity(void *argument)
{
    struct name_query *query = argument;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV }, *addresses;
    char port[16];
    snprintf(port, sizeof(port), "%d", query->port);
    if (!getaddrinfo(query->name, port, &hints, &addresses)) {
        for (struct addrinfo *a = addresses; a; a = a->ai_next)
            if (send(query->fd, a->ai_addr, a->ai_addrlen, MSG_NOSIGNAL) < 0) break;
        freeaddrinfo(addresses);
    }
    close(query->fd);
    free(query);
    return NULL;
}
static int query_capacity(const struct dcc_hostdef *host, struct dcc_capacity *cap)
{
    int64_t deadline = milliseconds() + 250;
    struct sockaddr_in v4 = { .sin_family = AF_INET, .sin_port = htons(host->stats_port) };
    struct sockaddr_in6 v6 = { .sin6_family = AF_INET6, .sin6_port = htons(host->stats_port) };
    if (inet_pton(AF_INET, host->hostname, &v4.sin_addr) == 1)
        return capacity_at((struct sockaddr *)&v4, sizeof(v4), deadline, cap);
    if (inet_pton(AF_INET6, host->hostname, &v6.sin6_addr) == 1)
        return capacity_at((struct sockaddr *)&v6, sizeof(v6), deadline, cap);
    int pair[2], ret = -1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return -1;
    struct name_query *query = malloc(sizeof(*query) + strlen(host->hostname) + 1);
    if (query) {
        query->fd = pair[1]; query->port = host->stats_port;
        strcpy(query->name, host->hostname);
        pthread_t thread;
        pthread_attr_t attr;
        int started = 0;
        if (!pthread_attr_init(&attr)) {
            size_t stack = 128U * 1024;
            if (stack < (size_t)PTHREAD_STACK_MIN) stack = PTHREAD_STACK_MIN;
            started = !pthread_attr_setstacksize(&attr, stack) &&
                      !pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) &&
                      !pthread_create(&thread, &attr, resolve_capacity, query);
            pthread_attr_destroy(&attr);
        }
        if (started) {
            while (wait_socket(pair[0], POLLIN, deadline) == 0) {
                struct sockaddr_storage address;
                ssize_t n = recv(pair[0], &address, sizeof(address), MSG_DONTWAIT);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                ret = capacity_at((struct sockaddr *)&address, n, deadline, cap);
                if (!ret) break;
            }
            close(pair[0]);
            return ret;
        }
        free(query);
    }
    close(pair[0]); close(pair[1]);
    return ret;
}

/* Reuse the per-user lock namespace for a one-second, cross-process cache.
 * Failed probes are cached too, but stale successful metrics are never used. */
static int cached_capacity(const struct dcc_hostdef *host, struct dcc_capacity *cap)
{
    struct dcc_hostdef endpoint = *host;
    struct { time_t stamp; int valid; struct dcc_capacity capacity; } cached = {0};
    int fd;

    endpoint.port = host->stats_port;
    if (dcc_lock_host("stats3", &endpoint, 0, 1, &fd)) return -1;
    if (read(fd, &cached, sizeof(cached)) == sizeof(cached) &&
        cached.stamp == time(NULL) && (cached.valid == 0 || cached.valid == 1)) {
        *cap = cached.capacity;
        dcc_unlock(fd);
        return cached.valid ? 0 : -1;
    }
    memset(cap, 0, sizeof(*cap));
    cached.valid = query_capacity(host, cap) == 0;
    if (!cached.valid)
        rs_log_warning("adaptive stats unavailable for %s; using configured slots/speed",
                       host->hostname);
    cached.stamp = time(NULL);
    cached.capacity = *cap;
    /* Cache the parsed snapshot, including per-class memory capacities. The
     * user-owned locked file is transient state, not a second stats parser. */
    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) ||
        write(fd, &cached, sizeof(cached)) != sizeof(cached))
        rs_log_warning("could not cache adaptive stats");
    dcc_unlock(fd);
    return cached.valid ? 0 : -1;
}

/* The configured limit is a ceiling, not an estimate of CPU performance.
 * Without an explicit limit, stats-enabled hosts use their advertised CPUs. */
int dcc_host_slots(const struct dcc_hostdef *host)
{
    struct dcc_capacity cap;
    return dcc_capacity_parallelism(host,
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

int dcc_capacity_parallelism(const struct dcc_hostdef *host, const struct dcc_capacity *cap)
{
    /* Build tools sample this once. A cold memory estimate or temporary
     * pressure must not cap the whole build after capacity recovers. TCP
     * jobs wait in the broker and still pass live per-class memory admission
     * before preparation and execution. Keep the configured CPU ceiling. */
    return host->tcp_jobs ? dcc_capacity_slots(host, cap) : dcc_capacity_jobs(host, cap);
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
