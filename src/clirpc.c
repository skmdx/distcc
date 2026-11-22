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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "distcc.h"
#include "trace.h"
#include "exec.h"
#include "rpc.h"
#include "exitcode.h"
#include "util.h"
#include "clinet.h"
#include "bulk.h"
#include "file_cache.h"
#include "hosts.h"
#include "state.h"

/**
 * @file
 *
 * @brief Client-side RPC functions.
 **/

/*
 * Transmit header for whole request.
 */
int dcc_x_req_header(int fd,
                     enum dcc_protover protover)
{
     return dcc_x_token_int(fd, "DIST", protover);
}

/**
 * Read the "DONE" token from the network that introduces a response.
 **/
int dcc_r_result_header(int ifd,
                        enum dcc_protover expect_ver)
{
    unsigned vers;
    int ret;

    if ((ret = dcc_r_token_int(ifd, "DONE", &vers))) {
        rs_log_error("server provided no answer. "
                     "Is the server configured to allow access from your IP"
                     " address? Is the server performing authentication and"
                     " your client isn't? Does the server have the compiler"
                     " installed? Is the server configured to access the"
                     " compiler?");
        return ret;
    }

    if (vers != expect_ver) {
        rs_log_error("got version %d not %d in response from server",
                     vers, expect_ver);
        return EXIT_PROTOCOL_ERROR;
    }

    rs_trace("got response header");

    return 0;
}


int dcc_r_cc_status(int ifd, int *status)
{
    unsigned u_status;
    int ret;

    ret = dcc_r_token_int(ifd, "STAT", &u_status);
    *status = u_status;
    return ret;
}


/**
 * The second half of the client protocol: retrieve all results from the server.
 **/
int dcc_retrieve_results(int net_fd,
                         int *status,
                         const char *output_fname,
                         struct dcc_buffer *server_stderr,
                         struct dcc_buffer *server_stdout,
                         int preprocess_only,
                         struct dcc_hostdef *host)
{
    unsigned len;
    int ret;
    unsigned o_len;

    if ((ret = dcc_r_result_header(net_fd, host->protover)))
        return ret;

    /* We've started to see the response, so the server is done
     * compiling. */
    dcc_note_state(DCC_PHASE_RECEIVE, NULL, NULL, DCC_REMOTE);

    if ((ret = dcc_r_cc_status(net_fd, status)))
        return ret;

    if ((ret = dcc_r_token_int(net_fd, "SERR", &len)))
        return ret;

    /* Keep the decoded server-side errors in memory. This way, we can
       decide later whether we want to report them to the user
       or not. We don't want to report them to the user if
       we are going to redo the compilation locally, because then
       the local errors are going to appear.
       Always put the server-side errors in the email we will
       send to the maintainers, though.
    */

    if ((ret = dcc_r_buffer(server_stderr, net_fd, len, host->compr)))
        return ret;


    if ((ret = dcc_r_token_int(net_fd, "SOUT", &len))
        || (ret = dcc_r_buffer(server_stdout, net_fd, len, host->compr))
        || (ret = dcc_r_token_int(net_fd, "DOTO", &o_len)))
        return ret;


    /* If the compiler succeeded, then we always retrieve the result,
     * even if it's 0 bytes.  */
    if (*status == 0) {
        if ((ret = dcc_r_file_timed(net_fd,
                    !strcmp(output_fname, "-") ? "/dev/null" : output_fname,
                    o_len, host->compr)))
            return ret;
    } else if (preprocess_only && strcmp(output_fname, "-")) {
        struct stat st;
        if (!stat(output_fname, &st) && S_ISREG(st.st_mode))
            dcc_remove_if_exists(output_fname);
    } else if (o_len != 0) {
        rs_log_error("remote compiler failed but also returned output: "
                     "I don't know what to do");
    }

    return 0;
}

/* points_to must be at least MAXPATHLEN + 1 long */
int dcc_read_link(const char* fname, char *points_to)
{
    int len;
    if ((len = readlink(fname, points_to, MAXPATHLEN)) == -1) {
        rs_log_error("readlink '%s' failed: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }
    points_to[len] = '\0';
    return 0;
}

int dcc_is_link(const char *fname, int *is_link)
{
    struct stat buf;

    if (lstat(fname, &buf) == -1) {
        rs_log_error("stat '%s' failed: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }

    *is_link = S_ISLNK(buf.st_mode);
    return 0;
}
