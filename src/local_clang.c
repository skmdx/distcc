/* Client-owned compiler libraries, shared through the existing broker cache.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>
#include "distcc.h"
#include "clang_worker.h"
#include "broker.h"
#include "exec.h"
#include "exitcode.h"
#include "job.h"
#include "rpc.h"
#include "trace.h"
#include "util.h"

extern char **environ;

static int worker_key(const char *compiler, char *key, size_t size)
{
    XXH3_state_t *hash = XXH3_createState();
    if (!hash) return -1;
    XXH3_128bits_reset(hash);
    const char *paths[] = {compiler, "/proc/self/ns/mnt", "/proc/self/ns/pid", "/proc/self/ns/user"};
    for (unsigned i = 0; i < sizeof(paths)/sizeof(*paths); ++i) {
        struct stat info;
        if (stat(paths[i], &info)) { XXH3_freeState(hash); return -1; }
        XXH3_128bits_update(hash, &info.st_dev, sizeof(info.st_dev));
        XXH3_128bits_update(hash, &info.st_ino, sizeof(info.st_ino));
        if (!i) XXH3_128bits_update(hash, &info.st_ctim, sizeof(info.st_ctim));
    }
    /* Driver aliases of the same executable share libraries. Each request
     * carries its original argv[0], which owns target and C/C++ mode. */
    for (char **p = environ; *p; ++p) XXH3_128bits_update(hash, *p, strlen(*p)+1);
    gid_t groups[NGROUPS_MAX];
    int count = getgroups(NGROUPS_MAX, groups);
    if (count < 0) { XXH3_freeState(hash); return -1; }
    XXH3_128bits_update(hash, groups, count*sizeof(*groups));
    gid_t gid = getegid();
    XXH3_128bits_update(hash, &gid, sizeof(gid));
    mode_t mask = umask(0); umask(mask);
    XXH3_128bits_update(hash, &mask, sizeof(mask));
    XXH128_hash_t value = XXH3_128bits_digest(hash);
    XXH3_freeState(hash);
    snprintf(key, size, "clang-local:%016llx%016llx", (unsigned long long)value.high64,
             (unsigned long long)value.low64);
    return 0;
}

static int start_worker(const char *compiler)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return -1;
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
    char *command[] = {"distcc-clang", "--native-worker", descriptor, (char *)compiler, NULL};
    pid_t pid;
    int ret = fcntl(pair[1], F_SETFD, 0) ? EXIT_IO_ERROR :
        dcc_spawn_child(command, &pid, "/dev/null", "/dev/null", "/dev/null");
    close(pair[1]);
    struct pollfd ready = {pair[0], POLLIN, 0};
    char byte;
    if (ret || poll(&ready, 1, 5000) <= 0 || recv(pair[0], &byte, 1, 0) != 1 || byte != 'R') {
        close(pair[0]); return -1;
    }
    rs_trace("started local compiler libraries pid=%ld", (long)pid);
    return pair[0];
}

/* -1 means no request was accepted: ordinary execution is still safe. */
static int request(char **argv, char operation, int output, int *status, uint64_t *cpu_ns)
{
    if (!dcc_getenv_bool("DISTCC_CLANG_LOCAL", 0) ||
        !strstr(dcc_find_basename(argv[0]), "clang")) return -1;
    /* Internal tool entry points bypass Clang's driver in the native binary. */
    if (argv[1] && !strncmp(argv[1], "-cc1", 4)) return -1;
    char *compiler = NULL, *cwd = NULL;
    int worker = -1, lease = -1, input = -1, pair[] = {-1,-1}, ret = -1;
    if (strchr(argv[0], '/')) compiler = strdup(argv[0]);
    else if (dcc_which(argv[0], &compiler)) goto out;
    char key[80];
    if (!compiler || worker_key(compiler, key, sizeof(key))) goto out;
    lease = dcc_broker_worker_get(key, &worker);
    if (worker < 0 && lease >= 0) {
        worker = start_worker(compiler);
        dcc_broker_worker_put(lease, worker); lease = -1;
    }
    if (worker < 0 || !(cwd = getcwd(NULL, 0)) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) ||
        (input = output >= 0 ? dup(output) : dcc_job_memfd("distcc-local-command")) < 0) goto out;
    char *directory[] = {cwd, compiler, NULL};
    if (dcc_x_argv(input, "ARGC", "ARGV", directory) ||
        dcc_x_argv(input, "ARGC", "ARGV", argv) || lseek(input, 0, SEEK_SET) < 0) goto out;
    int fds[] = {input, pair[1], 0, 1, 2};
    struct dcc_clang_request request = {operation, 0};
    if (dcc_send_fds(worker, &request, sizeof(request), fds, 5)) goto out;
    close(pair[1]); pair[1] = -1;
    struct dcc_clang_result result;
    ssize_t length;
    do { length = recv(pair[0], &result, sizeof(result), MSG_TRUNC); } while (length < 0 && errno == EINTR);
    ret = length == sizeof(result) && result.status <= 65535 ? 0 : EXIT_IO_ERROR;
    if (!ret) {
        *status = result.status;
        if (cpu_ns) *cpu_ns = result.cpu_ns;
        rs_trace("reused local compiler libraries status=%d operation=%c", *status, operation);
    }
out:
    if (lease >= 0) close(lease);
    if (worker >= 0) close(worker);
    if (input >= 0) close(input);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    free(compiler); free(cwd);
    return ret;
}

int dcc_clang_local(char **argv, int *status, uint64_t *cpu_ns)
{
    return request(argv, 'C', -1, status, cpu_ns);
}

int dcc_clang_local_prepare(char **argv, char ***command, char ***roots)
{
    int output = dcc_job_memfd("distcc-frontend-command"), status;
    if (output < 0) return EXIT_IO_ERROR;
    int ret = request(argv, 'P', output, &status, NULL);
    if (!ret) {
        if (status || lseek(output, 0, SEEK_SET) < 0) ret = EXIT_DISTCC_FAILED;
        else {
            char **groups[2];
            ret = dcc_r_argv_file(output, 2, groups);
            if (!ret) { *command = groups[0]; *roots = groups[1]; }
        }
    }
    close(output);
    return ret;
}
