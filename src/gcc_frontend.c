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
#include "util.h"
#include "argv_file.h"

extern char **environ;

static int start_planner(const char *compiler, char **argv, const char *library, const char *role)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return -1;
    pid_t child = fork();
    if (!child) {
        char descriptor[32], *preload = NULL;
        const char *previous = getenv("LD_PRELOAD");
        snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
        if (setpgid(0, 0) || dcc_redirect_fds("/dev/null", "/dev/null", NULL) ||
            asprintf(&preload, "%s%s%s", library, previous ? ":" : "", previous ? previous : "") < 0 ||
            fcntl(pair[1], F_SETFD, 0) ||
            setenv("LD_PRELOAD", preload, 1) || setenv("DISTCC_GCC_PLAN_CONTROL", descriptor, 1) ||
            setenv("DISTCC_GCC_PLAN_ROLE", role, 1)) _exit(1);
        execv(compiler, argv);
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
    rs_trace("started reusable GCC %s pid=%ld", role, (long)child);
    return pair[0];
}

/* The producer owns this complete command file. The worker replaces it with
 * the plan; a driver's relay can hand the same file straight to cc1. */
static int request_planner(const char *compiler, char **argv, int input,
                       const int standard[3], const char *role, int capture)
{
    const char *library = getenv("DISTCC_GCC_PLAN_LIBRARY");
    if (!library) library = GCC_PLAN_LIBRARY;
    /* This key shares a loaded image, not an argument or preprocessing
     * result. Build flags and make's invocation/subdirectory metadata arrive
     * with each request; its complete environment is restored before GCC main.
     * These exported copies and distcc logging do not change the loaded image.
     * Loader, allocator and sandbox configuration still belong to the key. */
    static const char *const invocation_environment[] = {
        "COLLECT_GCC_OPTIONS", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
        "COLLECT_AS_OPTIONS", "MAKEFLAGS", "MAKELEVEL", "MFLAGS", "subdir",
        "DISTCC_LOG", "DISTCC_VERBOSE", NULL
    };
    char key[80];
    int worker = -1, lease = -1, pair[2] = {-1, -1}, relay[2] = {-1, -1};
    int ret = EXIT_IO_ERROR;
    const char *stage = "worker identity";
    if (dcc_local_worker_key(compiler, library, invocation_environment, key, sizeof(key))) goto out;
    stage = "worker lookup";
    lease = dcc_broker_worker_get(key, &worker);
    if (worker < 0 && lease >= 0) {
        stage = "worker startup";
        worker = start_planner(compiler, argv, library, role);
        dcc_broker_worker_put(lease, worker); lease = -1;
    }
    if (worker < 0) goto out;
    stage = "command preparation";
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) ||
        (capture && socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, relay)) ||
        lseek(input, 0, SEEK_SET) < 0) goto out;
    int fds[] = {input, pair[1], standard[0], standard[1], standard[2], relay[1]};
    struct dcc_native_request request = {capture ? 'D' : 'P', 0};
    stage = "command send";
    if (dcc_send_fds(worker, &request, sizeof(request), fds, capture ? 6 : 5)) goto out;
    close(pair[1]); pair[1] = -1;
    if (relay[1] >= 0) { close(relay[1]); relay[1] = -1; }
    struct dcc_native_result result;
    ssize_t length;
    stage = "result receive";
    while (relay[0] >= 0) {
        struct pollfd p[] = {{pair[0], POLLIN, 0}, {relay[0], POLLIN, 0}};
        int ready;
        do { ready = poll(p, 2, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) goto out;
        if (p[1].revents & POLLIN) {
            char operation;
            struct dcc_argv_file invocation = {0};
            ssize_t received;
            do { received = recv(relay[0], &operation, 1, MSG_TRUNC); } while (received < 0 && errno == EINTR);
            /* The driver closes its relay before the worker reaps it and
             * sends final status. EOF is not a missing planner request. */
            if (!received) { close(relay[0]); relay[0] = -1; break; }
            if (received != 1 || operation != 'P' || lseek(input, 0, SEEK_SET) < 0) goto out;
            int status = dcc_argv_file_read(input, 3, &invocation);
            if (!status && !invocation.groups[1][0]) status = EXIT_PROTOCOL_ERROR;
            if (!status) {
                char **previous_environment = environ;
                environ = invocation.groups[2];
                status = request_planner(invocation.groups[1][0], invocation.groups[1],
                                         input, standard, "planner", 0);
                environ = previous_environment;
            }
            dcc_argv_file_close(&invocation);
            uint32_t reply = status;
            if (send(relay[0], &reply, sizeof(reply), MSG_NOSIGNAL) != sizeof(reply)) goto out;
        }
        if (p[0].revents) break;
        if (p[1].revents & (POLLHUP | POLLERR)) { close(relay[0]); relay[0] = -1; }
    }
    do { length = recv(pair[0], &result, sizeof(result), MSG_TRUNC); } while (length < 0 && errno == EINTR);
    if (length != sizeof(result) || result.status > 65535) goto out;
    ret = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
    stage = "compiler execution";
    if (ret) goto out;
    rs_trace("reused GCC %s status=%d cpu_ns=%llu", role, ret,
             (unsigned long long)result.cpu_ns);
out:
    if (ret) rs_log_warning("GCC %s failed during %s (status=%d, errno=%d)",
                            role, stage, ret, errno);
    if (lease >= 0) close(lease);
    if (worker >= 0) close(worker);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    if (relay[0] >= 0) close(relay[0]);
    if (relay[1] >= 0) close(relay[1]);
    return ret;
}

static int run_planner(const char *compiler, char **argv, int output,
                       const int standard[3], const char *role, int capture)
{
    int input = dcc_job_memfd("distcc-gcc-command"), ret = EXIT_IO_ERROR;
    char *cwd = getcwd(NULL, 0);
    const char *stage = "command preparation";
    struct stat info;
    off_t offset = 0;
    char *directory[] = {cwd, NULL};
    if (input < 0 || !cwd || dcc_x_argv(input, "ARGC", "ARGV", directory) ||
        dcc_x_argv(input, "ARGC", "ARGV", argv) ||
        dcc_x_argv(input, "ARGC", "ARGV", environ)) goto out;
    ret = request_planner(compiler, argv, input, standard, role, capture);
    stage = NULL; // request_planner already reports worker failures.
    if (ret) goto out;
    stage = "plan copy";
    ret = EXIT_IO_ERROR;
    if (fstat(input, &info) || ftruncate(output, 0) || lseek(output, 0, SEEK_SET) < 0) goto out;
    while (offset < info.st_size) {
        ssize_t n = sendfile(output, input, &offset, info.st_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
    }
    ret = 0;
out:
    if (ret && stage) rs_log_warning("GCC %s failed during %s (status=%d, errno=%d)", role, stage, ret, errno);
    if (input >= 0) close(input);
    free(cwd);
    return ret;
}

/* GCC -wrapper supplies its actual frontend argv: no driver-option parser
 * or cached search result is needed in this process. */
int dcc_gcc_plan_execute(char **argv)
{
    int output = -1;
    const char option[] = "-fplugin-arg-distcc_gcc_scan-plan-fd=";
    for (char **arg = argv; *arg; ++arg) if (!strncmp(*arg, option, sizeof(option) - 1)) {
        const char *value = *arg + sizeof(option) - 1;
        char *end;
        long fd = strtol(value, &end, 10);
        if (!*value || *end || fd < 3 || fd > INT_MAX) return EXIT_IO_ERROR;
        output = fd;
    }
    const int standard[] = {0, 1, 2};
    return output < 0 ? EXIT_IO_ERROR : run_planner(argv[0], argv, output, standard, "planner", 0);
}

static int plan_driver(char **argv, int output, int capture)
{
    char *compiler = NULL;
    if (strchr(argv[0], '/')) compiler = strdup(argv[0]);
    else if (dcc_which(argv[0], &compiler)) return EXIT_IO_ERROR;
    if (!compiler) return EXIT_OUT_OF_MEMORY;
    int file = open(compiler, O_RDONLY | O_CLOEXEC), ret = EXIT_IO_ERROR;
    int diagnostics = dcc_job_memfd("distcc-gcc-diagnostics");
    unsigned char magic[4];
    ssize_t length;
    if (file < 0 || diagnostics < 0) {
        if (file >= 0) close(file);
        goto out;
    }
    do { length = read(file, magic, sizeof(magic)); } while (length < 0 && errno == EINTR);
    close(file);
    if (length < 0) goto out;
    /* A script's ELF entry belongs to its interpreter and has different argv
     * semantics. Let exec handle those existing compiler wrappers normally. */
    if (length != sizeof(magic) || memcmp(magic, "\177ELF", sizeof(magic))) {
        pid_t child;
        int status;
        char diagnostic_path[64];
        snprintf(diagnostic_path, sizeof(diagnostic_path), "/proc/self/fd/%d", diagnostics);
        if (!dcc_spawn_child(argv, &child, "/dev/null", "/dev/null", diagnostic_path) &&
            !dcc_collect_child("gcc frontend plan", child, &status, -1, NULL, NULL) && !status) ret = 0;
    } else {
        if (capture) {
            /* The preload returns GCC's finalized argv/environment in place
             * of executing our wrapper. Script drivers still use the actual
             * wrapper above, and an explicit user wrapper is never replaced. */
            for (char **arg = argv; *arg; ++arg) if (!strcmp(*arg, "-wrapper")) {
                char *wrapper;
                if (asprintf(&wrapper, "%s-capture", arg[1]) < 0) goto out;
                free(arg[1]); arg[1] = wrapper;
                break;
            }
        }
        int quiet = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (quiet >= 0) {
            const int standard[] = {quiet, quiet, diagnostics};
            /* Ordinary driver spawning increments the recursion guard in its
             * child. Serialize that same environment for the resident driver,
             * then restore this single-threaded client's own environment. */
            const char *guard = getenv("_DISTCC_SAFEGUARD");
            char *previous = guard ? strdup(guard) : NULL;
            if (!guard || previous) {
                if (!dcc_increment_safeguard()) {
                    /* GCC locates cc1 relative to argv[0]. A bare alias would
                     * find the masquerade wrapper in PATH again, although
                     * compiler already names the executable we selected. */
                    char *alias = argv[0];
                    argv[0] = compiler;
                    ret = run_planner(compiler, argv, output, standard, "driver", capture);
                    argv[0] = alias;
                }
                if (previous ? setenv("_DISTCC_SAFEGUARD", previous, 1) : unsetenv("_DISTCC_SAFEGUARD"))
                    ret = EXIT_OUT_OF_MEMORY;
            }
            free(previous);
            close(quiet);
        }
    }
out:
    if (diagnostics >= 0) {
        struct stat info;
        if (ret && !fstat(diagnostics, &info) && info.st_size &&
            lseek(diagnostics, 0, SEEK_SET) >= 0)
            dcc_pump_readwrite(STDERR_FILENO, diagnostics, info.st_size);
        close(diagnostics);
    }
    free(compiler);
    return ret;
}

int dcc_gcc_frontend_prepare(char **argv, const char *input, struct dcc_job **result)
{
    int descriptor = dcc_job_memfd("distcc-gcc-plan"), ret = EXIT_IO_ERROR;
    char **command = NULL, **groups[2] = {NULL, NULL};
    struct dcc_job *job = NULL;
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
    if (plan_driver(command, descriptor, !wrapped) ||
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
