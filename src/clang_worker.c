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

void dcc_clang_pool_abandon(void)
{
    while (frontends) dcc_clang_pool_reaped(frontends->pid);
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

int dcc_clang_pool_get(const char *compiler)
{
    const char *helper = getenv("DISTCC_CLANG_DRIVER");
    if (!helper || !*helper) helper = "distcc-clang";
    char *selected = strdup(compiler);
    if (!selected) return -1;
    /* cc1 already owns language/target options. C and C++ driver spellings
     * select the same loaded frontend. Different compilers remain independent. */
    char *mode = strstr(selected, "clang++");
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
    int ret = posix_spawn_file_actions_init(&actions);
    if (!ret) {
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
    rs_trace("started reusable frontend pid=%ld", (long)f->pid);
    return f->channel;
}

int dcc_clang_worker(int session, char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak, uint64_t *cpu_ns)
{
    int ret = 0;
    struct dcc_clang_request request = {'C', getpgrp()};
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
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        struct pollfd polls[] = {{reply[0], POLLIN, 0}, {cancel, POLLIN, 0}};
        int timeout = -1;
        if (dcc_job_lifetime) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed = (now.tv_sec - started.tv_sec) * 1000LL +
                                (now.tv_nsec - started.tv_nsec) / 1000000;
            long long remaining = dcc_job_lifetime * 1000LL - elapsed;
            if (remaining <= 0) { ret = EXIT_TIMEOUT; break; }
            timeout = remaining > 2147483647 ? 2147483647 : remaining;
        }
        int ready = poll(polls, 2, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || polls[1].revents) { ret = EXIT_IO_ERROR; break; }
        if (!ready) { ret = EXIT_TIMEOUT; break; }
        if (polls[0].revents) {
            struct dcc_clang_result result;
            ssize_t length = recv(reply[0], &result, sizeof(result), MSG_TRUNC);
            if (length != sizeof(result) || result.status > 65535) { ret = EXIT_IO_ERROR; break; }
            *status = result.status; *peak = result.peak_bytes;
            *cpu_ns = result.cpu_ns;
            close(reply[0]);
            rs_trace("reused frontend status=%d peak=%llu", *status, *peak);
            return 0;
        }
    }
failed:
    close(reply[0]);
    /* The compiler joined this worker's group. Never terminate the shared
     * helper or another execution slot when cancelling this job. */
    if (dcc_worker_group) { killpg(getpgrp(), SIGKILL); _exit(ret); }
    return ret;
}
