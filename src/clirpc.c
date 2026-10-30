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
#include "include_server_if.h"
#include "emaillog.h"

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
 * Transmit the current working directory
 */
int dcc_x_cwd(int fd)
{
    int ret;
    char cwd[MAXPATHLEN + 1];
    char * cwd_ret;
    cwd_ret = getcwd(cwd, MAXPATHLEN);
    if (cwd_ret == NULL) {
        return 0;
    }
    ret = dcc_x_token_string(fd, "CDIR", cwd);
    return ret;
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
                         const char *deps_fname,
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

    if ((ret = dcc_add_buffer_to_log_email("server-side stderr", server_stderr)))
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
        if (host->cpp_where == DCC_CPP_ON_SERVER) {
            if ((ret = dcc_r_token_int(net_fd, "DOTD", &len)))
                return ret;
            return dcc_r_file_timed(net_fd, deps_fname ? deps_fname : "/dev/null",
                                    len, host->compr);
        }
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

/* Read digests of the include server's immutable compressed snapshots only
 * when the missing-set exchange can save a useful amount of traffic. */
struct dcc_input_digest *dcc_file_hashes(char **files, unsigned count)
{
    struct dcc_input_digest *hashes;
    unsigned i;
    int64_t savings = -12 - (int64_t)count; /* MISS token and bitmap. */
    if (!count || count > DCC_FILE_MANIFEST_LIMIT) return NULL;
    hashes = calloc(count, sizeof(*hashes));
    if (!hashes) return NULL;
    for (i = 0; i < count; ++i) {
        struct stat st;
        if (lstat(files[i], &st)) {
            char *name = NULL;
            int placeholder;
            if (errno != ENOENT || dcc_get_original_fname(files[i], &name)) goto failed;
            placeholder = str_endswith("/forcing_technique_271828", name);
            free(name);
            if (!placeholder) goto failed;
            continue;
        }
        if (S_ISLNK(st.st_mode)) continue;
        if (!S_ISREG(st.st_mode)) goto failed;
        hashes[i].kind = DCC_INPUT_CACHED;
        savings += st.st_size > INT_MAX ? INT_MAX : st.st_size - DCC_FILE_HASH_SIZE;
    }
    /* Avoid a missing-set round trip and sidecar reads when even an all-hit
     * manifest would save little traffic. Compressed sizes are already known. */
    if (savings < 16384) goto failed;
    for (i = 0; i < count; ++i) {
        char *sidecar = NULL;
        FILE *file;
        int valid;
        unsigned char size[8];
        size_t length;
        uint64_t plain_size = 0;
        if (!hashes[i].kind) continue;
        if (asprintf(&sidecar, "%s.xxh3", files[i]) < 0) goto failed;
        file = fopen(sidecar, "rb");
        free(sidecar);
        if (!file) goto failed;
        valid = fread(hashes[i].hash, 1, DCC_FILE_HASH_SIZE, file) == DCC_FILE_HASH_SIZE;
        length = fread(size, 1, sizeof(size), file);
        valid &= length == sizeof(size) && fgetc(file) == EOF && !ferror(file);
        fclose(file);
        if (!valid) goto failed;
        /* Exclude inputs the cache cannot keep. */
        for (unsigned j = 0; j < sizeof(size); ++j) plain_size = (plain_size << 8) | size[j];
        if (plain_size > DCC_FILE_CACHE_MAX) {
            struct stat st;
            if (lstat(files[i], &st)) goto failed;
            savings -= st.st_size > INT_MAX ? INT_MAX : st.st_size - DCC_FILE_HASH_SIZE;
            hashes[i].kind = DCC_INPUT_INLINE;
        }
    }
    if (savings < 16384) goto failed;
    return hashes;
failed:
    /* Missing or invalid metadata must never select a cached input. */
    free(hashes);
    return NULL;
}

/* fnames is the include server's null-terminated list of compressed snapshots
 * and symlinks for the sequential TCP protocol. */
struct manifest_writer { int fd; size_t used; char data[65537]; };

static int flush_manifest(struct manifest_writer *writer)
{
    int ret = writer->used ? dcc_writex(writer->fd, writer->data, writer->used) : 0;
    writer->used = 0;
    return ret;
}

static int manifest_token(struct manifest_writer *writer, const char *token,
                          unsigned value, const void *data)
{
    size_t size = 12 + (data ? value : 0);
    int ret;
    if (size >= sizeof(writer->data)) return EXIT_PROTOCOL_ERROR;
    if (writer->used + size >= sizeof(writer->data) && (ret = flush_manifest(writer))) return ret;
    ret = dcc_format_token_int(writer->data + writer->used, token, value);
    if (ret) return ret;
    if (data) memcpy(writer->data + writer->used + 12, data, value);
    writer->used += size;
    return 0;
}

int dcc_x_many_files(int ofd,
                     unsigned int n_files,
                     char **fnames, enum dcc_compress compr)
{
    int ret;
    char link_points_to[MAXPATHLEN + 1];
    int is_link;
    const char *fname;
    char *original_fname;
    struct manifest_writer writer;
    writer.fd = ofd;
    writer.used = 0;

    if ((ret = manifest_token(&writer, "NFIL", n_files, NULL))) return ret;

    for (; *fnames != NULL; ++fnames) {
        fname = *fnames;
        ret = dcc_get_original_fname(fname, &original_fname);
        if (ret) return ret;

        ret = manifest_token(&writer, "NAME", strlen(original_fname), original_fname);
        int placeholder = str_endswith("/forcing_technique_271828", original_fname);
        free(original_fname);
        if (ret) return ret;

        if (placeholder) {
            /* Directory placeholder files don't exist on disk.  Send
               an empty file to ensure the directory is created */
            ret = manifest_token(&writer, "FILE", 0, NULL);
            if (ret) return ret;
        } else {
            if ((ret = dcc_is_link(fname, &is_link))) {
                return ret;
            }

            if (is_link) {
                if ((ret = dcc_read_link(fname, link_points_to)) ||
                    (ret = manifest_token(&writer, "LINK", strlen(link_points_to), link_points_to))) {
                    return ret;
                }
            } else {
                /* Cache bytes carry the selected codec, without an RPC token. */
                if ((ret = flush_manifest(&writer))) return ret;
                if (compr == DCC_COMPRESS_LZ4_STREAM) {
                    ret = dcc_x_token_int(ofd, "FILE", 0xffffffffU);
                    if (!ret) ret = dcc_copy_file_to_fd(fname, ofd);
                } else ret = dcc_x_file(ofd, fname, "FILE", DCC_COMPRESS_NONE, NULL);
                if (ret) return ret;
            }
        }
    }
    if ((ret = flush_manifest(&writer))) return ret;
    return 0;
}
