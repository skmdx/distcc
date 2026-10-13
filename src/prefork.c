/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


              /* The dead cry out with joy when their books are reprinted
               */



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <poll.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "exitcode.h"
#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "dopt.h"
#include "exec.h"
#include "srvnet.h"
#include "types.h"
#include "daemon.h"
#include "netutil.h"
#include "stats.h"
#include "resource.h"
#include "http2.h"

void dcc_manage_kids(int listen_fd);
static void dcc_sigchld_handler(int sig);
static void dcc_create_kids(int listen_fd);
static int dcc_preforked_child(int listen_fd);

#ifdef __linux__
/* Only the parent owns idle TCP sockets. A worker owns exactly one active
 * socket, so an idle session never occupies a compiler slot. */
#define MAX_PENDING 256
#define SESSION_NEW 2
#define WORKER_RETIRING 4
#define SESSION_H2 8
#define WORKER_READY 16
#define WORKER_GRANT 32
#define WORKER_DRAIN 64
enum worker_phase { IDLE, EXECUTING, RECEIVING, WAITING, RETIRING, DRAINING };
struct worker { pid_t pid; int fd, busy, link; unsigned long ticket; };
struct pending {
    int fd, session, ready, used;
    time_t since;
    char request[12];
};
static struct worker *workers;
static struct pollfd *parent_polls;
static struct pending pending[MAX_PENDING];
static int worker_channel = -1;
static int worker_count, worker_capacity, worker_staged, worker_running;
static unsigned long next_ticket;
static struct { struct dcc_h2 *connection; time_t active; int poll_index; } h2[MAX_PENDING];

static int accept_h2_job(void *context, int fd, int link)
{
    int i, streams = 0;
    int message = SESSION_H2;
    (void)context;
    for (i = 0; i < MAX_PENDING; ++i) streams += dcc_h2_active(h2[i].connection);
    if (streams > MAX_PENDING) return -1;
    if (!dcc_resource_slots(dcc_max_kids)) return -1;
    for (i = 0; i < worker_count; ++i) {
        struct worker *w = &workers[i];
        if (w->fd < 0 || w->busy) continue;
        if (dcc_send_fd(w->fd, &message, sizeof(message), fd)) {
            close(w->fd);
            w->fd = -1;
            w->busy = 1;
            continue;
        }
        w->busy = RECEIVING;
        w->link = link;
        close(fd);
        return 0;
    }
    return -1;
}

int dcc_prefork_status(int *busy, int *queued, int *links)
{
    int i;
    *busy = *queued = *links = 0;
    if (!workers) return 0;
    for (i = 0; i < worker_count; ++i) {
        *busy += workers[i].fd >= 0 && workers[i].busy == EXECUTING;
        *links += workers[i].fd >= 0 && workers[i].busy == EXECUTING && workers[i].link;
        *queued += workers[i].fd >= 0 && (workers[i].busy == RECEIVING || workers[i].busy == WAITING);
    }
    for (i = 0; i < MAX_PENDING; ++i)
        *queued += pending[i].fd >= 0 && pending[i].ready;
#ifdef HAVE_GSSAPI
    if (dcc_auth_enabled) return 0;
#endif
    return 1;
}

void dcc_prefork_reaped(pid_t pid)
{
    int i;
    if (!workers) return;
    for (i = 0; i < worker_count; ++i)
        if (workers[i].pid == pid) {
            if (workers[i].fd >= 0) close(workers[i].fd);
            workers[i] = (struct worker){ .fd = -1 };
            return;
        }
}

static void park_connection(int fd, int session)
{
    int i;
    for (i = 0; i < MAX_PENDING; ++i) {
        if (pending[i].fd < 0) {
            pending[i] = (struct pending){ .fd = fd, .session = session, .since = time(NULL) };
            return;
        }
    }
    close(fd);
}

int dcc_prefork_poll(int listen_fd, struct pollfd *extra, int count, int timeout)
{
    struct pollfd *polls = parent_polls;
    int i, j, result, base = count + 1, sockets = base + worker_count;
    int total = sockets + MAX_PENDING;
    time_t now = time(NULL);
    for (i = 0; i < count; ++i) polls[i] = extra[i];
    polls[count] = (struct pollfd){ listen_fd, POLLIN, 0 };
    for (i = 0; i < worker_count; ++i)
        polls[base + i] = (struct pollfd){ workers[i].fd, POLLIN, 0 };
    for (i = 0; i < MAX_PENDING; ++i) {
        struct pending *p = &pending[i];
        if (p->fd >= 0 && now - p->since >= (p->ready ? dcc_get_io_timeout() : 30)) {
            close(p->fd);
            p->fd = -1;
        }
        polls[sockets + i] = (struct pollfd){ p->fd, p->ready ? 0 : POLLIN, 0 };
    }
    for (i = 0; i < MAX_PENDING; ++i) {
        h2[i].poll_index = -1;
        if (!h2[i].connection) continue;
        if (dcc_h2_idle(h2[i].connection) && now - h2[i].active >= 30) {
            dcc_h2_free(h2[i].connection);
            h2[i].connection = NULL;
            continue;
        }
        int added = dcc_h2_prepare(h2[i].connection, polls + total,
                    worker_count + 3 * MAX_PENDING + 3 - total);
        if (added < 0) return -1;
        h2[i].poll_index = total;
        total += added;
    }
    /* Bound idle expiry even without statistics or other activity. */
    result = poll(polls, total, timeout < 1000 ? timeout : 1000);
    for (i = 0; i < count; ++i) extra[i].revents = polls[i].revents;
    if (result < 0) return result;
    for (i = 0; i < worker_count; ++i) {
        if (polls[base + i].revents) {
            int fd, session;
            ssize_t n = dcc_recv_fd(workers[i].fd, &session, sizeof(session), &fd);
            if (n != sizeof(session)) {
                if (fd >= 0) close(fd);
                close(workers[i].fd);
                workers[i].fd = -1;
                workers[i].busy = 1;
            } else if (session == WORKER_READY && workers[i].busy == RECEIVING) {
                if (fd >= 0) close(fd);
                workers[i].busy = WAITING;
                workers[i].ticket = ++next_ticket;
            } else if (session == WORKER_DRAIN && workers[i].busy == EXECUTING) {
                if (fd >= 0) close(fd);
                workers[i].busy = DRAINING;
            } else {
                workers[i].busy = session & WORKER_RETIRING ? RETIRING : IDLE;
                workers[i].link = 0;
                if (fd >= 0) park_connection(fd, 1);
            }
        }
    }
    /* Ready inputs enter the compiler pool in arrival order. Receiving and
     * draining need no CPU grant; their total is bounded by the worker pool
     * and its fixed pipeline allowance. TCP reservations keep their existing
     * execution-slot contract. */
    for (;;) {
        int busy, queued, links, selected = -1, message = WORKER_GRANT;
        dcc_prefork_status(&busy, &queued, &links);
        if (busy >= dcc_resource_slots(dcc_max_kids)) break;
        for (i = 0; i < worker_count; ++i)
            if (workers[i].fd >= 0 && workers[i].busy == WAITING && !(workers[i].link && links) &&
                (selected < 0 || workers[i].ticket < workers[selected].ticket)) selected = i;
        if (selected < 0) break;
        struct worker *w = &workers[selected];
        if (dcc_send_fd(w->fd, &message, sizeof(message), -1)) {
            close(w->fd);
            w->fd = -1;
        }
        w->busy = EXECUTING;
    }
    for (i = 0; i < MAX_PENDING; ++i) {
        struct pending *p = &pending[i];
        if (p->fd >= 0 && polls[sockets + i].fd == p->fd && polls[sockets + i].revents) {
            char byte;
            ssize_t n = recv(p->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n > 0) {
                int admission = 1;
#ifdef HAVE_GSSAPI
                admission = !dcc_auth_enabled;
#endif
                if (admission && !p->used && byte == 'P') {
                    for (j = 0; j < MAX_PENDING && h2[j].connection; ++j) {}
                    if (j < MAX_PENDING) {
                        h2[j].connection = dcc_h2_new(p->fd, accept_h2_job, NULL);
                        h2[j].active = now;
                        h2[j].poll_index = -1;
                    }
                    if (j == MAX_PENDING || !h2[j].connection) close(p->fd);
                    else {
                        worker_count = worker_capacity;
                        dcc_stats_event(STATS_TCP_ACCEPT);
                    }
                    p->fd = -1;
                    continue;
                }
                if (admission && (p->used || byte == 'S')) {
                    n = recv(p->fd, p->request + p->used, 12 - p->used, MSG_DONTWAIT);
                    if (n > 0) p->used += n;
                    if (p->used == 12) {
                        if (memcmp(p->request, "SLOT00000001", 12) &&
                            memcmp(p->request, "SLOT00000002", 12)) {
                            close(p->fd);
                            p->fd = -1;
                        } else p->ready = 1;
                    }
                } else p->ready = 1;
            }
            else if (!n || (errno != EAGAIN && errno != EINTR)) {
                close(p->fd);
                p->fd = -1;
            }
        }
        if (p->fd < 0 || !p->ready) continue;
        int busy, queued, links;
        dcc_prefork_status(&busy, &queued, &links);
        int limit = dcc_resource_slots(dcc_max_kids);
        if (p->used) {
            int is_link = p->request[11] == '2';
            for (j = 0; j < worker_count; ++j)
                if (workers[j].fd >= 0 && !workers[j].busy) break;
            if (j == worker_count || busy >= limit || (is_link && links)) {
                /* Refuse immediately, retaining the TCP session for retry on
                 * another host. Idle sessions never reserve compiler slots. */
                if (send(p->fd, "BUSY00000000", 12, MSG_NOSIGNAL | MSG_DONTWAIT) != 12) {
                    close(p->fd);
                    p->fd = -1;
                }
                p->used = p->ready = 0;
                p->since = now;
                continue;
            }
            p->session |= 1;
        }
        if (busy >= limit) continue;
        for (j = 0; j < worker_count; ++j) {
            struct worker *w = &workers[j];
            if (w->fd < 0 || w->busy) continue;
            if (dcc_send_fd(w->fd, &p->session, sizeof(p->session), p->fd)) {
                close(w->fd);
                w->fd = -1;
                w->busy = 1;
                continue;
            }
            w->busy = 1;
            w->link = p->used && p->request[11] == '2';
            /* A dead worker channel must not produce a grant followed by a
             * second reply. Grant only after a worker owns the connection. */
            if (p->used && send(p->fd, "GRNT00000001", 12, MSG_NOSIGNAL | MSG_DONTWAIT) != 12)
                shutdown(p->fd, SHUT_RDWR);
            close(p->fd);
            p->fd = -1;
            break;
        }
    }
    for (i = 0; i < MAX_PENDING; ++i) {
        if (!h2[i].connection || h2[i].poll_index < 0) continue;
        if (dcc_h2_process(h2[i].connection, polls + h2[i].poll_index)) {
            dcc_h2_free(h2[i].connection);
            h2[i].connection = NULL;
        } else if (!dcc_h2_idle(h2[i].connection)) h2[i].active = now;
    }
    if (polls[count].revents & POLLIN) {
        struct sockaddr_storage address;
        socklen_t length = sizeof(address);
        int fd = accept4(listen_fd, (struct sockaddr *)&address, &length, SOCK_CLOEXEC);
        if (fd >= 0) {
            if (dcc_check_client((struct sockaddr *)&address, length, opt_allowed)) close(fd);
            else park_connection(fd, SESSION_NEW);
        }
    }
    return result;
}
#endif

int dcc_worker_acquire(int input)
{
#ifdef __linux__
    int message = WORKER_READY, fd = -1, ret;
    struct pollfd polls[2] = { { worker_channel, POLLIN, 0 }, { input, POLLIN, 0 } };
    if (!worker_staged) return 0;
    if (dcc_send_fd(worker_channel, &message, sizeof(message), -1)) return EXIT_IO_ERROR;
    do { ret = poll(polls, 2, dcc_get_io_timeout() * 1000); } while (ret < 0 && errno == EINTR);
    if (!ret) { ret = EXIT_TIMEOUT; goto cancelled; }
    if (ret < 0 || polls[1].revents || !(polls[0].revents & POLLIN)) {
        ret = EXIT_IO_ERROR;
        goto cancelled;
    }
    ret = dcc_recv_fd(worker_channel, &message, sizeof(message), &fd);
    if (fd >= 0) close(fd);
    if (ret != sizeof(message) || message != WORKER_GRANT) {
        ret = EXIT_PROTOCOL_ERROR;
        goto cancelled;
    }
    worker_running = 1;
    return 0;
cancelled:
    /* A grant may already be queued while input cancellation wakes us. Retire
     * this worker channel so that a stale grant cannot become the next job's
     * descriptor or release a slot belonging to that job. */
    shutdown(worker_channel, SHUT_RDWR);
    return ret;
#else
    (void)input;
#endif
    return 0;
}

void dcc_worker_release(void)
{
#ifdef __linux__
    int message = WORKER_DRAIN;
    if (worker_running) {
        dcc_send_fd(worker_channel, &message, sizeof(message), -1);
        worker_running = 0;
    }
#endif
}

/**
 * Main loop for the parent process with the new preforked implementation.
 * The parent maintains compiler workers. On Linux it also dispatches ready
 * connections, retaining idle sessions independently of those workers.
 **/
int dcc_preforking_parent(int listen_fd)
{
    int ret;
    /* use sigaction instead of signal() because we need persistent handler, not oneshot */
    struct sigaction act_child;
    memset(&act_child, 0, sizeof act_child);
    act_child.sa_handler = dcc_sigchld_handler;
    sigaction(SIGCHLD, &act_child, NULL);
#ifdef __linux__
    worker_count = dcc_max_kids;
    worker_capacity = dcc_max_kids + (dcc_max_kids < DCC_H2_PIPELINE ? dcc_max_kids : DCC_H2_PIPELINE);
    workers = calloc(worker_capacity, sizeof(*workers));
    parent_polls = calloc(worker_capacity + 3 * MAX_PENDING + 3, sizeof(*parent_polls));
    if (!workers || !parent_polls) return EXIT_OUT_OF_MEMORY;
    for (ret = 0; ret < worker_capacity; ++ret) workers[ret].fd = -1;
    for (ret = 0; ret < MAX_PENDING; ++ret) pending[ret].fd = -1;
#endif

    if (arg_stats) {

        ret = dcc_stats_init();
        if (ret) return ret;

        dcc_create_kids(listen_fd);

        /* Start the stats collection and web server */
        return dcc_stats_server(listen_fd);
    } else {
        while (1) {
            dcc_create_kids(listen_fd);

            /* wait for any children to exit, and then start some more */
#ifdef __linux__
            if (dcc_prefork_poll(listen_fd, NULL, 0, 1000) < 0 && errno != EINTR)
                return EXIT_IO_ERROR;
            dcc_reap_kids(FALSE);
#else
            dcc_reap_kids(TRUE);
#endif
        }
    }
}


static void dcc_sigchld_handler(int UNUSED(sig)) {
    /* Do nothing.  Only here to break out of select() in dcc_stats_server()
     * and select() in dcc_collect_child(). */
}


/**
 * Functions in the parent can call this to clean up and maintain the pool of
 * children
 **/
void dcc_manage_kids(int listen_fd) {
    dcc_reap_kids(FALSE);
    dcc_create_kids(listen_fd);
}

/**
 * Maintain execution workers plus the fixed HTTP/2 pipeline allowance.
 **/
static void dcc_create_kids(int listen_fd) {
    pid_t kid;
    int maximum = dcc_max_kids;
#ifdef __linux__
    maximum = worker_count;
#endif

    while (dcc_nkids < maximum) {
#ifdef __linux__
        int pair[2], slot, i;
        for (slot = 0; slot < worker_count && workers[slot].pid; ++slot) {}
        if (slot == worker_count || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
            dcc_exit(EXIT_OUT_OF_MEMORY);
#endif
        if ((kid = fork()) == -1) {
            rs_log_error("fork failed: %s", strerror(errno));
            dcc_exit(EXIT_OUT_OF_MEMORY); /* probably */
        } else if (kid == 0) {
#ifdef __linux__
            close(pair[0]);
            worker_channel = pair[1];
            close(listen_fd);
            for (i = 0; i < worker_count; ++i)
                if (workers[i].fd >= 0) close(workers[i].fd);
            for (i = 0; i < MAX_PENDING; ++i)
                if (pending[i].fd >= 0) close(pending[i].fd);
            for (i = 0; i < MAX_PENDING; ++i) {
                dcc_h2_free(h2[i].connection);
                h2[i].connection = NULL;
            }
#endif
            dcc_stats_init_kid();
            dcc_exit(dcc_preforked_child(listen_fd));
        } else {
#ifdef __linux__
            close(pair[1]);
            workers[slot] = (struct worker){ .pid = kid, .fd = pair[0] };
#endif
            /* in parent */
            ++dcc_nkids;
            rs_trace("up to %d children", dcc_nkids);
        }
    }
}



/**
 * Fork a child to repeatedly handle assigned (or directly accepted) jobs.
 *
 * To protect against leaks, we quit after 50 requests and let the parent
 * recreate us.
 **/
static int dcc_preforked_child(int listen_fd)
{
    int ireq;
    time_t start, now;
    const int child_requests = 50;
    const time_t child_lifetime = 60 /* seconds */;
    start = now = time(NULL);
#ifdef __linux__
    (void)listen_fd;
#endif

#ifdef HAVE_LINUX
    if (opt_oom_score_adj != INT_MIN) {
        FILE *f = fopen("/proc/self/oom_score_adj", "w");
        if (!f || fprintf(f, "%d\n", opt_oom_score_adj) < 0)
            rs_log_warning("failed to set oom_score_adj: %s", strerror(errno));
        if (f)
            fclose(f);
    }
#endif

    for (ireq = 0; ireq < child_requests || now - start < child_lifetime; ireq++) {
        int acc_fd;
        struct dcc_sockaddr_storage cli_addr;
        socklen_t cli_len;
#ifdef __linux__
        int message, session, ret;
#endif

        cli_len = sizeof cli_addr;

    /* Cancel any previously scheduled alarm */
        if (dcc_job_lifetime)
            alarm(0);

#ifdef __linux__
        if (dcc_recv_fd(worker_channel, &message, sizeof(message), &acc_fd) != sizeof(message) || acc_fd < 0)
            return 0;
        session = message & 1;
        worker_staged = !!(message & SESSION_H2);
        worker_running = 0;
        if (getpeername(acc_fd, (struct sockaddr *)&cli_addr, &cli_len)) {
            close(acc_fd);
            return EXIT_CONNECT_FAILED;
        }
#else
        do {
            acc_fd = accept(listen_fd, (struct sockaddr *) &cli_addr,
                            &cli_len);
        } while (acc_fd == -1 && errno == EINTR);
#endif

        /* Kill this process if the compile job takes too long.
         * The synchronous timeout should happen first, so this alarm
         * should fire only if the client stops transferring network data without disconnecting.
         */
        if (dcc_job_lifetime)
            alarm(dcc_job_lifetime+30);


        if (acc_fd == -1) {
            rs_log_error("accept failed: %s", strerror(errno));
            dcc_exit(EXIT_CONNECT_FAILED);
        }

#ifdef __linux__
        if (message & SESSION_NEW) dcc_stats_event(STATS_TCP_ACCEPT);
        /* HTTP/2 arrives over a private socketpair after the parent checked
         * the TCP peer against the ACL. Do not apply an IP ACL to AF_UNIX. */
        ret = dcc_service_job(acc_fd, acc_fd,
                message & SESSION_H2 ? NULL : (struct sockaddr *)&cli_addr,
                cli_len, &session);
        dcc_worker_release();
        now = time(NULL);
        message = ireq + 1 >= child_requests && now - start >= child_lifetime ? WORKER_RETIRING : 0;
        if (dcc_send_fd(worker_channel, &message, sizeof(message), !ret && session ? acc_fd : -1)) {
            close(acc_fd);
            return EXIT_IO_ERROR;
        }
#else
        dcc_stats_event(STATS_TCP_ACCEPT);

        dcc_service_job(acc_fd, acc_fd,
                           (struct sockaddr *) &cli_addr, cli_len, NULL);
#endif

        dcc_close(acc_fd);
        now = time(NULL);
    }

    rs_log_info("worn out");

    return 0;
}
