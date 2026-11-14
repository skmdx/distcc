/* Client-owned compiler libraries, shared through the existing broker cache.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <dlfcn.h>
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
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "distcc.h"
#include "native_worker.h"
#include "broker.h"
#include "exec.h"
#include "exitcode.h"
#include "frontend_fs.h"
#include "job.h"
#include "rpc.h"
#include "trace.h"
#include "util.h"
#include <sys/wait.h>

extern char **environ;

struct dcc_environment_entry {
    const char *text;
    size_t name_length, position;
};

static int compare_environment(const void *a, const void *b)
{
    const struct dcc_environment_entry *x = a, *y = b;
    size_t length = x->name_length < y->name_length ? x->name_length : y->name_length;
    int order = memcmp(x->text, y->text, length);
    if (order) return order;
    if (x->name_length != y->name_length)
        return (x->name_length > y->name_length) - (x->name_length < y->name_length);
    /* Retain the order of duplicate names: getenv observes the first one. */
    return (x->position > y->position) - (x->position < y->position);
}

static int omitted_environment(const char *binding, size_t length, const char *const *names)
{
    if (binding[length] != '=') return 0;
    for (; names && *names; ++names) {
        if ((*names)[0] == binding[0] && strlen(*names) == length &&
            !memcmp(binding, *names, length)) return 1;
    }
    return 0;
}

int dcc_local_worker_key(const char *compiler, const char *library,
                         const char *const *omit_environment, char *key, size_t size)
{
    /* Clients keep their credentials and namespaces for the invocation. Forked
     * clients start a fresh context; driver and cc1 lookups share this one. */
    static struct {
        pid_t pid;
        int context_ready, environment_ready;
        XXH128_hash_t context, environment;
        XXH64_hash_t environment_stamp;
        const char *const *omitted;
        unsigned next_file;
        struct { char *path; struct stat info; } files[8];
    } cached;
    pid_t pid = getpid();
    if (cached.pid != pid) {
        for (unsigned i = 0; i < 8; ++i) { free(cached.files[i].path); cached.files[i].path = NULL; }
        cached.next_file = 0;
        cached.pid = pid;
        cached.context_ready = cached.environment_ready = 0;
    }
    XXH3_state_t state = {0}, environment_state = {0};
    XXH3_state_t *hash = &state;
    XXH3_128bits_reset(hash);
    if (!cached.context_ready) {
        const char *namespaces[] = {"/proc/self/ns/mnt", "/proc/self/ns/pid", "/proc/self/ns/user"};
        for (unsigned i = 0; i < sizeof(namespaces)/sizeof(*namespaces); ++i) {
            struct stat info;
            if (stat(namespaces[i], &info)) return -1;
            XXH3_128bits_update(hash, &info.st_dev, sizeof(info.st_dev));
            XXH3_128bits_update(hash, &info.st_ino, sizeof(info.st_ino));
        }
        gid_t groups[NGROUPS_MAX];
        int count = getgroups(NGROUPS_MAX, groups);
        if (count < 0) return -1;
        XXH3_128bits_update(hash, groups, count*sizeof(*groups));
        uid_t uid = geteuid();
        gid_t gid = getegid();
        mode_t mask = umask(0); umask(mask);
        XXH3_128bits_update(hash, &uid, sizeof(uid));
        XXH3_128bits_update(hash, &gid, sizeof(gid));
        XXH3_128bits_update(hash, &mask, sizeof(mask));
        cached.context = XXH3_128bits_digest(hash);
        cached.context_ready = 1;
        XXH3_128bits_reset(hash);
    }
    XXH3_128bits_update(hash, &cached.context, sizeof(cached.context));
    const char *paths[] = {compiler, library};
    for (unsigned i = 0; i < 2; ++i) {
        if (!paths[i]) continue;
        unsigned slot = 0;
        while (slot < 8 && (!cached.files[slot].path || strcmp(cached.files[slot].path, paths[i]))) ++slot;
        if (slot == 8) {
            struct stat info;
            if (stat(paths[i], &info)) return -1;
            char *path = strdup(paths[i]);
            if (!path) return -1;
            slot = cached.next_file++ % 8;
            free(cached.files[slot].path);
            cached.files[slot].path = path;
            cached.files[slot].info = info;
        }
        const struct stat *info = &cached.files[slot].info;
        XXH3_128bits_update(hash, &info->st_dev, sizeof(info->st_dev));
        XXH3_128bits_update(hash, &info->st_ino, sizeof(info->st_ino));
        XXH3_128bits_update(hash, &info->st_ctim, sizeof(info->st_ctim));
    }
    /* Driver aliases of the same executable share libraries. Each request
     * carries its original argv[0], which owns target and C/C++ mode. */
    size_t capacity = 0, count_environment = 0;
    XXH3_64bits_reset(&environment_state);
    while (environ[capacity]) {
        if (!omitted_environment(environ[capacity], strcspn(environ[capacity], "="), omit_environment))
            XXH3_64bits_update(&environment_state, environ[capacity], strlen(environ[capacity]) + 1);
        ++capacity;
    }
    for (const char *const *name = omit_environment; name && *name; ++name)
        XXH3_64bits_update(&environment_state, *name, strlen(*name) + 1);
    XXH64_hash_t stamp = XXH3_64bits_digest(&environment_state);
    /* Relay arrays and invocation-only variables change without changing
     * startup context. Hash retained bindings, including in-place mutations. */
    if (cached.environment_ready &&
        cached.omitted == omit_environment && cached.environment_stamp == stamp) goto environment_ready;
    struct dcc_environment_entry *environment = calloc(capacity, sizeof(*environment));
    if (capacity && !environment) return -1;
    for (char **p = environ; *p; ++p) {
        size_t length = strcspn(*p, "=");
        if (!omitted_environment(*p, length, omit_environment)) {
            environment[count_environment] = (struct dcc_environment_entry){
                *p, length, count_environment};
            ++count_environment;
        }
    }
    /* Bindings determine startup behavior, not the order a parent exported
     * unrelated names. Only the identity is sorted; requests stay unchanged. */
    if (count_environment > 1)
        qsort(environment, count_environment, sizeof(*environment), compare_environment);
    XXH3_128bits_reset(&environment_state);
    for (size_t i = 0; i < count_environment; ++i)
        XXH3_128bits_update(&environment_state, environment[i].text, strlen(environment[i].text)+1);
    free(environment);
    cached.environment = XXH3_128bits_digest(&environment_state);
    cached.omitted = omit_environment;
    cached.environment_stamp = stamp;
    cached.environment_ready = 1;
environment_ready:
    XXH3_128bits_update(hash, &cached.environment, sizeof(cached.environment));
    XXH128_hash_t value = XXH3_128bits_digest(hash);
    snprintf(key, size, "compiler-local:%016llx%016llx", (unsigned long long)value.high64,
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
static int request(char **argv, char operation, int output, int errors, int *status, uint64_t *cpu_ns)
{
    if (!dcc_getenv_bool("DISTCC_CLANG_LOCAL", 0) ||
        !strstr(dcc_find_basename(argv[0]), "clang")) return -1;
    /* Internal tool entry points bypass Clang's driver in the native binary. */
    if (argv[1] && !strncmp(argv[1], "-cc1", 4)) return -1;
    char *compiler = NULL, *cwd = NULL;
    int worker = -1, lease = -1, input = -1, sink = -1, pair[] = {-1,-1}, ret = -1;
    if (strchr(argv[0], '/')) compiler = strdup(argv[0]);
    else if (dcc_which(argv[0], &compiler)) goto out;
    char key[80];
    if (!compiler || dcc_local_worker_key(compiler, NULL, NULL, key, sizeof(key))) goto out;
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
    /* Driver inspection may print bindings/phases. As with -### discovery,
     * inspection stdout must not escape before the actual command executes. */
    if (operation == 'L' && (sink = open("/dev/null", O_WRONLY | O_CLOEXEC)) < 0) goto out;
    int fds[] = {input, pair[1], 0, sink >= 0 ? sink : 1, errors};
    struct dcc_native_request request = {operation, 0};
    if (dcc_send_fds(worker, &request, sizeof(request), fds, 5)) goto out;
    close(pair[1]); pair[1] = -1;
    struct dcc_native_result result;
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
    if (sink >= 0) close(sink);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    free(compiler); free(cwd);
    return ret;
}

int dcc_clang_local(char **argv, int *status, uint64_t *cpu_ns)
{
    return request(argv, 'C', -1, 2, status, cpu_ns);
}

static int dcc_clang_local_prepare(char **argv, char ***command, char ***roots)
{
    int output = dcc_job_memfd("distcc-frontend-command"), status;
    if (output < 0) return EXIT_IO_ERROR;
    int ret = request(argv, 'P', output, 2, &status, NULL);
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

int dcc_clang_link_prepare(char **argv, int *plan, int *errors, int *remote)
{
    *plan = *errors = -1;
    *remote = 0;
    if (!dcc_getenv_bool("DISTCC_CLANG_LOCAL", 0) ||
        !strstr(dcc_find_basename(argv[0]), "clang")) return -1;
    *plan = dcc_job_memfd("distcc-link-plan");
    *errors = dcc_job_memfd("distcc-link-diagnostics");
    if (*plan < 0 || *errors < 0) return EXIT_IO_ERROR;
    int status, ret = request(argv, 'L', *plan, *errors, &status, NULL);
    /* No request accepted: let the link coordinator query the actual driver,
     * just as when resident compiler libraries are disabled or unavailable. */
    if (ret) return ret;
    if (status) {
        off_t size = lseek(*errors, 0, SEEK_END);
        if (size < 0 || lseek(*errors, 0, SEEK_SET) < 0) return EXIT_IO_ERROR;
        ret = dcc_pump_readwrite(2, *errors, size);
        return ret ? ret : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    char **groups[1];
    if (lseek(*plan, 0, SEEK_SET) < 0 ||
        (ret = dcc_r_argv_file(*plan, 1, groups))) return EXIT_IO_ERROR;
    *remote = groups[0][0] != NULL;
    dcc_free_argv(groups[0]);
    if (lseek(*plan, 0, SEEK_SET) < 0 || lseek(*errors, 0, SEEK_SET) < 0)
        return EXIT_IO_ERROR;
    return 0;
}

/* This owns client broker requests; shared argument parsing is also linked
 * into the daemon and standalone tools without the client worker cache. */
int dcc_frontend_prepare(char **argv, struct dcc_job **result)
{
    struct dcc_job *job = calloc(1, sizeof(*job));
    if (!job) return EXIT_OUT_OF_MEMORY;
    job->kind = DCC_JOB_FRONTEND;
    job->cwd = getcwd(NULL, 0);
    int ret = job->cwd ? dcc_clang_local_prepare(argv, &job->argv, &job->fs_roots) : EXIT_IO_ERROR;
    if (ret < 0) {
        void *handle = dcc_clang_driver();
        int (*capture)(char **, char ***, char ***) = handle ? dlsym(handle, "dcc_clang_frontend") : NULL;
        ret = capture ? capture(argv, &job->argv, &job->fs_roots) : -1;
        if (handle) dlclose(handle);
    }
    if (!ret) ret = dcc_frontend_fs_add_roots(job, getenv("DISTCC_FRONTEND_ROOTS"));
    if (ret) { dcc_job_free(job); return EXIT_DISTCC_FAILED; }
    *result = job;
    return 0;
}
