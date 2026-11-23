/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */



#include <config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h> // IWYU pragma: keep
#include <sys/stat.h>


#ifdef HAVE_FNMATCH_H
  #include <fnmatch.h>
#endif

#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <assert.h>

#include "distcc.h"
#include "helpers.h"
#include "job.h"
#include "frontend_fs.h"
#include "resource.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "implicit.h"
#include "exec.h"
#include "where.h"
#include "lock.h"
#include "timeval.h"
#include "compile.h"
#include "broker.h"
#include "workload.h"
#include "dotd.h"

static void release_host(struct dcc_hostdef *host, int *cpu_lock_fd,
                         int *local_cpu_lock_fd, int failed)
{
   if (failed && host && host->broker_managed && *cpu_lock_fd >= 0)
       dcc_broker_failed(host);
   if (host && host->broker_managed && host->broker_fd >= 0) {
       close(host->broker_fd);
       host->broker_fd = -1;
   }
   if (failed && host)
       dcc_disliked_host(host);

   if (*cpu_lock_fd != -1) {
       dcc_unlock(*cpu_lock_fd);
       *cpu_lock_fd = -1;
   }
   if (*local_cpu_lock_fd != -1) {
       dcc_unlock(*local_cpu_lock_fd);
       *local_cpu_lock_fd = -1;
   }
}



/**
 * Invoke a compiler locally.  This is, obviously, the alternative to
 * dcc_compile_remote().
 *
 * The server does basically the same thing, but it doesn't call this
 * routine because it wants to overlap execution of the compiler with
 * copying the input from the network.
 *
 * This routine used to exec() the compiler in place of distcc.  That
 * is slightly more efficient, because it avoids the need to create,
 * schedule, etc another process.  The problem is that in that case we
 * can't clean up our temporary files, and (not so important) we can't
 * log our resource usage.
 *
 * Callers reserve local capacity for compiler work; coordinators acquire
 * capacity for their children instead of occupying a local compiler lane.
 **/
int dcc_compile_local(char *argv[],
                      char *input_name, const char *prepared, uint64_t *cpu_ns)
{
    pid_t pid;
    int ret;
    int status;

    dcc_note_execution(dcc_hostdef_local, argv);
    dcc_note_state(DCC_PHASE_COMPILE, input_name, "localhost", DCC_LOCAL);

    if (!prepared && (ret = dcc_clang_local(argv, &status, cpu_ns)) >= 0)
        return ret ? ret : dcc_critique_status(status, "compile", input_name,
                                              dcc_hostdef_local, 1);

    /* An IR job opens its sealed input as stdin before exec closes the
     * original memfd. Ordinary local commands retain the caller's stdio. */
    if ((ret = dcc_spawn_child(argv, &pid, prepared, NULL, NULL)) != 0)
        return ret;

    if ((ret = dcc_collect_child("cc", pid, &status, timeout_null_fd, NULL, cpu_ns)))
        return ret;

    return dcc_critique_status(status, "compile", input_name,
                               dcc_hostdef_local, 1);
}

/* Helpers reserve local capacity only for work that actually runs locally.
 * Keep the lock in the waiting parent: a compiler may close inherited fds. */
int dcc_run_local_command(char **argv)
{
    int fd = -1, ret;
    dcc_read_localslots_configuration();
    if ((ret = dcc_lock_local(&fd))) return ret;
    ret = dcc_compile_local(argv, NULL, NULL, NULL);
    dcc_close(fd);
    return ret;
}

static int dcc_compile_local_ir(char **argv, char *input, const char *prepared, int fd, uint64_t *cpu_ns)
{
    char **stripped = NULL, **command = NULL;
    char *cwd = NULL;
    int ret = dcc_strip_local_args(argv, &stripped, input);
    if (!ret) ret = dcc_copy_argv(stripped, &command, 0);
    free(stripped);
    if (!ret) ret = dcc_set_input(command, (char *)(fd >= 0 ? "/proc/self/fd/0" : prepared));
    if (!ret && !(cwd = getcwd(NULL, 0))) ret = EXIT_IO_ERROR;
    if (!ret) ret = dcc_clang_command(&command, "", input, cwd, 1, NULL);
    if (!ret) ret = dcc_compile_local(command, input, fd >= 0 ? prepared : NULL, cpu_ns);
    free(cwd);
    dcc_free_argv(command);
    return ret;
}
#ifdef HAVE_FSTATAT
/* Re-write "cc" to directly call gcc or clang
 */
static void dcc_rewrite_generic_compiler(char **argv)
{
    char linkbuf[MAXPATHLEN + 1], *link = NULL, *t;
    int ret, dir;
    ssize_t ssz;
    struct stat st;
    bool cpp = false;

    assert(argv);

    if (strcmp(argv[0], "cc") == 0)
        ;
    else if (strcmp(argv[0], "c++") == 0)
        cpp = true;
    else
        return;

    ret = dcc_which(cpp ? "c++" : "cc", &link);
    if (ret < 0)
        return;
    t = strrchr(link, '/');
    if (!t)
        return;
    *t = '\0';
    dir = open(link, O_RDONLY);
    if (dir < 0)
        return;
    *t = '/';
    ret = fstatat(dir, t + 1, &st, AT_SYMLINK_NOFOLLOW);
    if (ret < 0)
        return;
    if ((st.st_mode & S_IFMT) != S_IFLNK)
        /* TODO use cc -v */
        return;
    ssz = readlinkat(dir, t + 1, linkbuf, sizeof(linkbuf) - 1);
    if (ssz < 0)
        return;
    linkbuf[ssz] = '\0';
    fstatat(dir, linkbuf, &st, AT_SYMLINK_NOFOLLOW);
    if ((st.st_mode & S_IFMT) == S_IFLNK) {
        /* this is a Debian thing. Fedora just has /usr/bin/cc -> gcc */
        if (strcmp(linkbuf, cpp ? "/etc/alternatives/c++" : "/etc/alternatives/cc") == 0) {
            char m[MAXPATHLEN + 1];

            m[0] = '\0';
            strcpy(m, linkbuf);

            ssz = readlinkat(dir, m, linkbuf, sizeof(linkbuf) - 1);
            linkbuf[ssz] = '\0';
        }
    }
    ret = faccessat(dir, linkbuf, X_OK, 0);
    if (ret < 0)
        return;

    if (        cpp && strcmp(strrchr(linkbuf, '/') ? strrchr(linkbuf, '/') + 1 : linkbuf, "clang++") == 0) {
        free(argv[0]);
        argv[0] = strdup("clang++");
        rs_trace("Rewriting '%s' to '%s'", "c++", "clang++");
    } else if (   cpp && strcmp(strrchr(linkbuf, '/') ? strrchr(linkbuf, '/') + 1 : linkbuf, "g++") == 0) {
        free(argv[0]);
        argv[0] = strdup("g++");
        rs_trace("Rewriting '%s' to '%s'", "c++", "g++");
    } else if (!cpp && strcmp(strrchr(linkbuf, '/') ? strrchr(linkbuf, '/') + 1 : linkbuf, "clang") == 0) {
        free(argv[0]);
        argv[0] = strdup("clang");
        rs_trace("Rewriting '%s' to '%s'", "cc", "clang");
    } else if (!cpp && strcmp(strrchr(linkbuf, '/') ? strrchr(linkbuf, '/') + 1 : linkbuf, "gcc") == 0) {
        free(argv[0]);
        argv[0] = strdup("gcc");
        rs_trace("Rewriting '%s' to '%s'", "cc", "gcc");
    } else
        return;
}
#endif


/* Clang is a native cross-compiler, but needs to be told to what target it is
 * building.
 * TODO: actually probe clang with clang --version, instead of trusting
 * autoheader.
 */
static int dcc_add_clang_target(char **argv)
{
        /* defined by autoheader */
    const char *target = NATIVE_COMPILER_TRIPLE;
    const char *compiler = dcc_find_basename(argv[0]);

    /* Internal Clang tools take -triple, not the driver's -target option. */
    if (argv[1] && !strncmp(argv[1], "-cc1", 4))
        return 0;

    if (strncmp(compiler, "clang", 5) || !dcc_clang_driver_mode(compiler))
        return 0;

    /* -target aarch64-linux-gnu */
    if (dcc_argv_search(argv, "-target"))
        return 0;

    /* --target=aarch64-linux-gnu */
    if (dcc_argv_startswith(argv, "--target"))
        return 0;

    rs_log_info("Adding '-target %s' to support clang cross-compilation.",
                target);
    dcc_argv_append(argv, strdup("-target"));
    dcc_argv_append(argv, strdup(target));
    return 1;
}

/*
 * Cross compilation for gcc
*/
/* A shallow wire view owns its compiler name in the array allocation. Resolve
 * an absolute Clang to the selected executable, not the worker's default LLVM
 * slot. The prepared job uses this same conversion. */
static char **dcc_remote_argv(char **argv)
{
    int argc = dcc_argv_len(argv);
    enum dcc_clang_mode clang = dcc_clang_driver_mode(argv[0]);
    char *resolved = clang && strchr(argv[0], '/') ? realpath(argv[0], NULL) : NULL;
    const char *compiler = argv[0];
    if (clang) {
        /* Masquerade links resolve to distcc itself; retain their tool name. */
        compiler = dcc_find_basename(resolved && dcc_clang_driver_mode(resolved) ? resolved : argv[0]);
    }
    char **view = malloc((argc + 2) * sizeof(*view) + strlen(compiler) + 1);
    if (view) {
        memcpy(view, argv, argc * sizeof(*view));
        view[0] = strcpy((char *)(view + argc + 2), compiler);
        if (clang == DCC_CLANG_CXX) view[argc++] = (char *)"--driver-mode=g++";
        if (clang == DCC_CLANG_CPP) view[argc++] = (char *)"--driver-mode=cpp";
        view[argc] = NULL;
    }
    free(resolved);
    return view;
}

static int dcc_gcc_rewrite_fqn(char **argv)
{
        /* defined by autoheader */
    const char *target_with_vendor = NATIVE_COMPILER_TRIPLE;
    char *newcmd, *t, *path;
    int pathlen = 0;
    int newcmd_len = 0;

    if (strcmp(argv[0], "gcc") == 0 || strncmp(argv[0], "gcc-", strlen("gcc-")) == 0 ||
        strcmp(argv[0], "g++") == 0 || strncmp(argv[0], "g++-", strlen("g++-")) == 0)
        ;
    else
        return -ENOENT;


    newcmd_len = strlen(target_with_vendor) + 1 + strlen(argv[0]) + 1;
    newcmd = malloc(newcmd_len);
    if (!newcmd)
        return -ENOMEM;
    memset(newcmd, 0, newcmd_len);

    strcpy(newcmd, target_with_vendor);


    strcat(newcmd, "-");
    strcat(newcmd, argv[0]);

    /* TODO, is this the right PATH? */
    path = getenv("PATH");
    do {
        char binname[strlen(path) + 1 + strlen(newcmd) + 1];
        int r;

        /* emulate strchrnul() */
        t = strchr(path, ':');
        if (!t)
            t = path + strlen(path);
        pathlen = t - path;
        if (*path == '\0')
            break;
        strncpy(binname, path, pathlen);
        binname[pathlen] = '\0';
        strcat(binname, "/");
        strcat(binname, newcmd);
        r = access(binname, X_OK);
        if (r < 0)
            continue;
        /* good!, now rewrite */
        rs_log_info("Re-writing call to '%s' to '%s' to support cross-compilation.",
                    argv[0], newcmd);
        free(argv[0]);
        argv[0] = newcmd;
        return 0;
    } while ((path += pathlen + 1));
    free(newcmd);
    return -ENOENT;
}

static int dcc_get_max_retries(void)
{
    /* Backoff is advisory and may be managed by another process. Never
     * depend on it to make a failed compile terminate. */
    return 3;
}

/* Only pure object/archive links are candidates. The helper validates options
 * and acquires a local slot only when it falls back to a local command. */
static int dcc_link_candidate(char **argv, const char *broker)
{
    int i, input = 0;
    const char *profile = getenv("DISTCC_LINK_SYSROOT");
    enum dcc_clang_mode clang = dcc_clang_driver_mode(argv[0]);
    if (!broker || !*broker || clang == DCC_CLANG_CPP || ((!profile || !*profile) && !clang))
        return 0;
    for (i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "-o")) { if (argv[i + 1]) i++; continue; }
        if (!clang && argv[i][0] != '-' &&
            (str_endswith(".os", argv[i]) || str_endswith(".so", argv[i]) || strstr(argv[i], ".so.")))
            return 0;
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S") ||
            !strcmp(argv[i], "-E") || dcc_is_source(argv[i]))
            return 0;
        if (str_endswith(".o", argv[i]) || str_endswith(".a", argv[i]) || argv[i][0] == '@' ||
            (clang && !strncmp(argv[i], "-l", 2)))
            input = 1;
    }
    return input;
}

static int dcc_multi_candidate(char **argv, const char *broker)
{
    int inputs = 0;
    if (!broker || dcc_getenv_bool("_DISTCC_MULTI_LOCAL", 0)) return 0;
    for (int i = 1; argv[i]; ++i) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S") ||
            !strcmp(argv[i], "-E") || !strcmp(argv[i], "-###")) return 0;
        if (dcc_is_source(argv[i])) ++inputs;
    }
    return inputs > 1;
}

static int dcc_metadata_query(char **argv)
{
    int query = 0;
    for (int i = 1; argv[i]; ++i) {
        /* Do not treat operands of other options as terminal driver flags. */
        if (argv[i][0] != '-') return 0;
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-dumpmachine") ||
            !strcmp(argv[i], "-dumpversion") || !strcmp(argv[i], "-dumpfullversion") ||
            !strcmp(argv[i], "-print-search-dirs") ||
            !strncmp(argv[i], "-print-file-name=", 17) ||
            !strncmp(argv[i], "-print-prog-name=", 17))
            query = 1;
    }
    return query;
}

/**
 * Execute the commands in argv remotely or locally as appropriate.
 *
 * We may need to run cpp locally; we can do that in the background
 * while trying to open a remote connection.
 *
 * This function is slightly inefficient when it falls back to running
 * gcc locally, because cpp may be run twice.  Perhaps we could adjust
 * the command line to pass in the .i file.  On the other hand, if
 * something has gone wrong, we should probably take the most
 * conservative course and run the command unaltered.  It should not
 * be a big performance problem because this should occur only rarely.
 *
 * @param argv Command to execute.  Does not include 0='distcc'.
 * Must be dynamically allocated.  This routine deallocates it.
 *
 * @param status On return, contains the waitstatus of the compiler or
 * preprocessor.  This function can succeed (in running the compiler) even if
 * the compiler itself fails.  If either the compiler or preprocessor fails,
 * @p status is guaranteed to hold a failure value.
 *
 * Implementation notes:
 *
 * Broker pools with a common input format finish include analysis before
 * selecting a worker. Local preprocessing likewise precedes selection.
 * Legacy or mixed-format pools still select a host first to determine the
 * include transfer format; their lock order is remote then local.
 */
static int
dcc_build_somewhere(char *argv[],
                    int sg_level,
                    int *status)
{
    const char *broker = getenv("DISTCC_BROKER");
    int rewrite_cross = !getenv("DISTCC_NO_REWRITE_CROSS");
    char *input_fname = NULL, *output_fname, *cpp_fname = NULL, *deps_fname = NULL;
    int cpp_fd = -1;
    char **auxiliary = NULL;
    struct dcc_job *prepared_job = NULL;
    char **localcpp_server_argv = NULL;
    struct dcc_buffer server_stderr = {0}, server_stdout = {0};
    int preprocess_only, offload_targets = 0, remote_frontend = 0;
    int needs_dotd = 0;
    int sets_dotd_target = 0;
    pid_t cpp_pid = 0;
    int cpu_lock_fd = -1, local_cpu_lock_fd = -1;
    int direct_lease = -1;
    int ret = 0;
    int remote_ret = 0;
    int remote_completed = 0, cpp_ready = 0;
    int retry_count = 0, max_retries, remote_unsupported = 0;
    struct dcc_hostdef *host = NULL;
    struct dcc_work_hint work = {0};
    struct dcc_source_features source_features = {0};
    char **new_argv;

    max_retries = dcc_get_max_retries();

    if (sg_level) /* Recursive distcc - run locally, and skip all locking. */
        goto run_local;

    /* Parse response files once, before identifying actions, native options
     * and inputs. No client-side @ paths may escape to an analyzer or worker. */
    if ((ret = dcc_expand_response_files(&argv)) != 0)
        goto lock_local;
    if ((ret = dcc_expand_preprocessor_options(&argv)) != 0)
        goto clean_up;

    /* Build-system metadata queries do not consume a compilation lane, even
     * when the build appends its usual optimization/target/linker flags. */
    if (dcc_metadata_query(argv)) goto run_local;

    if (dcc_multi_candidate(argv, broker)) {
        int argc = dcc_argv_len(argv);
        char **command = calloc(argc + 2, sizeof(*command));
        if (!command) { ret = EXIT_OUT_OF_MEMORY; goto clean_up; }
        char path[PATH_MAX];
        command[0] = dcc_helper_path("distcc-multi", path);
        memcpy(command + 1, argv, (argc + 1) * sizeof(*argv));
        /* The coordinator must not hold a local slot while waiting for its
         * children, which acquire capacity through the normal broker path. */
        ret = dcc_compile_local(command, NULL, NULL, NULL);
        free(command);
        goto clean_up;
    }

    /* Links belong to the link driver, not the compile-only Clang scanner. */
    if (dcc_link_candidate(argv, broker)) {
        ret = dcc_link_dispatch(argv);
        goto clean_up;
    }

    /* TODO: Perhaps tidy up these gotos. */

    /* FIXME: this may leak memory for argv. */

    /* Classify before probing: preprocessing and local links need no native
     * expansion. A successful expansion invalidates the scanner's pointers. */
#ifdef HAVE_FSTATAT
    if (rewrite_cross) dcc_rewrite_generic_compiler(argv);
#endif
    ret = dcc_prepare_clang(&argv, &auxiliary);
    if (ret == EXIT_LOCAL_COMPILE) goto lock_local;
    if (ret) goto fallback;
    ret = dcc_scan_args(argv, &input_fname, &output_fname, &new_argv, 1);
    dcc_free_argv(argv);
    argv = new_argv;
    preprocess_only = dcc_argv_search(argv, "-E");
    if (preprocess_only)
        ret = EXIT_LOCAL_CPP;
    /* Local execution consumes the same broker capacity as a local worker,
     * but needs neither native expansion nor a duplicate header/input tree. */
    if (!ret && !dcc_offload_language(argv) &&
        broker && *broker) {
        dcc_work_describe(&work, argv, input_fname, NULL, -1, 0, &source_features);
        direct_lease = dcc_broker_try_local(&work);
        if (direct_lease >= 0) goto run_local;
    }
    remote_frontend = !ret && !preprocess_only &&
        dcc_getenv_bool("DISTCC_FRONTEND_REMOTE", 0) && broker &&
        (dcc_cpp_ir(argv, input_fname) || dcc_cpp_gcc(argv, input_fname));
    /* Driver config files belong to the client. Only a captured Clang
     * frontend command has already expanded them before transport. */
    if (!ret && !(remote_frontend && dcc_cpp_ir(argv, input_fname)) &&
        (dcc_argv_search(argv, "--config") || dcc_argv_startswith(argv, "--config=")))
        ret = EXIT_LOCAL_COMPILE;
    /* The native GCC path retains '-' all the way to cc1 and transports its
     * input independently of pathname lookup. Other paths still inherit stdin. */
    if (!ret && !strcmp(input_fname, "-") &&
        (!remote_frontend || !dcc_cpp_gcc(argv, input_fname))) ret = EXIT_LOCAL_CPP;
    /* Assembly may read .include/.incbin after preprocessing. Only the
     * native frontend's filesystem carries those reads to the client. */
    if (!ret && !remote_frontend && dcc_cpp_assembly(argv, input_fname))
        ret = EXIT_LOCAL_CPP;
    if (!ret && (dcc_offload_language(argv) ||
        ((!remote_frontend || !dcc_cpp_ir(argv, input_fname)) && dcc_native_requested(argv)))) {
        if (dcc_offload_language(argv) ? dcc_prepare_offload(&argv, &auxiliary, &offload_targets) :
                                       dcc_expand_native(&argv, input_fname, output_fname)) {
            ret = EXIT_DISTCC_FAILED;
        } else {
            ret = dcc_scan_args(argv, &input_fname, &output_fname, &new_argv, 0);
            dcc_free_argv(argv);
            argv = new_argv;
            work = (struct dcc_work_hint){0};
        }
    }
    if (rewrite_cross) {
        int changed = dcc_add_clang_target(new_argv);
        changed |= dcc_gcc_rewrite_fqn(new_argv) == 0;
        if (changed) work = (struct dcc_work_hint){0};
    }
    if (ret != 0) {
        /* we need to scan the arguments even if we already know it's
         * local, so that we can pick up distcc client options. */
        goto lock_local;
    }

    if (offload_targets) {
        char **command = NULL;
        ret = dcc_offload_command(argv, input_fname, output_fname, &command);
        if (!ret) ret = dcc_compile_local(command, input_fname, NULL, NULL);
        if (command) dcc_free_argv(command);
        goto unlock_and_clean_up;
    }

#if 0
    /* turned off because we never spend long in this state. */
    dcc_note_state(DCC_PHASE_STARTUP, input_fname, NULL);
#endif
    /* Emit frontend IR once. The worker optimizes and generates machine code
     * without needing a second preprocessing pass or a header snapshot. */
    if (remote_frontend) {
        if (dcc_cpp_ir(argv, input_fname))
            ret = dcc_frontend_prepare(argv, input_fname, &prepared_job);
        else ret = dcc_gcc_frontend_prepare(argv, input_fname, &prepared_job);
        if (ret == EXIT_LOCAL_COMPILE) goto lock_local;
        if (ret) goto fallback;
        if (!strcmp(input_fname, "-") && dcc_job_capture_stdin(prepared_job)) {
            ret = EXIT_IO_ERROR; goto unlock_and_clean_up;
        }
    }
    /* With a broker, finish local preprocessing before reserving a worker.
     * Release the local lock before requesting the remote lease; never wait
     * for a remote slot while holding a local slot. Reuse this input on retry. */
    if (broker && *broker && !prepared_job) {
        dcc_read_localslots_configuration();
        if (local_cpu_lock_fd == -1 && !dcc_is_preprocessed(input_fname) &&
            (ret = dcc_lock_local_cpp(&local_cpu_lock_fd))) goto fallback;
        ret = dcc_cpp_maybe(argv, input_fname, &cpp_fname, &cpp_pid, &cpp_fd);
        if (!ret) ret = dcc_wait_for_cpp(cpp_pid, status, input_fname);
#ifdef __linux__
        if (!ret && !*status && cpp_pid > 0 && cpp_fd >= 0 &&
            (fchmod(cpp_fd, 0444) || fcntl(cpp_fd, F_ADD_SEALS,
                F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)))
            ret = EXIT_IO_ERROR;
#endif
        if (local_cpu_lock_fd != -1) dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1;
        cpp_pid = 0;
        if (!ret && *status)
            ret = dcc_critique_status(*status, "cpp", input_fname, dcc_hostdef_local, 0);
        if (ret) goto unlock_and_clean_up;
        cpp_ready = 1;
    }

    if (broker && (cpp_ready || !work.key))
        dcc_work_describe(&work, argv, input_fname, cpp_fname, cpp_fd, cpp_ready, &source_features);
    if (prepared_job && !dcc_job_frontend(prepared_job) && work.key)
        work.bytes = dcc_resource_input_bytes(prepared_job);
    else if (!cpp_ready && (!prepared_job || !dcc_job_frontend(prepared_job)))
        work = (struct dcc_work_hint){0};
    if (prepared_job && work.key)
        work.memory_class = dcc_resource_class(prepared_job);

    /* Lock ordering invariant: always acquire the lock for the
     * remote host (if any) first. */

    /* Choose the distcc server host (which could be either a remote
     * host or localhost) and acquire the lock for it.  */
  choose_host:
    dcc_free_buffer(&server_stderr);
    dcc_free_buffer(&server_stdout);
    remote_completed = 0;
    if ((ret = dcc_pick_host_from_list_and_lock_it(&host, &cpu_lock_fd,
            cpp_ready || (prepared_job && dcc_job_frontend(prepared_job)), &work)) != 0) {
        /* Doesn't happen at the moment: all failures are masked by
           returning localhost. */
        goto fallback;
    }
    if (host->mode == DCC_MODE_LOCAL) {
        if (host->broker_managed) {
            direct_lease = cpu_lock_fd;
            cpu_lock_fd = -1;
        }
        /* We picked localhost and already have a lock on it so no
         * need to lock it now. */
        goto run_local;
    }

    if (prepared_job && dcc_job_frontend(prepared_job)) {
        if (!host->broker_managed || host->compr != DCC_COMPRESS_LZ4_STREAM) goto fallback;
        char *target = NULL;
        if ((ret = dcc_get_dotd_info(argv, &deps_fname, &needs_dotd,
                                     &sets_dotd_target, &target)))
            goto fallback;
        ret = dcc_compile_direct(host, prepared_job, prepared_job->argv, NULL, -1, auxiliary,
                                output_fname, needs_dotd ? deps_fname : NULL,
                                &server_stderr, &server_stdout, status);
        goto remote_result;
    }

    if (!cpp_ready && !dcc_is_preprocessed(input_fname) &&
        (ret = dcc_lock_local_cpp(&local_cpu_lock_fd)))
        goto fallback;
    if (!cpp_ready && (ret = dcc_cpp_maybe(argv, input_fname, &cpp_fname, &cpp_pid, NULL)))
        goto fallback;
    if (!localcpp_server_argv &&
        (ret = dcc_strip_local_args(argv, &localcpp_server_argv, input_fname)))
        goto fallback;
    char **wire_argv = dcc_remote_argv(localcpp_server_argv);
    if (!wire_argv) { ret = EXIT_OUT_OF_MEMORY; goto unlock_and_clean_up; }
    ret = dcc_compile_remote(wire_argv,
                                  input_fname,
                                  cpp_fname,
                                  cpp_fd,
                                  auxiliary,
                                  prepared_job,
                                  output_fname,
                                  needs_dotd ? deps_fname : NULL,
                                  &server_stderr,
                                  &server_stdout,
                                  cpp_pid, local_cpu_lock_fd,
                  host, status, &remote_unsupported);
    free(wire_argv);
  remote_result:
    if (ret != 0) {
        /* Returns zero if we successfully ran the compiler, even if
         * the compiler itself bombed out. */

        if (remote_unsupported) {
	    /* Local files are necessary for compilation of the *preprocessed*
	     * source (i.e. due to .incbin assembler directive). */
	    goto fallback_unsupported;
	}

        /* dcc_compile_remote() already unlocked local_cpu_lock_fd. */
        local_cpu_lock_fd = -1;
        if (ret == EXIT_BUSY) {
            release_host(host, &cpu_lock_fd, &local_cpu_lock_fd, 0);
            goto choose_host;
        }
        if (ret == DCC_REMOTE_LOCAL_ERROR) {
            rs_log_error("could not publish the completed remote result locally");
            ret = EXIT_IO_ERROR;
            goto unlock_and_clean_up;
        }
        release_host(host, &cpu_lock_fd, &local_cpu_lock_fd, 1);
        retry_count++;
        if (retry_count < max_retries)
            goto choose_host;
        else {
            rs_log_warning("Couldn't find a host in %d attempts, retrying locally",
                           retry_count);
            goto fallback;
	}
    }
    /* dcc_compile_remote() already unlocked local_cpu_lock_fd. */
    local_cpu_lock_fd = -1;
    remote_completed = 1;

    dcc_enjoyed_host(host);

    dcc_unlock(cpu_lock_fd);
    cpu_lock_fd = -1;

    ret = dcc_critique_status(*status, "compile", input_fname, host, 1);
    if (ret == 0) {
        /* Try to write the server-side errors to stderr.
         * If that fails, even though the compilation succeeded,
         * we haven't managed to give these errors to the user,
         * so we have to try again.
         * FIXME: This is unlikely to fail, if it does it's unlikely any other
         * operation will work, and this makes the mistake of
         * blaming the server for what is (clearly?) a local failure.
         */
        if (dcc_writex(STDERR_FILENO, server_stderr.data, server_stderr.size)) {
            rs_log_warning("Could not show server-side errors");
            goto fallback;
        }
        /* SUCCESS! */
        ret = dcc_writex(STDOUT_FILENO, server_stdout.data, server_stdout.size);
        goto clean_up;
    }
    if (ret < 128) {
        /* Remote compile just failed, e.g. with syntax error.
           It may be that the remote compilation failed because
           the file has an error, or because we did something
           wrong (e.g. we did not send all the necessary files.)
           Retry locally. If the local compilation also fails,
           then we know it's the program that has the error,
           and it doesn't really matter that we recompile, because
           this is rare.
           If the local compilation succeeds, then we know it's our
           fault, and we should do something about it later.
           (Currently, we send email to an appropriate email address).
        */
        if (getenv("DISTCC_SKIP_LOCAL_RETRY")) {
            /* don't retry locally. We'll treat the remote failure as
               if it was a local one. But if we can't get the failures
               then we need to retry regardless.
            */
            if (dcc_writex(STDERR_FILENO, server_stderr.data, server_stderr.size)) {
                rs_log_warning("remote compilation of '%s' failed",\
                               input_fname);
                rs_log_warning("Could not show server-side errors, retrying locally");
                goto fallback;
            }
            /* Not retrying */
            if (dcc_writex(STDOUT_FILENO, server_stdout.data, server_stdout.size))
                ret = EXIT_IO_ERROR;
            goto clean_up;
        } else {
            rs_log_warning("remote compilation of '%s' failed, retrying locally",
                           input_fname);
            remote_ret = ret;
            goto fallback;
        }
    }


  fallback:
    /* Unsupported local analysis says nothing about the worker's health. */
    release_host(host, &cpu_lock_fd, &local_cpu_lock_fd, ret != EXIT_LOCAL_CPP);

    if (!dcc_getenv_bool("DISTCC_FALLBACK", 1)) {
        rs_log_error("failed to distribute and fallbacks are disabled");
        /* Try copying any server-side error message to stderr;
         * If we fail the user will miss all the messages from the server; so
         * we pretend we failed remotely.
         */
        if (dcc_writex(STDERR_FILENO, server_stderr.data, server_stderr.size)) {
            rs_log_error("Could not print server-side error messages");
        }
        if (remote_completed &&
            dcc_writex(STDOUT_FILENO, server_stdout.data, server_stdout.size))
            ret = EXIT_IO_ERROR;
        goto clean_up;
    }

    /* At this point, we can abandon the remote errors. */
    dcc_free_buffer(&server_stderr);
    dcc_free_buffer(&server_stdout);

    /* "You guys are so lazy!  Do I have to do all the work myself??" */
    if (host) {
        rs_log(RS_LOG_WARNING|RS_LOG_NONAME,
               "failed to distribute %s to %s, running locally instead",
               input_fname ? input_fname : "(unknown)",
               host->hostdef_string);
    } else {
        rs_log_warning("failed to distribute %s (status=%d), running locally instead",
                       input_fname ? input_fname : "(unknown)", ret);
    }

    fallback_unsupported:
    if (remote_unsupported) {
        rs_log_warning("%s: can't distribute due to unsupported directives, running locally",
                       input_fname ? input_fname : "(unknown)");
    }

  lock_local:
    dcc_read_localslots_configuration();
    if (ret == EXIT_LOCAL_CPP) {
        if (local_cpu_lock_fd == -1) dcc_lock_local_cpp(&local_cpu_lock_fd);
    } else {
        dcc_lock_local(&cpu_lock_fd);
    }

  run_local:
    /* Either compile locally, after remote failure, or simply do other cc tasks
       as assembling, linking, etc. */
    {
        struct timespec started, finished;
        uint64_t cpu_ns = 0;
        char stdin_path[64], *prepared_stdin = NULL;
        if (prepared_job && prepared_job->stdin_fd) {
            snprintf(stdin_path, sizeof(stdin_path), "/proc/self/fd/%d", prepared_job->stdin_fd - 1);
            prepared_stdin = stdin_path;
        }
        if (direct_lease >= 0) clock_gettime(CLOCK_MONOTONIC, &started);
        if (!ret && !remote_ret && prepared_job && prepared_job->kind == DCC_JOB_FRONTEND) {
            char **command = prepared_job->argv + 2;
            char *identity = command[0];
            command[0] = prepared_job->argv[0];
            ret = dcc_compile_local(command, input_fname, prepared_stdin, &cpu_ns);
            command[0] = identity;
        } else if (!ret && !remote_ret && cpp_ready && dcc_cpp_ir(argv, input_fname))
            ret = dcc_compile_local_ir(argv, input_fname, cpp_fname, cpp_fd, &cpu_ns);
        else
            ret = dcc_compile_local(argv, input_fname, prepared_stdin, &cpu_ns);
        if (direct_lease >= 0) {
            clock_gettime(CLOCK_MONOTONIC, &finished);
            dcc_broker_local_done(direct_lease, finished.tv_sec - started.tv_sec +
                                 (finished.tv_nsec - started.tv_nsec) / 1e9, cpu_ns, ret);
            direct_lease = -1;
        }
    }
    if (remote_ret && host) dcc_enjoyed_host(host);

  unlock_and_clean_up:
    if (cpu_lock_fd != -1) {
        dcc_unlock(cpu_lock_fd);
        cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }
    if (local_cpu_lock_fd != -1) {
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }

  clean_up:
    dcc_job_free(prepared_job);
    if (auxiliary) dcc_free_argv(auxiliary);
    if (cpp_fd >= 0) { close(cpp_fd); free(cpp_fname); }
    dcc_free_argv(argv);
    if (localcpp_server_argv != NULL) {
        free(localcpp_server_argv);
    }
    dcc_free_buffer(&server_stdout);
    dcc_free_buffer(&server_stderr);
    return ret;
}

/*
 * argv must be dynamically allocated.
 * This routine will deallocate it.
 */
int dcc_build_somewhere_timed(char *argv[],
                              int sg_level,
                              int *status)
{
    struct timeval before, after, delta;
    int ret;

    if (gettimeofday(&before, NULL))
        rs_log_warning("gettimeofday failed");

    ret = dcc_build_somewhere(argv, sg_level, status);

    if (gettimeofday(&after, NULL)) {
        rs_log_warning("gettimeofday failed");
    } else {
        /* TODO: Show rate based on cpp size?  Is that meaningful? */
        timeval_subtract(&delta, &after, &before);

        rs_log(RS_LOG_INFO|RS_LOG_NONAME,
               "elapsed compilation time %lld.%06lds",
               (long long) delta.tv_sec, (long) delta.tv_usec);
    }

    return ret;
}
