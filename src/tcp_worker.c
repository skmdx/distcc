/* Execute a prepared job. One result message transfers all artifact FDs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "distcc.h"
#include "exitcode.h"
#include "tcp_job.h"

int dcc_tcp_worker(int channel, const struct dcc_tcp_run *run, int metadata, int retiring)
{
    struct dcc_job *job = NULL;
    struct dcc_job_result result;
    struct dcc_tcp_finished finished = { .id = run->id, .retiring = retiring };
    int fds[DCC_JOB_RESULT_COUNT], ret;
    unsigned count = 0;
    const char *setting = getenv("TMPDIR");
    char *saved_tmp = setting ? strdup(setting) : NULL;
    int changed_tmp = 0;
    dcc_job_result_init(&result);
    if (setting && !saved_tmp) finished.error = EXIT_OUT_OF_MEMORY;
    else if (!memchr(run->root, 0, sizeof(run->root)) || run->root[0] != '/' ||
        dcc_job_decode(metadata, &job)) finished.error = EXIT_PROTOCOL_ERROR;
    else if (setenv("TMPDIR", run->root, 1)) finished.error = EXIT_OUT_OF_MEMORY;
    else {
        changed_tmp = 1;
        /* Includes compiler-created temporaries: parent-owned cleanup must
         * cover even SIGKILL, when this worker cannot run its cleanup list. */
        finished.error = dcc_execute_job(job, run->root, channel, &result);
    }
    close(metadata);
    dcc_job_free(job);
    dcc_cleanup_tempfiles();
    if (changed_tmp && (saved_tmp ? setenv("TMPDIR", saved_tmp, 1) : unsetenv("TMPDIR")))
        finished.error = EXIT_OUT_OF_MEMORY;
    free(saved_tmp);
    finished.status = result.status;
    finished.mode = result.mode;
    for (unsigned i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        finished.size[i] = result.size[i];
        if (result.fd[i] >= 0) {
            finished.present |= 1U << i;
            fds[count++] = result.fd[i];
        }
    }
    if (finished.error) finished.retiring = 1;
    ret = dcc_send_fds(channel, &finished, sizeof(finished), fds, count);
    dcc_job_result_free(&result);
    return ret || finished.error ? EXIT_IO_ERROR : 0;
}
