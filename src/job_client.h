/* Broker-owned HTTP job transaction. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_JOB_CLIENT_H
#define DCC_JOB_CLIENT_H
#include "http2.h"
#include "job_worker.h"
struct dcc_client_job;
extern const struct dcc_h2_events dcc_job_client_events;
struct dcc_client_job *dcc_job_client_start(struct dcc_h2 *, int local_control, int metadata);
void dcc_job_client_input(struct dcc_client_job *, const struct dcc_job_message *, int fd);
void dcc_job_client_free(struct dcc_client_job *);
void dcc_job_client_disconnected(struct dcc_client_job *);
#endif
