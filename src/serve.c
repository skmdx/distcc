/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool
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

                /* He who waits until circumstances completely favour *
                 * his undertaking will never accomplish anything.    *
                 *              -- Martin Luther                      */


/**
 * @file
 *
 * Actually serve remote requests.  Called from daemon.c.
 *
 * @todo Make sure wait statuses are packed in a consistent format
 * (exit<<8 | signal).  Is there any platform that doesn't do this?
 *
 * @todo The server should catch signals, and terminate the compiler process
 * group before handling them.
 *
 * @todo It might be nice to detect that the client has dropped the
 * connection, and then kill the compiler immediately.  However, we probably
 * won't notice that until we try to do IO.  SIGPIPE won't help because it's
 * not triggered until we try to do IO.  I don't think it matters a lot,
 * though, because the client's not very likely to do that.  The main case is
 * probably somebody getting bored and interrupting compilation.
 *
 * What might help is to select() on the network socket while we're waiting
 * for the child to complete, allowing SIGCHLD to interrupt the select() when
 * the child completes.  However I'm not sure if it's really worth the trouble
 * of doing that just to handle a fairly marginal case.
 **/



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <alloca.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "stats.h"
#include "rpc.h"
#include "exitcode.h"
#include "snprintf.h"
#include "dopt.h"
#include "bulk.h"
#include "exec.h"
#include "srvnet.h"
#include "netutil.h"
#include "hosts.h"
#include "daemon.h"
#include "stringmap.h"
#include "dotd.h"
#include "fix_debug_info.h"
#include "job.h"
#include "tcp_job.h"
#ifdef HAVE_GSSAPI
#include "auth.h"

/* Global security context in case confidentiality/integrity */
/* type services are needed in the future. */
extern gss_ctx_id_t distccd_ctx_handle;

/* Simple boolean, with a non-zero value indicating that the */
/* --auth option was specified. */
int dcc_auth_enabled = 0;
#endif

/**
 * We copy all serious distccd messages to this file, as well as sending the
 * compiler errors there, so they're visible to the client.
 **/
static int dcc_compile_log_fd = -1;

static int dcc_run_job(int in_fd, int out_fd, enum dcc_protover protover,
                       const struct dcc_job *job, const char *root,
                       struct dcc_job_result *result);

/* Default include search paths describe the client snapshots, not the worker
 * installation. Preserve their order after explicit -isystem arguments. */
static int add_system_includes(char ***argv, const char *root)
{
    char *directory = NULL, **args = NULL;
    DIR *dir = NULL;
    FILE *manifest = NULL;
    unsigned char header[8];
    char path[PATH_MAX];
    struct stat st;
    int ret = EXIT_PROTOCOL_ERROR;
    if (asprintf(&directory, "%s/" DCC_SYSTEM_HEADERS, root) < 0)
        return EXIT_OUT_OF_MEMORY;
    dir = opendir(directory);
    if (!dir) goto done;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (manifest) goto done;
        /* Same-host inputs may be links to sealed descriptors. */
        int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) goto done;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 16 * 1024 * 1024) {
            close(fd);
            goto done;
        }
        manifest = fdopen(fd, "rb");
        if (!manifest) { close(fd); goto done; }
        if (unlinkat(dirfd(dir), entry->d_name, 0)) goto done;
    }
    if (!manifest || fread(header, 1, 8, manifest) != 8 || memcmp(header, "DCSI", 4)) goto done;
    unsigned count = dcc_tcp_get32(header + 4);
    if (count > 4096) goto done;
    ret = dcc_copy_argv(*argv, &args, 4 * count + 1);
    if (ret) goto done;
    ret = EXIT_PROTOCOL_ERROR;
    char *option = strdup("-nostdinc");
    if (!option) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    dcc_argv_append(args, option);
    for (unsigned i = 0; i < count; ++i) {
        if (fread(header, 1, 8, manifest) != 8) goto done;
        unsigned kind = dcc_tcp_get32(header), length = dcc_tcp_get32(header + 4);
        if (kind > 5 || !length || length >= sizeof(path) ||
            fread(path, 1, length, manifest) != length ||
            (kind != 3 && kind != 4 && path[0] != '/') ||
            memchr(path, 0, length)) goto done;
        path[length] = 0;
        if (kind == 4) {
            if (strcmp(path, "gcc")) goto done;
            /* Resolve through the selected GCC's plugin directory. Only the
             * server administrator may override the module for development;
             * never load an executable path supplied by a client manifest. */
            const char *module = getenv("DISTCC_GCC_SCAN_LIBRARY");
            if (asprintf(&option, "-fplugin=%s", module ? module : "distcc_gcc_scan") < 0) {
                ret = EXIT_OUT_OF_MEMORY;
                goto done;
            }
            dcc_argv_append(args, option);
            if (asprintf(&option, "-fplugin-arg-distcc_gcc_scan-input-root=%s", root) < 0) {
                ret = EXIT_OUT_OF_MEMORY;
                goto done;
            }
            dcc_argv_append(args, option);
            continue;
        }
        if (asprintf(&option, "%s%s%s", kind == 0 || kind == 5 ? "-isystem" : "",
                     kind == 3 ? "" : root, path) < 0) {
            ret = EXIT_OUT_OF_MEMORY;
            goto done;
        }
        const char *prefix[] = {"-Xclang", kind == 1 ? "-internal-isystem" : "-internal-externc-isystem", "-Xclang"};
        for (unsigned j = 0; j < (kind == 3 ? 1 : kind == 1 || kind == 2 ? 3 : 0); ++j) {
            char *arg = strdup(kind == 3 ? "-include" : prefix[j]);
            if (!arg) { free(option); ret = EXIT_OUT_OF_MEMORY; goto done; }
            dcc_argv_append(args, arg);
        }
        dcc_argv_append(args, option);
        if (kind == 5) {
            if (asprintf(&option, "-fplugin-arg-distcc_gcc_scan-cxx-system=%s%s", root, path) < 0) {
                ret = EXIT_OUT_OF_MEMORY;
                goto done;
            }
            dcc_argv_append(args, option);
        }
    }
    if (fgetc(manifest) != EOF || ferror(manifest) || rmdir(directory)) goto done;
    dcc_free_argv(*argv);
    *argv = args;
    args = NULL;
    ret = 0;
done:
    if (args) dcc_free_argv(args);
    if (manifest) fclose(manifest);
    if (dir) closedir(dir);
    free(directory);
    return ret;
}

int dcc_execute_job(struct dcc_job *job, const char *root, int cancel_fd,
                     struct dcc_job_result *result)
{
    enum dcc_protover version;
    int ret;
    dcc_job_result_init(result);
    if (job->kind == DCC_JOB_LINK || dcc_job_tree(job)) version = DCC_VER_LINK;
    else dcc_get_protover_from_features(DCC_COMPRESS_LZ4_STREAM,
            job->kind == DCC_JOB_PUMP ? DCC_CPP_ON_SERVER : DCC_CPP_ON_CLIENT, &version);
    dcc_job_summary_clear();
    ret = dcc_run_job(cancel_fd, -1, version, job, root, result);
    dcc_job_summary();
    if (ret) dcc_job_result_free(result);
    return ret;
}


/**
 * Copy all server messages to the error file, so that they can be
 * echoed back to the client if necessary.
 **/
static int dcc_add_log_to_file(const char *err_fname)
{
    if (dcc_compile_log_fd != -1) {
        rs_log_crit("compile log already open?");
        return 0;               /* continue? */
    }

    dcc_compile_log_fd = open(err_fname, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (dcc_compile_log_fd == -1) {
        rs_log_error("failed to open %s: %s", err_fname, strerror(errno));
        return EXIT_IO_ERROR;
    }

    /* Only send fairly serious errors back */
    rs_add_logger(rs_logger_file, RS_LOG_WARNING, NULL, dcc_compile_log_fd);

    return 0;
}



static int dcc_remove_log_to_file(void)
{
    if (dcc_compile_log_fd == -1) {
        rs_log_warning("compile log not open?");
        return 0;               /* continue? */
    }

    /* must exactly match call in dcc_add_log_to_file */
    rs_remove_logger(rs_logger_file, RS_LOG_WARNING, NULL,
                     dcc_compile_log_fd);

    dcc_close(dcc_compile_log_fd);

    dcc_compile_log_fd = -1;

    return 0;
}



/* Read and execute a job to/from socket.  This is the common entry point no
 * matter what mode the daemon is running in: preforked, nonforked, or
 * ssh/inetd.
 */
int dcc_service_job(int in_fd,
                    int out_fd,
                    struct sockaddr *cli_addr,
                    int cli_len, int *session)
{
    int ret;
    enum dcc_protover protover;

    dcc_job_summary_clear();
    dcc_buffer_input(-1);

    /* dcc_readx/dcc_writex enforce their deadlines on EAGAIN. Accepted
     * sockets must not block inside read/write before those waits run. */
    dcc_set_nonblocking(in_fd);
    if (out_fd != in_fd) dcc_set_nonblocking(out_fd);

    /* Log client name and check access if appropriate.  For ssh connections
     * the client comes from a unix-domain socket and that's always
     * allowed. */
    if ((ret = dcc_check_client(cli_addr, cli_len, opt_allowed)) != 0)
        goto out;

#ifdef HAVE_GSSAPI
    /* If requested perform authentication. */
    if (dcc_auth_enabled) {
	    rs_log_info("Performing authentication.");

        if ((ret = dcc_gssapi_check_client(in_fd, out_fd)) != 0) {
            goto out;
        }
    } else {
	    rs_log_info("No authentication requested.");
    }

    /* Context deleted here as we no longer need it.  However, we have it available */
    /* in case we want to use confidentiality/integrity type services in the future. */
    if (dcc_auth_enabled) {
        dcc_gssapi_delete_ctx(&distccd_ctx_handle);
    }
#endif

    dcc_ignore_sigpipe(1);
    if ((ret = dcc_r_request_header(in_fd, &protover))) goto out;
    if (protover == DCC_VER_SESSION) {
        int supported = session && !*session;
        if (session && *session) return EXIT_PROTOCOL_ERROR;
#ifdef HAVE_GSSAPI
        supported &= !dcc_auth_enabled;
#endif
        ret = dcc_x_token_int(out_fd, "POOL", supported);
        if (!ret && supported) *session = 1;
        return ret;
    }
    if (protover >= DCC_VER_STREAM) dcc_buffer_input(in_fd);
    ret = dcc_run_job(in_fd, out_fd, protover, NULL, NULL, NULL);
    if (!ret && session && *session)
        ret = dcc_x_token_int(out_fd, "JEND", 0);
    tcp_cork_sock(out_fd, 0);

    dcc_job_summary();

out:
    dcc_buffer_input(-1);
    return ret;
}


static int dcc_input_tmpnam(char * orig_input,
                            char **tmpnam_ret)
{
    const char *input_exten;

    rs_trace("input file %s", orig_input);
    input_exten = dcc_find_extension(orig_input);
    if (input_exten)
        input_exten = dcc_preproc_exten(input_exten);
    if (!input_exten)           /* previous line might return NULL */
        input_exten = ".tmp";
    return dcc_make_tmpnam("distccd", input_exten, tmpnam_ret);
}



/**
 * Check argv0 against a list of allowed commands, and possibly map it to a new value.
 * If *compiler_name is changed, the original value is free'd, and a new value is malloc'd.
 *
 * If the environment variable DISTCC_CMDLIST is set,
 * load a list of supported commands from the file named by DISTCC_CMDLIST, and
 * refuse to serve any command whose last DISTCC_CMDLIST_NUMWORDS last words
 * don't match those of a command in that list.
 * Each line of the file is simply a filename.
 * This is chiefly useful for those few installations which have so many
 * compilers available such that the compiler must be specified with an absolute pathname.
 *
 * Example: if the compilers are installed in a different location on
 * this server, e.g. if they've been copied from a shared NFS directory onto a
 * local hard drive, you might have lines like
 *   /local/tools/blort/sh4-linux/gcc-3.3.3-glibc-2.2.5/bin/sh4-linux-gcc
 *   /local/tools/blort/sh4-linux/gcc-2.95.3-glibc-2.2.5/bin/sh4-linux-gcc
 * and set DISTCC_CMDLIST_NUMWORDS=3; that way e.g. any of the commands
 *   /local/tools/gcc-3.3.3-glibc-2.2.5/bin/sh4-linux-gcc
 *   /shared/tools/gcc-3.3.3-glibc-2.2.5/bin/sh4-linux-gcc
 *   /zounds/gcc-3.3.3-glibc-2.2.5/bin/sh4-linux-gcc
 * will invoke
 *   /local/tools/blort/sh4-linux/gcc-3.3.3-glibc-2.2.5/bin/sh4-linux-gcc
 *
 * Returns 0 (which will abort the compile) if compiler not in list.
 * (This is because the list is intended to be complete,
 * and any attempt to use a command not in the list indicates a confused user.
 * FIXME: should probably give user the option of changing this
 * behavior at runtime, so normal command lookup can continue even if command
 * not found in table.)
 **/
static int dcc_remap_compiler(char **compiler_name)
{
    static int cmdlist_checked=0;
    static stringmap_t *map=0;
    const char *newname;

    /* load file if not already */
    if (!cmdlist_checked) {
        char *filename;
        cmdlist_checked = 1;
        filename = getenv("DISTCC_CMDLIST");
        if (filename) {
            const char *nw = getenv("DISTCC_CMDLIST_NUMWORDS");
            int numFinalWordsToMatch=1;
            if (nw)
                numFinalWordsToMatch = atoi(nw);
            map = stringmap_load(filename, numFinalWordsToMatch);
            if (map) {
                rs_trace("stringmap_load(%s, %d) found %d commands", filename, numFinalWordsToMatch, map->n);
            } else {
                rs_log_error("stringmap_load(%s, %d) failed: %s", filename, numFinalWordsToMatch, strerror(errno));
                return EXIT_IO_ERROR;
            }
        }
    }

    if (!map)
        return 1;    /* no list of allowed names, so ok */

    /* Find what this compiler maps to */
    newname = stringmap_lookup(map, *compiler_name);
    if (!newname) {
        rs_log_warning("lookup of %s in DISTCC_CMDLIST failed", *compiler_name);
        return 0;    /* not in list, so forbidden.  FIXME: make failure an option */
    }

    /* If mapping is not the identity mapping, replace the original name */
    if (strcmp(newname, *compiler_name)) {
        rs_trace("changed compiler from %s to %s", *compiler_name, newname);
        free(*compiler_name);
        *compiler_name = strdup(newname);
    }
    return 1;
}


/**
 * Find the absolute path for the first occurrence of @p compiler_name on the
 * PATH.  Print a warning if it looks like a symlink to distcc.
 *
 * We want to guard against somebody accidentally running the server with a
 * masqueraded compiler on its $PATH.  The worst that's likely to happen here
 * is wasting some time running a distcc or ccache client that does nothing,
 * so it's not a big deal.  (This could be easy to do if it's on the default
 * PATH and they start the daemon from the command line.)
 *
 * At the moment we don't look for the compiler too.
 **/
static int dcc_check_compiler_masq(char *compiler_name)
{
    const char *envpath, *p, *n;
    char *buf = NULL;
    struct stat sb;
    int len;
    char linkbuf[MAXPATHLEN];

    if (compiler_name[0] == '/')
        return 0;

    if (!(envpath = getenv("PATH"))) {
        rs_trace("PATH seems not to be defined");
        return 0;
    }

    for (n = p = envpath; *n; p = n) {
        n = strchr(p, ':');
        if (n)
            len = n++ - p;
        else {
            len = strlen(p);
            n = p + len;
        }
        if (asprintf(&buf, "%.*s/%s", len, p, compiler_name) == -1) {
            rs_log_crit("asprintf failed");
            return EXIT_DISTCC_FAILED;
        }

        if (lstat(buf, &sb) == -1)
            continue;           /* ENOENT, EACCESS, etc */
        if (!S_ISLNK(sb.st_mode)) {
            rs_trace("%s is not a symlink", buf);
            break;              /* found it */
        }
        if ((len = readlink(buf, linkbuf, sizeof linkbuf)) <= 0)
            continue;
        linkbuf[len] = '\0';

        if (strstr(linkbuf, "distcc")) {
            rs_log_warning("%s on distccd's path is %s and really a link to %s",
                           compiler_name, buf, linkbuf);
            break;              /* but use it anyhow */
        } else {
            rs_trace("%s is a safe symlink to %s", buf, linkbuf);
            break;              /* found it */
        }
    }

    free(buf);
    return 0;
}

/**
 * Make sure there is a masquerade to distcc in LIBDIR/distcc in order to
 * execute a binary of the same name.
 *
 * Before this it was possible to execute arbitrary command after connecting
 * to distcc, which is quite a security risk when combined with any local root
 * privilege escalation exploit. See CVE 2004-2687
 *
 * https://nvd.nist.gov/vuln/detail/CVE-2004-2687
 * https://github.com/distcc/distcc/issues/155
 **/
static int dcc_check_compiler_whitelist(char *_compiler_name)
{
    char *compiler_name = _compiler_name;

    /* Support QtCreator by treating /usr/bin and /bin absolute paths as non-absolute
     * see https://github.com/distcc/distcc/issues/279
     */
    const char *creator_paths[] = { "/bin/", "/usr/bin/", NULL };
    int i;
    for (i = 0 ; creator_paths[i] ; ++i) {
        size_t len = strlen(creator_paths[i]);
        // /bin and /usr/bin are absolute paths (= compare from the string start)
        // use strncasecmp() to support case-insensitive / (= on Mac).
        if (strncasecmp(_compiler_name, creator_paths[i], len) == 0) {
            compiler_name = _compiler_name + len;
            // stop at the first hit
            break;
        }
    }

    if (strchr(compiler_name, '/')) {
        rs_log_crit("compiler name <%s> cannot be an absolute path (or must set DISTCC_CMDLIST or pass --enable-tcp-insecure)", _compiler_name);
        return EXIT_BAD_ARGUMENTS;
    }

#ifdef HAVE_FSTATAT
    int dirfd = open(LIBDIR "/distcc", O_RDONLY);

    if (dirfd < 0 || faccessat(dirfd, compiler_name, X_OK, 0) < 0) {
        char *compiler_path = NULL;
        if (asprintf(&compiler_path, "/usr/lib/distcc/%s", compiler_name) >= 0) {
            if (access(compiler_path, X_OK) < 0) {
                free(compiler_path);
                close(dirfd);
                rs_log_crit("%s not in %s or %s whitelist.", compiler_name, LIBDIR "/distcc", "/usr/lib/distcc");
                return EXIT_BAD_ARGUMENTS;           /* ENOENT, EACCESS, etc */
            }
            free(compiler_path);
        }
    }

    close(dirfd);

    rs_trace("%s in" LIBDIR "/distcc whitelist", compiler_name);
    return 0;
#else
    // make do with access():
    char *compiler_path;
    int ret = 0;
    if (asprintf(&compiler_path, "%s/distcc/%s", LIBDIR, compiler_name) >= 0) {
        if (access(compiler_path, X_OK) < 0) {
            free(compiler_path);
            /* check /usr/lib/distcc too */
            if (asprintf(&compiler_path, "/usr/lib/distcc/%s", compiler_name) >= 0) {
                if (access(compiler_path, X_OK) < 0) {
                    rs_log_crit("%s not in %s or %s whitelist.", compiler_name, LIBDIR "/distcc", "/usr/lib/distcc");
                    ret = EXIT_BAD_ARGUMENTS;           /* ENOENT, EACCESS, etc */
                }
                free(compiler_path);
            }
        } else {
            free(compiler_path);
	}
        rs_trace("%s in" LIBDIR "/distcc whitelist", compiler_name);
    } else {
        rs_log_crit("Couldn't check if %s is in %s whitelist.", compiler_name, LIBDIR "/distcc");
        ret = EXIT_DISTCC_FAILED;
    }
    return ret;
#endif
}

static const char *include_options[] = {
    "-I",
    "-include",
    "-imacros",
    "-idirafter",
    "-iprefix",
    "-iwithprefix",
    "-iwithprefixbefore",
    "-isystem",
    "-iquote",
    NULL
};


/**
 * Prepend @p root_dir string to source file if absolute.
 **/
static int tweak_input_argument_for_server(char **argv,
                                           const char *root_dir)
{
    unsigned i;
    /* Look for the source file and act if absolute. Note: dcc_scan_args
     * rejects compilations with more than one source file. */
    for (i=0; argv[i]; i++)
        if (dcc_is_source(argv[i]) && argv[i][0]=='/') {
            unsigned j = 0;
            char *prefixed_name;
            while (argv[i][j] == '/') j++;
            if (asprintf(&prefixed_name, "%s/%s",
                         root_dir,
                         argv[i] + j) == -1) {
                rs_log_crit("asprintf failed");
                return EXIT_OUT_OF_MEMORY;
            }
            rs_trace("changed input from \"%s\" to \"%s\"", argv[i],
                     prefixed_name);
            free(argv[i]);
            argv[i] = prefixed_name;
            dcc_trace_argv("command after", argv);
            return 0;
        }
    return 0;
}


static int pack_include_arguments(char **argv, const unsigned *indices, unsigned count)
{
    char path[] = ".distcc-includes-XXXXXX", *option = NULL;
    FILE *file;
    unsigned i, read, write, next;
    int fd, error;
    if (!count) return 0;
    /* The current directory is inside this job's private input tree. Its
     * removal owns this file too. A relative name also avoids commas in
     * TMPDIR being interpreted as -Wp argument separators. */
    fd = mkstemp(path);
    if (fd < 0) return EXIT_IO_ERROR;
    file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(path); return EXIT_IO_ERROR; }
    for (i = 0; i < count; ++i) {
        fputc('"', file);
        for (const char *p = argv[indices[i]]; *p; ++p) {
            if (*p == '\\' || *p == '"') fputc('\\', file);
            fputc(*p, file);
        }
        fputs("\"\n", file);
    }
    error = ferror(file);
    if (fclose(file)) error = 1;
    if (error) { unlink(path); return EXIT_IO_ERROR; }
    /* A driver @file is expanded before GCC builds COLLECT_GCC_OPTIONS,
     * which still exceeds Linux's per-string limit. Let cc1 expand only
     * the ordered -I options, leaving driver actions and other flags intact. */
    if (asprintf(&option, "-Wp,@%s", path) < 0) {
        unlink(path);
        return EXIT_OUT_OF_MEMORY;
    }
    for (read = write = next = 0; argv[read]; ++read) {
        if (next < count && read == indices[next]) {
            free(argv[read]);
            if (!next) argv[write++] = option;
            ++next;
        } else argv[write++] = argv[read];
    }
    argv[write] = NULL;
    return 0;
}

/** Relocate include paths and pack bulky GCC search lists for cc1. */
static int tweak_include_arguments_for_server(char **argv,
                                              const char *root_dir)
{
    int index_of_first_filename_char = 0;
    const char *include_option;
    unsigned int i, j, count = 0, *indices = NULL;
    int ret = 0;
    size_t bytes = 0, prefix = strlen(root_dir);
    for (i = 0; argv[i]; ++i) bytes += strlen(argv[i]) + prefix + 3;
    if (bytes >= 64 * 1024 && !strstr(dcc_find_basename(argv[0]), "clang")) {
        indices = malloc(i * sizeof(*indices));
        if (!indices) return EXIT_OUT_OF_MEMORY;
    }
    for (i = 0; argv[i]; ++i) {
        for (j = 0; include_options[j]; ++j) {
            if (str_startswith(include_options[j], argv[i])) {
                int pack = indices && !strcmp(include_options[j], "-I");
                if (pack) indices[count++] = i;
                if (strcmp(argv[i], include_options[j]) == 0) {
                    /* "-I foo" , change the next argument */
                    ++i;
                    if (pack && argv[i]) indices[count++] = i;
                    include_option = "";
                    index_of_first_filename_char = 0;
                } else {
                    /* "-Ifoo", change this argument */
                    include_option = include_options[j];
                    index_of_first_filename_char = strlen(include_option);
                }
                if (argv[i] != NULL) {  /* in case of a dangling -I */
                    if (argv[i][index_of_first_filename_char] == '/') {
                        char *buf;
                        checked_asprintf(&buf, "%s%s%s",
                                 include_option,
                                 root_dir,
                                 argv[i] + index_of_first_filename_char);
                        if (buf == NULL) {
                            ret = EXIT_OUT_OF_MEMORY;
                            goto done;
                        }
                        free(argv[i]);
                        argv[i] = buf;
                    }
                }
                break;  /* from the inner loop; go look at the next argument */
            }
        }
        if (!argv[i]) break;
    }
    ret = pack_include_arguments(argv, indices, count);
done:
    free(indices);
    return ret;
}

/* The -MT command line flag does not work as advertised for distcc:
 * it augments, rather than replace, the list of targets in the dotd file.
 * The behavior we want though, is the replacing behavior.
 * So here we delete the "-MT target" arguments, and we return the target,
 * for use in the .d rewriting in dotd.c.
 */
static int dcc_convert_mt_to_dotd_target(char **argv, char **dotd_target)
{
    int i;
    *dotd_target = NULL;

    for (i = 0; argv[i]; ++i) {
        if (strcmp(argv[i], "-MT") == 0) {
            break;
        }
    }

    /* if we reached the end without finding -MT, fine. */
    if (argv[i] == NULL)
        return 0;

    /* if we find -MT but only at the very end, that's an error. */
    if (argv[i+1] == NULL) {
        rs_trace("found -MT at the end of the command line");
        return 1;
    }

    /* the dotd_target is the argument of -MT */
    *dotd_target = argv[i+1];

    /* copy the next-next argument on top of this. */
    for (; argv[i+2]; ++i) {
        argv[i] = argv[i+2];
    }

    /* and then put the terminal null in. */
    argv[i] = argv[i+2];

    return 0;
}


/**
 * Add -MMD and -MF to get a .d file.
 * Find what the dotd target should be (if any).
 * Prepend @p root_dir to every command
 * line argument that refers to a file/dir by an absolute name.
 **/
static int tweak_arguments_for_server(char **argv,
                                      const char *root_dir,
                                      const char *deps_fname,
                                      char **dotd_target,
                                      char ***tweaked_argv)
{
    int ret;
    char *debug_map, *macro_map;
    int argc = dcc_argv_len(argv), i, client_maps = 0;
    int preserve_spelling = strstr(dcc_find_basename(argv[0]), "clang") != NULL;
    for (i = 1; argv[i]; i++)
        if (str_startswith("-fdebug-prefix-map=/", argv[i]) ||
            str_startswith("-fmacro-prefix-map=/", argv[i]) ||
            str_startswith("-ffile-prefix-map=/", argv[i]))
            client_maps++;
    for (i = 1; preserve_spelling && argv[i]; i++)
        if (str_startswith("-ffile-prefix-map=", argv[i]) ||
            str_startswith("-fmacro-prefix-map=", argv[i]) ||
            !strcmp(argv[i], "-ffile-reproducible") ||
            !strcmp(argv[i], "-fno-file-reproducible"))
            preserve_spelling = 0;
    *dotd_target = 0;
    if ((ret = dcc_copy_argv(argv, tweaked_argv, 5 + preserve_spelling + client_maps)))
      return 1;

    /* Remap absolute input names and DWARF in the compiler, preserving relative
     * __FILE__/__BASE_FILE__ spellings and user mappings. A #line prefix cannot
     * represent the same header reached through different include spellings. */
    /* GCC gives file-prefix maps priority over explicit macro-prefix maps.
     * Use separate defaults so the user's mappings can override each class. */
    checked_asprintf(&debug_map, "-fdebug-prefix-map=%s=", root_dir);
    if (!debug_map) return EXIT_OUT_OF_MEMORY;
    checked_asprintf(&macro_map, "-fmacro-prefix-map=%s=", root_dir);
    if (!macro_map) { free(debug_map); return EXIT_OUT_OF_MEMORY; }
    memmove(*tweaked_argv + 3 + preserve_spelling, *tweaked_argv + 1, argc * sizeof(char *));
    (*tweaked_argv)[1] = debug_map;
    (*tweaked_argv)[2] = macro_map;
    /* Clang's prefix map implicitly normalizes './' even for unmatched paths.
     * Restore its default only when the user did not request that behavior. */
    if (preserve_spelling &&
        !((*tweaked_argv)[3] = strdup("-fno-file-reproducible")))
        return EXIT_OUT_OF_MEMORY;

    /* Preserve user mappings after relocating absolute input paths. Keep the
     * user's order and let their mappings take precedence over our default. */
    for (i = 3 + preserve_spelling; i < argc + 2 + preserve_spelling; ++i) {
        char *arg = (*tweaked_argv)[i];
        if (str_startswith("-fdebug-prefix-map=/", arg) ||
            str_startswith("-fmacro-prefix-map=/", arg) ||
            str_startswith("-ffile-prefix-map=/", arg)) {
            char *value = strchr(arg, '=') + 1;
            char *mapped;
            if (!strchr(value, '=')) continue;
            checked_asprintf(&mapped, "%.*s%s%s", (int)(value - arg), arg, root_dir, value);
            if (!mapped) return EXIT_OUT_OF_MEMORY;
            /* Native frontends also expose original client names, including
             * absolute include operands. Preserve mappings for those names
             * as well as the relocated worker paths. */
            dcc_argv_append(*tweaked_argv, arg);
            (*tweaked_argv)[i] = mapped;
        }
    }

    if (!dcc_argv_search(argv, "-E") &&
        (ret = dcc_convert_mt_to_dotd_target(*tweaked_argv, dotd_target)))
      return 1;

    if (!dcc_argv_search(*tweaked_argv, "-MD") && !dcc_argv_search(*tweaked_argv, "-MMD")) {
        dcc_argv_append(*tweaked_argv, strdup("-MMD"));
    }
    dcc_argv_append(*tweaked_argv, strdup("-MF"));
    dcc_argv_append(*tweaked_argv, strdup(deps_fname));

    if ((ret = tweak_include_arguments_for_server(*tweaked_argv, root_dir)))
        return ret;
    return tweak_input_argument_for_server(*tweaked_argv, root_dir);
}


/**
 * Read the client working directory from in_fd socket,
 * and set up the server side directory corresponding to that.
 * Inputs:
 *   @p in_fd: the file descriptor for the socket.
 * Outputs:
 *   @p temp_dir: a temporary directory on the server,
 *                corresponding to the client's root directory (/),
 *   @p client_side_cwd: the current directory on the client
 *   @p server_side_cwd: the corresponding directory on the server;
 *                server_side_cwd = temp_dir + client_side_cwd
 **/
static int make_temp_dir_and_chdir_for_cpp(int in_fd,
        char **temp_dir, char **client_side_cwd, char **server_side_cwd)
{

        int ret = 0;

        if ((ret = dcc_get_new_tmpdir(temp_dir)))
            return ret;
        /* Compilers canonicalize their working directory in DWARF. Keep the
         * remapping prefix in the same form when TMPDIR contains symlinks. */
        char *canonical = realpath(*temp_dir, NULL);
        if (!canonical) return EXIT_IO_ERROR;
        free(*temp_dir);
        *temp_dir = canonical;
        if ((ret = dcc_r_cwd(in_fd, client_side_cwd)))
            return ret;

        checked_asprintf(server_side_cwd, "%s%s", *temp_dir, *client_side_cwd);
        if (*server_side_cwd == NULL) {
            ret = EXIT_OUT_OF_MEMORY;
        } else if ((ret = dcc_mk_tmp_ancestor_dirs(*server_side_cwd))) {
            ; /* leave ret the way it is */
        } else if ((ret = dcc_mk_tmpdir(*server_side_cwd))) {
            ; /* leave ret the way it is */
        } else if (chdir(*server_side_cwd) == -1) {
            ret = EXIT_IO_ERROR;
        }
        return ret;
}


/* Open before cleanup, retaining the inode rather than its temporary name. */
static int retain_artifact(struct dcc_job_result *result, unsigned index, const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return EXIT_IO_ERROR;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return EXIT_IO_ERROR;
    }
    result->fd[index] = fd;
    result->size[index] = st.st_size;
    return 0;
}

/* Execute an isolated file-tree backend or a sysroot-aware whole link. */
static int dcc_run_helper_job(int in_fd, int out_fd,
                            const char *out_fname, const char *err_fname, int *status,
                            enum dcc_protover protover, const char *input_archive,
                            struct dcc_job_result *result, const char *helper)
{
    char *archive = NULL, *output = NULL;
    char *argv[5];
    struct stat st;
    pid_t pid;
    int ret;
    enum dcc_compress compr = protover == DCC_VER_STREAM_LINK ? DCC_COMPRESS_LZ4_STREAM : DCC_COMPRESS_NONE;
    if (!strcmp(helper, "distcc-link") && !getenv("DISTCCD_LINK_ROOT")) {
        rs_log_error("remote linking requires DISTCCD_LINK_ROOT");
        return EXIT_PROTOCOL_ERROR;
    }
    if (input_archive) {
        if (!(archive = strdup(input_archive))) return EXIT_OUT_OF_MEMORY;
    } else if ((ret = dcc_make_tmpnam("distcc-link", ".tar", &archive)) ||
               (ret = dcc_r_token_file(in_fd, "LINK", archive, compr)))
        goto done;
    if ((ret = dcc_make_tmpnam("distcc-link", ".out", &output)))
        goto done;
    argv[0] = (char *)helper;
    argv[1] = (char *)"--worker";
    argv[2] = archive;
    argv[3] = output;
    argv[4] = NULL;
    if ((ret = dcc_spawn_child(argv, &pid, "/dev/null", out_fname, err_fname)) ||
        (ret = dcc_collect_child(helper, pid, status, in_fd,
                                 result ? &result->peak_bytes : NULL)))
        *status = W_EXITCODE(ret, 0);
    if (!*status && stat(output, &st))
        *status = W_EXITCODE(EXIT_IO_ERROR, 0);
    if (result) {
        result->status = *status;
        result->mode = *status ? 0 : st.st_mode & 0777;
        if ((ret = retain_artifact(result, DCC_JOB_STDERR, err_fname)) ||
            (ret = retain_artifact(result, DCC_JOB_STDOUT, out_fname))) goto done;
        if (!*status) ret = retain_artifact(result, DCC_JOB_OBJECT, output);
        goto done;
    }
    if ((ret = dcc_x_result_header(out_fd, protover)) ||
        (ret = dcc_x_cc_status(out_fd, *status)) ||
        (ret = dcc_x_file(out_fd, err_fname, "SERR", compr, NULL)) ||
        (ret = dcc_x_file(out_fd, out_fname, "SOUT", compr, NULL)))
        goto done;
    if (!*status) {
        if (!(ret = dcc_x_token_int(out_fd, "MODE", st.st_mode & 0777)))
            ret = dcc_x_file(out_fd, output, "DOTO", compr, NULL);
    } else {
        if (!(ret = dcc_x_token_int(out_fd, "MODE", 0)))
            ret = dcc_x_token_int(out_fd, "DOTO", 0);
    }
done:
    free(archive);
    free(output);
    return ret;
}

/* Preprocessor line markers and expanded __FILE__ strings must name client
 * paths, not the private directory used to assemble the remote input tree. */
static int remap_preprocessed(const char *file, const char *root)
{
    char *replacement = NULL, *line = NULL;
    size_t capacity = 0, root_len = strlen(root);
    FILE *input = NULL, *output = NULL;
    int ret = EXIT_IO_ERROR;
    if (dcc_make_tmpnam("distccd", ".i", &replacement))
        goto done;
    if (!(input = fopen(file, "r")) || !(output = fopen(replacement, "w")))
        goto done;
    while (getline(&line, &capacity, input) >= 0) {
        char *from = line, *match;
        while ((match = strstr(from, root))) {
            if (fwrite(from, 1, match - from, output) != (size_t)(match - from))
                goto done;
            from = match + root_len;
        }
        if (fputs(from, output) == EOF) goto done;
    }
    if (ferror(input) || fflush(output)) goto done;
    if (rename(replacement, file)) goto done;
    ret = 0;
done:
    if (input) fclose(input);
    if (output && fclose(output)) ret = EXIT_IO_ERROR;
    free(line);
    free(replacement);
    return ret;
}

/** Read a request, run the compiler, and send a response. */
static int dcc_run_job(int in_fd,
                       int out_fd, enum dcc_protover protover,
                       const struct dcc_job *job, const char *root,
                       struct dcc_job_result *result)
{
    char **argv = NULL;
    char **tweaked_argv = NULL;
    int status = 0;
    char *temp_i = NULL, *temp_o = NULL;
    char *err_fname = NULL, *out_fname = NULL, *deps_fname = NULL;
    char *temp_dir = NULL; /* for receiving multiple files */
    int ret = 0, compile_ret = 0;
    char *orig_input = NULL, *orig_output = NULL;
    char *orig_input_tmp, *orig_output_tmp;
    char *dotd_target = NULL;
    pid_t cc_pid;
    enum dcc_compress compr;
    struct timeval start, end;
    int time_ms;
    char *time_str;
    int job_result = -1;
    enum dcc_cpp_where cpp_where;
    char *server_cwd = NULL;
    char *client_cwd = NULL;
    int changed_directory = 0;
    int preprocess_only = 0;
    int stdout_output = 0;

    gettimeofday(&start, NULL);

    if ((ret = dcc_make_tmpnam("distcc", ".deps", &deps_fname)))
        goto out_cleanup;
    if ((ret = dcc_make_tmpnam("distcc", ".stderr", &err_fname)))
        goto out_cleanup;
    if ((ret = dcc_make_tmpnam("distcc", ".stdout", &out_fname)))
        goto out_cleanup;

    /* Keep the exclusive name reservations until this job's cleanup. The
     * compiler and log redirection reopen these files. Unlinking them here
     * lets another worker claim the same name and later remove our result. */

    /* Capture any messages relating to this compilation to the same file as
     * compiler errors so that they can all be sent back to the client. */
    dcc_add_log_to_file(err_fname);

    /* Ignore SIGPIPE; we consistently check error codes and will see the
     * EPIPE.  Note that it is set back to the default behaviour when spawning
     * a child, to handle cases like the assembler dying while its being fed
     * from the compiler */
    dcc_ignore_sigpipe(1);

    /* Allow output to accumulate into big packets. */
    if (!job) tcp_cork_sock(out_fd, 1);

    if (protover == DCC_VER_LINK || protover == DCC_VER_STREAM_LINK) {
        int tree = job && dcc_job_tree(job);
        const char *helper = !tree ? "distcc-link" :
            job->kind == DCC_JOB_RUST ? "distcc-rust" : "distcc-thinlto";
        argv = calloc(2, sizeof(*argv));
        orig_input = strdup(helper);
        if (!argv || !(argv[0] = strdup(helper)) || !orig_input) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out_cleanup;
        }
        char *input_archive = NULL;
        if (tree && !(input_archive = strdup(root))) {
            ret = EXIT_OUT_OF_MEMORY;
            goto out_cleanup;
        }
        if (job && !tree && (job->count != 1 ||
            asprintf(&input_archive, "%s%s", root, job->inputs[0].path) < 0)) {
            ret = EXIT_PROTOCOL_ERROR;
            goto out_cleanup;
        }
        ret = dcc_run_helper_job(in_fd, out_fd, out_fname, err_fname, &status,
                              protover, input_archive, result, helper);
        free(input_archive);
        job_result = ret || status ? STATS_COMPILE_ERROR : STATS_COMPILE_OK;
        goto out_cleanup;
    }

    dcc_get_features_from_protover(protover, &compr, &cpp_where);

    if (cpp_where == DCC_CPP_ON_SERVER) {
        if (job) {
            temp_dir = strdup(root);
            client_cwd = strdup(job->cwd);
            if (!temp_dir || !client_cwd ||
                asprintf(&server_cwd, "%s%s", root, job->cwd) < 0) {
                ret = EXIT_OUT_OF_MEMORY;
                goto out_cleanup;
            }
            if ((ret = dcc_mk_tmp_ancestor_dirs(server_cwd)) ||
                (ret = dcc_mk_tmpdir(server_cwd))) goto out_cleanup;
            if (chdir(server_cwd)) { ret = EXIT_IO_ERROR; goto out_cleanup; }
        } else if ((ret = make_temp_dir_and_chdir_for_cpp(in_fd,
                          &temp_dir, &client_cwd, &server_cwd)))
            goto out_cleanup;
        changed_directory = 1;
    }

    if ((ret = job ? dcc_copy_argv(job->argv, &argv, 0) :
                    dcc_r_argv(in_fd, "ARGC", "ARGV", &argv))
        || (ret = dcc_scan_args(argv, &orig_input_tmp, &orig_output_tmp,
                                &tweaked_argv, 0)))
        goto out_cleanup;

    /* The orig_input_tmp and orig_output_tmp values returned by dcc_scan_args()
     * are aliased with some element of tweaked_argv.  We need to copy them,
     * because the calls to dcc_set_input() and dcc_set_output() below will
     * free those elements. */
    orig_input = strdup(orig_input_tmp);
    orig_output = strdup(orig_output_tmp);
    preprocess_only = dcc_argv_search(argv, "-E");
    stdout_output = preprocess_only && !strcmp(orig_output_tmp, "-");
    if (orig_input == NULL || orig_output == NULL) {
      ret = EXIT_OUT_OF_MEMORY;
      goto out_cleanup;
    }

    /* Our new argv is what dcc_scan_args put into tweaked_argv */
    /* Put tweaked_argv into argv, and free old argv */
    dcc_free_argv(argv);
    argv = tweaked_argv;
    tweaked_argv = NULL;

    rs_trace("output file %s", orig_output);
    if ((ret = dcc_make_tmpnam("distccd", ".o", &temp_o)))
        goto out_cleanup;

    /* if the protocol is multi-file, then we need to do the following
     * in a loop.
     */
    if (cpp_where == DCC_CPP_ON_SERVER) {
        if ((!job && (ret = dcc_r_many_files(in_fd, temp_dir, compr)))
            || (!stdout_output && (ret = dcc_set_output(argv, temp_o)))
            || (ret = tweak_arguments_for_server(argv, temp_dir, deps_fname,
                                                &dotd_target, &tweaked_argv)))
            goto out_cleanup;
        /* Repeat the switcharoo trick a few lines above. */
        dcc_free_argv(argv);
        argv = tweaked_argv;
        tweaked_argv = NULL;
    } else {
        if (job) {
            if (!job->count || asprintf(&temp_i, "%s%s", root, job->inputs[0].path) < 0) {
                ret = EXIT_PROTOCOL_ERROR;
                goto out_cleanup;
            }
        } else if (dcc_argv_search(argv, "-x")) {
            /* Rewritten Clang input uses an explicit language. Its basename
             * is observable in the object even without debug information. */
            if ((ret = dcc_get_new_tmpdir(&temp_dir))) goto out_cleanup;
            if (asprintf(&temp_i, "%s/%s", temp_dir, dcc_find_basename(orig_input)) < 0) {
                ret = EXIT_OUT_OF_MEMORY;
                goto out_cleanup;
            }
            if ((ret = dcc_add_cleanup(temp_i))) goto out_cleanup;
        } else if ((ret = dcc_input_tmpnam(orig_input, &temp_i))) goto out_cleanup;
        if ((!job && (ret = dcc_r_token_file(in_fd, "DOTI", temp_i, compr)))
            || (ret = dcc_set_input(argv, temp_i))
            || (ret = dcc_set_output(argv, temp_o)))
            goto out_cleanup;
    }

    if (!dcc_remap_compiler(&argv[0])) {
        ret = EXIT_BAD_ARGUMENTS;
        goto out_cleanup;
    }

    if ((ret = dcc_check_compiler_masq(argv[0])))
        goto out_cleanup;

    if (!opt_enable_tcp_insecure &&
        !getenv("DISTCC_CMDLIST") &&
        (ret = dcc_check_compiler_whitelist(argv[0])))
        goto out_cleanup;

    /* unsafe compiler options. See  https://youtu.be/bSkpMdDe4g4?t=53m12s
       on securing https://godbolt.org/ */
    char *a;
    int i;
    for (i = 0; (a = argv[i]); i++) {
        if (strncmp(a, "-fplugin=", strlen("-fplugin=")) == 0) {
            rs_log_warning("-fplugin= passed, which are insecure and not supported.");
            ret = EXIT_BAD_ARGUMENTS;
            goto out_cleanup;
        }
        if (strncmp(a, "-specs=", strlen("-specs=")) == 0) {
            int fail = 1;
            if (arg_sysroot) {
                char *spec_file = strchr(a, '=') + 1;
                char *spec_path = alloca(strlen(spec_file) + strlen(arg_sysroot) + 8);
                sprintf(spec_path, "%s/%s", arg_sysroot, spec_file);
                struct stat spec_stat;
                if (stat(spec_path, &spec_stat) != -1 && (spec_stat.st_mode & S_IFMT) == S_IFREG) {
                  fail = 0;
                }
            }
            if (fail) {
              rs_log_warning("-specs= passed, but we cannot find the specs.");
              ret = EXIT_BAD_ARGUMENTS;
              goto out_cleanup;
            }
       }
    }

    if (cpp_where == DCC_CPP_ON_SERVER &&
        (ret = add_system_includes(&argv, temp_dir))) {
        if (ret != EXIT_PROTOCOL_ERROR) goto out_cleanup;
        /* The request arrived intact but its metadata is invalid. Return a
         * completed error instead of making the client quarantine a healthy
         * worker and wait for network recovery. */
        rs_log_error("invalid system include manifest");
        status = W_EXITCODE(ret, 0);
        ret = 0;
        goto report_result;
    }

    /* Pump inputs need Clang's rooted filesystem for absolute includes and
     * __has_include probes. The frontend also preserves source identity for
     * LLVM IR PGO names, which prefix-map options do not affect. */
    if (dcc_offload_language(argv)) {
        for (i = 1; argv[i]; ++i) {
            if (root && !strncmp(argv[i], "--rocm-device-lib-path=", 23)) {
                char *path;
                if (asprintf(&path, "--rocm-device-lib-path=%s%s", root, argv[i] + 23) < 0) {
                    ret = EXIT_OUT_OF_MEMORY;
                    goto out_cleanup;
                }
                free(argv[i]);
                argv[i] = path;
            }
        }
    } else if (strstr(dcc_find_basename(argv[0]), "clang")) {
        int frontend = cpp_where == DCC_CPP_ON_SERVER || (job && job->count > 1);
        int argc = dcc_argv_len(argv);
        for (i = 1; i < argc; ++i)
            if (!strcmp(argv[i], "-fprofile-generate") ||
                !strncmp(argv[i], "-fprofile-generate=", 19)) frontend = 1;
        if (frontend) {
            const char *helper = getenv("DISTCC_CLANG_DRIVER");
            char **wrapped = calloc(argc + 6, sizeof(char *));
            if (!wrapped) { ret = EXIT_OUT_OF_MEMORY; goto out_cleanup; }
            wrapped[0] = strdup(helper && *helper ? helper : "distcc-clang");
            wrapped[1] = strdup(job ? root : cpp_where == DCC_CPP_ON_SERVER ? temp_dir : "");
            wrapped[2] = strdup(orig_input);
            wrapped[3] = strdup(job ? job->cwd : client_cwd ? client_cwd : "");
            wrapped[4] = strdup(cpp_where == DCC_CPP_ON_SERVER ? "pump" : "preprocessed");
            if (!wrapped[0] || !wrapped[1] || !wrapped[2] || !wrapped[3] || !wrapped[4]) {
                for (int k = 0; k < 5; ++k) free(wrapped[k]);
                free(wrapped);
                ret = EXIT_OUT_OF_MEMORY;
                goto out_cleanup;
            }
            memcpy(wrapped + 5, argv, (argc + 1) * sizeof(char *));
            free(argv);
            argv = wrapped;
        }
    }

    if ((compile_ret = dcc_spawn_child(argv, &cc_pid,
                                       "/dev/null", out_fname, err_fname))
        || (compile_ret = dcc_collect_child("cc", cc_pid, &status, in_fd,
                                          result ? &result->peak_bytes : NULL))) {
        /* We didn't get around to finding a wait status from the actual
         * compiler */
        status = W_EXITCODE(compile_ret, 0);
    }

report_result:
    if (preprocess_only && cpp_where == DCC_CPP_ON_SERVER) {
        if (((stdout_output || !status) &&
             (ret = remap_preprocessed(stdout_output ? out_fname : temp_o, temp_dir))) ||
            (ret = remap_preprocessed(err_fname, temp_dir)))
            goto out_cleanup;
    }

    if (result) {
        result->status = status;
        if ((ret = retain_artifact(result, DCC_JOB_STDERR, err_fname)) ||
            (ret = retain_artifact(result, DCC_JOB_STDOUT, out_fname))) goto out_cleanup;
        if (!status) {
            if (cpp_where == DCC_CPP_ON_SERVER && !preprocess_only &&
                (ret = dcc_fix_debug_info(temp_o, "/", temp_dir))) goto out_cleanup;
            if ((ret = retain_artifact(result, DCC_JOB_OBJECT, temp_o))) goto out_cleanup;
            if (cpp_where == DCC_CPP_ON_SERVER) {
                char *cleaned = NULL;
                ret = dcc_cleanup_dotd(deps_fname, &cleaned, temp_dir,
                                      dotd_target ? dotd_target : orig_output, temp_o);
                if (!ret) ret = retain_artifact(result, DCC_JOB_DEPS, cleaned);
                free(cleaned);
                if (ret) goto out_cleanup;
            }
        }
        job_result = status ? STATS_COMPILE_ERROR : STATS_COMPILE_OK;
    } else if ((ret = dcc_x_result_header(out_fd, protover))
        || (ret = dcc_x_cc_status(out_fd, status))
        || (ret = dcc_x_file(out_fd, err_fname, "SERR", compr, NULL))
        || (ret = dcc_x_file(out_fd, out_fname, "SOUT", compr, NULL))) {
          /* We get a protocol derailment if we send DOTO 0 here */

        if (job_result == -1)
            job_result = STATS_COMPILE_ERROR;
    } else if (WIFSIGNALED(status) || WEXITSTATUS(status)) {
        /* Something went wrong, so send DOTO 0 */
        ret = dcc_x_token_int(out_fd, "DOTO", 0);

        if (job_result == -1)
            job_result = STATS_COMPILE_ERROR;
    } else {
        if (cpp_where == DCC_CPP_ON_SERVER && !preprocess_only) {
          rs_trace("fixing up debug info");
          /*
           * We update the debugging information, replacing all occurrences
           * of temp_dir (the server temp directory that corresponds to the
           * client's root directory) with "/", to convert server path
           * names to client path names.  This is safe to do only because
           * temp_dir is of the form "/var/tmp/distccd-XXXXXX" where XXXXXX
           * is randomly chosen by mkdtemp(), which makes it inconceivably
           * unlikely that this pattern could occur in the debug info by
           * chance.
           */
          if ((ret = dcc_fix_debug_info(temp_o, "/", temp_dir)))
            goto out_cleanup;
        }
        if ((ret = dcc_x_file(out_fd, temp_o, "DOTO", compr, NULL)))
            goto out_cleanup;

        if (cpp_where == DCC_CPP_ON_SERVER) {
            char *cleaned_dotd;
            ret = dcc_cleanup_dotd(deps_fname,
                                   &cleaned_dotd,
                                   temp_dir,
                                   dotd_target ? dotd_target : orig_output,
                                   temp_o);
            if (ret) goto out_cleanup;
            ret = dcc_x_file(out_fd, cleaned_dotd, "DOTD", compr, NULL);
            free(cleaned_dotd);
        }

        job_result = STATS_COMPILE_OK;
    }

    if (compile_ret == EXIT_IO_ERROR) {
        job_result = STATS_CLI_DISCONN;
    } else if (compile_ret == EXIT_TIMEOUT) {
        job_result = STATS_COMPILE_TIMEOUT;
    }

    dcc_critique_status(status, argv[0], orig_input, dcc_hostdef_local,
                        0);
    rs_log(RS_LOG_INFO|RS_LOG_NONAME, "job complete");

out_cleanup:

    /* Restore the working directory, if needed. */
    if (changed_directory) {
      if (chdir(dcc_daemon_wd) != 0) {
        rs_log_warning("chdir(%s) failed: %s", dcc_daemon_wd, strerror(errno));
        ret = EXIT_IO_ERROR;
      }
    }

    switch (ret) {
    case EXIT_BUSY: /* overloaded */
        job_result = STATS_REJ_OVERLOAD;
        break;
    case EXIT_IO_ERROR: /* probably client disconnected */
        job_result = STATS_CLI_DISCONN;
        break;
    case EXIT_PROTOCOL_ERROR:
        job_result = STATS_REJ_BAD_REQ;
        break;
    default:
        if (job_result != STATS_COMPILE_ERROR
            && job_result != STATS_COMPILE_OK
        && job_result != STATS_CLI_DISCONN
        && job_result != STATS_COMPILE_TIMEOUT) {
            job_result = STATS_OTHER;
        }
    }

    gettimeofday(&end, NULL);
    time_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;

    dcc_job_summary_append(" ");
    dcc_job_summary_append(stats_text[job_result]);

    if (job_result == STATS_COMPILE_OK) {
        /* special case, also log compiler, file and time */
        dcc_stats_compile_ok(argv[0], orig_input, start, end, time_ms);
    } else {
        dcc_stats_event(job_result);
    }

    checked_asprintf(&time_str, " exit:%d sig:%d core:%d ret:%d time:%dms ",
                     WEXITSTATUS(status), WTERMSIG(status), WCOREDUMP(status),
                     ret, time_ms);
    if (time_str != NULL) dcc_job_summary_append(time_str);
    free(time_str);

    /* append compiler and input file info */
    if (job_result == STATS_COMPILE_ERROR
        || job_result == STATS_COMPILE_OK) {
        dcc_job_summary_append(argv[0]);
        dcc_job_summary_append(" ");
        dcc_job_summary_append(orig_input);
    }

    dcc_remove_log_to_file();
    dcc_cleanup_tempfiles();

    free(orig_input);
    free(orig_output);

    if (argv)
        dcc_free_argv(argv);
    if (tweaked_argv)
        dcc_free_argv(tweaked_argv);

    free(temp_dir);
    free(temp_i);
    free(temp_o);

    free(deps_fname);
    free(err_fname);
    free(out_fname);

    free(client_cwd);
    free(server_cwd);

    return ret;
}
