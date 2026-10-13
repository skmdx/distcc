/* Bounded CPU work for the TCP event loop. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_TCP_ASYNC_H
#define DCC_TCP_ASYNC_H
#include <stdatomic.h>
struct dcc_tcp_work {
    struct dcc_tcp_work *next;
    void (*run)(void *);
    void *argument;
    atomic_int done;
};
/* At most one outstanding task per admitted connection. Only the event loop
 * submits or frees tasks. Workers touch only the task's exclusive storage. */
int dcc_tcp_async_submit(struct dcc_tcp_work *, void (*)(void *), void *);
int dcc_tcp_async_fd(void);
void dcc_tcp_async_drain(void);
/* Join all threads before fork/shutdown; submission restarts them lazily. */
void dcc_tcp_async_stop(void);
void dcc_tcp_async_child(void);
unsigned long dcc_tcp_async_tasks(void);
unsigned dcc_tcp_async_peak(void);
#endif
