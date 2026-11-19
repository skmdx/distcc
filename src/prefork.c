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
#include "cpuperf.h"

static void dcc_sigchld_handler(int sig);
static void dcc_create_kids(void);

#include "tcp_job.h"
#include "rust_job.h"
#include "tcp_async.h"
#include "clang_worker.h"
/* Only the parent owns idle sockets. Legacy workers borrow an active socket;
 * prepared-job workers receive input metadata instead. */
#define MAX_PENDING 256
#define SESSION_NEW 2
#define WORKER_RETIRING 4
enum worker_phase { IDLE, EXECUTING, RETIRING, SUSPENDED, RESUMING };
struct worker {
    pid_t pid;
    int fd, busy, link;
    unsigned codegen_cpus;
    struct dcc_tcp_server *tcp;
    unsigned memory_class;
    unsigned long long reservation;
};
struct pending {
    int fd, session, ready, used;
    time_t since;
    char request[12];
};
static struct worker *workers;
#include "epoll_set.h"
struct interest {
    struct dcc_watch watch;
    int fd_plus_one;
    unsigned events;
};
static struct interest *interests;
static int interest_count, parent_ep = -1;
static void watch_parent(int index, int fd, unsigned events)
{
    interests[index].fd_plus_one = fd + 1;
    interests[index].events = events;
}
static struct pending pending[MAX_PENDING];
static int worker_channel = -1;
static int worker_count;
static int local_listener = -1;
static struct dcc_tcp_server *tcp[MAX_PENDING];
static volatile sig_atomic_t shutdown_signal;
enum { LISTEN, ASYNC, LOCAL, WORKERS };
static void forget_parent(int index)
{
    if (index < interest_count) dcc_watch_drop(parent_ep, &interests[index].watch);
}
void dcc_prefork_forget_extra(int index)
{
    forget_parent(WORKERS + worker_count + 3 * MAX_PENDING + index);
}
static void close_worker(struct worker *w)
{
    forget_parent(WORKERS + (w - workers));
    close(w->fd);
    w->fd = -1;
}

static void receive_worker(struct worker *);
static void receive_worker_message(struct worker *);

static int accept_tcp_job(struct dcc_tcp_server *job, const struct dcc_tcp_run *run,
                          int fd, int input, const struct dcc_job *description)
{
    struct dcc_server_load load;
    dcc_prefork_status(&load);
    int link = description->kind == DCC_JOB_LINK;
    unsigned memory_class = dcc_resource_class(description);
    if (load.busy >= dcc_cpu_perf_limit(dcc_max_kids) || (link && load.links)) return 0;
    if (!dcc_resource_admit(memory_class, load.reserved)) return EXIT_BUSY;
    for (int i = 0; i < worker_count; ++i) {
        struct worker *w = &workers[i];
        if (w->fd < 0 || w->busy) continue;
        size_t length = offsetof(struct dcc_tcp_run, root) + strlen(run->root) + 1;
        if (description->kind == DCC_JOB_RUST && (input = dcc_rust_catalog_fd()) < 0) return -1;
        int descriptors[3] = {fd, input, -1};
        unsigned count = input >= 0 ? 2 : 1;
        if (description->kind == DCC_JOB_FRONTEND) {
            descriptors[2] = dcc_clang_pool_get(description->argv[0]);
            if (descriptors[2] < 0) return -1;
            count = 3;
        }
        if (dcc_send_fds(w->fd, run, length, descriptors, count)) {
            close_worker(w); w->busy = RETIRING;
            return -1;
        }
        w->tcp = job; w->busy = EXECUTING; w->link = link;
        w->memory_class = memory_class;
        w->reservation = dcc_resource_estimate(memory_class);
        dcc_cpu_perf_started();
        return 1;
    }
    return 0;
}
static void cancel_tcp_job(struct dcc_tcp_server *job)
{
    for (int i = 0; i < worker_count; ++i) if (workers[i].tcp == job) {
        struct worker *w = &workers[i];
        /* Keep the job/tree until the worker is actually reaped. */
        if (w->fd >= 0) { shutdown(w->fd, SHUT_RDWR); close_worker(w); }
        w->busy = RETIRING;
    }
}
static unsigned reserve_codegen(struct dcc_tcp_server *, unsigned);
static int reserve_execution(struct dcc_tcp_server *, unsigned);
static const struct dcc_tcp_dispatch tcp_dispatch = {
    accept_tcp_job, cancel_tcp_job, reserve_codegen, reserve_execution
};

int dcc_prefork_shutdown(int signal_number)
{
    if (!workers) return 0;
    shutdown_signal = signal_number;
    return 1;
}

static void finish_shutdown(void)
{
    if (local_listener >= 0) {
        struct sockaddr_un address;
        close(local_listener);
        if (!dcc_tcp_local_address(arg_port, &address)) unlink(address.sun_path);
    }
    dcc_tcp_async_finish();
    dcc_clang_pool_shutdown();
    /* Run outside the signal handler: tree cleanup uses allocator/directory
     * routines, and must follow worker death rather than race compiler IO. */
    for (int i = 0; i < worker_count; ++i) if (workers[i].pid > 0) {
        killpg(workers[i].pid, SIGKILL);
        kill(workers[i].pid, SIGTERM);
    }
    for (int i = 0; i < worker_count; ++i) if (workers[i].pid > 0) {
        pid_t pid = workers[i].pid;
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        dcc_prefork_reaped(pid);
    }
    for (int i = 0; i < MAX_PENDING; ++i) {
        dcc_tcp_server_free(tcp[i]); tcp[i] = NULL;
    }
    dcc_tcp_async_finish(); /* Includes trees detached during final reclamation. */
    dcc_remove_pid();
    raise(shutdown_signal);
    _exit(128 + shutdown_signal);
}


int dcc_prefork_status(struct dcc_server_load *load)
{
    int i;
    *load = (struct dcc_server_load){0};
    if (!workers) return 0;
    for (i = 0; i < worker_count; ++i) {
        load->busy += workers[i].fd >= 0 && workers[i].busy == EXECUTING;
        load->busy += workers[i].codegen_cpus;
        load->links += workers[i].fd >= 0 && workers[i].busy == EXECUTING && workers[i].link;
        load->queued += workers[i].fd >= 0 && workers[i].busy == RESUMING;
        load->suspended += workers[i].fd >= 0 && workers[i].busy == SUSPENDED;
        struct worker *w = &workers[i];
        if (!w->busy || !w->reservation) continue;
        unsigned long long charge = w->reservation;
        if (w->tcp) {
            unsigned long long estimate = dcc_resource_estimate(w->memory_class);
            if (estimate > charge) charge = estimate;
        }
        unsigned copies = 1 + w->codegen_cpus;
        charge = charge <= ULLONG_MAX / copies ? charge * copies : ULLONG_MAX;
        load->reserved = charge <= ULLONG_MAX - load->reserved ? load->reserved + charge : ULLONG_MAX;
    }
    for (i = 0; i < MAX_PENDING; ++i)
        load->queued += pending[i].fd >= 0 && pending[i].ready;
    for (i = 0; i < MAX_PENDING; ++i)
        dcc_tcp_server_load(tcp[i], load);
#ifdef HAVE_GSSAPI
    if (dcc_auth_enabled) return 0;
#endif
    return 1;
}

static int reserve_execution(struct dcc_tcp_server *job, unsigned active)
{
    for (int i = 0; i < worker_count; ++i) {
        struct worker *w = &workers[i];
        if (w->tcp != job) continue;
        if (!active) {
            if (w->busy != EXECUTING || w->codegen_cpus) return -1;
            w->busy = SUSPENDED;
            rs_trace("execution lane suspended worker=%ld", (long)w->pid);
            return 1;
        }
        if (active != 1 || (w->busy != SUSPENDED && w->busy != RESUMING)) return -1;
        struct dcc_server_load load;
        dcc_prefork_status(&load);
        w->busy = RESUMING;
        if (load.busy >= dcc_cpu_perf_limit(dcc_max_kids)) return 0;
        w->busy = EXECUTING;
        dcc_cpu_perf_started();
        rs_trace("execution lane resumed worker=%ld", (long)w->pid);
        return 1;
    }
    return -1;
}

static unsigned reserve_codegen(struct dcc_tcp_server *job, unsigned requested)
{
    for (int i = 0; i < worker_count; ++i) {
        struct worker *w = &workers[i];
        if (w->tcp != job || w->busy != EXECUTING) continue;
        if (!requested) { w->codegen_cpus = 0; return 0; }
        if (w->codegen_cpus) return w->codegen_cpus;
        struct dcc_server_load load;
        dcc_prefork_status(&load);
        int logical, physical;
        double core_scale, smt_gain;
        dcc_cpu_topology(&logical, &physical, &core_scale, &smt_gain);
        int limit = dcc_cpu_perf_limit(dcc_max_kids), available = dcc_resource_cpus();
        if (limit > physical) limit = physical;
        if (limit > available) limit = available;
        int running = dcc_getcurrentload();
        if (running > 0) --running; /* Exclude this serving parent. */
        int occupied = load.busy > running ? load.busy : running;
        occupied += load.preparing;
        /* Optional backend lanes never wait or displace ready compilations.
         * The parent accounts for them through merge, cancellation and reaping. */
        while (!load.queued && w->codegen_cpus < requested && occupied < limit &&
               dcc_resource_admit(w->memory_class, load.reserved)) {
            ++w->codegen_cpus; ++occupied;
            dcc_prefork_status(&load);
        }
        rs_trace("codegen CPUs requested=%u granted=%u worker=%ld", requested, w->codegen_cpus, (long)w->pid);
        return w->codegen_cpus;
    }
    return 0;
}

void dcc_prefork_reaped(pid_t pid)
{
    int i;
    if (!workers) return;
    for (i = 0; i < worker_count; ++i)
        if (workers[i].pid == pid) {
            /* A killed prepared worker cannot report or reap its compiler. */
            killpg(pid, SIGKILL);
            /* A retiring worker can exit immediately after sending its
             * result. SIGCHLD may precede the control-fd poll event. Drain
             * those owned descriptors before reporting a lost worker. */
            while (workers[i].fd >= 0) {
                struct pollfd p = {workers[i].fd, POLLIN, 0};
                if (poll(&p, 1, 0) <= 0) break;
                receive_worker(&workers[i]);
            }
            dcc_tcp_server_lost(workers[i].tcp);
            if (workers[i].fd >= 0) close_worker(&workers[i]);
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

static void receive_worker(struct worker *w)
{
    dcc_tcp_ready_idle_ns();
    receive_worker_message(w);
    dcc_tcp_ready_idle_ns();
}

static void receive_worker_message(struct worker *w)
{
    if (w->tcp) {
        struct dcc_tcp_finished finished;
        int fds[DCC_IPC_FDS_MAX];
        unsigned count = DCC_IPC_FDS_MAX;
        ssize_t n = dcc_recv_fds(w->fd, &finished, sizeof(finished), fds, &count);
        if (n != sizeof(finished)) {
            for (unsigned i = 0; i < count; ++i) close(fds[i]);
            /* EOF alone does not prove the compiler has stopped using inputs. */
            close_worker(w); w->busy = RETIRING;
            return;
        }
        struct dcc_tcp_server *job = w->tcp;
        dcc_resource_observe(w->memory_class, finished.peak_bytes, !finished.error && !finished.status);
        w->reservation = 0;
        w->codegen_cpus = 0;
        w->tcp = NULL; w->busy = finished.retiring ? RETIRING : IDLE; w->link = 0;
        dcc_tcp_ready_idle_ns();
        dcc_tcp_server_finished(job, &finished, fds, count);
        return;
    }
    int fd, session;
    ssize_t n = dcc_recv_fd(w->fd, &session, sizeof(session), &fd);
    if (n != sizeof(session)) {
        if (fd >= 0) close(fd);
        close_worker(w);
        w->busy = RETIRING;
    } else {
        w->busy = session & WORKER_RETIRING ? RETIRING : IDLE;
        w->link = 0;
        w->reservation = 0;
        if (fd >= 0) park_connection(fd, 1);
    }
}

int dcc_prefork_poll(int listen_fd, struct pollfd *extra, int count, int timeout)
{
    if (shutdown_signal) finish_shutdown();
    int i, j, result, base = WORKERS, sockets = base + worker_count;
    int tcp_base = sockets + MAX_PENDING, fs_base = tcp_base + MAX_PENDING;
    int extras = fs_base + MAX_PENDING, total = extras + count;
    if (parent_ep < 0 && (parent_ep = epoll_create1(EPOLL_CLOEXEC)) < 0) return -1;
    if (total > interest_count) {
        interests = realloc(interests, total * sizeof(*interests));
        if (!interests) dcc_exit(EXIT_OUT_OF_MEMORY);
        memset(interests + interest_count, 0, (total - interest_count) * sizeof(*interests));
        interest_count = total;
    }
    time_t now = time(NULL);
    watch_parent(LISTEN, listen_fd, EPOLLIN);
    watch_parent(ASYNC, dcc_tcp_async_fd(), EPOLLIN);
    watch_parent(LOCAL, local_listener, EPOLLIN);
    for (i = extras; i < interest_count; ++i)
        watch_parent(i, i < total ? extra[i-extras].fd : -1, i < total ? extra[i-extras].events : 0);
    for (i = 0; i < worker_count; ++i) watch_parent(base+i, workers[i].fd, EPOLLIN);
    int busy = 0;
    for (i = 0; i < worker_count; ++i)
        busy += workers[i].fd >= 0 && workers[i].busy == EXECUTING;
    timeout = dcc_cpu_perf_poll(busy, timeout);
    for (i = 0; i < MAX_PENDING; ++i) {
        struct pending *p = &pending[i];
        if (p->fd >= 0 && now - p->since >= (p->ready ? dcc_get_io_timeout() : 30)) {
            forget_parent(sockets+i);
            close(p->fd);
            p->fd = -1;
        }
        watch_parent(sockets+i, p->fd, p->ready ? 0 : EPOLLIN);
    }
    int runnable = dcc_tcp_server_pending();
    for (i = 0; i < MAX_PENDING; ++i) {
        struct pollfd net = {.fd = -1}, fs = {.fd = -1};
        if (tcp[i]) {
            runnable |= dcc_tcp_server_poll(tcp[i], &net);
            dcc_tcp_server_fs_poll(tcp[i], &fs);
        }
        watch_parent(tcp_base+i, net.fd, net.events);
        watch_parent(fs_base+i, fs.fd, fs.events);
    }
    /* A pending connection can move to a TCP job under the same fd. Remove
     * old owners before registering new ones, including reordered stats clients. */
    for (i = 0; i < interest_count; ++i)
        if (interests[i].fd_plus_one != interests[i].watch.fd_plus_one)
            dcc_watch_drop(parent_ep, &interests[i].watch);
    for (i = 0; i < total; ++i)
        if (dcc_watch_set(parent_ep, &interests[i].watch, interests[i].fd_plus_one - 1,
                          interests[i].events, i)) return -1;
    struct epoll_event events[128];
    result = epoll_wait(parent_ep, events, 128, runnable ? 0 : timeout < 1000 ? timeout : 1000);
    for (i = 0; i < result; ++i)
        dcc_watch_ready(&interests[events[i].data.u64 >> 32].watch, events[i]);
    if (shutdown_signal) finish_shutdown();
    for (i = 0; i < count; ++i) extra[i].revents = interests[extras+i].watch.ready;
    if (result < 0) return result;
    if (interests[ASYNC].watch.ready) dcc_tcp_async_drain();
    dcc_tcp_server_collect();
    for (i = 0; i < worker_count; ++i)
        if (interests[base+i].watch.ready) receive_worker(&workers[i]);
    for (i = 0; i < MAX_PENDING; ++i) {
        struct pending *p = &pending[i];
        if (p->fd >= 0 && interests[sockets+i].watch.fd_plus_one == p->fd + 1 && interests[sockets+i].watch.ready) {
            char byte;
            ssize_t n = recv(p->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n > 0) {
                int admission = 1;
#ifdef HAVE_GSSAPI
                admission = !dcc_auth_enabled;
#endif
                if (admission && !p->used && byte == 'T') {
                    for (j = 0; j < MAX_PENDING && tcp[j]; ++j) {}
                    forget_parent(sockets+i);
                    if (j == MAX_PENDING || !(tcp[j] = dcc_tcp_server_new(p->fd, &tcp_dispatch))) close(p->fd);
                    else dcc_stats_event(STATS_TCP_ACCEPT);
                    p->fd = -1;
                    continue;
                }
                if (admission && (p->used || byte == 'S')) {
                    n = recv(p->fd, p->request + p->used, 12 - p->used, MSG_DONTWAIT);
                    if (n > 0) p->used += n;
                    if (p->used == 12) {
                        if (memcmp(p->request, "SLOT00000001", 12) &&
                            memcmp(p->request, "SLOT00000002", 12)) {
                            forget_parent(sockets+i);
                            close(p->fd);
                            p->fd = -1;
                        } else p->ready = 1;
                    }
                } else p->ready = 1;
            }
            else if (!n || (errno != EAGAIN && errno != EINTR)) {
                forget_parent(sockets+i);
                close(p->fd);
                p->fd = -1;
            }
        }
        if (p->fd < 0 || !p->ready) continue;
        struct dcc_server_load load;
        dcc_prefork_status(&load);
        int limit = dcc_cpu_perf_limit(dcc_resource_slots(dcc_max_kids));
        if (p->used) {
            int is_link = p->request[11] == '2';
            for (j = 0; j < worker_count; ++j)
                if (workers[j].fd >= 0 && !workers[j].busy) break;
            if (j == worker_count || load.busy >= limit || (is_link && load.links)) {
                /* Refuse immediately, retaining the TCP session for retry on
                 * another host. Idle sessions never reserve compiler slots. */
                if (send(p->fd, "BUSY00000000", 12, MSG_NOSIGNAL | MSG_DONTWAIT) != 12) {
                    forget_parent(sockets+i);
                    close(p->fd);
                    p->fd = -1;
                }
                p->used = p->ready = 0;
                p->since = now;
                continue;
            }
            p->session |= 1;
        }
        if (load.busy >= limit) continue;
        for (j = 0; j < worker_count; ++j) {
            struct worker *w = &workers[j];
            if (w->fd < 0 || w->busy) continue;
            if (dcc_send_fd(w->fd, &p->session, sizeof(p->session), p->fd)) {
                close_worker(w);
                w->busy = 1;
                continue;
            }
            w->busy = 1;
            w->link = p->used && p->request[11] == '2';
            w->memory_class = 0;
            w->reservation = dcc_resource_job_bytes();
            dcc_cpu_perf_started();
            /* A dead worker channel must not produce a grant followed by a
             * second reply. Grant only after a worker owns the connection. */
            if (p->used && send(p->fd, "GRNT00000001", 12, MSG_NOSIGNAL | MSG_DONTWAIT) != 12)
                shutdown(p->fd, SHUT_RDWR);
            forget_parent(sockets+i);
            close(p->fd);
            p->fd = -1;
            break;
        }
    }
    for (i = 0; i < MAX_PENDING; ++i) {
        if (tcp[i] && (dcc_tcp_server_fs_process(tcp[i], interests[fs_base+i].watch.ready) ||
                       dcc_tcp_server_process(tcp[i], interests[tcp_base+i].watch.ready))) {
            forget_parent(tcp_base+i);
            forget_parent(fs_base+i);
            dcc_tcp_server_free(tcp[i]); tcp[i] = NULL;
        }
    }
    /* Suspended compilers retain their result/cancel owner, but must not take
     * an executor away from ready work. Spawn only the missing owners; the
     * existing CPU and memory admission limits still govern execution. */
    dcc_create_kids();
    dcc_tcp_dispatch_ready();
    if (interests[LISTEN].watch.ready & POLLIN) {
        struct sockaddr_storage address;
        socklen_t length = sizeof(address);
        int fd = accept4(listen_fd, (struct sockaddr *)&address, &length, SOCK_CLOEXEC);
        if (fd >= 0) {
            if (dcc_check_client((struct sockaddr *)&address, length, opt_allowed)) close(fd);
            else park_connection(fd, SESSION_NEW);
        }
    }
    if (interests[LOCAL].watch.ready & POLLIN) {
        int fd = accept4(local_listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            for (j = 0; j < MAX_PENDING && tcp[j]; ++j) {}
            if (j == MAX_PENDING || !(tcp[j] = dcc_tcp_server_new(fd, &tcp_dispatch))) close(fd);
        }
    }
    return result;
}

/**
 * Main loop for the parent process with the new preforked implementation.
 * The parent maintains compiler workers and dispatches ready
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
    worker_count = dcc_max_kids + MAX_PENDING;
    workers = calloc(worker_count, sizeof(*workers));
    if (!workers) return EXIT_OUT_OF_MEMORY;
    for (ret = 0; ret < worker_count; ++ret) workers[ret].fd = -1;
    for (ret = 0; ret < MAX_PENDING; ++ret) pending[ret].fd = -1;
    if (getenv("DISTCC_LOCAL_SOCKET")) {
        local_listener = dcc_tcp_local_listener(arg_port);
        if (local_listener < 0) {
            rs_log_error("cannot create local job socket: %s", strerror(errno));
            return EXIT_BIND_FAILED;
        }
    }

    if (arg_stats) {

        ret = dcc_stats_init();
        if (ret) return ret;

        dcc_create_kids();

        /* Start the stats collection and web server */
        return dcc_stats_server(listen_fd);
    } else {
        while (1) {
            dcc_create_kids();

            /* wait for any children to exit, and then start some more */
            if (dcc_prefork_poll(listen_fd, NULL, 0, 1000) < 0 && errno != EINTR)
                return EXIT_IO_ERROR;
            dcc_reap_kids(FALSE);
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
void dcc_manage_kids(void) {
    dcc_reap_kids(FALSE);
    dcc_create_kids();
}

/**
 * Maintain the compiler execution worker pool.
 **/
static void dcc_create_kids(void) {
    pid_t kid;
    int maximum = dcc_max_kids;
    for (int i = 0; i < worker_count; ++i)
        maximum += workers[i].busy == SUSPENDED || workers[i].busy == RESUMING;
    /* Once the waits end, retire surplus idle owners without interrupting a
     * compiler. At most MAX_PENDING TCP jobs can retain suspended owners. */
    int retiring = 0;
    for (int i = 0; i < worker_count; ++i) retiring += workers[i].busy == RETIRING;
    for (int i = worker_count; i-- && dcc_nkids - retiring > maximum;) {
        struct worker *w = &workers[i];
        if (w->pid && w->fd >= 0 && w->busy == IDLE) {
            close_worker(w); w->busy = RETIRING; ++retiring;
        }
    }

    while (dcc_nkids < maximum) {
        int pair[2], slot;
        for (slot = 0; slot < worker_count && workers[slot].pid; ++slot) {}
        if (slot == worker_count || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
            dcc_exit(EXIT_OUT_OF_MEMORY);
        int error = dcc_spawn_worker(&kid, pair[1]);
        close(pair[1]);
        if (error) {
            rs_log_error("spawn worker: %s", strerror(error));
            dcc_exit(EXIT_DISTCC_FAILED);
        }
        workers[slot] = (struct worker){ .pid = kid, .fd = pair[0] };
        ++dcc_nkids;
        rs_trace("up to %d children", dcc_nkids);
    }
}



/**
 * Repeatedly handle assigned jobs.
 *
 * To protect against leaks, we quit after 50 requests and let the parent
 * recreate us.
 **/
int dcc_run_worker(void)
{
    worker_channel = DCC_WORKER_CHANNEL_FD;
    if (set_cloexec_flag(worker_channel, 1) ||
        (arg_stats && set_cloexec_flag(DCC_WORKER_STATS_FD, 1))) return EXIT_IO_ERROR;
    dcc_statspipe[1] = DCC_WORKER_STATS_FD;
    dcc_daemon_catch_signals();
    int ireq;
    time_t start, now;
    const int child_requests = 50;
    const time_t child_lifetime = 60 /* seconds */;
    start = now = time(NULL);

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
        int message, session, ret;

        cli_len = sizeof cli_addr;

    /* Cancel any previously scheduled alarm */
        if (dcc_job_lifetime)
            alarm(0);

        struct dcc_tcp_run run;
        int descriptors[3];
        unsigned descriptor_count = 3;
        ssize_t request_size = dcc_recv_fds(worker_channel, &run, sizeof(run), descriptors, &descriptor_count);
        if (request_size < 0 || !descriptor_count) return 0;
        acc_fd = descriptors[0];
        int input = descriptor_count >= 2 ? descriptors[1] : -1;
        int frontend = descriptor_count == 3 ? descriptors[2] : -1;
        message = run.operation;
        if (request_size > (ssize_t)offsetof(struct dcc_tcp_run, root) &&
            request_size <= (ssize_t)sizeof(run) && message == DCC_TCP_RUN &&
            run.root[request_size - offsetof(struct dcc_tcp_run, root) - 1] == 0) {
            if (dcc_new_pgrp()) {
                for (unsigned i = 0; i < descriptor_count; ++i) close(descriptors[i]);
                return EXIT_IO_ERROR;
            }
            dcc_worker_group = 1;
            now = time(NULL);
            int retiring = ireq + 1 >= child_requests && now - start >= child_lifetime;
            ret = dcc_tcp_worker(worker_channel, &run, acc_fd, input, frontend, retiring);
            if (ret || retiring) return ret;
            /* The loop must use the same lifetime snapshot as the result's
             * retiring flag. Crossing the deadline during this job must not
             * exit after advertising an idle worker to the parent. */
            continue;
        }
        if (request_size != sizeof(message) || input >= 0) {
            for (unsigned i = 0; i < descriptor_count; ++i) close(descriptors[i]);
            return 0;
        }
        dcc_worker_group = 0;
        session = message & 1;
        if (getpeername(acc_fd, (struct sockaddr *)&cli_addr, &cli_len)) {
            close(acc_fd);
            return EXIT_CONNECT_FAILED;
        }

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

        if (message & SESSION_NEW) dcc_stats_event(STATS_TCP_ACCEPT);
        ret = dcc_service_job(acc_fd, acc_fd,
                (struct sockaddr *)&cli_addr,
                cli_len, &session);
        now = time(NULL);
        message = ireq + 1 >= child_requests && now - start >= child_lifetime ? WORKER_RETIRING : 0;
        if (dcc_send_fd(worker_channel, &message, sizeof(message), !ret && session ? acc_fd : -1)) {
            close(acc_fd);
            return EXIT_IO_ERROR;
        }

        dcc_close(acc_fd);
    }

    rs_log_info("worn out");

    return 0;
}
