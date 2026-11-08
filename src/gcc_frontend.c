/* Plan GCC search roots without scanning the translation unit.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include "distcc.h"
#include "exitcode.h"
#include "job.h"
#include "rpc.h"
#include "exec.h"
#include "frontend_fs.h"
#include "frontend_identity.h"
#include "native_worker.h"
#include "broker.h"
#include "trace.h"

extern char **environ;

static int start_planner(char **argv, const char *library)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return -1;
    pid_t child = fork();
    if (!child) {
        char descriptor[32], *preload = NULL;
        const char *previous = getenv("LD_PRELOAD");
        snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
        if (setpgid(0, 0) || dcc_redirect_fds("/dev/null", "/dev/null", "/dev/null") ||
            asprintf(&preload, "%s%s%s", library, previous ? ":" : "", previous ? previous : "") < 0 ||
            fcntl(pair[1], F_SETFD, 0) ||
            setenv("LD_PRELOAD", preload, 1) || setenv("DISTCC_GCC_PLAN_CONTROL", descriptor, 1)) _exit(1);
        execv(argv[0], argv);
        _exit(1);
    }
    close(pair[1]);
    struct pollfd ready = {pair[0], POLLIN, 0};
    char byte;
    if (child < 0 || poll(&ready, 1, dcc_get_io_timeout() * 1000) <= 0 ||
        recv(pair[0], &byte, 1, 0) != 1 || byte != 'R') {
        close(pair[0]);
        if (child > 0) {
            kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        }
        return -1;
    }
    rs_trace("started reusable GCC planner pid=%ld", (long)child);
    return pair[0];
}

/* GCC -wrapper supplies its actual frontend argv: no driver-option parser
 * or cached search result is needed in this process. */
int dcc_gcc_plan_execute(char **argv)
{
    const char *library = getenv("DISTCC_GCC_PLAN_LIBRARY");
    if (!library) library = GCC_PLAN_LIBRARY;
    const char *const generated_environment[] = {"COLLECT_GCC_OPTIONS", NULL};
    char key[80], *cwd = NULL;
    int worker = -1, lease = -1, input = -1, output = -1, pair[2] = {-1, -1};
    int ret = EXIT_IO_ERROR;
    const char option[] = "-fplugin-arg-distcc_gcc_scan-plan-fd=";
    for (char **arg = argv; *arg; ++arg) if (!strncmp(*arg, option, sizeof(option) - 1)) {
        const char *value = *arg + sizeof(option) - 1;
        char *end;
        long fd = strtol(value, &end, 10);
        if (!*value || *end || fd < 3 || fd > INT_MAX) goto out;
        output = fd;
    }
    if (output < 0 || dcc_local_worker_key(argv[0], library, generated_environment, key, sizeof(key))) goto out;
    lease = dcc_broker_worker_get(key, &worker);
    if (worker < 0 && lease >= 0) {
        worker = start_planner(argv, library);
        dcc_broker_worker_put(lease, worker); lease = -1;
    }
    if (worker < 0 || !(cwd = getcwd(NULL, 0)) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) ||
        (input = dcc_job_memfd("distcc-gcc-command")) < 0) goto out;
    char *directory[] = {cwd, NULL};
    if (dcc_x_argv(input, "ARGC", "ARGV", directory) ||
        dcc_x_argv(input, "ARGC", "ARGV", argv) ||
        dcc_x_argv(input, "ARGC", "ARGV", environ) || lseek(input, 0, SEEK_SET) < 0) goto out;
    int fds[] = {input, pair[1], 0, 1, 2};
    struct dcc_native_request request = {'P', 0};
    if (dcc_send_fds(worker, &request, sizeof(request), fds, 5)) goto out;
    close(pair[1]); pair[1] = -1;
    struct dcc_native_result result;
    ssize_t length;
    do { length = recv(pair[0], &result, sizeof(result), MSG_TRUNC); } while (length < 0 && errno == EINTR);
    if (length != sizeof(result) || result.status > 65535) goto out;
    ret = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
    if (ret) goto out;
    struct stat info;
    if (fstat(input, &info) || ftruncate(output, 0) || lseek(output, 0, SEEK_SET) < 0) {
        ret = EXIT_IO_ERROR; goto out;
    }
    off_t offset = 0;
    while (offset < info.st_size) {
        ssize_t n = sendfile(output, input, &offset, info.st_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ret = EXIT_IO_ERROR; goto out; }
    }
    rs_trace("reused GCC planner status=%d", ret);
out:
    if (lease >= 0) close(lease);
    if (worker >= 0) close(worker);
    if (input >= 0) close(input);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    free(cwd);
    return ret;
}

int dcc_gcc_frontend_prepare(char **argv, const char *input, struct dcc_job **result)
{
    int descriptor = dcc_job_memfd("distcc-gcc-plan"), status, ret = EXIT_IO_ERROR;
    char **command = NULL, **groups[2] = {NULL, NULL};
    struct dcc_job *job = NULL;
    pid_t child;
    if (descriptor < 0 || fcntl(descriptor, F_SETFD, 0) ||
        dcc_copy_argv(argv, &command, 5) || dcc_set_output(command, "/dev/null")) goto done;
    const char *module = getenv("DISTCC_GCC_SCAN_LIBRARY");
    char *plugin = NULL, *option = NULL;
    if (asprintf(&plugin, "-fplugin=%s", module ? module : "distcc_gcc_scan") < 0 ||
        asprintf(&option, "-fplugin-arg-distcc_gcc_scan-plan-fd=%d", descriptor) < 0) {
        free(plugin); free(option); goto done;
    }
    dcc_argv_append(command, strdup("-E"));
    dcc_argv_append(command, plugin);
    dcc_argv_append(command, option);
    int wrapped = 0;
    for (char **arg = argv; *arg; ++arg)
        if (!strcmp(*arg, "-wrapper") || !strncmp(*arg, "-wrapper=", 9)) wrapped = 1;
    // An explicitly selected wrapper owns subprogram execution. Do not
    // replace it with our resident planner or change its argv contract.
    if (!wrapped) {
        char self[PATH_MAX], *wrapper = NULL;
        ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (length < 0 || length == sizeof(self) - 1) goto done;
        self[length] = 0;
        if (asprintf(&wrapper, "%s,--gcc-plan", self) < 0) goto done;
        dcc_argv_append(command, strdup("-wrapper"));
        dcc_argv_append(command, wrapper);
    }
    if (dcc_spawn_child(command, &child, "/dev/null", "/dev/null", "/dev/null") ||
        dcc_collect_child("gcc frontend plan", child, &status, -1, NULL, NULL) || status ||
        lseek(descriptor, 0, SEEK_SET) < 0 || dcc_r_argv_file(descriptor, 2, groups)) goto done;
    job = calloc(1, sizeof(*job));
    if (!job) goto done;
    job->kind = DCC_JOB_GCC_FRONTEND;
    job->cwd = getcwd(NULL, 0);
    unsigned defaults = dcc_argv_len(groups[0]), directories = dcc_argv_len(groups[1]);
    unsigned argc = dcc_argv_len(argv);
    job->argv = calloc(defaults + directories + argc + 6, sizeof(*job->argv));
    if (!job->cwd || !job->argv) goto done;
    job->argv[0] = strdup(argv[0]);
    job->argv[1] = strdup(input);
    job->argv[2] = dcc_frontend_identity();
    if (!job->argv[0] || !job->argv[1] || !job->argv[2]) goto done;
    unsigned position = 3;
    for (unsigned group = 0; group < 2; ++group) {
        for (char **value = groups[group]; *value; ++value) {
            job->argv[position] = strdup(*value);
            if (!job->argv[position++]) goto done;
        }
        job->argv[position] = strdup("");
        if (!job->argv[position++]) goto done;
    }
    for (unsigned i = 0; i < argc; ++i) {
        job->argv[position] = strdup(argv[i]);
        if (!job->argv[position++]) goto done;
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
