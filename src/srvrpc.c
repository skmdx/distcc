/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
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



/**
 * @file
 *
 * Server-specific RPC code.
  **/



#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "rpc.h"
#include "exitcode.h"
#include "dopt.h"
#include "hosts.h"
#include "bulk.h"
#include "file_cache.h"
#include "snprintf.h"

int dcc_r_request_header(int ifd,
                         enum dcc_protover *ver_ret)
{
    unsigned vers;
    int ret;

    if ((ret = dcc_r_token_int(ifd, "DIST", &vers)) != 0) {
        rs_log_error("client did not provide distcc magic fairy dust");
        return ret;
    }

    if (vers < DCC_VER_1 || vers > DCC_VER_CACHED_CPP) {
        rs_log_error("can't handle requested protocol version is %d", vers);
        return EXIT_PROTOCOL_ERROR;
    }

    *ver_ret = (enum dcc_protover) vers;

    return 0;
}


 /**
  * Receive the working directory from the client
  */
int dcc_r_cwd(int ifd, char **cwd)
{
    return dcc_r_token_string(ifd, "CDIR", cwd);
}

/* @p path must be point to malloc'ed memory
 * Replaces **path with a pointer to a string containing
 * dirname + path.
 * path must be absolute.
 */
static int prepend_dir_to_name(const char *dirname, char **path)
{
    char *buf;
    checked_asprintf(&buf, "%s%s", dirname, *path);
    if (buf == NULL) {
            return EXIT_OUT_OF_MEMORY;
    }
    free(*path);
    *path = buf;
        return 0;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int dcc_r_many_files(int in_fd, int out_fd,
                     const char *dirname,
                     enum dcc_compress compr, int cached)
{
    int ret = 0;
    unsigned int n_files;
    unsigned int i;
    char *name = 0;
    char *link_target = 0;
    char token[5];
    struct cached_file { char *name; unsigned char hash[DCC_FILE_HASH_SIZE]; } *files = NULL;
    unsigned char *wanted = NULL;
    char **names = NULL;

    if ((ret = dcc_r_token_int(in_fd, "NFIL", &n_files)))
        return ret;
    if (cached) {
        if (!n_files || n_files > DCC_FILE_MANIFEST_LIMIT) return EXIT_PROTOCOL_ERROR;
        files = calloc(n_files, sizeof(*files));
        wanted = calloc(n_files, 1);
        names = calloc(n_files, sizeof(*names));
        if (!files || !wanted || !names) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    }

    for (i = 0; i < n_files; ++i) {
        /* like dcc_r_argv */
        unsigned int link_or_file_len;

        if (cached) {
            unsigned length;
            if ((ret = dcc_r_token_int(in_fd, "NAME", &length))) goto out_cleanup;
            if (!length || length >= MAXPATHLEN) { ret = EXIT_PROTOCOL_ERROR; goto out_cleanup; }
            if ((ret = dcc_r_str_alloc(in_fd, length, &name))) goto out_cleanup;
            if (strlen(name) != length) { ret = EXIT_PROTOCOL_ERROR; goto out_cleanup; }
        } else if ((ret = dcc_r_token_string(in_fd, "NAME", &name))) goto out_cleanup;

        if (cached && (name[0] != '/' || strstr(name, "/../") ||
                       strstr(name, "//") || strstr(name, "/./") ||
                       str_endswith("/", name) || str_endswith("/.", name) ||
                       str_endswith("/..", name))) {
            ret = EXIT_PROTOCOL_ERROR;
            goto out_cleanup;
        }

        /* FIXME: verify that name starts with '/' and doesn't contain '..'. */
        if ((ret = prepend_dir_to_name(dirname, &name)))
            goto out_cleanup;

        if ((ret = dcc_r_sometoken_int(in_fd, token, &link_or_file_len)))
            goto out_cleanup;

        /* Must prepend the dirname for the file name, a link's target name. */
        if (strncmp(token, "LINK", 4) == 0) {

            if (cached && (!link_or_file_len || link_or_file_len >= MAXPATHLEN)) {
                ret = EXIT_PROTOCOL_ERROR;
                goto out_cleanup;
            }

            if ((ret = dcc_r_str_alloc(in_fd, link_or_file_len, &link_target))){
                goto out_cleanup;
            }
            /* FIXME: verify that link_target doesn't contain '..'.
             * But the include server uses '..' to reference system
             * directories (see _MakeLinkFromMirrorToRealLocation
             * in include_server/compiler_defaults.py), so we'll need to
             * modify that first. */
            if (link_target[0] == '/') {
                if ((ret = prepend_dir_to_name(dirname, &link_target))) {
                    goto out_cleanup;
                }
            }
            if ((ret = dcc_mk_tmp_ancestor_dirs(name))) {
                goto out_cleanup;
            }
            if (symlink(link_target, name) != 0) {
                rs_log_error("failed to create path for %s: %s", name,
                             strerror(errno));
                ret = 1;
                goto out_cleanup;
            }
            if ((ret = dcc_add_cleanup(name))) {
                /* bailing out */
                unlink(name);
                goto out_cleanup;
            }
        } else if (cached && !strcmp(token, "HASH")) {
            if (link_or_file_len != DCC_FILE_HASH_SIZE) {
                ret = EXIT_PROTOCOL_ERROR;
                goto out_cleanup;
            }
            if ((ret = dcc_readx(in_fd, files[i].hash, DCC_FILE_HASH_SIZE)) ||
                (ret = dcc_add_cleanup(name))) goto out_cleanup;
            wanted[i] = !dcc_file_cache_get(files[i].hash, name);
        } else if (strncmp(token, "FILE", 4) == 0) {
            if (cached && link_or_file_len) {
                ret = EXIT_PROTOCOL_ERROR;
                goto out_cleanup;
            }
            if ((ret = dcc_r_file(in_fd, name, link_or_file_len, compr))) {
                goto out_cleanup;
            }
            if ((ret = dcc_add_cleanup(name))) {
              /* bailing out */
              unlink(name);
              goto out_cleanup;
            }
        } else {
            char buf[4 + sizeof(link_or_file_len)];
            /* unexpected token */
            rs_log_error("protocol derailment: expected token FILE or LINK");
            /* We should explain what happened here, but we have already read
             * a few more bytes.
             */
            memcpy(buf, token, 4);
            /* TODO(manos): this is probably not kosher */
            memcpy(&buf[4], &link_or_file_len, sizeof(link_or_file_len));
            dcc_explain_mismatch(buf, 12, in_fd);
            ret = EXIT_PROTOCOL_ERROR;
            goto out_cleanup;
        }

out_cleanup:
        if (cached && !ret) {
            files[i].name = names[i] = name;
            name = NULL;
        }
        free(name);
        name = NULL;
        free(link_target);
        link_target = NULL;
        if (ret)
            break;
    }
    if (!ret && cached) {
        qsort(names, n_files, sizeof(*names), compare_names);
        for (i = 1; i < n_files; ++i) {
            if (!strcmp(names[i - 1], names[i])) { ret = EXIT_PROTOCOL_ERROR; goto done; }
        }
        ret = dcc_x_token_int(out_fd, "MISS", n_files);
        if (!ret) ret = dcc_writex(out_fd, wanted, n_files);
        tcp_cork_sock(out_fd, 0);
        for (i = 0; !ret && i < n_files; ++i) {
            if (!wanted[i]) continue;
            ret = dcc_r_token_file(in_fd, "FILE", files[i].name, compr);
            if (!ret && !dcc_file_cache_verify(files[i].hash, files[i].name)) {
                rs_log_error("input content does not match its SHA-256 manifest");
                ret = EXIT_PROTOCOL_ERROR;
            }
            if (!ret) dcc_file_cache_put(files[i].hash, files[i].name);
        }
    }
done:
    if (files) for (i = 0; i < n_files; ++i) free(files[i].name);
    free(names);
    free(files);
    free(wanted);
    return ret;
}
