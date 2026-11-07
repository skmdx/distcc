/* Plan GCC search roots without scanning the translation unit.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "distcc.h"
#include "exitcode.h"
#include "job.h"
#include "rpc.h"
#include "exec.h"
#include "frontend_fs.h"
#include "frontend_identity.h"

int dcc_gcc_frontend_prepare(char **argv, const char *input, struct dcc_job **result)
{
    int descriptor = dcc_job_memfd("distcc-gcc-plan"), status, ret = EXIT_IO_ERROR;
    char **command = NULL, **groups[2] = {NULL, NULL};
    struct dcc_job *job = NULL;
    pid_t child;
    if (descriptor < 0 || fcntl(descriptor, F_SETFD, 0) ||
        dcc_copy_argv(argv, &command, 3) || dcc_set_output(command, "/dev/null")) goto done;
    const char *module = getenv("DISTCC_GCC_SCAN_LIBRARY");
    char *plugin = NULL, *option = NULL;
    if (asprintf(&plugin, "-fplugin=%s", module ? module : "distcc_gcc_scan") < 0 ||
        asprintf(&option, "-fplugin-arg-distcc_gcc_scan-plan-fd=%d", descriptor) < 0) {
        free(plugin); free(option); goto done;
    }
    dcc_argv_append(command, strdup("-E"));
    dcc_argv_append(command, plugin);
    dcc_argv_append(command, option);
    if (dcc_spawn_child(command, &child, "/dev/null", "/dev/null", "/dev/null") ||
        dcc_collect_child("gcc frontend plan", child, &status, -1, NULL, NULL) || status ||
        lseek(descriptor, 0, SEEK_SET) < 0 || dcc_r_argv_file(descriptor, 2, groups)) goto done;
    job = calloc(1, sizeof(*job));
    if (!job) goto done;
    job->kind = DCC_JOB_GCC_FRONTEND;
    job->cwd = getcwd(NULL, 0);
    unsigned defaults = dcc_argv_len(groups[0]), argc = dcc_argv_len(argv);
    job->argv = calloc(defaults + argc + 5, sizeof(*job->argv));
    if (!job->cwd || !job->argv) goto done;
    job->argv[0] = strdup(argv[0]);
    job->argv[1] = strdup(input);
    job->argv[2] = dcc_frontend_identity();
    if (!job->argv[0] || !job->argv[1] || !job->argv[2]) goto done;
    for (unsigned i = 0; i < defaults + argc + 1; ++i) {
        const char *value = i < defaults ? groups[0][i] : i == defaults ? "" : argv[i-defaults-1];
        job->argv[i+3] = strdup(value);
        if (!job->argv[i+3]) goto done;
    }
    // GCC's native planner reports exact directory spellings. Preserve them
    // for filesystem lookup; authorization normalizes only its comparison.
    for (unsigned root = 0; groups[1][root]; ++root)
        if (dcc_frontend_fs_add_root(job, groups[1][root])) goto done;
    if (dcc_frontend_fs_add_root(job, job->cwd) ||
        dcc_frontend_fs_add_roots(job, getenv("DISTCC_FRONTEND_ROOTS"))) goto done;
    *result = job; job = NULL;
    ret = 0;
done:
    if (descriptor >= 0) close(descriptor);
    if (command) dcc_free_argv(command);
    for (unsigned i = 0; i < 2; ++i) if (groups[i]) dcc_free_argv(groups[i]);
    dcc_job_free(job);
    return ret;
}
