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


            /* 4: The noise of a multitude in the
             * mountains, like as of a great people; a
             * tumultuous noise of the kingdoms of nations
             * gathered together: the LORD of hosts
             * mustereth the host of the battle.
             *        -- Isaiah 13 */



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#include "distcc.h"
#include "helpers.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "adaptive.h"
#include "where.h"
#include "broker.h"
#include "implicit.h"
#include "compile.h"
#include "job.h"
#include "rust/api.h"
#include "native_worker.h"


/* Name of this program, for trace.c */
const char *rs_program_name = "distcc";

/**
 * @file
 *
 * Entry point for the distcc client.
 *
 * There are three methods of use for distcc: explicit (distcc gcc -c
 * foo.c), implicit (distcc -c foo.c) and masqueraded (gcc -c foo.c,
 * where gcc is really a link to distcc).
 *
 * Detecting these is relatively easy by examining the first one or
 * two words of the command.  We also need to make sure that when we
 * go to run the compiler, we run the one intended by the user.
 *
 * In particular, for masqueraded mode, we want to make sure that we
 * don't invoke distcc recursively.
 **/

static void dcc_show_usage(void)
{
    dcc_show_version("distcc");
    printf(
"Usage:\n"
"   distcc [COMPILER] [compile options] -o OBJECT -c SOURCE\n"
"   distcc [--help|--version|--show-hosts|-j]\n"
"\n"
"Options:\n"
"   COMPILER                   Defaults to \"cc\".\n"
"   --help                     Explain usage, and exit.\n"
"   --version                  Show version, and exit.\n"
"   --show-hosts               Show host list, and exit.\n"
"   --show-helpers             Show private helper directory, and exit.\n"
"   --lto-capacity             Show broker compute capacity as JSON.\n"
"   -j                         Show the concurrency level, as calculated from\n"
"                              the host list, and exit.\n"
#ifdef HAVE_GSSAPI
"   --show-principal           Show current distccd GSS-API principal and exit.\n"
#endif
"\n"
"Environment variables:\n"
"   See the manual page for a complete list.\n"
"   DISTCC_VERBOSE=1           Give debug messages.\n"
"   DISTCC_BROKER              Use a local distccd broker's UNIX socket.\n"
"   DISTCC_LOG                 Send messages to file, not stderr.\n"
"   DISTCC_SSH                 Command to run to open SSH connections.\n"
"   DISTCC_DIR                 Directory for host list and locks.\n"
#ifdef HAVE_GSSAPI
"   DISTCC_PRINCIPAL	      The name of the server principal to connect to.\n"
#endif
"\n"
"Server specification:\n"
"A list of servers is taken from the environment variable $DISTCC_HOSTS, or\n"
"$DISTCC_DIR/hosts, or ~/.distcc/hosts, or %s/distcc/hosts.\n"
"Each host can be given in any of these forms, see the manual for details:\n"
"\n"
"   localhost                  Run in place.\n"
"   HOST                       TCP connection, port %d.\n"
"   HOST:PORT                  TCP connection, specified port.\n"
"   @HOST                      SSH connection to specified host.\n"
"   USER@HOST                  SSH connection to specified username at host.\n"
"   HOSTSPEC,lz4               Enable LZ4 compression.\n"
"   HOSTSPEC,lzo               Enable legacy LZO compression.\n"
"   HOSTSPEC,auth              Enable GSS-API based mutual authenticaton.\n"
"   --randomize                Randomize the server list before execution.\n"
"\n"
"distcc distributes compilation jobs across volunteer machines running\n"
"distccd.  Jobs that cannot be distributed, such as linking, are run locally.\n"
"distcc should be used with make's -jN option to execute in parallel on\n"
"several machines.\n",
    SYSCONFDIR,
    DISTCC_DEFAULT_PORT);
}


static void dcc_client_signalled (int whichsig)
{
    signal(whichsig, SIG_DFL);

    dcc_log_signal_termination(whichsig);

    dcc_cleanup_tempfiles_from_signal_handler();

    raise(whichsig);

}


static void dcc_client_catch_signals(void)
{
    signal(SIGTERM, &dcc_client_signalled);
    signal(SIGINT, &dcc_client_signalled);
    signal(SIGHUP, &dcc_client_signalled);
}

static void dcc_free_hostlist(struct dcc_hostdef *list) {
    while (list) {
        struct dcc_hostdef *l = list;
        list = list->next;
        dcc_free_hostdef(l);
    }
}

static void dcc_show_hosts(void) {
    struct dcc_hostdef *list, *l;
    int nhosts;

    if (dcc_get_hostlist(&list, &nhosts) != 0) {
        rs_log_crit("Failed to get host list");
        return;
    }

    for (l = list; l; l = l->next)
        printf("%s\n", l->hostdef_string);

    dcc_free_hostlist(list);
}

int dcc_lto_capacity(char **capacity) {
    const char *broker = getenv("DISTCC_BROKER");
    if (!broker || !*broker) {
        rs_log_error("LTO capacity requires DISTCC_BROKER");
        return EXIT_BAD_ARGUMENTS;
    }
    return dcc_broker_jobs(capacity) < 0 ? EXIT_CONNECT_FAILED : 0;
}

static void dcc_concurrency_level(void) {
    struct dcc_hostdef *list, *l;
    int nhosts;
    int nslots = 0;
    const char *broker_path = getenv("DISTCC_BROKER");

    if (broker_path && *broker_path) {
        nslots = dcc_broker_jobs(NULL);
        if (nslots < 0)
            dcc_exit(EXIT_CONNECT_FAILED);
        printf("%i\n", nslots);
        return;
    }

    if (dcc_get_hostlist(&list, &nhosts) != 0) {
        rs_log_crit("Failed to get host list");
        return;
    }

    for (l = list; l; l = l->next) {
        int slots = l->n_slots;
        if (l->stats_port)
            slots = dcc_host_slots(l);
        nslots += slots;
    }

    dcc_free_hostlist(list);

    printf("%i\n", nslots);
}

#ifdef HAVE_GSSAPI
/*
 * Print out the name of the principal.
 */
static void dcc_gssapi_show_principal(void) {
    char *princ_env_val = NULL;

    if((princ_env_val = getenv("DISTCC_PRINCIPAL"))) {
	    printf("Principal is\t: %s\n", princ_env_val);
    } else {
        printf("Principal\t: Not Set.\n");
    }
}
#endif

int main(int argc, char **argv)
{
    int status, sg_level, tweaked_path = 0, masquerade = 0;
    char **compiler_args = NULL; /* dynamically allocated */
    char *compiler_name; /* points into argv[0] */
    int ret;

    if (!strcmp(argv[0], "/__distcc_rust/dtlto-distributor"))
        return dcc_lto_distribute(argc, argv);

    dcc_client_catch_signals();
    atexit(dcc_cleanup_tempfiles);
    atexit(dcc_remove_state_file);

    dcc_set_trace_from_env();
    dcc_raise_fd_limit();

    dcc_trace_version();

    /* TODO: Expand @file arguments? Previously this was done only when built with libiberty. */

    compiler_name = (char *) dcc_find_basename(argv[0]);

    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);

    if (argc > 2 && !strcmp(argv[1], "--gcc-plan"))
        dcc_exit(dcc_gcc_plan_execute(argv + 2));

    /* Link coordinators have no local lease until they choose local work. */
    if (argc > 2 && !strcmp(argv[1], "--local-command"))
        dcc_exit(dcc_run_local_command(argv + 2));

    if (argc == 5 && !strcmp(argv[1], "--rust-cgu"))
        dcc_exit(dcc_rust_submit_cgu(argv[2], argv[3], argv[4]));

    if (!strcmp(compiler_name, "distcc-thinlto") || !strcmp(compiler_name, "distcc-offload-link"))
        dcc_exit(dcc_lto_distribute(argc, argv));

    if (argc >= 6 && !strcmp(argv[1], "--file-job")) {
        long queued_fd = -1;
        if (strcmp(argv[4], "-")) {
            char *end;
            errno = 0;
            queued_fd = strtol(argv[4], &end, 10);
            if (errno || !argv[4][0] || *end || queued_fd < 0 || queued_fd > INT_MAX ||
                fcntl((int)queued_fd, F_GETFD) < 0)
                dcc_exit(EXIT_BAD_ARGUMENTS);
        }
        dcc_exit(dcc_file_transfer(argv[2], argv[3], argv + 5, (int)queued_fd));
    }

    if (argc > 1 && (!strcmp(compiler_name, "distcc-rust") || !strcmp(dcc_find_basename(argv[1]), "rustc") ||
                    !strncmp(dcc_find_basename(argv[1]), "rustc-", 6))) {
        dcc_exit(dcc_rust_client(argv + 1));
    }

    sg_level = dcc_recursion_safeguard();

    rs_trace("compiler name is \"%s\"", compiler_name);

    if (dcc_is_launcher(compiler_name)) {
        /* Either "distcc -c hello.c" or "distcc gcc -c hello.c" */
        if (argc <= 1) {
            fprintf (stderr,
                     "%s: missing option/operand\n"
                     "Try `%s --help' for more information.\n",
                     argv[0], argv[0]);
            ret = EXIT_BAD_ARGUMENTS;
            goto out;
        }

        if (!strcmp(argv[1], "--help")) {
            dcc_show_usage();
            ret = 0;
            goto out;
        }

        if (!strcmp(argv[1], "--version")) {
            dcc_show_version("distcc");
            ret = 0;
            goto out;
        }

        if (!strcmp(argv[1], "--show-helpers")) {
            char path[PATH_MAX];
            dcc_helper_path("distcc-link", path);
            *strrchr(path, '/') = '\0';
            puts(path);
            return 0;
        }

        if (!strcmp(argv[1], "--show-hosts")) {
            dcc_show_hosts();
            ret = 0;
            goto out;
        }


        if (!strcmp(argv[1], "-j") || !strcmp(argv[1], "--lto-capacity")) {
            if (!strcmp(argv[1], "-j")) dcc_concurrency_level();
            else {
                char *capacity = NULL;
                if ((ret = dcc_lto_capacity(&capacity))) goto out;
                puts(capacity);
                free(capacity);
            }
            ret = 0;
            goto out;
        }


#ifdef HAVE_GSSAPI
	    if (!strcmp(argv[1], "--show-principal")) {
	        dcc_gssapi_show_principal();
	        ret = 0;
	        goto out;
	    }
#endif

        if ((ret = dcc_find_compiler(argv, &compiler_args)) != 0) {
            goto out;
        }
        /* compiler_args is now respectively either "cc -c hello.c" or
         * "gcc -c hello.c" */

        /* Build-system launchers may wrap an absolute masquerade link.
         * Unwrap it before compiler discovery and RPC, just
         * as when the same link is invoked directly. */
        masquerade = strchr(compiler_args[0], '/') && dcc_is_distcc_path(compiler_args[0]);
    } else {
        /* Invoked as "cc -c hello.c", with masqueraded path */
        if ((ret = dcc_copy_argv(argv, &compiler_args, 0)) != 0) {
            goto out;
        }
        masquerade = 1;
    }

    if (masquerade) {
        compiler_name = (char *) dcc_find_basename(compiler_args[0]);
        if ((ret = dcc_support_masquerade(compiler_args, compiler_name,
                                        &tweaked_path)) != 0)
            goto out;
        char *name = strdup(compiler_name);
        free(compiler_args[0]);
        compiler_args[0] = name;
        if (!compiler_args[0]) {
            rs_log_error("strdup failed - out of memory?");
            ret = EXIT_OUT_OF_MEMORY;
            goto out;
        }
    }

    if (sg_level - tweaked_path > 0) {
        rs_log_crit("distcc seems to have invoked itself recursively!");
        ret = EXIT_RECURSION;
        goto out;
    }

    ret = dcc_build_somewhere_timed(compiler_args, sg_level, &status);
    compiler_args = NULL; /* dcc_build_somewhere_timed already free'd it. */

    out:
    if (compiler_args) {
      dcc_free_argv(compiler_args);
    }
    dcc_exit(ret);
}
