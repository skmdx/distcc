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
#include <errno.h>
#include <sys/stat.h>

#include <ctype.h>

#ifdef HAVE_FNMATCH_H
  #include <fnmatch.h>
#endif

#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <assert.h>

#include "distcc.h"
#include "job.h"
#include "frontend_fs.h"
#include "resource.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "bulk.h"
#include "implicit.h"
#include "exec.h"
#include "where.h"
#include "lock.h"
#include "timeval.h"
#include "compile.h"
#include "broker.h"
#include "workload.h"
#include "include_server_if.h"
#include "emaillog.h"
#include "dotd.h"

/**
 * This boolean is true iff --scan-includes option is enabled.
 * If so, distcc will just run the source file through the include server,
 * and print out the list of header files that might be #included,
 * rather than actually compiling the sources.
 */
int dcc_scan_includes = 0;

static const char *const include_server_port_suffix = "/socket";
static const char *const discrepancy_suffix = "/discrepancy_counter";

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

static int dcc_get_max_discrepancies_before_demotion(void)
{
    /* Warning: the default setting here should have the same value as in the
     * pump.in script! */
    static const int default_setting = 1;
    static int current_setting = 0;

    if (current_setting > 0)
        return current_setting;

    const char *user_setting = getenv("DISTCC_MAX_DISCREPANCY");
    if (user_setting) {
        int parsed_user_setting = atoi(user_setting);
        if (parsed_user_setting <= 0) {
            rs_log_error("Bad DISTCC_MAX_DISCREPANCY value: %s", user_setting);
            exit(EXIT_BAD_ARGUMENTS);
        }
        current_setting = parsed_user_setting;
    } else {
        current_setting = default_setting;
    }
    return current_setting;
}

/**
 * Return in @param filename the name of the file we use as unary counter of
 * discrepancies (a compilation failing on the server, but succeeding
 * locally. This function may return NULL in @param filename if the name cannot
 * be determined.
 **/
int dcc_discrepancy_filename(char **filename)
{
    const char *include_server_port = getenv("INCLUDE_SERVER_PORT");
    *filename = NULL;
    if (include_server_port == NULL) {
        return 0;
    } else if (str_endswith(include_server_port_suffix,
                            include_server_port)) {
        /* We're going to make a longer string from include_server_port: one
         * that replaces include_server_port_suffix with discrepancy_suffix. */
        int delta = strlen(discrepancy_suffix) -
            strlen(include_server_port_suffix);
        assert (delta > 0);
        *filename = malloc(strlen(include_server_port) + 1 + delta);
        if (!*filename) {
            rs_log_error("failed to allocate space for filename");
            return EXIT_OUT_OF_MEMORY;
        }
        strcpy(*filename, include_server_port);
        int slash_pos = strlen(include_server_port)
                        - strlen(include_server_port_suffix);
        /* Because include_server_port_suffix is a suffix of include_server_port
         * we expect to find a '/' at slash_pos in filename. */
        assert((*filename)[slash_pos] == '/');
        (void) strcpy(*filename + slash_pos, discrepancy_suffix);
        return 0;
    } else
        return 0;
}


/**
 * Return the length of the @param discrepancy_filename in newly allocated
 * memory; return 0 if it's not possible to determine the length (if
 * e.g. @param discrepancy_filename is NULL).
 **/
static int dcc_read_number_discrepancies(const char *discrepancy_filename)
{
    if (!discrepancy_filename) return 0;
    struct stat stat_record;
    if (stat(discrepancy_filename, &stat_record) == 0) {
        size_t size = stat_record.st_size;
        /* Does size fit in an 'int'? */
        if ((((size_t) (int) size) == size) &&
            ((int) size) > 0)
            return ((int) size);
        else
            return INT_MAX;
    } else
        return 0;
}


/**
 *  Lengthen the file whose name is @param discrepancy_filename by one byte. Or,
 *  do nothing, if @param discrepancy_filename is NULL.
 **/
static int dcc_note_discrepancy(const char *discrepancy_filename)
{
    FILE *discrepancy_file;
    if (!discrepancy_filename) return 0;
    if (!(discrepancy_file = fopen(discrepancy_filename, "a"))) {
        rs_log_error("failed to open discrepancy_filename file: %s: %s",
                     discrepancy_filename,
                     strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (fputc('@', discrepancy_file) == EOF) {
        rs_log_error("failed to write to discrepancy_filename file: %s",
                     discrepancy_filename);
        fclose(discrepancy_file);
        return EXIT_IO_ERROR;
    }
    /* The file position is a property of the stream, so we are
    assured that exactly one process will take the 'if' branch when
    max_discrepancies_before_demotion failures is reached. */
    if (ftell(discrepancy_file) == 
        (long int)dcc_get_max_discrepancies_before_demotion()) {
        rs_log_warning("now using plain distcc, possibly due to "
                       "inconsistent file system changes during build");
    }
    fclose(discrepancy_file);
    return 0;
}


/**
 * In some cases, it is ill-advised to preprocess on the server. Check for such
 * situations. If they occur, then change protocol version.
 **/
static void dcc_perhaps_adjust_cpp_where_and_protover(
    char *input_fname,
    struct dcc_hostdef *host,
    char *discrepancy_filename)
{
    /* It's unfortunate that the variable that controls preprocessing is in the
       "host" datastructure. See elaborate complaint in dcc_build_somewhere. */

    const char *include_server = getenv("INCLUDE_SERVER_PORT");
    if (!include_server || !*include_server) {
        host->cpp_where = DCC_CPP_ON_CLIENT;
        dcc_get_protover_from_features(host->compr, host->cpp_where, &host->protover);
        return;
    }

    /* Check whether there has been too much trouble running distcc-pump during
       this build. */
    if (dcc_read_number_discrepancies(discrepancy_filename) >=
        dcc_get_max_discrepancies_before_demotion()) {
        /* Give up on using distcc-pump */
        host->cpp_where = DCC_CPP_ON_CLIENT;
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }

    /* Don't do anything silly for already preprocessed files. */
    if (dcc_is_preprocessed(input_fname)) {
        /* Don't subject input file to include analysis. */
        rs_log_warning("cannot use distcc_pump on already preprocessed file"
                       " (such as emitted by ccache)");
        host->cpp_where = DCC_CPP_ON_CLIENT;
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }
    /* Environment variables CPATH and two friends are hidden ways of passing
     * -I's. Beware! */
    if (getenv("CPATH") || getenv("C_INCLUDE_PATH")
        || getenv("CPLUS_INCLUDE_PATH")) {
        rs_log_warning("cannot use distcc_pump with any of environment"
                       " variables CPATH, C_INCLUDE_PATH or CPLUS_INCLUDE_PATH"
                       " set, preprocessing locally");
        host->cpp_where = DCC_CPP_ON_CLIENT;
        dcc_get_protover_from_features(host->compr,
                                       host->cpp_where,
                                       &host->protover);
    }
}



/**
 * Do a time analysis of dependencies in dotd file.  First, if @param dotd_fname
 * is created before @param reference_time, then return NULL in result.  Second,
 * if one of the files mentioned in the @param dotd_fname is modified after time
 * @param reference_time, then return non-NULL in result. Otherwise return NULL
 * in result.  A non-NULL value in result is a pointer to a newly allocated
 * string describing the offending dependency.

 * If @param exclude_pattern is not NULL, then files matching the glob @param
 * exclude_pattern are not considered in the above comparison.
 *
 *  This function is not declared static --- for purposes of testing.
 **/
int dcc_fresh_dependency_exists(const char *dotd_fname,
                                const char *exclude_pattern,
                                time_t reference_time,
                                char **result)
{
    struct stat stat_dotd;
    off_t dotd_fname_size = 0;
    FILE *fp;
    int c;
    int res;
    char *dep_name;

    *result = NULL;
    /* Allocate buffer for dotd contents and open it. */
    res = stat(dotd_fname, &stat_dotd);
    if (res) {
        rs_trace("could not stat \"%s\": %s", dotd_fname, strerror(errno));
        return 0;
    }
    if (stat_dotd.st_mtime < reference_time) {
        /* That .d file appears to be too old; don't trust it for this
         * analysis. */
        rs_trace("old dotd file \"%s\"", dotd_fname);
        return 0;
    }
    dotd_fname_size = stat_dotd.st_size;
    /* Is dotd_fname_size representable as a size_t value ? */
    if ((off_t) (size_t) dotd_fname_size == dotd_fname_size) {
        dep_name = malloc((size_t) dotd_fname_size);
        if (!dep_name) {
            rs_log_error("failed to allocate space for dotd file");
            return EXIT_OUT_OF_MEMORY;
        }
    } else { /* This is exceedingly unlikely. */
        rs_trace("file \"%s\" is too big", dotd_fname);
        return 0;
    }
    if ((fp = fopen(dotd_fname, "r")) == NULL) {
        rs_trace("could not open \"%s\": %s", dotd_fname, strerror(errno));
        free(dep_name);
        return 0;
    }

    /* Find ':'. */
    while ((c = getc(fp)) != EOF && c != ':');
    if (c != ':') goto return_0;

    /* Process dependencies. */
    while (c != EOF) {
        struct stat stat_dep;
        int i = 0;
        /* Skip whitespaces and backslashes. */
        while ((c = getc(fp)) != EOF && (isspace(c) || c == '\\'));
        /* Now, we're at start of file name. */
        ungetc(c, fp);
        while ((c = getc(fp)) != EOF &&
               (!isspace(c) || c == '\\')) {
            if (i >= dotd_fname_size) {
                /* Impossible */
                rs_log_error("not enough room for dependency name");
                goto return_0;
            }
            if (c == '\\') {
                /* Skip the newline. */
                if ((c = getc(fp)) != EOF)
                    if (c != '\n') ungetc(c, fp);
            }
            else dep_name[i++] = c;
        }
        if (i != 0) {
            dep_name[i] = '\0';
#ifdef HAVE_FNMATCH_H
            if (exclude_pattern == NULL ||
                fnmatch(exclude_pattern, dep_name, 0) == FNM_NOMATCH) {
#else
            /* Tautology avoids compiler warning about unused variable. */
            if (exclude_pattern == exclude_pattern) {
#endif
                /* The dep_name is not excluded; now verify that it is not too
                 * young. */
                rs_log_info("Checking dependency: %s", dep_name);
                res = stat(dep_name, &stat_dep);
                if (res) goto return_0;
                if (stat_dep.st_ctime >= reference_time) {
                    fclose(fp);
                    *result = realloc(dep_name, strlen(dep_name) + 1);
                    if (*result == NULL) {
                        rs_log_error("realloc failed");
                        return EXIT_OUT_OF_MEMORY;
                    }
                    return 0;
                }
            }
        }
    }
  return_0:
    fclose(fp);
    free(dep_name);
    return 0;
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
 * This is called with a lock on localhost already held.
 **/
static int dcc_compile_local(char *argv[],
                             char *input_name, const char *prepared)
{
    pid_t pid;
    int ret;
    int status;

    dcc_note_execution(dcc_hostdef_local, argv);
    dcc_note_state(DCC_PHASE_COMPILE, input_name, "localhost", DCC_LOCAL);

    if (!prepared && (ret = dcc_clang_local(argv, &status)) >= 0)
        return ret ? ret : dcc_critique_status(status, "compile", input_name,
                                              dcc_hostdef_local, 1);

    /* An IR job opens its sealed input as stdin before exec closes the
     * original memfd. Ordinary local commands retain the caller's stdio. */
    if ((ret = dcc_spawn_child(argv, &pid, prepared, NULL, NULL)) != 0)
        return ret;

    if ((ret = dcc_collect_child("cc", pid, &status, timeout_null_fd, NULL)))
        return ret;

    return dcc_critique_status(status, "compile", input_name,
                               dcc_hostdef_local, 1);
}

static int dcc_compile_local_ir(char **argv, char *input, const char *prepared, int fd)
{
    char **stripped = NULL, **command = NULL;
    char *cwd = NULL;
    int ret = dcc_strip_local_args(argv, &stripped, input);
    if (!ret) ret = dcc_copy_argv(stripped, &command, 0);
    free(stripped);
    if (!ret) ret = dcc_set_input(command, (char *)(fd >= 0 ? "/proc/self/fd/0" : prepared));
    if (!ret && !(cwd = getcwd(NULL, 0))) ret = EXIT_IO_ERROR;
    if (!ret) ret = dcc_clang_command(&command, "", input, cwd, 0, 1);
    if (!ret) ret = dcc_compile_local(command, input, fd >= 0 ? prepared : NULL);
    free(cwd);
    dcc_free_argv(command);
    return ret;
}


 /* Make the decision to send email about @param input_name, but only after a
  * little further investgation.
  *
  * We avoid sending email if there's a fresh dependency. To find out, we need
  * @param deps_fname, a .d file, created during the build.  We check each
  * dependency described there. If just one changed after the build started,
  * then we really don't want to hear about distcc-pump errors, because
  * dependencies shouldn't change. The files generated during the build are
  * exceptions. To disregard these, the distcc user may specify a glob pattern
  * in environment variable DISTCC_EXCLUDE_FRESH_FILES.
  *
  * Also, if there has been too many discrepancies (where the build has
  * succeeded remotely but failed locally), then we need to stop using
  * distcc-pump for the remainder of the build.  The present function
  * contributes to this logic: if it is determined that email must be sent, then
  * the count of such situations is incremented using the file @param
  * discrepancy_filename.
 */
static int dcc_please_send_email_after_investigation(
    const char *input_fname,
    const char *deps_fname,
    const char *discrepancy_filename) {

    int ret;
    char *fresh_dependency;
    const char *include_server_port = getenv("INCLUDE_SERVER_PORT");
    struct stat stat_port;
    rs_log_warning("remote compilation of '%s' failed, retried locally "
                   "and got a different result.", input_fname);
    if ((include_server_port != NULL) &&
        (stat(include_server_port, &stat_port)) == 0) {
        time_t build_start = stat_port.st_ctime;
        if (deps_fname) {
            const char *exclude_pattern =
                getenv("DISTCC_EXCLUDE_FRESH_FILES");

            if ((ret = dcc_fresh_dependency_exists(deps_fname,
                                                   exclude_pattern,
                                                   build_start,
                                                   &fresh_dependency))) {
                return ret;
            }
            if (fresh_dependency) {
                rs_log_warning("file '%s', a dependency of %s, "
                               "changed during the build", fresh_dependency,
                               input_fname);
                free(fresh_dependency);
                return dcc_note_discrepancy(discrepancy_filename);
            }
        }
    }
    dcc_please_send_email();
    return dcc_note_discrepancy(discrepancy_filename);
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
static void dcc_add_clang_target(char **argv)
{
        /* defined by autoheader */
    const char *target = NATIVE_COMPILER_TRIPLE;
    const char *compiler = dcc_find_basename(argv[0]);

    /* Frontend invocations already carry their driver's resolved -triple. */
    if (dcc_argv_search(argv, "-cc1"))
        return;

    if (strcmp(compiler, "clang") == 0 || strncmp(compiler, "clang-", strlen("clang-")) == 0 ||
        strcmp(compiler, "clang++") == 0 || strncmp(compiler, "clang++-", strlen("clang++-")) == 0)
        ;
    else
        return;

    /* -target aarch64-linux-gnu */
    if (dcc_argv_search(argv, "-target"))
        return;

    /* --target=aarch64-linux-gnu */
    if (dcc_argv_startswith(argv, "--target"))
        return;

    rs_log_info("Adding '-target %s' to support clang cross-compilation.",
                target);
    dcc_argv_append(argv, strdup("-target"));
    dcc_argv_append(argv, strdup(target));
}

/*
 * Cross compilation for gcc
*/
/* A shallow wire view owns its compiler name in the array allocation. Resolve
 * an absolute Clang to the selected executable, not the worker's default LLVM
 * slot. Both pump and locally preprocessed jobs use this same conversion. */
static char **dcc_remote_argv(char **argv)
{
    int argc = dcc_argv_len(argv);
    int clang = strstr(dcc_find_basename(argv[0]), "clang") != NULL;
    char *resolved = clang && strchr(argv[0], '/') ? realpath(argv[0], NULL) : NULL;
    const char *compiler = argv[0];
    if (clang) {
        /* Masquerade links resolve to distcc itself; retain their tool name. */
        compiler = dcc_find_basename(resolved && strstr(dcc_find_basename(resolved), "clang") ? resolved : argv[0]);
    }
    char **view = malloc((argc + 2) * sizeof(*view) + strlen(compiler) + 1);
    if (view) {
        memcpy(view, argv, argc * sizeof(*view));
        view[0] = strcpy((char *)(view + argc + 2), compiler);
        if (clang && strstr(dcc_find_basename(argv[0]), "++")) view[argc++] = (char *)"--driver-mode=g++";
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
 * and runs unsupported commands locally under the existing local slot lock. */
static int dcc_link_candidate(char **argv)
{
    int i, input = 0;
    const char *profile = getenv("DISTCC_LINK_SYSROOT");
    const char *broker = getenv("DISTCC_BROKER");
    if (!broker || !*broker || ((!profile || !*profile) && !strstr(argv[0], "clang")))
        return 0;
    for (i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S") ||
            !strcmp(argv[i], "-E") || dcc_is_source(argv[i]))
            return 0;
        if (str_endswith(".o", argv[i]) || str_endswith(".a", argv[i]) || argv[i][0] == '@')
            input = 1;
    }
    return input;
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
    char *input_fname = NULL, *output_fname, *cpp_fname = NULL, *deps_fname = NULL;
    int cpp_fd = -1;
    char **files = NULL;
    char **auxiliary = NULL;
    struct dcc_job *prepared_job = NULL;
    char **server_side_argv = NULL;
    char **localcpp_server_argv = NULL;
    char **remotecpp_server_argv = NULL;
    struct dcc_buffer server_stderr = {0}, server_stdout = {0};
    int preprocess_only, offload_targets = 0, remote_frontend = 0;
    int needs_dotd = 0;
    int sets_dotd_target = 0;
    pid_t cpp_pid = 0;
    int cpu_lock_fd = -1, local_cpu_lock_fd = -1;
    int direct_lease = -1;
    int ret;
    int remote_ret = 0;
    int remote_completed = 0, cpp_ready = 0;
    int retry_count = 0, max_retries, remote_unsupported = 0;
    struct dcc_hostdef *host = NULL;
    struct dcc_work_hint work = {0};
    char *discrepancy_filename = NULL;
    char **new_argv;

    max_retries = dcc_get_max_retries();

    if ((ret = dcc_discrepancy_filename(&discrepancy_filename)))
        goto clean_up;

    if (sg_level) /* Recursive distcc - run locally, and skip all locking. */
        goto run_local;

    /* Parse response files once, before identifying actions, native options
     * and inputs. No client-side @ paths may escape to an analyzer or worker. */
    if ((ret = dcc_expand_response_files(&argv)) != 0)
        goto lock_local;
    if ((ret = dcc_expand_preprocessor_options(&argv)) != 0)
        goto clean_up;

    /* Links belong to the link driver, not the compile-only Clang scanner. */
    if (dcc_link_candidate(argv)) {
        int argc = dcc_argv_len(argv);
        char **link_argv = calloc(argc + 2, sizeof(*link_argv));
        if (!link_argv || !(link_argv[0] = strdup("distcc-link"))) {
            free(link_argv);
            ret = EXIT_OUT_OF_MEMORY;
            goto clean_up;
        }
        memcpy(link_argv + 1, argv, (argc + 1) * sizeof(*argv));
        free(argv);
        argv = link_argv;
        goto lock_local;
    }

    /* TODO: Perhaps tidy up these gotos. */

    /* FIXME: this may leak memory for argv. */

    /* Classify before probing: preprocessing and local links need no native
     * expansion. A successful expansion invalidates the scanner's pointers. */
    ret = dcc_prepare_clang(&argv, &auxiliary);
    if (ret) goto lock_local;
    ret = dcc_scan_args(argv, &input_fname, &output_fname, &new_argv, 1);
    dcc_free_argv(argv);
    argv = new_argv;
    if (dcc_offload_language(argv)) unsetenv("INCLUDE_SERVER_PORT");
    preprocess_only = dcc_argv_search(argv, "-E");
    if (preprocess_only && (ret || !getenv("INCLUDE_SERVER_PORT")))
        ret = EXIT_LOCAL_CPP;
    /* Local execution consumes the same broker capacity as a local worker,
     * but needs neither native expansion nor a duplicate header/input tree. */
    if (!ret && !dcc_scan_includes && !dcc_offload_language(argv) &&
        getenv("DISTCC_BROKER") && *getenv("DISTCC_BROKER")) {
        dcc_work_describe(&work, argv, input_fname, NULL, -1, 0);
        direct_lease = dcc_broker_try_local(&work);
        if (direct_lease >= 0) goto run_local;
    }
    remote_frontend = !ret && !preprocess_only && !dcc_scan_includes &&
        dcc_getenv_bool("DISTCC_FRONTEND_REMOTE", 0) && getenv("DISTCC_BROKER") &&
        dcc_cpp_ir(argv, input_fname);
    if (!ret && (dcc_offload_language(argv) || (!remote_frontend && dcc_native_requested(argv)))) {
        if (dcc_offload_language(argv) ? dcc_prepare_offload(&argv, &auxiliary, &offload_targets) :
                                       dcc_expand_native(&argv, input_fname, output_fname)) {
            ret = EXIT_DISTCC_FAILED;
        } else {
            ret = dcc_scan_args(argv, &input_fname, &output_fname, &new_argv, 0);
            dcc_free_argv(argv);
            argv = new_argv;
        }
    }
    /* Configure probes must not wait behind a build's local compilation
     * backlog. Only standalone metadata queries bypass CPU admission. */
    if (argv[1] && !argv[2] &&
        (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-dumpmachine") ||
         !strcmp(argv[1], "-dumpversion") || !strcmp(argv[1], "-dumpfullversion")))
        goto run_local;
    if (!getenv("DISTCC_NO_REWRITE_CROSS")) {
#ifdef HAVE_FSTATAT
        dcc_rewrite_generic_compiler(new_argv);
#endif
        dcc_add_clang_target(new_argv);
        dcc_gcc_rewrite_fqn(new_argv);
    }
    if (ret != 0) {
        /* we need to scan the arguments even if we already know it's
         * local, so that we can pick up distcc client options. */
        goto lock_local;
    }

    if (offload_targets) {
        char **command = NULL;
        ret = dcc_offload_command(argv, input_fname, output_fname, &command);
        if (!ret) ret = dcc_compile_local(command, input_fname, NULL);
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
        if ((ret = dcc_lock_local_cpp(&local_cpu_lock_fd))) goto fallback;
        ret = dcc_frontend_prepare(argv, &prepared_job);
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1;
        if (ret) goto fallback;
    }
    if (!prepared_job && !preprocess_only && !dcc_scan_includes && dcc_cpp_ir(argv, input_fname))
        unsetenv("INCLUDE_SERVER_PORT");

    /* A homogeneous broker pool has a known input format before selection.
     * Finish include analysis before reserving a worker or opening its socket:
     * analysis must not consume worker slots or their idle timeout. */
    if (getenv("DISTCC_BROKER") && *getenv("DISTCC_BROKER") &&
        getenv("INCLUDE_SERVER_PORT") && !dcc_scan_includes && !prepared_job) {
        struct dcc_hostdef *list = NULL, *entry;
        int count, same_format = 1;
        enum dcc_compress compr = DCC_COMPRESS_NONE;
        if ((ret = dcc_get_hostlist(&list, &count))) goto fallback;
        for (entry = list; entry;) {
            struct dcc_hostdef *next = entry->next;
            enum dcc_compress format = entry->compr == DCC_COMPRESS_NONE ?
                DCC_COMPRESS_LZO1X : entry->compr;
            if (entry->mode != DCC_MODE_TCP ||
                (compr != DCC_COMPRESS_NONE && compr != format)) same_format = 0;
            compr = format;
            dcc_free_hostdef(entry);
            entry = next;
        }
        if (count && same_format) {
            ret = dcc_talk_to_include_server(argv, &files, compr);
        }
        if (!ret && files && compr == DCC_COMPRESS_LZ4_STREAM) {
            prepared_job = dcc_job_describe(argv, NULL, -1, files);
            if (!prepared_job) { ret = EXIT_IO_ERROR; goto fallback; }
        }
        if (count && same_format && ret) {
            rs_log_warning("failed to get includes from include server, "
                           "preprocessing locally before worker selection");
            if (dcc_getenv_bool("DISTCC_TESTING_INCLUDE_SERVER", 0)) dcc_exit(ret);
            files = NULL;
            unsetenv("INCLUDE_SERVER_PORT");
            if (preprocess_only) goto fallback;
        }
    }

    /* With a broker, finish local preprocessing before reserving a worker.
     * Release the local lock before requesting the remote lease; never wait
     * for a remote slot while holding a local slot. Reuse this input on retry. */
    if (getenv("DISTCC_BROKER") && *getenv("DISTCC_BROKER") &&
        !getenv("INCLUDE_SERVER_PORT") && !dcc_scan_includes && !prepared_job) {
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

    if (getenv("DISTCC_BROKER"))
        dcc_work_describe(&work, argv, input_fname, cpp_fname, cpp_fd, cpp_ready);
    if (prepared_job && prepared_job->kind != DCC_JOB_FRONTEND && work.key)
        work.bytes = dcc_resource_input_bytes(prepared_job);
    else if (!cpp_ready && (!prepared_job || prepared_job->kind != DCC_JOB_FRONTEND))
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
            cpp_ready || (prepared_job && prepared_job->kind == DCC_JOB_FRONTEND), &work)) != 0) {
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

    if (prepared_job && prepared_job->kind == DCC_JOB_FRONTEND) {
        if (!host->broker_managed || host->compr != DCC_COMPRESS_LZ4_STREAM) goto fallback;
        char *target = NULL;
        dcc_get_dotd_info(argv, &deps_fname, &needs_dotd, &sets_dotd_target, &target);
        ret = dcc_compile_direct(host, prepared_job, prepared_job->argv, NULL, -1, NULL, auxiliary,
                                output_fname, needs_dotd ? deps_fname : NULL,
                                &server_stderr, &server_stdout, status);
        goto remote_result;
    }

    if (!cpp_ready && host->cpp_where == DCC_CPP_ON_CLIENT && !dcc_is_preprocessed(input_fname)) {
        /* Lock the local CPU, since we're going to be doing preprocessing
         * or include scanning. */
        if ((ret = dcc_lock_local_cpp(&local_cpu_lock_fd)) != 0) {
            goto fallback;
        }
    }

    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        /* Perhaps it is not a good idea to preprocess on the server. */
        dcc_perhaps_adjust_cpp_where_and_protover(input_fname, host,
                                                  discrepancy_filename);
    }
    if (dcc_scan_includes) {
        ret = dcc_approximate_includes(host, argv);
        goto unlock_and_clean_up;
    }
    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        if (!files && (ret = dcc_talk_to_include_server(argv, &files, host->compr))) {
            /* Fallback to doing cpp locally */
            /* It's unfortunate that the variable that controls that is in the
             * "host" datastructure, even though in this case it's the client
             * that fails to support it,  but "host" is what gets passed
             * around in the client code. We are, in essence, throwing away
             * the host's capability to do cpp, so if this code was to execute
             * again (it won't, not in the same process) we wouldn't know if
             * the server supports it or not.
             */
            rs_log_warning("failed to get includes from include server, "
                           "preprocessing locally");
            if (dcc_getenv_bool("DISTCC_TESTING_INCLUDE_SERVER", 0))
                dcc_exit(ret);
            host->cpp_where = DCC_CPP_ON_CLIENT;
            dcc_get_protover_from_features(host->compr,
                                           host->cpp_where,
                                           &host->protover);
        } else if (local_cpu_lock_fd != -1) {
            /* Include server succeeded. */
            /* We're done with local "preprocessing" (include scanning). */
            dcc_unlock(local_cpu_lock_fd);
            /* Don't try to unlock again in dcc_compile_remote. */
            local_cpu_lock_fd = -1;
        }

    }

    if (host->cpp_where == DCC_CPP_ON_CLIENT) {
        if (preprocess_only) {
            ret = EXIT_LOCAL_CPP;
            goto fallback;
        }
        files = NULL;
        dcc_job_free(prepared_job);
        prepared_job = NULL;

        /* Include analysis may have fallen back after host selection. */
        if (!cpp_ready && local_cpu_lock_fd == -1 && !dcc_is_preprocessed(input_fname) &&
            (ret = dcc_lock_local_cpp(&local_cpu_lock_fd)))
            goto fallback;

        if (!cpp_ready && (ret = dcc_cpp_maybe(argv, input_fname, &cpp_fname, &cpp_pid, NULL)))
            goto fallback;

        /* localcpp_server_argv may already be processed from a previous bad host */
        if (localcpp_server_argv == NULL) {
            if ((ret = dcc_strip_local_args(argv, &localcpp_server_argv, input_fname)))
                goto fallback;
        }
        server_side_argv = localcpp_server_argv;
    } else {
        cpp_fname = NULL;
        cpp_pid = 0;
        /* remotecpp_server_argv may already be processed from a previous bad host */
        if (remotecpp_server_argv == NULL) {
            char *dotd_target = NULL;
            dcc_get_dotd_info(argv, &deps_fname, &needs_dotd,
                              &sets_dotd_target, &dotd_target);
            if ((ret = dcc_copy_argv(argv, &remotecpp_server_argv, 2)))
                goto fallback;
            if (needs_dotd && !sets_dotd_target && (!preprocess_only || dotd_target)) {
               dcc_argv_append(remotecpp_server_argv, strdup("-MT"));
               if (dotd_target == NULL)
                   dcc_argv_append(remotecpp_server_argv, strdup(output_fname));
               else
                   dcc_argv_append(remotecpp_server_argv, strdup(dotd_target));
            }
        }
        server_side_argv = remotecpp_server_argv;
    }
    char **wire_argv = dcc_remote_argv(server_side_argv);
    if (!wire_argv) { ret = EXIT_OUT_OF_MEMORY; goto unlock_and_clean_up; }
    ret = dcc_compile_remote(wire_argv,
                                  input_fname,
                                  cpp_fname,
                                  cpp_fd,
                                  files,
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
        rs_log_warning("failed to distribute, running locally instead");
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
        if (direct_lease >= 0) clock_gettime(CLOCK_MONOTONIC, &started);
        if (direct_lease >= 0 && cpp_ready && dcc_cpp_ir(argv, input_fname))
            ret = dcc_compile_local_ir(argv, input_fname, cpp_fname, cpp_fd);
        else
            ret = dcc_compile_local(argv, input_fname, NULL);
        if (direct_lease >= 0) {
            clock_gettime(CLOCK_MONOTONIC, &finished);
            dcc_broker_local_done(direct_lease, finished.tv_sec - started.tv_sec +
                                 (finished.tv_nsec - started.tv_nsec) / 1e9, ret);
            direct_lease = -1;
        }
    }
    if (remote_ret != 0) {
        if (remote_ret != ret && host->cpp_where == DCC_CPP_ON_SERVER) {
            /* Oops! it seems what we did remotely is not the same as what we did
              locally. We normally send email in such situations (if emailing is
              enabled), but we attempt an a time analysis of source files in order
              to avoid doing so in case source files we changed during the build.
            */
            (void) dcc_please_send_email_after_investigation(
                input_fname,
                deps_fname,
                discrepancy_filename);
        } else if (host) {
            /* A local-CPP diagnostic difference can result from lost macro
             * provenance; it does not implicate the include graph or worker.
             * Matching compiler failures likewise say nothing about health.
             */
            dcc_enjoyed_host(host);
        }
    }

  unlock_and_clean_up:
    if (cpu_lock_fd != -1) {
        dcc_unlock(cpu_lock_fd);
        cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }
    /* For the --scan_includes case. */
    if (local_cpu_lock_fd != -1) {
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1; /* Not really needed, just for consistency. */
    }

  clean_up:
    dcc_job_free(prepared_job);
    if (auxiliary) dcc_free_argv(auxiliary);
    if (cpp_fd >= 0) { close(cpp_fd); free(cpp_fname); }
    dcc_free_argv(argv);
    if (remotecpp_server_argv != NULL) {
        dcc_free_argv(remotecpp_server_argv);
    }
    if (localcpp_server_argv != NULL) {
        free(localcpp_server_argv);
    }
    free(discrepancy_filename);
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
