/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
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
 * Run the preprocessor.  Client-side only.
 **/

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "util.h"
#include "implicit.h"
#include "exec.h"
#include "dotd.h"

static int spawn_preprocessor(char **argv, pid_t *pid, const char *output)
{
#ifdef __linux__
    const char *library = getenv("DISTCC_CPP_LOOKUP_LIBRARY");
    if (!library) library = CPP_LOOKUP_LIBRARY;
    if (dcc_getenv_bool("DISTCC_CPP_LOOKUP", 1) && !access(library, R_OK)) {
        const char *preload = getenv("LD_PRELOAD"), *setting = getenv("DISTCC_CPP_LOOKUP");
        char *old_preload = preload ? strdup(preload) : NULL;
        char *old_setting = setting ? strdup(setting) : NULL;
        char *value = NULL;
        if ((!preload || old_preload) && (!setting || old_setting) &&
            asprintf(&value, "%s%s%s", library, preload ? ":" : "", preload ? preload : "") >= 0) {
            int ready = !setenv("LD_PRELOAD", value, 1) && !setenv("DISTCC_CPP_LOOKUP", "1", 1);
            int ret = ready ? dcc_spawn_child(argv, pid, "/dev/null", output, NULL) : EXIT_OUT_OF_MEMORY;
            if (old_preload) setenv("LD_PRELOAD", old_preload, 1); else unsetenv("LD_PRELOAD");
            if (old_setting) setenv("DISTCC_CPP_LOOKUP", old_setting, 1); else unsetenv("DISTCC_CPP_LOOKUP");
            free(value); free(old_preload); free(old_setting);
            return ret;
        }
        free(old_preload); free(old_setting);
    }
#endif
    return dcc_spawn_child(argv, pid, "/dev/null", output, NULL);
}

/* Expanding macros on the client loses their provenance, changing Clang's
 * diagnostics (including -Werror) and source locations. Rewrite includes only
 * and let the remote frontend expand macros in the original language. */
const char *dcc_cpp_rewrite_language(char **argv, const char *input_fname)
{
    const char *name = dcc_find_basename(argv[0]);
    const char *clang = strstr(name, "clang");
    const char *ext, *language = NULL;
    int i;
    if (!clang || (clang != name && clang[-1] != '-') ||
        (clang[5] && clang[5] != '-' && strcmp(clang + 5, "++") &&
         strncmp(clang + 5, "++-", 3)) || dcc_is_preprocessed(input_fname))
        return NULL;
    ext = dcc_find_extension_const(input_fname);
    if (ext) ext = dcc_preproc_exten(ext);
    if (ext) {
        if (!strcmp(ext, ".i")) language = !strncmp(clang + 5, "++", 2) ? "c++" : "c";
        else if (!strcmp(ext, ".ii")) language = "c++";
        else if (!strcmp(ext, ".mi")) language = "objective-c";
        else if (!strcmp(ext, ".mii")) language = "objective-c++";
    }
    for (i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "-Xclang") && argv[i + 1]) { i++; continue; }
        if (!strcmp(argv[i], "-x") && argv[i + 1]) language = argv[++i];
        else if (!strncmp(argv[i], "-x", 2) && argv[i][2]) language = argv[i] + 2;
    }
    return language && (!strcmp(language, "c") || !strcmp(language, "c++") ||
                       !strcmp(language, "objective-c") ||
                       !strcmp(language, "objective-c++")) ? language : NULL;
}

/**
 * If the input filename is a plain source file rather than a
 * preprocessed source file, then preprocess it to a temporary file
 * and return the name in @p cpp_fname.
 *
 * The preprocessor may still be running when we return; you have to
 * wait for @p cpp_fid to exit before the output is complete.  This
 * allows us to overlap opening the TCP socket, which probably doesn't
 * use many cycles, with running the preprocessor.
 **/
int dcc_cpp_maybe(char **argv, char *input_fname, char **cpp_fname,
          pid_t *cpp_pid, int *cpp_fd)
{
    char **cpp_argv;
    int ret;
    char *input_exten;
    const char *output_exten;
    const char *output = NULL;
    char *deps = NULL, *target = NULL;
    int needs_deps = 0, sets_target = 0, i;

    *cpp_pid = 0;

    if (dcc_is_preprocessed(input_fname)) {
        /* TODO: Perhaps also consider the option that says not to use cpp.
         * Would anyone do that? */
        rs_trace("input is already preprocessed");

        /* already preprocessed, great. */
        if (!(*cpp_fname = strdup(input_fname))) {
            rs_log_error("couldn't duplicate string");
            return EXIT_OUT_OF_MEMORY;
        }
        return 0;
    }

    input_exten = dcc_find_extension(input_fname);
    output_exten = dcc_preproc_exten(input_exten);
#ifdef __linux__
    if (cpp_fd && !dcc_getenv_bool("DISTCC_SAVE_TEMPS", 0)) {
        *cpp_fd = memfd_create("distcc-preprocessed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (*cpp_fd < 0) return EXIT_IO_ERROR;
        if (asprintf(cpp_fname, "/proc/self/fd/%d", *cpp_fd) < 0) {
            *cpp_fname = NULL;
            return EXIT_OUT_OF_MEMORY;
        }
    } else
#else
    (void)cpp_fd;
#endif
    {
        if ((ret = dcc_make_tmpnam("distcc", output_exten, cpp_fname)))
            return ret;
    }

    /* CPP writes to stdout. Preserve the dependency filename and target that
     * the original -o would otherwise select. argv has a normalized -o. */
    for (i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "-o") && argv[i + 1]) output = argv[++i];
        else if (!strcmp(argv[i], "-MD") || !strcmp(argv[i], "-MMD")) needs_deps = 1;
    }
    if (needs_deps && (ret = dcc_get_dotd_info(argv, &deps, &needs_deps,
                                             &sets_target, &target)))
        return ret;

    if ((ret = dcc_strip_dasho(argv, &cpp_argv))
        || (ret = dcc_set_action_opt(cpp_argv, "-E")))
        { free(deps); return ret; }

    {
        char **expanded = realloc(cpp_argv, (dcc_argv_len(cpp_argv) + 6) * sizeof(*cpp_argv));
        if (!expanded) { free(cpp_argv); free(deps); return EXIT_OUT_OF_MEMORY; }
        cpp_argv = expanded;
    }
    if (dcc_cpp_rewrite_language(argv, input_fname))
        dcc_argv_append(cpp_argv, (char *)"-frewrite-includes");
    if (needs_deps) {
        dcc_argv_append(cpp_argv, (char *)"-MF");
        dcc_argv_append(cpp_argv, deps);
        if (!sets_target && !target && output) {
            dcc_argv_append(cpp_argv, (char *)"-MQ");
            dcc_argv_append(cpp_argv, (char *)output);
        }
    }
    ret = spawn_preprocessor(cpp_argv, cpp_pid, *cpp_fname);
    free(cpp_argv);
    free(deps);
    return ret;
}
