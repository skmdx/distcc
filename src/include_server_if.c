/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
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

/* Authors: Manos Renieris, Fergus Henderson */

#include <config.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "distcc.h"
#include "trace.h"
#include "rpc.h"
#include "clinet.h"
#include "exitcode.h"
#include "util.h"
#include "hosts.h"
#include "include_server_if.h"
#include "job.h"
#include "lock.h"
#include "where.h"

static int dcc_categorize_file(const char *include_server_filename);

/** Talks to the include server, over the AF_UNIX socket specified
 * in env variable INCLUDE_SERVER_PORT. If all goes well,
 * it returns the array of files in @p files and returns 0;
 * if anything goes wrong, it returns a non-zero value.
 */

int dcc_talk_to_include_server(char **argv, char ***files, enum dcc_compress compr)
{
    char *include_server_port;
    int fd = -1, lock_fd = -1;
    struct sockaddr_un sa;

    int ret;
    char *stub;

    /* for testing purposes, if INCLUDE_SERVER_STUB is set,
       use its value rather than the include server */
    stub = getenv("INCLUDE_SERVER_STUB");
    if (stub != NULL) {
        ret = dcc_tokenize_string(stub, files);
        rs_log_warning("INCLUDE_SERVER_STUB is set to '%s'; "
                       "ignoring include server",
                       dcc_argv_tostr(*files));
        return ret;
    }

    include_server_port = getenv("INCLUDE_SERVER_PORT");
    if (include_server_port == NULL) {
        rs_log_warning("INCLUDE_SERVER_PORT not set - "
                       "did you forget to run under 'pump'?");
        return 1;
    }

    if (strlen(include_server_port) >= ((int)sizeof(sa.sun_path) - 1)) {
        rs_log_warning("$INCLUDE_SERVER_PORT is longer than %ld characters",
                       ((long) sizeof(sa.sun_path) - 1));
        return 1;
    }

    strcpy(sa.sun_path, include_server_port);
    sa.sun_family = AF_UNIX;

    /* Native analysis and ordinary preprocessing share the client's CPU and
     * memory admission budget. The include-server pool owns private caches;
     * congestion no longer changes preprocessing mode or compiler semantics. */
    dcc_read_localslots_configuration();
    ret = dcc_lock_local_analysis(&lock_fd);
    if (ret) return ret;
    ret = 1;
    if (dcc_connect_by_addr((struct sockaddr *) &sa, sizeof(sa), &fd))
        goto out;

    /* TODO? switch include_server to use more appropriate token names */
    if (dcc_x_cwd(fd) ||
        dcc_x_argv(fd, compr == DCC_COMPRESS_LZ4_STREAM ? "LZ4S" : compr == DCC_COMPRESS_LZ4 ? "LZ4C" : "ARGC", "ARGV", argv) ||
        dcc_r_argv(fd, "ARGC", "ARGV", files)) {
        rs_log_warning("failed to talk to include server '%s'",
                       include_server_port);
        goto out;
    }

    if (dcc_argv_len(*files) == 0) {
        rs_log_warning("include server gave up analyzing");
        goto out;
    }
    ret = 0;
out:
    if (fd != -1 && dcc_close(fd)) ret = 1;
    dcc_unlock(lock_fd);
    return ret;
}

/**
 * This implements the --scan_includes option.
 * Talks to the include server, and prints the results to stdout.
 */
int
dcc_approximate_includes(struct dcc_hostdef *host, char **argv)
{
    char **files;
    int i;
    int ret;

    if (host->cpp_where != DCC_CPP_ON_SERVER) {
        rs_log_error("'--scan_includes' specified, "
                     "but distcc wouldn't have used include server "
                     "(make sure hosts list includes ',cpp' option?)");
        return EXIT_DISTCC_FAILED;
        //return 0;
    }

    if ((ret = dcc_talk_to_include_server(argv, &files, host->compr))) {
        rs_log_error("failed to get includes from include server");
        return ret;
    }

    for (i = 0; files[i]; i++) {
        if ((ret = dcc_categorize_file(files[i])))
            return ret;
    }

    return 0;
}

/*
 * A subroutine of dcc_approximate_includes().
 * Take a filename output from the include server,
 * convert the filename back so that it refers to the original source tree
 * (as opposed to the include server's mirror tree),
 * categorize it as DIRECTORY, SYMLINK, or FILE,
 * and print the category and original name to stdout.
 */
static int
dcc_categorize_file(const char *include_server_filename) {
    char *filename;
    int is_symlink = 0;
    int is_forced_directory = 0;
    int ret;

    if ((ret = dcc_get_original_fname(include_server_filename, &filename))) {
        rs_log_error("dcc_get_original_fname failed");
        return ret;
    }

    if (!strncmp(filename, "/" DCC_SYSTEM_HEADERS "/", sizeof(DCC_SYSTEM_HEADERS) + 1)) {
        free(filename);
        return 0;
    }

    if (str_endswith("/forcing_technique_271828", filename)) {
        /* Replace "<foo>/forcing_technique_271818" with "<foo>". */
        filename[strlen(filename) - strlen("/forcing_technique_271828")]
            = '\0';
        is_forced_directory = 1;
    }

    /* forced directories don't exist */
    if (!is_forced_directory &&
        (ret = dcc_is_link(include_server_filename, &is_symlink))) {
        free(filename);
        return ret;
    }

    printf("%-9s %s\n", is_forced_directory         ? "DIRECTORY" :
                        is_symlink                  ? "SYMLINK" :
                                                      "FILE",
                        filename);

    free(filename);
    return 0;
}
