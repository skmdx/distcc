/* HTTP resources backed by the existing compiler worker pool.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_JOB_SERVER_H
#define DCC_JOB_SERVER_H
#include "http2.h"
#include "job_worker.h"
struct dcc_job_server;
struct dcc_server_job;
/* Dispatch sends metadata by SCM_RIGHTS and returns the worker control fd.
 * WAIT means the bounded pool is occupied or replenishing; -1 rejects a job.
 * Cancel must detach the job from that worker before retiring its channel. */
#define DCC_JOB_DISPATCH_WAIT (-2)
struct dcc_job_dispatch {
    int (*start)(void *, struct dcc_server_job *, int metadata, int link);
    void (*cancel)(void *, struct dcc_server_job *);
};
struct dcc_job_server *dcc_job_server_new(int fd, const struct dcc_job_dispatch *, void *);
void dcc_job_server_free(struct dcc_job_server *);
/* After fork, discard inherited references without cancelling parent jobs. */
void dcc_job_server_abandon(struct dcc_job_server *);
struct dcc_h2 *dcc_job_server_connection(struct dcc_job_server *);
void dcc_job_server_message(struct dcc_server_job *, const struct dcc_job_message *, int fd);
void dcc_job_server_worker_lost(struct dcc_server_job *);
void dcc_job_server_maintain(struct dcc_job_server *, time_t now);
int dcc_job_server_waiting(struct dcc_job_server *);
int dcc_job_server_active(struct dcc_job_server *);
#endif
