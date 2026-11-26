/* Scheduling state and decisions shared with the broker event loop. */
#pragma once
#include <sys/socket.h>
#include "adaptive.h"
#define MAX_HOSTS 128
#define MAX_CLIENTS DCC_BROKER_CLIENT_MAX
#include "lane_schedule.h"
struct schedule {
    struct lane_schedule cpu, leases;
    int waiting;
};
struct peer {
    struct schedule schedule;
    struct dcc_hostdef *host;
    struct sockaddr_storage address;
    socklen_t address_len;
    struct dcc_capacity metrics;
    double valid_until, retry_at, deadline;
    int busy, stats_fd, stats_reading;
    uint64_t serial, probe_serial, observed_serial;
    int completed;
    double full_until, link_until, probe_after;
    int idle_head; /* Pool indices are stored plus one. */
    int pool_supported;
    int admission_supported;
    int local, direct;
    size_t used;
    char stats[DCC_STATS_SIZE];
    /* Seconds/byte for prepared network input; seconds/job otherwise. */
    double prepare_cost[DCC_WORK_CLASSES];
    double mean_cost;
    uint64_t scopes[16], report_stamp;
    /* A collision suppresses speculative transmission. Keep this independent
     * of timings, which are unavailable for preprocessor-only requests. */
    unsigned next_scope;
    double report_until;
};
enum client_state { WAITING, CONNECTING, HELLO_SEND, HELLO_READ,
                    RESERVE_SEND, RESERVE_READ, ACTIVE };
struct client {
    struct client *session_next;
    unsigned tag;
    int session;
    int fd, net, peer, cpp_reserved, direct;
    unsigned flags;
    uint64_t serial;
    enum client_state state;
    uint32_t command, local_limit;
    double deadline;
    int pooled;
    int reused;
    int admission;
    size_t used;
    char hello[12];
    struct dcc_work_hint hint;
    double cost, ready_at, run_at, factor, waiting_since;
};
struct broker_schedule {
    struct peer peers[MAX_HOSTS];
    struct client clients[MAX_CLIENTS];
    int count, tcp_jobs, cpp_busy, cpp_limit;
    uint64_t direct_completed, direct_prepared_completed;
    double local_prepare[DCC_WORK_CLASSES];
    struct dcc_work_model model;
};
struct prediction { double start, prepare, factor, finish; };
/* Rebuilt once per dispatch pass. A waiting job reserves a forecast position,
 * not a server slot: later jobs must not all wait for the same fast CPU. */
struct occupancy {
    int recent, links;
    uint64_t pending, new_memory;
};
struct dispatch_batch {
    unsigned count;
    struct occupancy peers[MAX_HOSTS];
    const struct client *owners[2 * MAX_CLIENTS], *scopes[2 * MAX_CLIENTS];
};
struct waiting_job { struct client *client; double cost, age; };
struct placement {
    int peer, direct, probe, local_cpp;
    double cost;
    struct prediction forecast;
};
enum decision { HOLD_JOB, ANSWER, LOCAL_SLOT, ASSIGN_JOB };
int schedule_batch(struct broker_schedule *, struct dispatch_batch *, struct waiting_job *, double now, int probes_pending);
enum decision schedule_request(struct broker_schedule *, struct client *, double now, struct dispatch_batch *,
                               struct reply *, struct placement *);
void schedule_assign(struct broker_schedule *, struct client *, struct dispatch_batch *, const struct placement *, double now);
void observe_local_prepare(struct broker_schedule *, unsigned kind, double sample);
void observe(struct broker_schedule *, struct client *, const struct feedback *, double now);
