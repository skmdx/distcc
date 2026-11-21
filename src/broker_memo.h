/* Per-user plan and resident worker cache. */
#pragma once
#include <sys/socket.h>
#include <stdint.h>
#include <stddef.h>

struct dcc_memo_store;
struct memo_lease;
struct memo_session {
    int fd, plan_access, closed;
    struct ucred peer;
    struct memo_lease *leases;
};
struct dcc_memo_store *dcc_memo_store_new(int ep);
void dcc_memo_store_free(struct dcc_memo_store *);
void dcc_memo_drain(struct dcc_memo_store *);
int dcc_memo_has(const struct memo_session *, unsigned tag);
int dcc_memo_release(struct dcc_memo_store *, struct memo_session *, unsigned tag);
void dcc_memo_disconnect(struct dcc_memo_store *, struct memo_session *);
int dcc_memo_request(struct dcc_memo_store *, struct memo_session *, unsigned tag, unsigned command,
                     const char *payload, size_t size, const uint8_t *digest, int descriptor);
