/* Client-owned compiler libraries, shared through the existing broker cache.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
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
#include "helpers.h"
#include "native_worker.h"
#include "plan_memo.h"
#include "broker.h"
#include "lock.h"
#include "where.h"
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

static int context_binding(const char *binding, size_t length, enum dcc_compiler_context context)
{
    static const char *const startup[] = {
        "GCONV_PATH", "LOCPATH", "NLSPATH", "TZ", "LANG", "LANGUAGE",
        "LLVM_PROFILE_FILE", "DISTCC_GCC_SCAN_LIBRARY", NULL
    };
    static const char *const prefixes[] = {"LD_", "SANDBOX_", "GLIBC_", "MALLOC_",
        "ASAN_", "UBSAN_", "LSAN_", "TSAN_", "MSAN_", "LC_", NULL};
    static const char *const metadata[] = {"PWD", "OLDPWD", "SHLVL", "_", "DISTCC_LOG",
        "DISTCC_VERBOSE", "DISTCC_IO_TIMEOUT", "MAKEFLAGS", "MAKELEVEL", "MFLAGS",
        "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "subdir", NULL};
    int image = context == DCC_WORKER_IMAGE;
    if (image)
        for (const char *const *p = prefixes; *p; ++p)
            if (!strncmp(binding, *p, strlen(*p))) return 1;
    for (const char *const *p = image ? startup : metadata; *p; ++p)
        if (strlen(*p) == length && !memcmp(binding, *p, length)) return image;
    return !image;
}

int dcc_local_worker_key(const char *compiler, const char *library,
                         enum dcc_compiler_context context, char *key, size_t size)
{
    /* Clients keep their credentials and namespaces for the invocation. Forked
     * clients start a fresh context; driver and cc1 lookups share this one. */
    static struct {
        pid_t pid;
        int context_ready, environment_ready;
        XXH128_hash_t context, environment;
        XXH64_hash_t environment_stamp;
        enum dcc_compiler_context kind;
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
    if (library) XXH3_128bits_update(hash, library, strlen(library)+1);
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
        if (context_binding(environ[capacity], strcspn(environ[capacity], "="), context))
            XXH3_64bits_update(&environment_state, environ[capacity], strlen(environ[capacity]) + 1);
        ++capacity;
    }
    XXH64_hash_t stamp = XXH3_64bits_digest(&environment_state);
    /* Relay arrays and invocation-only variables change without changing
     * startup context. Hash retained bindings, including in-place mutations. */
    if (cached.environment_ready &&
        cached.kind == context && cached.environment_stamp == stamp) goto environment_ready;
    struct dcc_environment_entry *environment = calloc(capacity, sizeof(*environment));
    if (capacity && !environment) return -1;
    for (char **p = environ; *p; ++p) {
        size_t length = strcspn(*p, "=");
        if (context_binding(*p, length, context)) {
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
    cached.kind = context;
    cached.environment_stamp = stamp;
    cached.environment_ready = 1;
environment_ready:
    XXH3_128bits_update(hash, &cached.environment, sizeof(cached.environment));
    XXH128_hash_t value = XXH3_128bits_digest(hash);
    snprintf(key, size, "compiler-local:%016llx%016llx", (unsigned long long)value.high64,
             (unsigned long long)value.low64);
    return 0;
}

static int start_worker(const char *compiler, int *worker)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return EXIT_IO_ERROR;
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
    char path[PATH_MAX];
    char *command[] = {dcc_helper_path("distcc-clang", path), (char *)"--native-worker",
                       descriptor, (char *)compiler, NULL};
    pid_t pid = -1;
    int ret = fcntl(pair[1], F_SETFD, 0) ? EXIT_IO_ERROR :
        dcc_spawn_child(command, &pid, "/dev/null", "/dev/null", "/dev/null");
    close(pair[1]);
    char byte;
    /* Loading compiler libraries may fault them in from slow storage. Use the
     * same transport deadline as other compiler requests, not a startup-only
     * five-second limit. The caller owns a helper until its ready handshake. */
    if (!ret && !dcc_select_for_read(pair[0], dcc_get_io_timeout()) &&
        recv(pair[0], &byte, 1, MSG_TRUNC) == 1) {
        if (byte == 'R') {
            rs_trace("started local compiler libraries pid=%ld", (long)pid);
            *worker = pair[0];
            return 0;
        }
        if (byte == 'U') ret = EXIT_LOCAL_COMPILE;
    }
    close(pair[0]);
    if (pid > 0) {
        int status;
        pid_t waited;
        if (ret != EXIT_LOCAL_COMPILE) kill(-pid, SIGKILL);
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        if (ret == EXIT_LOCAL_COMPILE && (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)))
            ret = EXIT_DISTCC_FAILED;
    }
    if (ret != EXIT_LOCAL_COMPILE)
        rs_log_error("local Clang helper for %s failed to start", compiler);
    return ret ? ret : EXIT_DISTCC_FAILED;
}

static int get_worker(const char *compiler, int planning, int *worker)
{
    const char *broker = getenv("DISTCC_BROKER");
    if (!broker || !*broker)
        return planning ? start_worker(compiler, worker) : -1;

    char key[80], path[PATH_MAX];
    if (dcc_local_worker_key(compiler, dcc_helper_path("distcc-clang", path),
                             DCC_WORKER_IMAGE, key, sizeof(key))) return EXIT_IO_ERROR;
    int owner = dcc_broker_worker_get(key, worker);
    if (*worker >= 0) return 0;
    if (owner < 0) return planning ? EXIT_DISTCC_FAILED : -1;

    /* The worker lease serializes startup. Only a missing version package is
     * memoized; a dead worker for an installed version must be restartable. */
    char *missing = NULL;
    int memo = dcc_broker_memo_get(key, &missing, NULL, NULL);
    int ret = EXIT_LOCAL_COMPILE;
    if (!missing) ret = memo >= 0 ? start_worker(compiler, worker) : EXIT_DISTCC_FAILED;
    free(missing);
    if (memo >= 0) dcc_broker_memo_put(memo, ret == EXIT_LOCAL_COMPILE ? "U" : NULL, 1);
    dcc_broker_worker_put(owner, *worker);
    return ret;
}

/* -1 means no request was accepted: ordinary execution is still safe. */
static int request(char **argv, char operation, int output, int errors, int journal, int templated, int *status, uint64_t *cpu_ns)
{
    int reuse = dcc_getenv_bool("DISTCC_CLANG_LOCAL", 0);
    int planning = operation == 'P' || operation == 'I';
    int lto = operation == 'A' || operation == 'M' || operation == 'N';
    const char *driver = argv[0];
    if ((!reuse && !planning && !lto) || !dcc_clang_driver_mode(driver))
        return operation == 'N' ? EXIT_LOCAL_COMPILE : -1;
    if (argv[1] && !strncmp(argv[1], "-cc1", 4) &&
        !(operation == 'C' && !strcmp(argv[1], "-cc1"))) return planning ? EXIT_LOCAL_COMPILE : -1;
    char *compiler = NULL, *cwd = NULL;
    int worker = -1, cpu = -1, input = -1, sink = -1, pair[] = {-1,-1}, ret = EXIT_IO_ERROR;
    int lookup = 0;
    if (strchr(driver, '/')) {
        struct stat st;
        if (stat(driver, &st)) lookup = -errno;
        else compiler = strdup(driver);
    } else {
        lookup = dcc_which(driver, &compiler);
    }
    if (lookup == -ENOENT || lookup == -ENOTDIR) return planning || lto ? EXIT_LOCAL_COMPILE : -1;
    if (!compiler) goto out;
    ret = get_worker(compiler, planning || lto, &worker);
    if (ret) goto out;
    if (operation == 'P' && (ret = dcc_lock_local_cpp(&cpu))) goto out;
    ret = EXIT_IO_ERROR;
    if (!(cwd = getcwd(NULL, 0)) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) ||
        (input = output >= 0 ? dup(output) : dcc_job_memfd("distcc-local-command")) < 0) goto out;
    char *directory[] = {cwd, compiler, templated ? (char *)"template" : NULL, NULL};
    if (dcc_x_argv(input, "ARGC", "ARGV", directory) ||
        dcc_x_argv(input, "ARGC", "ARGV", argv) ||
        dcc_x_argv(input, "ARGC", "ARGV", environ) || lseek(input, 0, SEEK_SET) < 0) goto out;
    /* Driver inspection may print bindings/phases. As with -### discovery,
     * inspection stdout must not escape before the actual command executes. */
    if (operation == 'L' && (sink = open("/dev/null", O_WRONLY | O_CLOEXEC)) < 0) goto out;
    int fds[] = {input, pair[1], journal >= 0 ? journal : 0, sink >= 0 ? sink : 1, errors};
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
    if (cpu >= 0) dcc_unlock(cpu);
    if (worker >= 0) close(worker);
    if (input >= 0) close(input);
    if (sink >= 0) close(sink);
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    free(compiler); free(cwd);
    return ret;
}

int dcc_prepare_clang(char ***argv, char ***files)
{
    int needed = 0;
    if (dcc_offload_language(*argv)) return 0;
    if (!dcc_clang_driver_mode((*argv)[0])) return 0;
    for (char **p = *argv + 1; *p; ++p)
        if (str_endswith(".a", *p) ||
            (str_startswith("-fprofile", *p) && !str_startswith("-fprofile-generate", *p)) ||
            str_startswith("-fauto-profile", *p) ||
            str_startswith("-fmemory-profile-use", *p)) needed = 1;
    if (!needed) return 0;
    int output = dcc_job_memfd("distcc-clang-inputs"), status = 0;
    if (output < 0) return EXIT_IO_ERROR;
    char **groups[2];
    int ret = request(*argv, 'I', output, STDERR_FILENO, -1, 0, &status, NULL);
    if (!ret) {
        if (status) ret = EXIT_DISTCC_FAILED;
        else if (lseek(output, 0, SEEK_SET) < 0) ret = EXIT_IO_ERROR;
        else ret = dcc_r_argv_file(output, 2, groups);
    }
    close(output);
    if (ret) return ret;
    char **normalized = groups[0];
    *files = groups[1];
    char *resolved = normalized[0];
    normalized[0] = (*argv)[0];
    (*argv)[0] = resolved;
    dcc_free_argv(*argv);
    *argv = normalized;
    return 0;
}

int dcc_clang_local(char **argv, int *status, uint64_t *cpu_ns)
{
    /* Recursive compiler probes are already admitted by their caller. They
     * must execute the compiler directly, without another broker/helper wait. */
    if (dcc_recursion_safeguard()) return -1;
    int ret = request(argv, 'C', -1, 2, -1, 0, status, cpu_ns);
    return ret == EXIT_LOCAL_COMPILE ? -1 : ret;
}

static int dcc_clang_local_prepare(char **argv, const char *input, char ***command, char ***roots)
{
    int output = dcc_job_memfd("distcc-frontend-command"), status, ret = EXIT_IO_ERROR;
    if (output < 0) return ret;
    int journal = -1, errors = -1;
    int lease = -1;
    struct dcc_plan_template plan = {0};
    int verified = 0;
    unsigned dependencies = 0;
    size_t length = 0;
    char *memo = NULL, *compiler = NULL, *cwd = getcwd(NULL, 0);
    char key[96], context[80], path[PATH_MAX];
    if (strchr(argv[0], '/')) compiler = strdup(argv[0]);
    else dcc_which(argv[0], &compiler);
    if (compiler && cwd && getenv("DISTCC_BROKER") &&
        !dcc_local_worker_key(compiler, dcc_helper_path("distcc-clang", path),
                              DCC_PLAN_ENVIRONMENT, context, sizeof(context))) {
        dcc_clang_plan_template(&plan, argv, input);
        XXH3_state_t hash;
        XXH3_128bits_reset(&hash);
        XXH3_128bits_update(&hash, context, strlen(context)+1);
        XXH3_128bits_update(&hash, cwd, strlen(cwd)+1);
        for (char **p = plan.argv ? plan.argv : argv; *p; ++p) XXH3_128bits_update(&hash, *p, strlen(*p)+1);
        XXH128_hash_t digest = XXH3_128bits_digest(&hash);
        snprintf(key, sizeof(key), "clang-plan:%c:%016llx%016llx", plan.argv ? 't' : 'v',
                 (unsigned long long)digest.high64, (unsigned long long)digest.low64);
        lease = dcc_broker_memo_get(key, &memo, &length, &verified);
        while (memo) {
            if (dcc_plan_memo_decode(memo, length, output, &dependencies, verified)) {
                rs_trace("Clang plan memo hit dependencies=%u watched=%d", dependencies, !!verified);
                goto decode;
            }
            char *stale = memo;
            memo = NULL;
            lease = dcc_broker_memo_refresh(key, stale, length, &memo, &length, &verified);
            free(stale);
        }
    }
    journal = dcc_job_memfd("distcc-clang-plan-dependencies");
    if (journal < 0 || write(journal, "\0\0\0\0", 4) != 4) goto done;
    if (lease >= 0 && (errors = dcc_job_memfd("distcc-clang-plan-diagnostics")) < 0) goto done;
    if (lease < 0) dcc_free_plan_template(&plan);
    ret = request(plan.argv ? plan.argv : argv, 'P', output, errors >= 0 ? errors : 2,
                  journal, plan.argv != NULL, &status, NULL);
    if (ret) goto done;
    if (plan.argv && (status || lseek(errors, 0, SEEK_END) != 0)) {
        /* A driver that inspects a derived name or emits diagnostics needs
         * the actual command, including its original diagnostic filenames. */
        dcc_free_plan_template(&plan);
        dcc_broker_release(lease); lease = -1;
        close(errors); errors = -1;
        if (ftruncate(output, 0) || lseek(output, 0, SEEK_SET) < 0) { ret = EXIT_IO_ERROR; goto done; }
        ret = request(argv, 'P', output, 2, journal, 0, &status, NULL);
    }
    if (errors >= 0) {
        off_t size = lseek(errors, 0, SEEK_END);
        if (size < 0 || lseek(errors, 0, SEEK_SET) < 0) { ret = EXIT_IO_ERROR; goto done; }
        if (size) {
            dcc_broker_release(lease); lease = -1;
            if (dcc_pump_readwrite(2, errors, size)) { ret = EXIT_IO_ERROR; goto done; }
        }
    }
    if (ret || status) { if (!ret) ret = EXIT_DISTCC_FAILED; goto done; }
    if (lease >= 0) {
        memo = dcc_plan_memo_encode(journal, output, &dependencies, &length);
        rs_trace("Clang plan memo %s dependencies=%u", memo ? "publish" : "uncacheable", dependencies);
        dcc_broker_memo_put(lease, memo, length);
        lease = -1;
    }
decode:
    if (lseek(output, 0, SEEK_SET) < 0) ret = EXIT_IO_ERROR;
    else {
        char **groups[2];
        ret = dcc_r_argv_file(output, 2, groups);
        if (!ret && plan.argv && (dcc_bind_plan(&plan, groups[0]) || dcc_bind_plan(&plan, groups[1]))) {
            dcc_free_argv(groups[0]); dcc_free_argv(groups[1]); ret = EXIT_DISTCC_FAILED;
        }
        if (!ret) { *command = groups[0]; *roots = groups[1]; }
    }
done:
    dcc_free_plan_template(&plan);
    if (lease >= 0) dcc_broker_release(lease);
    free(memo); free(compiler); free(cwd);
    if (errors >= 0) close(errors);
    if (journal >= 0) close(journal);
    close(output);
    return ret;
}

int dcc_clang_local_tool(char **argv, char operation, int output, uint64_t *cpu_ns)
{
    int status;
    int ret = request(argv, operation, output, 2, -1, 0, &status, cpu_ns);
    return ret ? (ret < 0 ? EXIT_DISTCC_FAILED : ret) :
        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int dcc_clang_link_prepare(char **argv, int *plan, int *errors, char ***invocation, int32_t *bytes)
{
    *plan = *errors = -1;
    *invocation = NULL;
    if (!dcc_getenv_bool("DISTCC_CLANG_LOCAL", 0) ||
        !dcc_clang_driver_mode(argv[0])) return -1;
    *plan = dcc_job_memfd("distcc-link-plan");
    *errors = dcc_job_memfd("distcc-link-diagnostics");
    if (*plan < 0 || *errors < 0) return EXIT_IO_ERROR;
    int status, ret = request(argv, 'L', *plan, *errors, -1, 0, &status, NULL);
    if (ret == -1 || ret == EXIT_LOCAL_COMPILE) return 0; /* Use the local driver. */
    if (ret) return ret;
    if (status) {
        off_t size = lseek(*errors, 0, SEEK_END);
        if (size < 0 || lseek(*errors, 0, SEEK_SET) < 0) return EXIT_IO_ERROR;
        ret = dcc_pump_readwrite(2, *errors, size);
        return ret ? ret : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (lseek(*plan, 0, SEEK_SET) < 0 ||
        dcc_readx(*plan, bytes, sizeof(*bytes)) ||
        dcc_r_argv_file(*plan, 1, invocation)) return EXIT_IO_ERROR;
    if (lseek(*plan, sizeof(*bytes), SEEK_SET) < 0 || lseek(*errors, 0, SEEK_SET) < 0)
        return EXIT_IO_ERROR;
    return 0;
}

/* This owns client broker requests; shared argument parsing is also linked
 * into the daemon and standalone tools without the client worker cache. */
int dcc_frontend_prepare(char **argv, const char *input, struct dcc_job **result)
{
    struct dcc_job *job = calloc(1, sizeof(*job));
    if (!job) return EXIT_OUT_OF_MEMORY;
    job->kind = DCC_JOB_FRONTEND;
    job->cwd = getcwd(NULL, 0);
    int ret = job->cwd ? dcc_clang_local_prepare(argv, input, &job->argv, &job->fs_roots) : EXIT_IO_ERROR;
    if (!ret && dcc_frontend_fs_add_roots(job, getenv("DISTCC_FRONTEND_ROOTS")))
        ret = EXIT_DISTCC_FAILED;
    if (ret) { dcc_job_free(job); return ret; }
    *result = job;
    return 0;
}
