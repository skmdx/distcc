/* Reuse a loaded frontend; each invocation retains a separate compiler child.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "distcc.h"
#include "helpers.h"
#include "exec.h"
#include "exitcode.h"
#include "job.h"
#include "trace.h"
#include "clang_worker.h"

extern char **environ;

struct frontend {
    struct frontend *next;
    char *compiler, *helper;
    pid_t pid;
    int channel;
};
static struct frontend *frontends;

int dcc_clang_pool_reaped(pid_t pid)
{
    for (struct frontend **entry = &frontends; *entry; entry = &(*entry)->next) {
        struct frontend *f = *entry;
        if (f->pid != pid) continue;
        *entry = f->next;
        close(f->channel);
        free(f->compiler); free(f->helper); free(f);
        return 1;
    }
    return 0;
}

void dcc_clang_pool_shutdown(void)
{
    for (struct frontend *f = frontends; f; f = f->next) kill(f->pid, SIGTERM);
    while (frontends) {
        pid_t pid = frontends->pid;
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        dcc_clang_pool_reaped(pid);
    }
}

int dcc_clang_pool_get(const struct dcc_job *job, const char *resident_helper)
{
    char path[PATH_MAX];
    const char *helper = resident_helper ? resident_helper : dcc_helper_path("distcc-clang", path);
    char *selected = strdup(resident_helper ? helper : job->argv[0]);
    if (!selected) return -1;
    /* cc1 already owns language/target options. C and C++ driver spellings
     * select the same loaded frontend. Different compilers remain independent. */
    char *mode = resident_helper ? NULL : strstr(selected, "clang++");
    if (mode) memmove(mode + 5, mode + 7, strlen(mode + 7) + 1);
    for (struct frontend *f = frontends; f; f = f->next) {
        if (strcmp(f->compiler, selected) || strcmp(f->helper, helper)) continue;
        if (waitpid(f->pid, NULL, WNOHANG) == f->pid) {
            dcc_clang_pool_reaped(f->pid);
            break;
        }
        free(selected);
        return f->channel;
    }
    struct frontend *f = calloc(1, sizeof(*f));
    if (!f) { free(selected); return -1; }
    f->compiler = selected;
    f->helper = strdup(helper);
    int pair[2];
    if (!f->helper || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) {
        free(f->helper); free(selected); free(f); return -1;
    }
    /* The daemon has asynchronous IO threads: spawn only, never run Python
     * or compiler library code after a fork of the daemon's address space. */
    posix_spawn_file_actions_t actions;
    int ret = resident_helper ? dcc_spawn_backend(&f->pid, pair[1], helper, job) :
                           posix_spawn_file_actions_init(&actions);
    if (!ret && !resident_helper) {
        char option[] = "--worker", descriptor[] = "3";
        char *command[] = {f->helper, option, descriptor, selected, NULL};
        ret = posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
        if (!ret) ret = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (!ret) ret = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
        if (!ret) ret = posix_spawnp(&f->pid, helper, &actions, NULL, command, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pair[1]);
    if (ret) {
        close(pair[0]); free(f->helper); free(selected); free(f);
        errno = ret;
        return -1;
    }
    f->channel = pair[0];
    f->next = frontends; frontends = f;
    /* Requests queue during startup. Results use private reply sockets, so
     * the helper's single readiness byte need not block the daemon loop. */
    const char *kind = job->kind == DCC_JOB_RUST_CGU ? "sandboxed Rust CGU worker" :
                       resident_helper ? "sandboxed ThinLTO worker" : "frontend";
    rs_trace("started reusable %s pid=%ld", kind, (long)f->pid);
    return f->channel;
}

int dcc_clang_worker(int session, char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns)
{
    int ret = 0;
    struct dcc_native_request request = {'C', getpgrp()};
    int reply[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply)) return EXIT_IO_ERROR;
    int fds[] = {dcc_job_memfd("distcc-clang-command"), reply[1], filesystem,
                open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600),
                open(errors, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    if (fds[0] < 0 || fds[3] < 0 || fds[4] < 0) ret = EXIT_IO_ERROR;
    else {
        ret = dcc_x_argv(fds[0], "ARGC", "ARGV", argv);
        if (!ret && (lseek(fds[0], 0, SEEK_SET) < 0 ||
                     dcc_send_fds(session, &request, sizeof(request), fds, 5))) ret = EXIT_IO_ERROR;
    }
    for (unsigned i = 0; i < 5; ++i) if (i != 2 && fds[i] >= 0) close(fds[i]);
    if (ret) goto failed;
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    struct dcc_native_result result;
    ret = dcc_compiler_result(reply[0], cancel, &started, &result);
    if (!ret) {
        *status = result.status; *peak = result.peak_bytes; *cpu_ns = result.cpu_ns;
        close(reply[0]);
        rs_trace("reused frontend status=%d peak=%llu", *status, *peak);
        return 0;
    }
failed:
    close(reply[0]);
    if (dcc_worker_group) { killpg(getpgrp(), SIGKILL); _exit(ret); }
    return ret;
}

int dcc_wait_compiler_fd(int fd, short events, int cancel, const struct timespec *started)
{
    for (;;) {
        struct pollfd polls[] = {{fd, events, 0}, {cancel, POLLIN, 0}};
        int timeout = -1;
        if (dcc_job_lifetime) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed = (now.tv_sec - started->tv_sec) * 1000LL +
                                (now.tv_nsec - started->tv_nsec) / 1000000;
            long long remaining = dcc_job_lifetime * 1000LL - elapsed;
            if (remaining <= 0) return EXIT_TIMEOUT;
            timeout = remaining > 2147483647 ? 2147483647 : remaining;
        }
        int ready = poll(polls, 2, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || polls[1].revents) return EXIT_IO_ERROR;
        return ready ? 0 : EXIT_TIMEOUT;
    }
}

int dcc_compiler_result(int fd, int cancel, const struct timespec *started,
                        struct dcc_native_result *result)
{
    int ret = dcc_wait_compiler_fd(fd, POLLIN, cancel, started);
    if (ret) return ret;
    return recv(fd, result, sizeof(*result), MSG_TRUNC) == sizeof(*result) &&
           result->status <= 65535 ? 0 : EXIT_IO_ERROR;
}
