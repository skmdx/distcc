/* Persistent Linux readiness registrations. Each owner keeps its watch through
 * state changes. One-shot events require a rearm only after actual readiness. */
#ifndef DCC_EPOLL_SET_H
#define DCC_EPOLL_SET_H
#include <sys/epoll.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

struct dcc_watch { int fd_plus_one; unsigned events, ready; };
static inline void dcc_watch_drop(int ep, struct dcc_watch *w)
{
    if (w->fd_plus_one) epoll_ctl(ep, EPOLL_CTL_DEL, w->fd_plus_one - 1, NULL);
    *w = (struct dcc_watch){0};
}
static inline int dcc_watch_set(int ep, struct dcc_watch *w, int fd, unsigned events, unsigned token)
{
    if (w->fd_plus_one != fd + 1) dcc_watch_drop(ep, w);
    if (fd < 0) return 0;
    if (w->fd_plus_one && w->events == events && !w->ready) return 0;
    struct epoll_event e = {.events = events | EPOLLRDHUP | EPOLLONESHOT,
                            .data.u64 = ((uint64_t)token << 32) | (unsigned)fd};
    if (epoll_ctl(ep, w->fd_plus_one ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &e)) {
        int retry = errno == EEXIST ? EPOLL_CTL_MOD : errno == ENOENT ? EPOLL_CTL_ADD : -1;
        if (retry < 0 || epoll_ctl(ep, retry, fd, &e)) return -1;
    }
    *w = (struct dcc_watch){fd + 1, events, 0};
    return 0;
}
static inline void dcc_watch_ready(struct dcc_watch *w, struct epoll_event e)
{
    if (w->fd_plus_one == (int)(uint32_t)e.data.u64 + 1)
        w->ready = e.events | ((e.events & EPOLLRDHUP) ? EPOLLHUP : 0);
}
#endif
