/* Execute a prepared job. One result message transfers all artifact FDs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include "distcc.h"
#include "exitcode.h"
#include "tcp_job.h"

int dcc_tcp_worker(int channel, const struct dcc_tcp_run *run, int metadata, int input, int retiring)
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
        dcc_job_decode(metadata, 0, &job)) finished.error = EXIT_PROTOCOL_ERROR;
    else if (setenv("TMPDIR", run->root, 1)) finished.error = EXIT_OUT_OF_MEMORY;
    else {
        changed_tmp = 1;
        if (input >= 0) {
            if (job->kind == DCC_JOB_FRONTEND) {
                job->fs_channel = input + 1;
            } else {
            char target[64], path[PATH_MAX];
            snprintf(target, sizeof(target), "/proc/self/fd/%d", input);
            /* Only this job's compiler inherits the read-only input. The
             * symlink retains the source basename and avoids materializing
             * another tmpfs copy. The worker closes it before its next job. */
            if (job->kind != DCC_JOB_COMPILE || job->count != 1 ||
                snprintf(path, sizeof(path), "%s%s", run->root, job->inputs[0].path) >= (int)sizeof(path) ||
                fcntl(input, F_SETFD, 0) || symlink(target, path))
                finished.error = EXIT_IO_ERROR;
            }
        }
        /* Includes compiler-created temporaries: parent-owned cleanup must
         * cover even SIGKILL, when this worker cannot run its cleanup list. */
        if (!finished.error) finished.error = dcc_execute_job(job, run->root, channel, &result);
    }
    if (input >= 0) close(input);
    close(metadata);
    dcc_job_free(job);
    dcc_cleanup_tempfiles();
    if (changed_tmp && (saved_tmp ? setenv("TMPDIR", saved_tmp, 1) : unsetenv("TMPDIR")))
        finished.error = EXIT_OUT_OF_MEMORY;
    free(saved_tmp);
    finished.status = result.status;
    finished.peak_bytes = result.peak_bytes;
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
