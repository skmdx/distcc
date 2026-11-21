#pragma once
#include <sys/uio.h>
#include "broker_protocol.h"

/* A successful decoder returns zero; the caller owns any received descriptor.
 * On error it returns errno; the session owns cleanup and reconnect policy. */
typedef int (*dcc_reply_decoder)(unsigned command, unsigned flags, void *data, size_t size, int descriptor);
int dcc_session_call(struct broker_request_hdr *header, struct iovec *parts, size_t count,
                     struct iovec reply, int *descriptor, size_t *received, int *queued_fd,
                     int io_timeout, dcc_reply_decoder decode);
void dcc_session_finish(int lease);
enum dcc_lease_end { KEEP_LEASE, RETIRE_LEASE, FINISH_LEASE };
void dcc_session_send_lease(int lease, unsigned command, const struct iovec *payload,
                            size_t count, int descriptor, enum dcc_lease_end end);
