/* Persistent worker control messages; file bodies travel only by FD.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_JOB_WORKER_H
#define DCC_JOB_WORKER_H
#include <stdint.h>
enum dcc_job_operation {
    DCC_JOB_NEED = 65536, DCC_JOB_INPUT, DCC_JOB_ARTIFACT, DCC_JOB_FINISHED,
    DCC_JOB_ABORT, DCC_JOB_INPUT_DONE
};
struct dcc_job_message { uint32_t operation, index; };
#define DCC_JOB_RETIRING (1U << 31)
int dcc_job_worker(int channel, int metadata, int *result_fd);
#endif
