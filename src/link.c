/* Route a resolved Clang linker job. Only sysroot jobs need the Python
 * coordinator. A broker execution lease replaces the ordinary local lock. */
#include <config.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "distcc.h"
#include "compile.h"
#include "broker.h"
#include "exitcode.h"
#include "helpers.h"
#include "hosts.h"
#include "job.h"
#include "resource.h"
#include "trace.h"
#include "util.h"
#include "where.h"
#include "workload.h"

static int python_link(char **argv, int plan, int errors)
{
    int n = dcc_argv_len(argv);
    char **cmd = calloc(n + 5, sizeof(*cmd));
    if (!cmd) return EXIT_OUT_OF_MEMORY;
    char helper[PATH_MAX], p[32], e[32];
    int i = 0;
    cmd[i++] = dcc_helper_path("distcc-link", helper);
    if (plan >= 0) {
        if (fcntl(plan, F_SETFD, 0) || fcntl(errors, F_SETFD, 0)) {
            free(cmd);
            return EXIT_IO_ERROR;
        }
        snprintf(p, sizeof(p), "%d", plan);
        snprintf(e, sizeof(e), "%d", errors);
        cmd[i++] = (char *)"--clang-plan"; cmd[i++] = p; cmd[i++] = e;
    }
    memcpy(cmd+i, argv, (n+1)*sizeof(*argv));
    int ret = dcc_compile_local(cmd, NULL, NULL, NULL);
    free(cmd);
    return ret;
}

static int lld(const char *path)
{
    const char *name = dcc_find_basename(path);
    return !strcmp(name, "ld.lld") || (!strncmp(name, "ld.lld-", 7) &&
        name[7] && name[7+strspn(name+7, "0123456789")] == '\0');
}

static int local_lease(int bytes)
{
    struct dcc_work_hint hint = {.kind = DCC_WORK_THINLTO, .bytes = bytes,
        .memory_class = 1 + DCC_JOB_THINLTO * DCC_MEMORY_BUCKETS + dcc_resource_bucket(bytes)};
    return dcc_broker_try_local(&hint);
}

static int run_link(char **argv, char **args, int errors, int bytes, int offload, int supplied)
{
    int n = dcc_argv_len(args);
    char **cmd = calloc(n+6, sizeof(*cmd));
    if (!cmd) return EXIT_OUT_OF_MEMORY;
    int lease = bytes > 0 ? local_lease(bytes) : -1;
    int distributed = bytes < 0 || (bytes > 0 && lease < 0);
    int i = 0, lock = -1, ret = EXIT_OUT_OF_MEMORY;
    char *resolved = NULL, *device = NULL, *driver = NULL, *transport = NULL, *compiler = NULL;
    char wrapper[PATH_MAX], path[PATH_MAX];
    struct timespec start, finish;
    uint64_t cpu = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    cmd[i++] = argv[0];
    cmd[i++] = offload ? dcc_helper_path("distcc-linker-wrapper", wrapper) : args[0];
    if (distributed) {
        if (strchr(argv[0], '/')) resolved = strdup(argv[0]);
        else if (dcc_which(argv[0], &resolved)) { ret = EXIT_IO_ERROR; goto out; }
        if (!resolved) goto out;
        const char *absolute = dcc_abspath(resolved, 0);
        if (offload) {
            if (asprintf(&device, "--device-distributor=%s", dcc_helper_path("distcc-offload-link", path)) < 0 ||
                asprintf(&driver, "--device-compiler-path=%s", absolute) < 0) goto out;
            cmd[i++] = device; cmd[i++] = driver;
        }
        if (!supplied &&
            (asprintf(&transport, "--thinlto-distributor=%s", dcc_helper_path("distcc-thinlto", path)) < 0 ||
             asprintf(&compiler, "--thinlto-remote-compiler=%s", absolute) < 0)) goto out;
    }
    for (int j = 1; j < n; ++j) cmd[i++] = args[j];
    if (transport) { cmd[i++] = transport; cmd[i++] = compiler; }
    struct stat st;
    if (fstat(errors, &st)) { ret = EXIT_IO_ERROR; goto out; }
    if ((ret = dcc_pump_readwrite(STDERR_FILENO, errors, st.st_size))) goto out;
    if (lease < 0) {
        if ((ret = dcc_lock_local(&lock))) goto out;
    }
    ret = dcc_clang_run_link(cmd, &cpu);
    if (ret && distributed && dcc_getenv_bool("DISTCC_FALLBACK", 1)) {
        rs_log_warning("distributed link failed; retrying locally");
        memcpy(cmd+1, args, (n+1)*sizeof(*args));
        ret = dcc_clang_run_link(cmd, &cpu);
    }
out:
    if (lease >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &finish);
        dcc_broker_local_done(lease, finish.tv_sec-start.tv_sec +
                              (finish.tv_nsec-start.tv_nsec)/1e9, cpu, ret);
    }
    dcc_close(lock);
    free(resolved); free(device); free(driver); free(transport); free(compiler); free(cmd);
    return ret;
}

int dcc_link_dispatch(char **argv)
{
    if (dcc_argv_search(argv+1, "-###")) return dcc_run_local_command(argv);
    int plan, errors;
    int32_t bytes = -1;
    char **args;
    int ret = dcc_clang_link_prepare(argv, &plan, &errors, &args, &bytes);
    if (ret < 0) return python_link(argv, -1, -1);
    if (ret) goto out;
    if (!args || !args[0]) { ret = dcc_run_local_command(argv); goto out; }
    int offload = !strcmp(dcc_find_basename(args[0]), "clang-linker-wrapper"), supplied = 0;
    const char *linker = offload ? "" : args[0], *sysroot = getenv("DISTCC_LINK_SYSROOT");
    for (char **p = args+1; *p; ++p) {
        if (offload && !strncmp(*p, "--linker-path=", 14)) linker = *p+14;
        if (!strncmp(*p, "--thinlto-distributor=", 22)) supplied = 1;
    }
    if (!lld(linker) || (supplied && !offload)) ret = dcc_run_local_command(argv);
    else if ((offload || dcc_argv_search(args, "-plugin-opt=thinlto") || !sysroot || !*sysroot) && dcc_tcp_job_pool())
        ret = run_link(argv, args, errors, offload ? -1 : bytes, offload, supplied);
    else ret = python_link(argv, plan, errors);
out:
    if (args) dcc_free_argv(args);
    dcc_close(plan); dcc_close(errors);
    return ret;
}
