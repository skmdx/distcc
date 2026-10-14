/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright 2005 Google Inc.
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

/* Author: Thomas Kho */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/statvfs.h>

#include "exitcode.h"
#include "distcc.h"
#include "trace.h"
#include "dopt.h"
#include "stats.h"
#include "srvnet.h"
#include "util.h"
#include "netutil.h"
#include "fcntl.h"
#include "daemon.h"
#include "resource.h"
#include "cpuperf.h"
#include <poll.h>
#ifdef __linux__
#include "tcp_job.h"
#include "tcp_async.h"
#endif

int dcc_statspipe[2];

#define MAX_FILENAME_LEN 1024

/* in prefork.c */
void dcc_manage_kids(int listen_fd);

struct stats_s {
    int counters[STATS_ENUM_MAX];
    int kids_avg[3]; /* 1, 5, 15m */
    int longest_job_time;
    char longest_job_name[MAX_FILENAME_LEN];
    char longest_job_compiler[MAX_FILENAME_LEN];
    int io_rate; /* read/write sectors per second */
    int compile_timeseries[300]; /* 300 3-sec time intervals */

    /*
     * linked list of statsdata. This is to keep the last
     * couple of minutes of compile stats for analysis.
     */
    struct statsdata *sd_root;
} dcc_stats;

struct statsdata {
    enum stats_e type;
    struct statsdata *next;

    /* used only for STATS_COMPILE_OK */
    struct timeval start;
    struct timeval stop;
    int time;
    char filename[MAX_FILENAME_LEN];
    char compiler[MAX_FILENAME_LEN];
};

const char *stats_text[20] = { "TCP_ACCEPT", "REJ_BAD_REQ", "REJ_OVERLOAD",
    "COMPILE_OK", "COMPILE_ERROR", "COMPILE_TIMEOUT", "CLI_DISCONN",
    "OTHER" };

/* Call this to initialize stats */
int dcc_stats_init(void) {
    if (arg_stats) {
        if (pipe(dcc_statspipe) == -1) {
            return -1;
        }
    }
    memset(&dcc_stats, 0, sizeof(dcc_stats));
    return 0;
}


/* In the preforking model call this to initialize stats for forked children */
void dcc_stats_init_kid(void) {
    if (arg_stats) {
        close(dcc_statspipe[0]);
    }
}


/**
 * Logs countable event of type e to stats server
 **/
void dcc_stats_event(enum stats_e e) {
    if (arg_stats) {
        struct statsdata sd;
        memset(&sd, 0, sizeof(sd));
        sd.type = e;
        dcc_writex(dcc_statspipe[1], &sd, sizeof(sd));
    }
}


/**
 * Logs a completed job to stats server
 **/
void dcc_stats_compile_ok(char *compiler, char *filename, struct timeval start,
     struct timeval stop, int time_usec) {
    if (arg_stats) {
        struct statsdata sd;
        memset(&sd, 0, sizeof(sd));

        sd.type = STATS_COMPILE_OK;
        /* also send compiler, filename & runtime */
        memcpy(&(sd.start), &start, sizeof(struct timeval));
        memcpy(&(sd.stop), &stop, sizeof(struct timeval));
        sd.time = time_usec;
        strncpy(sd.filename, filename, MAX_FILENAME_LEN - 1);
        strncpy(sd.compiler, compiler, MAX_FILENAME_LEN - 1);
        dcc_writex(dcc_statspipe[1], &sd, sizeof(sd));
    }
}


/*
 * tracks the compile times
 */
static void dcc_stats_update_compile_times(struct statsdata *sd) {
    struct statsdata *prev_sd = NULL;
    struct statsdata *curr_sd = NULL;
    struct statsdata *new_sd;
    time_t two_min_ago = time(NULL) - 120;

    /* Record file with longest runtime */
    if (dcc_stats.longest_job_time < sd->time) {
        dcc_stats.longest_job_time = sd->time;
        strncpy(dcc_stats.longest_job_name, sd->filename,
                MAX_FILENAME_LEN);
        strncpy(dcc_stats.longest_job_compiler, sd->compiler,
                MAX_FILENAME_LEN);
    }

    /* store stats for compile time calcs */
    new_sd = malloc(sizeof(struct statsdata));
    memcpy(new_sd, sd, sizeof(struct statsdata));
    new_sd->next = dcc_stats.sd_root;
    dcc_stats.sd_root = new_sd;


    /* drop elements older than 2min */
    curr_sd = dcc_stats.sd_root;
    while (curr_sd != NULL) {
        if (curr_sd->stop.tv_sec < two_min_ago) {
            /* delete the stat */
            if (prev_sd == NULL) {
                dcc_stats.sd_root = curr_sd->next;
            } else {
                prev_sd->next = curr_sd->next;
            }
            free(curr_sd);
            curr_sd = prev_sd->next;
        } else {
            /* we didn't delete anything. move forward by one */
            prev_sd = curr_sd;
            curr_sd = curr_sd->next;
        }
    }
}

/* caclulate the avg kids used */
static void dcc_stats_calc_kid_avg(void) {
    static int total_5m[5] = {0};
    static int total_15m[15] = {0};
    static int pos_5m = 0;
    static int pos_15m = 0;
    static time_t last = 0;
    struct timeval now;
    struct timeval time_p;
    struct statsdata *curr_sd;
    int total_running = 0;
    int running = 0;
    int t = 0;
    int x = 0;
    int total = 0;

    gettimeofday(&now, NULL);
    
    /* calculate average kids used over the last minute */
    if ((now.tv_sec - 60) >= last) {
        /* we look at 1min ago back to 2 min ago because we only register
         * compiles when they complete. If we look right now, we miss all 
         * the current compiles that haven't completed.
         */
        for (t=60; t<120; t++) {
            time_p.tv_usec = now.tv_usec;
            time_p.tv_sec = now.tv_sec - t;
            running = 0;
            curr_sd = dcc_stats.sd_root;

            while (curr_sd != NULL) {
                if ((dcc_timecmp(curr_sd->start, time_p) <= 0) &&
                    (dcc_timecmp(curr_sd->stop, time_p) >= 0)) {
                    running++;
                }
                curr_sd = curr_sd->next;
            }
            total_running += running;
        }
        dcc_stats.kids_avg[0] = total_running / 60;


        /* populate 5m kid avgs */
        total_5m[pos_5m] = dcc_stats.kids_avg[0];
        pos_5m++;
        if (pos_5m >= 5)
            pos_5m = 0;

        /* calc 5m kid avg */
        total = 0;
        for (x=0; x<5; x++)
            total += total_5m[x];
        dcc_stats.kids_avg[1] = total/5;


        /* populate 15m kid avgs */
        total_15m[pos_15m] = dcc_stats.kids_avg[0];
        pos_15m++;
        if (pos_15m >= 15)
            pos_15m = 0;

        /* calc 15m kid avg */
        total = 0;
        for (x=0; x<15; x++)
            total += total_15m[x];
        dcc_stats.kids_avg[2] = total/15;

        last = now.tv_sec;
    }
}


/*
 * Updates a running total of compiles in the last 1, 5, 15 minutes
 *
 * Returns the oldest slot
 */
static int dcc_stats_update_running_total(int increment) {
    static int prev_slot;
    static time_t last = 0;
    static int total = 0;
    int i;
    int cur_slot;
    int *const cts = dcc_stats.compile_timeseries;
    time_t now = time(NULL);

    cur_slot = (now / 3) % 300;

    if (last + 900 < now) {
        /* reset all stats; last call was >15 min ago */
        for (i = 0; i < 300; i++)
            cts[i] = total;
        prev_slot = cur_slot;
    }

    if (prev_slot != cur_slot) {
        /* different timeslot, so set the interval [prev, cur) */
        for (i = (prev_slot)%300; i != cur_slot; i = (i+1)%300) {
            cts[i] = total;
        }
        prev_slot = cur_slot;
    }

    total += increment;
    last = now;

    return cur_slot;
}


static int *dcc_stats_get_compile_totals(void) {
    int cur_slot;
    static int ct[3]; /* 1, 5, 15 min compile totals */
    int *const cts = dcc_stats.compile_timeseries;

    cur_slot = dcc_stats_update_running_total(0);

    ct[0] = cts[(cur_slot + 299)%300] - cts[(cur_slot+280)%300];
    ct[1] = cts[(cur_slot + 299)%300] - cts[(cur_slot+200)%300];
    ct[2] = cts[(cur_slot + 299)%300] - cts[(cur_slot+1)%300];

    return ct;
}


/* Sets dcc_stats.io_rate at most 50 secs */
static void dcc_stats_minutely_update(void) {
    static int prev_io_tot = -1;
    static time_t last = 0;
    int n_reads, n_writes;
    time_t now = time(NULL);

    if (last + 50 < now) {
        dcc_get_disk_io_stats(&n_reads, &n_writes);

        if (prev_io_tot == -1)
            dcc_stats.io_rate = -1;
        else
            dcc_stats.io_rate = (n_reads + n_writes - prev_io_tot) / (now - last);

        prev_io_tot = n_reads + n_writes;
        last = now;
    }
}

static long dcc_get_tmpdirinfo(void) {
    const char *tmp_dir;
    struct statvfs buf;

    if (dcc_get_tmp_top(&tmp_dir) != 0)
        return -1;

    if (statvfs(tmp_dir, &buf) != 0)
        return -1;

    if (buf.f_bsize >= 1024)
        return buf.f_bavail * (buf.f_bsize / 1024) / 1024;
    else
        return (buf.f_bavail * buf.f_bsize) / (1024 * 1024);
}

/**
 * Accept a connection on the stats port and send the reply, regardless of data
 * that the client sends us.
 **/
static void dcc_service_stats_request(int http_fd) {
    int acc_fd;
    int *ct;
    size_t reply_len;
    char challenge[1024];
    char reply[4096];
    struct dcc_sockaddr_storage cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    double loadavg[3];
    int free_space_mb;
    int free_mem_mb;
    int ncpus, ncores;
    double core_scale, smt_gain;
    struct dcc_cpu_perf perf;
    struct dcc_server_load load = { .busy = -1, .queued = -1, .links = -1,
                                    .preparing = -1, .returning = -1 };
    int admission = 0, available = -1, capacity = -1;
    int running = dcc_getcurrentload();
    double memory_pressure = -1;
    unsigned long long ready_idle_ns = 0;
    unsigned long parallel_tasks = 0;
    unsigned parallel_peak = 0;
    ssize_t ret;

    const char replytemplate[] = "\
HTTP/1.0 200 OK\n\
Content-Type: text/plain\n\
Connection: close\n\n\
argv /distccd\n\
<distccstats>\n\
dcc_tcp_accept %d\n\
dcc_rej_bad_req %d\n\
dcc_rej_overload %d\n\
dcc_compile_ok %d\n\
dcc_compile_error %d\n\
dcc_compile_timeout %d\n\
dcc_cli_disconnect %d\n\
dcc_other %d\n\
dcc_longest_job %s\n\
dcc_longest_job_compiler %s\n\
dcc_longest_job_time_msecs %d\n\
dcc_max_kids %d\n\
dcc_cpus %d\n\
dcc_cores %d\n\
dcc_core_scale %.3f\n\
dcc_smt_gain %.3f\n\
dcc_cpu_scale %.6f\n\
dcc_cpu_age %.3f\n\
dcc_cpu_probe %d\n\
dcc_cpu_frequency %.0f\n\
dcc_cpu_reference %.0f\n\
dcc_cpu_active %.3f\n\
dcc_cpu_samples %lu\n\
dcc_avg_kids1 %d\n\
dcc_avg_kids2 %d\n\
dcc_avg_kids3 %d\n\
dcc_current_load %d\n\
dcc_load1 %1.2lf\n\
dcc_load2 %1.2lf\n\
dcc_load3 %1.2lf\n\
dcc_running %d\n\
dcc_busy %d\n\
dcc_queued %d\n\
dcc_preparing %d\n\
dcc_returning %d\n\
dcc_capacity %d\n\
dcc_job_memory_bytes %llu\n\
dcc_job_memory_samples %u\n\
dcc_job_peak_bytes %llu\n\
dcc_memory_spare_bytes %llu\n\
dcc_local_input_jobs %llu\n\
dcc_local_input_bytes %llu\n\
dcc_tcp_ready_idle_ns %llu\n\
dcc_tcp_parallel_tasks %lu\n\
dcc_tcp_parallel_peak %u\n\
dcc_tcp_input_prefetch_bytes %llu\n\
dcc_tcp_output_overlaps %llu\n\
dcc_links %d\n\
dcc_admission %d\n\
dcc_available %d\n\
dcc_memory_pressure %.2f\n\
dcc_num_compiles1 %d\n\
dcc_num_compiles2 %d\n\
dcc_num_compiles3 %d\n\
dcc_io_rate %d\n\
dcc_free_space %d MB\n\
dcc_free_mem %d MB\n\
</distccstats>\n";

    dcc_stats_minutely_update(); /* force update to get fresh disk io data */
    ct = dcc_stats_get_compile_totals();
    dcc_getloadavg(loadavg);
    dcc_cpu_topology(&ncpus, &ncores, &core_scale, &smt_gain);
    dcc_cpu_perf_get(&perf);
#ifdef __linux__
    admission = dcc_prefork_status(&load);
    capacity = dcc_cpu_perf_limit(dcc_resource_slots(dcc_max_kids));
    available = capacity - load.busy;
    if (available < 0) available = 0;
    memory_pressure = dcc_memory_pressure();
    ready_idle_ns = dcc_tcp_ready_idle_ns();
    parallel_tasks = dcc_tcp_async_tasks();
    parallel_peak = dcc_tcp_async_peak();
    if (running > 0) running--; /* Exclude this statistics request itself. */
#endif

    free_space_mb = dcc_get_tmpdirinfo();
    free_mem_mb = dcc_get_available_memory_mb();

    if (dcc_stats.longest_job_name[0] == 0)
        strcpy(dcc_stats.longest_job_name, "none");
    if (dcc_stats.longest_job_compiler[0] == 0)
        strcpy(dcc_stats.longest_job_compiler, "none");

    acc_fd = accept(http_fd, (struct sockaddr *) &cli_addr, &cli_len);
    if (dcc_check_client((struct sockaddr *)&cli_addr,
                         (int) cli_len,
                         opt_allowed) == 0) {
        reply_len = snprintf(reply, sizeof(reply), replytemplate,
                               dcc_stats.counters[STATS_TCP_ACCEPT],
                               dcc_stats.counters[STATS_REJ_BAD_REQ],
                               dcc_stats.counters[STATS_REJ_OVERLOAD],
                               dcc_stats.counters[STATS_COMPILE_OK],
                               dcc_stats.counters[STATS_COMPILE_ERROR],
                               dcc_stats.counters[STATS_COMPILE_TIMEOUT],
                               dcc_stats.counters[STATS_CLI_DISCONN],
                               dcc_stats.counters[STATS_OTHER],
                               dcc_stats.longest_job_name,
                               dcc_stats.longest_job_compiler,
                               dcc_stats.longest_job_time,
                               dcc_max_kids,
                               ncpus,
                               ncores, core_scale, smt_gain,
                               perf.scale, perf.age, perf.probe,
                               perf.frequency, perf.reference, perf.active, perf.samples,
                               dcc_stats.kids_avg[0],
                               dcc_stats.kids_avg[1],
                               dcc_stats.kids_avg[2],
                               dcc_getcurrentload(),
                               loadavg[0], loadavg[1], loadavg[2],
                               running, load.busy, load.queued, load.preparing, load.returning,
                               capacity, dcc_resource_job_bytes(), dcc_resource_samples(),
                               dcc_resource_peak_bytes(), dcc_resource_spare_bytes(),
                               (unsigned long long)dcc_tcp_local_input_jobs(),
                               (unsigned long long)dcc_tcp_local_input_bytes(),
                               ready_idle_ns, parallel_tasks, parallel_peak,
                               (unsigned long long)dcc_tcp_input_prefetch_bytes(),
                               (unsigned long long)dcc_tcp_output_overlaps(),
                               load.links, admission, available, memory_pressure,
                               ct[0], ct[1], ct[2],
                               dcc_stats.io_rate,
                               free_space_mb,
                               free_mem_mb
                               );
        if (reply_len >= sizeof(reply)) {
            rs_log_warning("statistics response truncated");
            reply_len = sizeof(reply) - 1;
        }
        dcc_set_nonblocking(acc_fd);
        ret = read(acc_fd, challenge, 1024); /* empty the receive queue */
        if (ret < 0) rs_log_info("read on acc_fd failed");
        dcc_writex(acc_fd, reply, reply_len);
    }

    /* Don't think we need this to prevent RST anymore, since we read() now */
#if 0
    shutdown(acc_fd, SHUT_WR); /* prevent connection reset */
#endif
    dcc_close(acc_fd);
}


/**
 * Process a packet of stats data
 **/
static void dcc_stats_process(struct statsdata *sd) {
    if (sd->type > STATS_ENUM_MAX) {
        /* Got a bad message */
        return;
    }

    if (sd->type == STATS_COMPILE_OK) dcc_stats_update_compile_times(sd);
    switch (sd->type) {

    case STATS_TCP_ACCEPT:
    case STATS_REJ_BAD_REQ:
    case STATS_REJ_OVERLOAD:
        break;
    case STATS_COMPILE_OK:
    case STATS_COMPILE_ERROR:
    case STATS_COMPILE_TIMEOUT:
    case STATS_CLI_DISCONN:
        /* We want to update the running compile total for all jobs that
         * non-trivially tax the CPU */
        dcc_stats_update_running_total(1);
    default: ;
    }

    dcc_stats.counters[sd->type]++;
}


/**
 * Collect runtime statistics from kids and serve them via HTTP
 * Also, maintains the pool of kids.
 **/
int dcc_stats_server(int listen_fd)
{
    int http_fd;
#ifndef __linux__
    int max_fd;
    struct timeval timeout;
#endif
    int i, ret;
    fd_set fds, fds_master;
    struct statsdata sd;

    /* clear stats data */
    for (i = 0; i < STATS_ENUM_MAX; i++)
        dcc_stats.counters[i] = 0;
    dcc_stats.longest_job_time = -1;
    dcc_stats.longest_job_name[0] = 0;
    dcc_stats.io_rate = -1;

    if ((ret = dcc_socket_listen(arg_stats_port, &http_fd,
                                    opt_listen_addr)) != 0) {
        return ret;
    }
    rs_log_info("HTTP server started on port %d\n", arg_stats_port);

    /* We don't want children to inherit this FD */
    fcntl(http_fd, F_SETFD, FD_CLOEXEC);

#ifndef __linux__
    max_fd = (http_fd > dcc_statspipe[0]) ? (http_fd + 1)
                                           : (dcc_statspipe[0] + 1);
#endif

    FD_ZERO(&fds_master);
    FD_SET(dcc_statspipe[0], &fds_master);
    FD_SET(http_fd, &fds_master);

    while (1) {
        dcc_stats_minutely_update();
        dcc_stats_calc_kid_avg();

#ifndef __linux__
        timeout.tv_sec = 60;
        timeout.tv_usec = 0;
#endif
        fds = fds_master;
#ifdef __linux__
        {
            struct pollfd extra[2] = {{ dcc_statspipe[0], POLLIN, 0 }, { http_fd, POLLIN, 0 }};
            ret = dcc_prefork_poll(listen_fd, extra, 2, 60000);
            FD_ZERO(&fds);
            if (extra[0].revents & POLLIN) FD_SET(dcc_statspipe[0], &fds);
            if (extra[1].revents & POLLIN) FD_SET(http_fd, &fds);
        }
#else
        ret = select(max_fd, &fds, NULL, NULL, &timeout);
#endif
        if (ret != -1) {
            if (FD_ISSET(dcc_statspipe[0], &fds)) {
                /* Received stats report from a child */
                if (read(dcc_statspipe[0], &sd, sizeof(sd)) != -1) {
                    dcc_stats_process(&sd);
                }
            }

            if (FD_ISSET(http_fd, &fds)) {
                /* Received request on stats reporting port */
                dcc_service_stats_request(http_fd);
            }
        } else {
            if (errno == EINTR) {
                /* Interrupted -- SIGCHLD? */
            }
        }

        dcc_manage_kids(listen_fd);
    }
}
