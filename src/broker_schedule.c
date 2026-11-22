/* Broker workload learning, capacity projection and placement. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#ifdef __linux__
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "distcc.h"
#include "hosts.h"
#include "broker.h"
#include "broker_schedule.h"
#include "tcp_job.h"
#include "trace.h"
#include "exitcode.h"

static int effective_lanes(const struct dcc_capacity *cap)
{
    int lanes = cap->cpus < cap->jobs ? cap->cpus : cap->jobs;
    return cap->capacity >= 0 && cap->capacity < lanes ? cap->capacity : lanes;
}

static struct dcc_capacity observed_capacity(const struct peer *peer, int cpp)
{
    struct dcc_capacity cap = peer->metrics;
    if (cap.running >= 0) cap.running += peer->direct + (peer->local ? cpp : 0);
    if (cap.busy >= 0) cap.busy += peer->direct;
    return cap;
}

/* Service time while executing, excluding the queue predicted below. Replace
 * this daemon's runnable compilers with the projected execution concurrency;
 * external CPU demand remains in the existing contention calculation. */
static double execution_factor(const struct peer *peer, int work, int recovering, int capacity, int cpp)
{
    struct dcc_capacity cap = observed_capacity(peer, cpp);
    cap.capacity = capacity;
    if (recovering) cap.cpu_scale = 1;
    int parallel = effective_lanes(&cap);
    if (parallel < 1) parallel = 1;
    int executing = work < parallel ? work + 1 : parallel;
    if (cap.running >= 0) {
        cap.running -= cap.busy > 0 ? cap.busy : 0;
        if (cap.running < 0) cap.running = 0;
        cap.running += executing - 1;
        if (cap.running < 0) cap.running = 0;
    } else {
        cap.load -= cap.busy > 0 ? cap.busy : 0;
        if (cap.load < 0) cap.load = 0;
        cap.load += executing - 1;
    }
    cap.busy = executing - 1;
    cap.queued = 0;
    return dcc_capacity_score(peer->host, &cap, executing - 1);
}

static void lease_add(struct lane_schedule *leases, double finish)
{
    lane_add(leases, ++leases->count, finish);
}

struct residual { double left, ready; uint64_t serial; };
static int longest_first(const void *a, const void *b)
{
    const struct residual *x = a, *y = b;
    return (x->left < y->left) - (x->left > y->left);
}
static int oldest_first(const void *a, const void *b)
{
    const struct residual *x = a, *y = b;
    return (x->serial > y->serial) - (x->serial < y->serial);
}
static int bulk_input(const struct peer *peer, const struct dcc_work_hint *hint)
{
    /* Local inputs normally pass a sealed FD. Only prepared sources and offload
     * inputs describe the payload before a remote transfer. */
    return !peer->local && (hint->kind <= 3 || hint->kind == DCC_WORK_OFFLOAD);
}

struct peer_view {
    struct dcc_capacity metrics;
    int fresh, slots, capacity, lanes, pipeline, inflight, blocked, link_busy, memory_full, local_exec, work;
};

static struct prediction predict(struct broker_schedule *broker, const struct client *client,
                                  int peer_index, const struct peer_view *view, double cost, double now, int recovering)
{
    struct peer *peer = &broker->peers[peer_index];
    int work = view->work, capacity = view->metrics.capacity;
    struct schedule *s = &peer->schedule;
    int waiting = s->waiting;
    struct prediction result = {.factor = execution_factor(peer, work + waiting, recovering, capacity, broker->cpp_busy)};
    int bulk = bulk_input(peer, &client->hint);
    double prepare = peer->prepare_cost[client->hint.kind - 1];
    if (!prepare) prepare = bulk ? 1.0 / (64 * 1048576) : peer->local ? .001 : .005;
    result.prepare = bulk ? client->hint.bytes * prepare + .005 : prepare;
    if (client->command == TRY_LOCAL) {
        double local_prepare = broker->local_prepare[client->hint.kind - 1];
        if (!local_prepare) local_prepare = .001 + client->hint.bytes / (256.0 * 1048576);
        result.prepare = peer->local ? 0 : result.prepare + local_prepare;
    }
    if (!s->cpu.count) {
        int lanes = view->lanes;
        if (lanes < 1) lanes = 1;
        if (lanes > MAX_CLIENTS) lanes = MAX_CLIENTS;
        s->cpu.count = lanes;
        s->cpu.root = s->leases.root = 0;
        /* The client lease limit may grow for jobs already returning. Retain
         * all live release times so each request can query its actual limit. */
        s->leases.count = 0;
        struct residual jobs[MAX_CLIENTS];
        int count = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            const struct client *c = &broker->clients[i];
            if (c->fd < 0 || c->peer != peer_index || c->cost <= 0 || c->factor <= 0) continue;
            double elapsed = now > c->run_at ? now - c->run_at : 0;
            double left = (c->cost - elapsed / c->factor) * result.factor;
            jobs[count++] = (struct residual){left > 0 ? left : elapsed,
                c->ready_at > now ? c->ready_at - now : 0, c->serial};
        }
        int unknown = work > count ? work - count : 0;
        double average = (peer->mean_cost > 0 ? peer->mean_cost : 1) * result.factor;
        for (int i = 0; i < lanes; ++i) {
            int rounds = (unknown + i) / lanes;
            lane_add(&s->cpu, i + 1, rounds * average);
        }
        for (int i = count; i < peer->busy; ++i) lease_add(&s->leases, average);
        /* Retain the longest plausible residuals for CPUs reported busy.
         * Leases returning artifacts still occupy the client connection limit. */
        if (count > work) {
            qsort(jobs, count, sizeof(*jobs), longest_first);
            while (count > work) {
                const struct residual *r = &jobs[--count];
                lease_add(&s->leases, r->left + r->ready);
            }
        }
        qsort(jobs, count, sizeof(*jobs), oldest_first);
        for (int i = 0; i < count; ++i) {
            double ready = lane_ready(&s->cpu, MAX_CLIENTS);
            double finish = (jobs[i].ready > ready ? jobs[i].ready : ready) + jobs[i].left;
            lane_finish(&s->cpu, finish);
            lease_add(&s->leases, finish);
        }
    }
    result.start = result.prepare;
    if (s->leases.count >= (unsigned)view->inflight) result.start += lane_ready(&s->leases, view->inflight);
    double cpu_ready = lane_ready(&s->cpu, capacity > 0 ? (unsigned)capacity : s->cpu.count);
    if (result.start < cpu_ready) result.start = cpu_ready;
    result.finish = result.start + cost * result.factor;
    return result;
}

void observe_local_prepare(struct broker_schedule *broker, unsigned kind, double sample)
{
    if (sample > 0 && sample < 86400) {
        double *estimate = &broker->local_prepare[kind - 1];
        *estimate = *estimate ? .875 * *estimate + .125 * sample : sample;
    }
}

void observe(struct broker_schedule *broker, struct client *client, const struct feedback *timing, double now)
{
    if (!timing->execute_ns || client->factor <= 0 ||
        timing->execute_ns > 86400ULL * 1000000000 ||
        timing->prepare_ns > 86400ULL * 1000000000 ||
        timing->queue_ns > 86400ULL * 1000000000) {
        rs_trace("unmeasured return: key=%llx client=%td request=%.6f execute_ns=%llu factor=%.6f",
                 (unsigned long long)client->hint.key, client - broker->clients, client->waiting_since,
                 (unsigned long long)timing->execute_ns, client->factor);
        return;
    }
    struct peer *peer = &broker->peers[client->peer];
    if (client->hint.scope_key) {
        unsigned i = 0;
        while (i < 16 && peer->scopes[i] != client->hint.scope_key) ++i;
        if (i == 16) peer->scopes[peer->next_scope++ % 16] = client->hint.scope_key;
    }
    double cost = timing->execute_ns / 1e9 / client->factor;
    /* Both direct compilation and remote jobs train the input-cost model. */
    dcc_work_observe(&broker->model, &client->hint, cost, client->peer);
    peer->mean_cost = peer->mean_cost > 0 ? .875 * peer->mean_cost + .125 * cost : cost;
    int bulk = bulk_input(peer, &client->hint);
    /* Tiny transfers cannot measure bandwidth reliably. Include fast samples
     * as well as slow ones, so a transient delay cannot poison the estimate. */
    if (timing->prepare_ns && (!bulk || client->hint.bytes >= DCC_TCP_CHUNK)) {
        double sample = timing->prepare_ns / 1e9;
        if (bulk) sample /= client->hint.bytes;
        double *previous = &peer->prepare_cost[client->hint.kind - 1];
        double prior = *previous > 0 ? *previous : bulk ? 1.0 / (64 * 1048576) : peer->local ? .001 : .005;
        *previous = .875 * prior + .125 * sample;
    }
    rs_trace("job cost: key=%llx kind=%u bytes=%llu units=%u predicted=%.6f factor=%.6f "
             "execute=%.6f prepare=%.6f queue=%.6f normalized=%.6f host=%s now=%.6f client=%td request=%.6f family=%llx cpu=%.6f",
             (unsigned long long)client->hint.key, client->hint.kind,
             (unsigned long long)client->hint.bytes, client->hint.work_units,
             client->cost, client->factor, timing->execute_ns / 1e9,
             timing->prepare_ns / 1e9, timing->queue_ns / 1e9, cost, peer->host->hostname,
             now, client - broker->clients, client->waiting_since, (unsigned long long)client->hint.family,
             timing->cpu_ns / 1e9);
}

static unsigned long long memory_forecast(const struct dcc_capacity *cap,
                                          const struct dcc_work_hint *hint, int *slots)
{
    if (slots) *slots = cap->capacity;
    if (!hint->memory_class) return cap->job_memory;
    unsigned kind = (hint->memory_class - 1) / DCC_MEMORY_BUCKETS;
    unsigned bucket = (hint->memory_class - 1) % DCC_MEMORY_BUCKETS;

    unsigned row;
    const int *capacities;
    switch (kind) {
    case DCC_JOB_FRONTEND: row = 4; capacities = cap->frontend_capacity; break;
    case DCC_JOB_RUST_CGU: row = 5; capacities = cap->cgu_capacity; break;
    case DCC_JOB_COMPILE: row = 0; capacities = cap->compile_capacity; break;
    case DCC_JOB_RUST: row = 1; capacities = cap->rust_capacity; break;
    case DCC_JOB_THINLTO: row = 2; capacities = cap->thinlto_capacity; break;
    case DCC_JOB_OFFLOAD_LINK: row = 3; capacities = cap->offload_capacity; break;
    default: return cap->job_memory;
    }
    uint64_t bytes = cap->memory_estimates[row][bucket];
    if (slots) {
        *slots = capacities[bucket];
        if (cap->live_report && bytes) {
            uint64_t extra = cap->memory_headroom / bytes;
            *slots = extra >= (unsigned)cap->available ? cap->capacity : cap->busy + (int)extra;
        }
    }
    return bytes;
}

static unsigned owner_slot(struct dispatch_batch *batch, int peer, const struct dcc_work_hint *hint)
{
    unsigned i = (hint->key ^ hint->family ^ (unsigned)peer) % (2 * MAX_CLIENTS);
    for (;;) {
        const struct client *c = batch->owners[i];
        if (!c || (c->peer == peer && c->hint.key == hint->key && c->hint.family == hint->family &&
                   c->hint.bytes == hint->bytes)) return i;
        i = (i + 1) % (2 * MAX_CLIENTS);
    }
}
static unsigned scope_slot(struct dispatch_batch *batch, int peer, uint64_t scope)
{
    unsigned i = (scope ^ (unsigned)peer) % (2 * MAX_CLIENTS);
    while (batch->scopes[i] && (batch->scopes[i]->peer != peer || batch->scopes[i]->hint.scope_key != scope))
        i = (i + 1) % (2 * MAX_CLIENTS);
    return i;
}
static int warm_scope(struct dispatch_batch *batch, const struct peer *peer, int index, uint64_t scope)
{
    if (!scope) return 0;
    if (batch->scopes[scope_slot(batch, index, scope)]) return 1;
    for (unsigned i = 0; i < 16; ++i) if (peer->scopes[i] == scope) return 1;
    return 0;
}
static void account_lease(struct broker_schedule *broker, struct dispatch_batch *batch, struct client *c)
{
    const struct peer *peer = &broker->peers[c->peer];
    struct occupancy *o = &batch->peers[c->peer];
    uint64_t bytes = memory_forecast(&peer->metrics, &c->hint, NULL);
    o->pending += bytes > UINT64_MAX - o->pending ? UINT64_MAX - o->pending : bytes;
    if (!c->direct && c->serial > peer->observed_serial) {
        ++o->recent;
        o->links += c->command == ACQUIRE_LINK;
        o->new_memory += bytes > UINT64_MAX - o->new_memory ? UINT64_MAX - o->new_memory : bytes;
    }
    if (c->hint.scope_key) batch->scopes[scope_slot(batch, c->peer, c->hint.scope_key)] = c;
    if (c->hint.key) batch->owners[owner_slot(batch, c->peer, &c->hint)] = c;
}

static int cpp_limit(const struct broker_schedule *broker)
{
    if (broker->cpp_limit) return broker->cpp_limit;
    int limit = dcc_resource_cpus();
    for (int i = 0; i < broker->count; ++i) {
        const struct peer *p = &broker->peers[i];
        if (!p->local) continue;
        const struct dcc_capacity *cap = &p->metrics;
        int slots = cap->cpus > 0 ? cap->cpus : limit;
        /* Worker memory admission belongs to the worker's cgroup. The client
         * checks its own memory budget before starting this frontend. */
        int busy = p->busy;
        if (cap->cpus > 0 && cap->busy >= 0 && cap->busy + p->direct > busy)
            busy = cap->busy + p->direct;
        if (slots - busy < limit) limit = slots - busy;
    }
    /* Execution cannot feed remote workers. Keep one producer able to make
     * progress even when local compilers occupy every CPU; further producers
     * still share the remaining local capacity. Explicit limits stay exact. */
    return limit > 0 ? limit : 1;
}

static int pipeline_slots(const struct dcc_hostdef *host, int lanes)
{
    if (!host->tcp_jobs || host->mode != DCC_MODE_TCP || lanes < 2) return 0;
    int staging = (lanes + 3) / 4;
    return staging < DCC_TCP_PIPELINE ? staging : DCC_TCP_PIPELINE;
}

/* Both lists use sorted LLVM names. Match complete tokens, including x86
 * families whose names are prefixes of other features. An absent offer
 * cannot authorize loading client code. */
static int compatible_rust_host(const char *required, const char *offered)
{
    if (!*required) return 1;
    if (!*offered) return 0;
    if (!strcmp(required, "-")) return 1;
    while (*required && *offered) {
        size_t need = strcspn(required, ","), have = strcspn(offered, ",");
        int cmp = strncmp(required, offered, need < have ? need : have);
        if (cmp < 0 || (!cmp && need < have)) return 0;
        if (!cmp && need == have) required += need + (required[need] == ',');
        offered += have + (offered[have] == ',');
    }
    return !*required;
}

static int compatible_cgu(const char *required, const char *offered)
{
    size_t version = strcspn(required, " ");
    if (!version || required[version] != ' ') return 0;
    const char *target = required + version + 1;
    size_t length = strcspn(target, " ");
    while (*offered) {
        const char *end = offered + strcspn(offered, ";");
        if ((size_t)(end - offered) > version && !strncmp(required, offered, version + 1)) {
            for (const char *p = offered + version + 1; p < end;) {
                size_t n = strcspn(p, " ;");
                if (n == length && !memcmp(p, target, n)) return 1;
                p += n;
                if (p < end) ++p;
            }
        }
        offered = *end ? end + 1 : end;
    }
    return 0;
}

static enum decision dispatch_local_throttle(struct broker_schedule *broker, struct client *client, struct reply *reply, double now)
{
    int limit = cpp_limit(broker);
    /* Queue against the client's budget before granting a lease. Otherwise
     * a smaller client rejects the broker's grant and sleeps for a second
     * even when the next compiler releases its slot immediately. */
    unsigned occupied = broker->cpp_busy;
    if (broker->cpp_busy < limit && occupied < client->local_limit) {
        reply->slots = occupied + 1;
        ++broker->cpp_busy;
        client->cpp_reserved = 1;
        client->state = ACTIVE;
        client->run_at = now;
        return LOCAL_SLOT;
    }
    return HOLD_JOB;
}

struct rust_routes { int native, cgu, pending; };
static int capacity_pending(const struct peer *p, const struct client *client, double now)
{
    return p->address_len && p->host->stats_port && p->valid_until <= now &&
           (p->stats_fd >= 0 || p->deadline < client->waiting_since);
}

static struct rust_routes rust_routes(const struct peer *p, const struct client *client, double now)
{
    struct rust_routes routes = {0};
    if (!p->host->tcp_jobs || !p->host->stats_port) return routes;
    if (p->valid_until <= now) {
        routes.pending = capacity_pending(p, client, now);
        return routes;
    }
    const struct dcc_capacity *cap = &p->metrics;
    for (unsigned k = 0; k < cap->rust_compiler_count; ++k) {
        if (cap->rust_compilers[k] != client->hint.rust_compiler) continue;
        routes.native = compatible_rust_host(client->hint.rust_host_features,
            cap->rust_host_features + cap->rust_feature_offset[k]);
        break;
    }
    routes.cgu = client->hint.kind == DCC_WORK_RUST_CGU &&
              compatible_cgu(client->hint.cgu_backend, cap->llvm_cgu);
    return routes;
}

static enum decision answer_cgu_capacity(const struct broker_schedule *broker, const struct client *client, double now,
                                         struct reply *reply)
{
    for (int i = 0; i < broker->count; ++i) {
        struct rust_routes routes = rust_routes(&broker->peers[i], client, now);
        if (routes.pending) return HOLD_JOB;
        reply->native_peers += routes.native;
        reply->slots += routes.cgu;
        const struct peer *p = &broker->peers[i];
        if (p->host->tcp_jobs && p->host->stats_port && p->valid_until <= now)
            reply->status = EXIT_CONNECT_FAILED;
    }
    if (reply->native_peers || reply->slots) reply->status = 0;
    return ANSWER;
}

static enum decision answer_cluster_jobs(const struct broker_schedule *broker, const struct client *client,
                                         double now, struct reply *reply)
{
    double work_rate = 0, min_rate = 0;
    int available = 0;
    for (int i = 0; i < broker->count; ++i) {
        const struct peer *p = &broker->peers[i];
        struct dcc_capacity observed = observed_capacity(p, broker->cpp_busy);
        const struct dcc_capacity *cap = p->valid_until > now ? &observed : NULL;
        if (!p->address_len || p->retry_at > now || dcc_capacity_slots(p->host, cap) <= 0) continue;
        if (capacity_pending(p, client, now)) return HOLD_JOB;
        ++available;
        int jobs = dcc_capacity_parallelism(p->host, cap);
        reply->slots += jobs + pipeline_slots(p->host, jobs);
        if (jobs > 0) {
            struct dcc_capacity full;
            if (cap) {
                full = *cap;
                /* Grain spans the whole link. Transient runnable work
                 * (including its planner) must not reshuffle partitions
                 * on every invocation. Placement still uses live load. */
                full.running = full.busy = jobs - 1;
                full.capacity = jobs;
                full.queued = 0;
            }
            double factor = dcc_capacity_score(p->host, cap ? &full : NULL, jobs - 1);
            double rate = 1 / factor;
            work_rate += jobs * rate;
            if (!min_rate || rate < min_rate) min_rate = rate;
        }
    }
    snprintf(reply->host, sizeof(reply->host),
             "{\"jobs\":%d,\"work_rate\":%.9g,\"min_rate\":%.9g,\"direct_completed\":%llu,"
             "\"direct_prepared_completed\":%llu}",
             reply->slots, work_rate, min_rate, (unsigned long long)broker->direct_completed,
             (unsigned long long)broker->direct_prepared_completed);
    reply->status = available ? 0 : EXIT_CONNECT_FAILED;
    return ANSWER;
}

static struct peer_view project_peer(const struct peer *p, const struct occupancy *occupied,
                                     const struct client *client, int cpp, double now, int link_backend)
{
    struct peer_view view = {.fresh = p->valid_until > now};
    struct dcc_capacity *cap = view.fresh ? &view.metrics : NULL;
    if (cap) *cap = observed_capacity(p, cpp);
    if (cap && p->host->tcp_jobs && client->hint.kind >= 1 && client->hint.kind <= DCC_WORK_CLASSES) {
        /* A refreshed idle snapshot must not erase leases whose upload has
         * not arrived yet. Running jobs already have server reservations;
         * returning jobs no longer need a compiler working set. */
        unsigned long long unseen = 0;
        if (cap->busy >= 0 && cap->returning >= 0 && p->busy > cap->busy + cap->returning &&
            occupied->pending > cap->memory_reserved)
            unseen = occupied->pending - cap->memory_reserved;
        if (occupied->new_memory > unseen) unseen = occupied->new_memory;
        uint64_t headroom = cap->memory_headroom > unseen ? cap->memory_headroom - unseen : 0;
        int limit;
        unsigned long long estimate = memory_forecast(cap, &client->hint, &limit);
        view.memory_full = (cap->busy > 0 || p->busy > (cap->returning > 0 ? cap->returning : 0)) && headroom < estimate;
        /* The snapshot's class capacity assumes no subsequent jobs have
         * reserved memory. Charge their own measured classes: one large
         * lease can consume several small-job memory slots. CPU counts
         * alone otherwise admit a burst that stalls after upload. */
        if (headroom < cap->memory_headroom && cap->busy >= 0 && limit >= 0) {
            int used = cap->busy + occupied->recent;
            int leased = p->busy - (cap->returning > 0 ? cap->returning : 0);
            if (leased > used) used = leased;
            unsigned long long extra = headroom / estimate;
            if (limit > used && extra < (unsigned)(limit - used))
                limit = used + (int)extra;
        }
        if (limit >= 0) cap->capacity = limit;
    }
    view.slots = dcc_capacity_slots(p->host, cap);
    int staged = p->host->tcp_jobs;
    view.local_exec = p->local && staged && (client->command == TRY_LOCAL ||
        (client->can_compile_locally && cap && cap->busy >= 0 &&
         (client->command == ACQUIRE_CPP || client->command == ACQUIRE_READY)));
    view.capacity = cap && cap->capacity >= 0 && cap->capacity < view.slots ? cap->capacity : view.slots;
    view.lanes = cap ? effective_lanes(cap) : view.slots;
    view.pipeline = link_backend || view.local_exec ? 0 : pipeline_slots(p->host, view.capacity);
    view.inflight = view.slots + view.pipeline;
    view.work = p->busy;
    if (staged && cap && cap->busy >= 0 && cap->queued >= 0 && cap->preparing >= 0) {
        int limit = view.lanes;
        int shared_pipeline = pipeline_slots(p->host, limit);
        /* Project the global snapshot, not local leases plus the same
         * server queue. Requests dispatched during/after the probe are
         * conservatively charged until the next successful snapshot.
         * Returning jobs in that snapshot consume no compiler capacity. */
        /* A RETURN proves execution has ended without another IPC or
         * network round trip. First charge completions against jobs that
         * were already returning in the snapshot, never against CPU work. */
        int finished = p->completed - (cap->returning > 0 ? cap->returning : 0);
        if (finished < 0) finished = 0;
        int cpu = cap->busy + cap->queued;
        int preparing = cap->preparing;
        if (finished > cpu) {
            preparing -= finished - cpu;
            if (preparing < 0) preparing = 0;
        }
        cpu = finished < cpu ? cpu - finished : 0;
        view.work = cpu + occupied->recent;
        /* A lease can precede the worker's first input byte. A fresh idle
         * snapshot must not erase that work and repeatedly refill the
         * same worker while the client prepares its request. */
        int returning = cap->returning - p->completed;
        if (returning < 0) returning = 0;
        /* IR claim waiters retain memory and cancellation ownership, not
         * CPU lanes. The server admits/resumes them under the unchanged
         * CPU limit. As for returning leases, live global CPU/queue counts
         * account for work whose individual lease cannot be identified. */
        int suspended = cap->suspended > 0 ? cap->suspended : 0;
        int leased = p->busy - returning - suspended;
        if (leased < 0) leased = 0;
        view.blocked = view.work + preparing >= limit + shared_pipeline || leased >= limit + shared_pipeline;
        /* Input preparation overlaps execution; only excess preparation
         * beyond the bounded pipeline adds CPU queueing delay. */
        if (preparing > shared_pipeline) view.work += preparing - shared_pipeline;
        if (leased > view.work) view.work = leased;
        if (returning > 0)
            view.inflight += returning < view.slots ? returning : view.slots;
        view.inflight += suspended;
        view.link_busy = cap->links > 0 || occupied->links;
        cap->busy = cpu;
        cap->queued = 0;
    }
    return view;
}

static enum decision place_job(struct broker_schedule *broker, struct client *client, double now, struct dispatch_batch *batch,
                               struct reply *reply, struct placement *choice)
{
    int i, available = 0, ready_hosts = 0, abi_mismatches = 0, pending_cgu_probe = 0;
    struct selection {
        int peer, probe, wait, idle, enqueue, direct;
        double score, fill, marginal_factor;
        struct prediction forecast;
    } best = {.peer = -1}, ready = {.peer = -1}, affinity = {.peer = -1};
    int direct = client->command == TRY_LOCAL;
    if (direct) {
        for (i = 0; i < broker->count; ++i)
            if (broker->peers[i].local && broker->peers[i].host->tcp_jobs) break;
        if (i == broker->count) return ANSWER;
    }
    double cost = dcc_work_predict(&broker->model, &client->hint);
    int link = client->command == ACQUIRE_LINK;
    int link_backend = client->hint.kind == DCC_WORK_RUST_CGU || client->hint.kind == DCC_WORK_THINLTO ||
                       client->hint.kind == DCC_WORK_OFFLOAD_LINK;
    int local_cpp = !link && !direct && client->command != ACQUIRE_CPP && client->command != ACQUIRE_READY;
    /* Placement history is only a locality hint. The receiving worker still
     * validates its compiler/input certificate; this never authorizes reuse.
     * Keep the hint in the existing bounded cost history, and consult live
     * leases for work whose frontend may have completed before its backend. */
    int source = client->hint.kind == 4 || client->hint.kind == 5 ||
                 client->hint.kind == DCC_WORK_RUST;
    int locality = !direct && !link && source;
    /* A cold byte prior is not evidence for a finite completion comparison. */
    int thinlto = client->hint.kind == DCC_WORK_THINLTO;
    int compare_finish = source || thinlto;
    int completion_model = compare_finish && broker->model.classes[client->hint.kind - 1].xx > 0;
    int previous = locality ? dcc_work_peer(&broker->model, &client->hint) : -1;
    for (i = 0; i < broker->count; i++) {
        struct peer *p = &broker->peers[i];
        /* An empty preflight describes an extra local codegen lane, not a
         * movable compiler invocation. It has no remote execution method. */
        if (direct && !client->hint.key && !p->local) continue;
        if (client->hint.kind == DCC_WORK_RUST || client->hint.kind == DCC_WORK_RUST_CGU) {
            struct rust_routes routes = rust_routes(p, client, now);
            if (!(client->hint.kind == DCC_WORK_RUST ? routes.native : routes.cgu)) {
                pending_cgu_probe |= routes.pending;
                if (!p->host->tcp_jobs || !p->host->stats_port || p->valid_until > now) ++abi_mismatches;
                continue;
            }
        }
        struct peer_view view = project_peer(p, &batch->peers[i], client, broker->cpp_busy, now, link_backend);
        const struct dcc_capacity *cap = view.fresh ? &view.metrics : NULL;
        int slots = view.slots, blocked = view.blocked, work = view.work;
        int local_exec = view.local_exec, inflight = view.inflight, pipeline = view.pipeline;
        if (direct && p->local && !broker->cpp_limit &&
            broker->cpp_busy >= cpp_limit(broker)) continue;
        if (!p->address_len || p->retry_at > now || slots <= 0)
            continue;
        available++;
        if (view.memory_full || p->valid_until < 0) continue; /* Admission was refused: refresh first. */
        int staged = p->host->tcp_jobs;
        int predicted = staged && cap && cost > 0 && !link;
        if (cap && (cap->capacity == 0 || cap->memory_pressure >= 10)) continue;
        if (cap && cap->available == 0 && !staged) continue;
        if (p->full_until > now || (link && p->link_until > now)) continue;
        if ((link && view.link_busy) || (blocked && !predicted)) continue;
        /* Codegen's S/k estimate assumes independent lanes. A free logical
         * slot on an occupied SMT core does not supply that service rate;
         * cloning/merging still costs work and delays ordinary compiles.
         * Empty hints are optional parallelism, so declining them leaves the
         * caller's existing compiler lease running. Normal compiles continue
         * to use the full measured SMT capacity. */
        if (direct && !client->hint.key && cap && cap->cores > 0 &&
            (work >= cap->cores ||
             (cap->running >= 0 ? cap->running : cap->load) >= cap->cores))
            continue;
        if (p->busy < inflight || predicted) {
            /* A recovery trial uses the host's calibrated speed, not a zero
             * completion time. An uncompetitive host need not delay this job
             * just to refresh stale frequency telemetry. */
            int probe = !link && cap && cap->cpu_probe && work == 0 && now >= p->probe_after;
            double score = probe ? execution_factor(p, work, 1, cap->capacity, broker->cpp_busy) :
                                   dcc_capacity_score(p->host, cap, work);
            struct prediction forecast = {0};
            if (client->hint.key || direct)
                forecast.factor = cap ? execution_factor(p, work, probe, cap->capacity, broker->cpp_busy) : score;
            int wait = 0, enqueue = 0;
            if (predicted) {
                forecast = predict(broker, client, i, &view, cost, now, probe);
                score = forecast.finish;
                wait = forecast.start > forecast.prepare + .02;
            } else if (cost > 0) score *= cost;
            if (staged && cap && (client->hint.key || local_exec) && !link) {
                /* Large workers may fill their bounded preparation queue;
                 * small workers and link backends keep jobs movable until an
                 * execution lane opens. Completion forecasts rank candidates
                 * but never relax the live resource or in-flight bounds. */
                int lanes = view.lanes;
                wait |= blocked || work >= lanes || p->busy >= inflight;
                enqueue = pipeline && !blocked && work < lanes + pipeline && p->busy < inflight;
            }
            /* Charge the throughput displaced by another compiler on every
             * worker, including existing compiles on a remote SMT core.
             * Minimizing this job's latency alone packs the fastest worker's
             * SMT lanes while independent physical cores remain unused. */
            double marginal_factor = forecast.factor;
            if (staged && cap && !link && !link_backend && (predicted ||
                (!blocked && work < dcc_capacity_jobs(p->host, cap)))) {
                struct dcc_capacity live = observed_capacity(p, broker->cpp_busy);
                double external = (live.running >= 0 ? live.running : live.load)
                                  - (live.busy > 0 ? live.busy : 0);
                int placement = work;
                if (wait) {
                    int lanes = view.lanes;
                    /* A queued choice still displaces CPU work when its next
                     * lane opens. Apply the same placement policy then as now;
                     * otherwise we reject a useful pipeline as CPU overload. */
                    if (placement >= lanes) placement = lanes > 0 ? lanes - 1 : 0;
                }
                double demand = placement + (external > 0 ? external : 0);
                /* Include the CPU work displaced by this compile, notably
                 * client preprocessing on a machine that also runs a worker. */
                double prior_factor = execution_factor(p, placement - 1, probe, cap->capacity, broker->cpp_busy);
                double before = demand ? demand / prior_factor : 0;
                double gain = (demand + 1) / execution_factor(p, placement, probe, cap->capacity, broker->cpp_busy) - before;
                /* At saturation there is no positive marginal throughput.
                 * Charge aggregate service demand instead of dropping the
                 * penalty or dividing by numerical noise. This finite bound
                 * also lets the least costly host progress when all are busy. */
                double penalty = (demand + 1) * execution_factor(p, placement, probe, cap->capacity, broker->cpp_busy);
                if (gain > 0 && gain * penalty > 1) penalty = 1 / gain;
                if (penalty > marginal_factor) marginal_factor = penalty;
                double throughput = forecast.start + (cost > 0 ? cost : 1) * penalty;
                if (predicted && completion_model) {
                    /* All times are relative to this dispatch. Convert the
                     * overlap to units of work before pricing the remainder
                     * on an empty host. Unleased tasks use typical service. */
                    /* Below one runnable job per physical core, the remaining
                     * leases need not share our execution resources. */
                    unsigned cores = cap->cores > 0 ? (unsigned)cap->cores : 1;
                    double end = lane_ready(&p->schedule.cpu, cores);
                    if (external > 0) {
                        const struct dcc_work_class *learned = &broker->model.classes[client->hint.kind - 1];
                        double competing = (p->mean_cost > 0 ? p->mean_cost : learned->mean) * prior_factor;
                        if (competing > end) end = competing;
                    }
                    if (end < forecast.finish) {
                        double contended = end > forecast.start ? (end - forecast.start) / forecast.factor : 0;
                        /* Preserve clock and memory observations after the CPU
                         * work drains, including the external tasks above. */
                        live.running = live.busy = live.queued = 0;
                        live.capacity = cap->capacity;
                        if (probe) live.cpu_scale = 1;
                        double tail = (cost - contended) * dcc_capacity_score(p->host, &live, 0);
                        forecast.finish = forecast.start + contended * forecast.factor + tail;
                        throughput = forecast.start + contended * penalty + tail;
                    }
                }
                score = predicted && forecast.finish > throughput ? forecast.finish : throughput;
            }
            double fill = (work + 1.0) / slots;
            if (link && cap && cap->links > 0) score *= 1 + cap->links;
            rs_trace("candidate host=%s work=%d capacity=%d cost=%.3f factor=%.3f prepare=%.3f start=%.3f score=%.3f wait=%d "
                     "finish=%.6f blocked=%d enqueue=%d busy=%d preparing=%d queued=%d leases=%d direct=%d cpp=%d planned=%u "
                     "now=%.6f client=%td request=%.6f key=%llx",
                     p->host->hostname, work, cap ? cap->capacity : -1, cost,
                     forecast.factor, forecast.prepare, forecast.start, score, wait,
                     forecast.finish, blocked, enqueue, cap ? cap->busy : -1, cap ? cap->preparing : -1,
                     cap ? cap->queued : -1, p->busy, p->direct, p->local ? broker->cpp_busy : 0, batch->count,
                     now, client - broker->clients, client->waiting_since,
                     (unsigned long long)client->hint.key);
            /* Without any input description there is no completion estimate
             * with which to justify waiting instead of using an idle CPU. */
            int idle = !cost && staged && cap && !blocked && work < dcc_capacity_jobs(p->host, cap);
            struct selection candidate = {i, probe, wait, idle, enqueue, local_exec,
                                          score, fill, marginal_factor, forecast};
            int warm = (locality && (i == previous || batch->owners[owner_slot(batch, i, &client->hint)])) ||
                       (!direct && !link && warm_scope(batch, p, i, client->hint.scope_key));
            if (warm && staged && predicted && (!wait || enqueue) &&
                (affinity.peer < 0 || marginal_factor < affinity.marginal_factor ||
                 (marginal_factor == affinity.marginal_factor && score < affinity.score)))
                affinity = candidate;
            if (best.peer < 0 || idle > best.idle || (idle == best.idle &&
                (score < best.score || (score == best.score && fill < best.fill)))) best = candidate;
            /* A source pipeline can accept work while its CPU lanes are busy.
             * Include it in the available alternatives: a blocked peer with a
             * lower throughput score must not make the wait/ready comparison
             * discard an admissible pipeline with an earlier completion. */
            if (predicted && (!wait || (locality && enqueue))) {
                ready_hosts++;
                if (ready.peer < 0 || score < ready.score ||
                    (score == ready.score && fill < ready.fill)) ready = candidate;
            }
        }
    }
    /* Throughput displacement ranks immediate placements, but is not elapsed
     * time. Waiting must actually finish sooner than an available placement;
     * comparing a queued latency with an immediate throughput penalty can
     * otherwise strand idle CPUs for an entire running job. Once waiting is
     * rejected, preserve throughput ranking among the available workers.
     * Compare the queued job with that actual ready choice: another worker's
     * shorter latency cannot justify choosing a slower ready alternative. */
    /* A cold prior cannot justify leaving a worker idle. ThinLTO modules
     * with measured costs can wait for a faster lane at the link barrier. */
    if (best.wait && ready.peer >= 0 &&
        ((link_backend && !thinlto) || (compare_finish && !completion_model) ||
         best.forecast.finish >= ready.forecast.finish))
        best = ready;
    /* Prefer an already populated, no-slower worker only inside its existing
     * immediate admission/pipeline bounds. Never wait for locality, increase
     * the pipeline, or pin a repeat to a slower owner. No speculative cache
     * speedup is subtracted from the normal execution-cost model. */
    if (affinity.peer >= 0 && best.peer >= 0 && affinity.peer != best.peer &&
        (!best.wait || affinity.forecast.finish <= best.forecast.finish) &&
        affinity.forecast.factor <= best.forecast.factor &&
        affinity.marginal_factor <= best.marginal_factor) {
        rs_trace("source affinity: key=%llx host=%s instead=%s",
                 (unsigned long long)client->hint.key, broker->peers[affinity.peer].host->hostname,
                 broker->peers[best.peer].host->hostname);
        best = affinity;
    }
    if (direct && !(best.peer >= 0 && (best.forecast.finish > 0 || best.idle) && !best.wait &&
                    broker->peers[best.peer].local && broker->peers[best.peer].host->tcp_jobs)) {
        return ANSWER;
    } else if (pending_cgu_probe && !available) {
        return HOLD_JOB;
    } else if (!available) {
        for (i = 0; i < broker->count; i++) {
            struct peer *p = &broker->peers[i];
            rs_log_warning("unavailable worker %s: address=%u up=%d slots=%d cpus=%d jobs=%d retry=%.3fs",
                           p->host->hostname, (unsigned)p->address_len, p->host->is_up,
                           p->host->n_slots, p->metrics.cpus, p->metrics.jobs,
                           p->retry_at > now ? p->retry_at - now : 0);
        }
        reply->status = abi_mismatches == broker->count && broker->count ? EXIT_LOCAL_COMPILE : EXIT_CONNECT_FAILED;
        return ANSWER;
    } else if (best.peer >= 0 && best.wait && !best.enqueue) {
        rs_trace("waiting for host=%s cost=%.3f score=%.3f finish=%.6f ready=%d ready_finish=%.6f "
                 "now=%.6f client=%td request=%.6f key=%llx",
                 broker->peers[best.peer].host->hostname, cost, best.score, best.forecast.finish,
                 ready_hosts, ready.forecast.finish, now, client - broker->clients, client->waiting_since,
                 (unsigned long long)client->hint.key);
        ++batch->count;
        struct schedule *schedule = &broker->peers[best.peer].schedule;
        ++schedule->waiting;
        if (schedule->cpu.count) {
            lane_finish(&schedule->cpu, best.forecast.finish);
            lease_add(&schedule->leases, best.forecast.finish);
        }
        double delay = best.forecast.start - best.forecast.prepare - .02;
        if (delay < .05) delay = .05;
        if (delay > 1) delay = 1;
        client->deadline = now + delay;
    } else if (best.peer >= 0 && (!local_cpp || broker->cpp_busy < cpp_limit(broker))) {
        struct peer *p = &broker->peers[best.peer];
        rs_trace("assigning host=%s cost=%.3f score=%.3f broker_wait=%.6f key=%llx units=%u "
                 "now=%.6f client=%td request=%.6f",
                 p->host->hostname, cost, best.score, now - client->waiting_since,
                 (unsigned long long)client->hint.key, client->hint.work_units,
                 now, client - broker->clients, client->waiting_since);
        *choice = (struct placement){best.peer, best.direct, best.probe, local_cpp, cost, best.forecast};
        return ASSIGN_JOB;
    }
    return HOLD_JOB;
}

void schedule_assign(struct broker_schedule *broker, struct client *client, struct dispatch_batch *batch,
                     const struct placement *choice, double now)
{
    struct peer *p = &broker->peers[choice->peer];
    client->direct = choice->direct;
    if (p->schedule.cpu.count && choice->forecast.finish > 0) {
        lane_finish(&p->schedule.cpu, choice->forecast.finish);
        lease_add(&p->schedule.leases, choice->forecast.finish);
    }
    client->peer = choice->peer;
    client->cost = choice->cost;
    if (client->command == TRY_LOCAL && !client->cost) client->cost = p->mean_cost;
    client->factor = choice->forecast.factor;
    client->ready_at = now + (client->direct ? 0 : choice->forecast.prepare);
    client->run_at = now + (client->direct ? 0 : choice->forecast.start);
    if (choice->probe) p->probe_after = now + 10;
    client->serial = ++p->serial;
    client->state = CONNECTING;
    client->deadline = now + 3;
    p->busy++;
    account_lease(broker, batch, client);
    p->direct += client->direct;
    if (client->direct)
        rs_trace("direct local compile host=%s prepared=%d client=%td", p->host->hostname,
                 client->command != TRY_LOCAL, client - broker->clients);
    broker->cpp_busy += choice->local_cpp;
    client->cpp_reserved = choice->local_cpp;
}

enum decision schedule_request(struct broker_schedule *broker, struct client *client, double now, struct dispatch_batch *batch,
                               struct reply *reply, struct placement *choice)
{
    switch (client->command) {
    case LOCAL_CPP: return dispatch_local_throttle(broker, client, reply, now);
    case CGU_CAPACITY: return answer_cgu_capacity(broker, client, now, reply);
    case JOBS: return answer_cluster_jobs(broker, client, now, reply);
    default: return place_job(broker, client, now, batch, reply, choice);
    }
}

static int waiting_order(const void *a, const void *b)
{
    const struct waiting_job *left = a, *right = b;
    int lc = left->client->command == LOCAL_CPP || left->client->command == JOBS;
    int rc = right->client->command == LOCAL_CPP || right->client->command == JOBS;
    if (lc != rc) return rc - lc;
    /* Large jobs have the greatest execution penalty on a slower worker.
     * Aging gives sustained arrivals a bounded opportunity to overtake. */
    int la = left->age >= 1, ra = right->age >= 1;
    if (la != ra) return ra - la;
    if (la) return (right->age > left->age) - (right->age < left->age);
    if (left->cost != right->cost)
        return (right->cost > left->cost) - (right->cost < left->cost);
    return (right->age > left->age) - (right->age < left->age);
}
int schedule_batch(struct broker_schedule *broker, struct dispatch_batch *batch, struct waiting_job *waiting,
                   double now, int probes_pending)
{
    for (int h = 0; h < broker->count; ++h) {
        broker->peers[h].schedule.cpu.count = 0;
        broker->peers[h].schedule.waiting = 0;
    }
    int i, count_waiting = 0;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        struct client *c = &broker->clients[i];
        if (c->fd >= 0 && c->state == WAITING &&
            (c->command == LOCAL_CPP || !probes_pending))
            waiting[count_waiting++] = (struct waiting_job){c,
                dcc_work_predict(&broker->model, &c->hint), now - c->waiting_since};
    }
    if (count_waiting) {
        for (i = 0; i < MAX_CLIENTS; ++i)
            if (broker->clients[i].fd >= 0 && broker->clients[i].peer >= 0)
                account_lease(broker, batch, &broker->clients[i]);
    }
    qsort(waiting, count_waiting, sizeof(*waiting), waiting_order);
    return count_waiting;
}
#endif
