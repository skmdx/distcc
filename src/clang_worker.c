/* Reuse a loaded frontend; each invocation retains a separate compiler child.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

static int session = -1;
static pid_t helper_pid = -1;
static char *compiler;

static void close_session(void)
{
    if (session >= 0) close(session);
    session = -1;
    if (helper_pid > 0)
        while (waitpid(helper_pid, NULL, 0) < 0 && errno == EINTR) {}
    helper_pid = -1;
    free(compiler); compiler = NULL;
}

static int start_session(char **argv)
{
    if (helper_pid > 0 && waitpid(helper_pid, NULL, WNOHANG) == helper_pid)
        close_session();
    char *selected = strdup(argv[7]);
    if (!selected) return EXIT_OUT_OF_MEMORY;
    /* cc1 already owns language/target options. C and C++ driver spellings
     * select the same loaded frontend, while a changed compiler replaces it. */
    char *mode = strstr(selected, "clang++");
    if (mode) memmove(mode + 5, mode + 7, strlen(mode + 7) + 1);
    if (session >= 0 && !strcmp(compiler, selected)) { free(selected); return 0; }
    close_session();
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) {
        free(selected); return EXIT_IO_ERROR;
    }
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
    char *command[] = {argv[0], "--worker", descriptor, argv[7], NULL};
    int ret = fcntl(pair[1], F_SETFD, 0) ? EXIT_IO_ERROR :
        dcc_spawn_child(command, &helper_pid, "/dev/null", NULL, NULL);
    close(pair[1]);
    if (ret) { close(pair[0]); free(selected); return ret; }
    session = pair[0];
    compiler = selected;
    rs_trace("started reusable frontend pid=%ld", (long)helper_pid);
    return 0;
}

int dcc_clang_worker(char **argv, int filesystem, int cancel,
                     const char *output, const char *errors,
                     int *status, unsigned long long *peak)
{
    int ret = start_session(argv);
    if (ret) return ret;
    int fds[] = {dcc_job_memfd("distcc-clang-command"), filesystem,
                open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600),
                open(errors, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    if (fds[0] < 0 || fds[2] < 0 || fds[3] < 0) ret = EXIT_IO_ERROR;
    else {
        ret = dcc_x_argv(fds[0], "ARGC", "ARGV", argv);
        if (!ret && (lseek(fds[0], 0, SEEK_SET) < 0 ||
                     dcc_send_fds(session, "C", 1, fds, 4))) ret = EXIT_IO_ERROR;
    }
    for (unsigned i = 0; i < 4; ++i) if (i != 1 && fds[i] >= 0) close(fds[i]);
    if (ret) goto failed;
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        struct pollfd polls[] = {{session, POLLIN, 0}, {cancel, POLLIN, 0}};
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
            uint64_t reply[2];
            ssize_t length = recv(session, reply, sizeof(reply), MSG_TRUNC);
            if (length != sizeof(reply) || reply[0] > 65535) { ret = EXIT_IO_ERROR; break; }
            *status = reply[0]; *peak = reply[1];
            rs_trace("reused frontend pid=%ld status=%d peak=%llu", (long)helper_pid, *status, *peak);
            return 0;
        }
    }
failed:
    /* Cancellation and helper failure have the same ownership as ordinary
     * compiler children. The daemon reaps this group before deleting its tree. */
    if (dcc_worker_group) { killpg(getpgrp(), SIGKILL); _exit(ret); }
    kill(helper_pid, SIGKILL);
    close_session();
    return ret;
}
