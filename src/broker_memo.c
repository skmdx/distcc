/* Broker-owned single-flight compiler cache. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#ifdef __linux__
#include <errno.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xxhash.h>
#include "distcc.h"
#include "broker.h"
#include "broker_protocol.h"
#include "broker_memo.h"
#include "plan_memo.h"
#include "rust/api.h"

#define MEMO_BYTES (64U * 1024U * 1024U)
struct memo_lease {
    struct memo *memo;
    struct memo_session *session;
    unsigned tag;
    struct memo_lease *session_next, *waiting_prev, *waiting_next;
};
struct memo {
    char *key, *value;
    size_t length, watch_bytes;
    uid_t owner;
    int worker, socket;
    struct dcc_plan_watch *watch;
    struct memo *previous, *next;
    struct memo_lease *publisher, *waiters, *last_waiter;
};
struct dcc_memo_store {
    void *memos;
    struct memo *memo_first, *memo_last;
    size_t memo_bytes;
    unsigned memo_workers, leases;
    struct dcc_plan_monitor *plans;
};

static int memo_ready(const struct memo *memo) { return memo->value || memo->socket; }

static int memo_order(const void *aa, const void *bb)
{
    const struct memo *a=aa,*b=bb;
    if (a->owner!=b->owner) return a->owner<b->owner ? -1 : 1;
    if (a->worker!=b->worker) return a->worker<b->worker ? -1 : 1;
    return strcmp(a->key,b->key);
}
static void memo_unlink(struct dcc_memo_store *store, struct memo *memo)
{
    if (memo->previous) memo->previous->next=memo->next;
    else if (store->memo_first==memo) store->memo_first=memo->next;
    if (memo->next) memo->next->previous=memo->previous;
    else if (store->memo_last==memo) store->memo_last=memo->previous;
    memo->next=memo->previous=NULL;
}
static void memo_touch(struct dcc_memo_store *store, struct memo *memo)
{
    memo_unlink(store,memo);
    memo->previous=store->memo_last;
    if (memo->previous) memo->previous->next=memo;
    else store->memo_first=memo;
    store->memo_last=memo;
}
static void memo_forget(struct dcc_memo_store *store, struct memo *memo)
{
    memo_unlink(store,memo);
    tdelete(memo,&store->memos,memo_order);
    store->memo_bytes-=sizeof(*memo)+strlen(memo->key)+1;
    if (memo->value) store->memo_bytes-=memo->length+1+memo->watch_bytes;
    if (memo->socket) { close(memo->socket-1); --store->memo_workers; }
    dcc_plan_watch_free(memo->watch);
    free(memo->key); free(memo->value); free(memo);
}

static int session_plan_access(struct dcc_memo_store *store, struct memo_session *session)
{
    if (!session->plan_access)
        session->plan_access = dcc_plan_peer_matches(store->plans, session->fd, session->peer.pid,
                                                     session->peer.uid, session->peer.gid) ? 1 : -1;
    return session->plan_access > 0;
}

static void memo_reply(struct dcc_memo_store *store, struct memo_session *session, const struct memo *memo)
{
    int fd = session->fd;
    if (memo->socket) dcc_send_fd(fd, "W", 2, memo->socket - 1);
    else if (memo->value) {
        uint32_t count = memo->watch && session_plan_access(store, session)
                ? dcc_plan_watch_verified(memo->watch) : 0;
        struct iovec parts[] = {{memo->value, memo->length}, {&count, count ? sizeof(count) : 0},
                                {(void *)(count ? "\1" : ""), 1}};
        struct msghdr msg = {.msg_iov = parts, .msg_iovlen = 3};
        sendmsg(fd, &msg, MSG_NOSIGNAL);
    }
}


static struct memo_lease *find_lease(const struct memo_session *session, unsigned tag)
{
    for (struct memo_lease *p = session->leases; p; p = p->session_next)
        if (p->tag == tag) return p;
    return NULL;
}

int dcc_memo_has(const struct memo_session *session, unsigned tag)
{
    return find_lease(session, tag) != NULL;
}

/* A tag-zero caller uses one connection per lease and observes EOF on completion. */
static void finish(struct dcc_memo_store *store, struct memo_lease *lease)
{
    struct memo_lease **p = &lease->session->leases;
    while (*p != lease) p = &(*p)->session_next;
    *p = lease->session_next;
    if (!lease->tag) lease->session->closed = 1;
    --store->leases;
    free(lease);
}

static void unlink_waiter(struct memo *memo, struct memo_lease *lease)
{
    if (lease->waiting_prev) lease->waiting_prev->waiting_next = lease->waiting_next;
    else memo->waiters = lease->waiting_next;
    if (lease->waiting_next) lease->waiting_next->waiting_prev = lease->waiting_prev;
    else memo->last_waiter = lease->waiting_prev;
}

static void abandon(struct dcc_memo_store *store, struct memo_lease *lease)
{
    struct memo *memo = lease->memo;
    if (lease != memo->publisher) {
        unlink_waiter(memo, lease);
        finish(store, lease);
        return;
    }
    finish(store, lease);
    while ((memo->publisher = memo->waiters)) {
        struct memo_lease *next = memo->publisher;
        unlink_waiter(memo, next);
        if (send(next->session->fd, "", 1, MSG_NOSIGNAL) == 1) return;
        shutdown(next->session->fd, SHUT_RDWR);
        finish(store, next);
    }
    memo_forget(store, memo);
}

int dcc_memo_release(struct dcc_memo_store *store, struct memo_session *session, unsigned tag)
{
    struct memo_lease *lease = find_lease(session, tag);
    if (!lease) return 0;
    abandon(store, lease);
    return 1;
}

void dcc_memo_disconnect(struct dcc_memo_store *store, struct memo_session *session)
{
    while (session->leases) abandon(store, session->leases);
}

static void evict(struct dcc_memo_store *store)
{
    while ((store->memo_bytes > MEMO_BYTES || store->memo_workers > DCC_BROKER_CLIENT_MAX) && store->memo_first) {
        struct memo *oldest = store->memo_first;
        if (store->memo_bytes <= MEMO_BYTES)
            while (!oldest->socket) oldest = oldest->next;
        memo_forget(store, oldest);
    }
}

int dcc_memo_request(struct dcc_memo_store *store, struct memo_session *session, unsigned tag, unsigned command,
                     const char *payload, size_t size, const uint8_t *digest, int descriptor)
{
    int worker = command == WORKER_GET || command == WORKER_PUT;
    if (command == MEMO_PUT || command == WORKER_PUT) {
        struct memo_lease *lease = find_lease(session, tag);
        if (!lease || lease->memo->publisher != lease || lease->memo->worker != worker) return -1;
        struct memo *memo = lease->memo;
        if (worker) {
            memo->socket = descriptor + 1;
            ++store->memo_workers;
        } else {
            memo->value = malloc(size);
            if (!memo->value) return -1;
            memcpy(memo->value, payload, size);
            memo->length = size - 1;
            store->memo_bytes += size;
            if ((!strncmp(memo->key, "gcc-plan:", 9) || !strncmp(memo->key, "clang-plan:", 11) ||
                 !strncmp(memo->key, "rust-plan:", 10)) && session_plan_access(store, session))
                memo->watch = dcc_plan_watch_new(store->plans, memo->value, memo->length, &memo->watch_bytes);
            store->memo_bytes += memo->watch_bytes;
        }
        memo->publisher = NULL;
        finish(store, lease);
        while (memo->waiters) {
            struct memo_lease *waiter = memo->waiters;
            unlink_waiter(memo, waiter);
            struct memo_session *recipient = waiter->session;
            memo_reply(store, recipient, memo);
            finish(store, waiter);
            if (recipient != session && recipient->closed)
                shutdown(recipient->fd, SHUT_RDWR);
        }
        memo_touch(store, memo);
        evict(store);
        return 0;
    }

    struct memo lookup = {.key = (char *)payload, .owner = session->peer.uid, .worker = worker};
    struct memo **found = tfind(&lookup, &store->memos, memo_order);
    struct memo *memo = found ? *found : NULL;
    if (memo && memo->socket && strcmp(memo->key, DCC_RUST_DIGEST_TABLE)) {
        char byte;
        if (recv(memo->socket - 1, &byte, 1, MSG_PEEK | MSG_DONTWAIT) != -1 ||
            (errno != EAGAIN && errno != EWOULDBLOCK)) {
            memo_forget(store, memo);
            memo = NULL;
        }
    }
    if (memo && digest && memo->value) {
        XXH128_canonical_t current;
        XXH128_canonicalFromHash(&current, XXH3_128bits(memo->value, memo->length));
        if (!memcmp(current.digest, digest, sizeof(current.digest))) {
            memo_forget(store, memo);
            memo = NULL;
        }
    }
    if (memo && memo_ready(memo)) {
        memo_touch(store, memo);
        memo_reply(store, session, memo);
        if (!tag) session->closed = 1;
        return 0;
    }
    if (store->leases >= DCC_BROKER_CLIENT_MAX) return -1;
    struct memo_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) return -1;
    if (!memo) {
        memo = calloc(1, sizeof(*memo));
        if (!memo) { free(lease); return -1; }
        memo->owner = lookup.owner;
        memo->worker = worker;
        memo->key = strdup(payload);
        if (!memo->key || !tsearch(memo, &store->memos, memo_order)) {
            free(memo->key); free(memo); free(lease); return -1;
        }
        store->memo_bytes += sizeof(*memo) + strlen(memo->key) + 1;
    }
    *lease = (struct memo_lease){.memo = memo, .session = session, .tag = tag, .session_next = session->leases};
    session->leases = lease;
    ++store->leases;
    if (memo->publisher) {
        lease->waiting_prev = memo->last_waiter;
        if (memo->last_waiter) memo->last_waiter->waiting_next = lease;
        else memo->waiters = lease;
        memo->last_waiter = lease;
        return 0;
    }
    memo->publisher = lease;
    return send(session->fd, "", 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

struct dcc_memo_store *dcc_memo_store_new(int ep)
{
    struct dcc_memo_store *store = calloc(1, sizeof(*store));
    if (store) store->plans = dcc_plan_monitor_new(ep);
    return store;
}

void dcc_memo_drain(struct dcc_memo_store *store) { dcc_plan_monitor_drain(store->plans); }
void dcc_memo_store_free(struct dcc_memo_store *store)
{
    while (store->memo_first) memo_forget(store, store->memo_first);
    dcc_plan_monitor_free(store->plans);
    free(store);
}
#endif
