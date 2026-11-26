/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
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
#include <string.h>
#include <stdlib.h>

#include "distcc.h"
#include "exitcode.h"
#include "dotd.h"
#include "snprintf.h"

/* GCC separates Make names with spaces and quotes embedded whitespace
 * with an odd number of backslashes. Keep the original quoted bytes. */
static size_t make_word_length(const char *word)
{
    const char *end = word;
    while (*end && *end != ' ') {
        if (*end == '\\' && end[1]) ++end;
        ++end;
    }
    return end - word;
}

/* Given the name of a dotd file, and the name of the directory
 * masquerading as root, write a @p new_dotd file that
 * contains everything in dotd, but with the "root" directory removed.
 * It will also substitute client_out_name for server_out_name,
 * rewriting the dependency target.
 */
int dcc_cleanup_dotd(const char *dotd_fname,
                     char **new_dotd_fname,
                     const char *root_dir,
                     const char *client_out_name,
                     const char *server_out_name)
{
    FILE *dotd, *tmp_dotd;
    int ret;

    dotd = fopen(dotd_fname, "r");
    if (dotd == NULL) {
        return 1;
    }
    ret = dcc_make_tmpnam(dcc_find_basename(dotd_fname),
                          ".d", new_dotd_fname);

    if (ret) {
        fclose(dotd);
        return ret;
    }

    tmp_dotd = fopen(*new_dotd_fname, "w");
    if (tmp_dotd == NULL) {
        fclose(dotd);
        return 1;
    }

    char *line = NULL;
    size_t capacity = 0, column = 0, root_len = strlen(root_dir);
    size_t target_len = server_out_name ? strlen(server_out_name) : 0;
    int continued = 0, relocated = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, dotd)) >= 0) {
        int wrap = length >= 3 && !memcmp(line + length - 3, " \\\n", 3);
        if (wrap) line[length - 3] = '\0';
        else if (length && line[length - 1] == '\n') line[length - 1] = '\0';
        for (const char *next = line; *next;) {
            int space = *next == ' ';
            const char *word = next + space;
            size_t size = make_word_length(word);
            next = word + size;
            int colon = 0;
            if (size > root_len && word[root_len] == '/' && !memcmp(word, root_dir, root_len)) {
                word += root_len;
                size -= root_len;
                relocated = 1;
            } else if (target_len && (size == target_len ||
                       (size == target_len + 1 && word[target_len] == ':')) &&
                       !memcmp(word, server_out_name, target_len)) {
                colon = size != target_len;
                word = client_out_name;
                size = strlen(word);
                relocated = 1;
            }
            if (space) {
                /* libcpp wraps before a name when column + quoted length
                 * exceeds 72. Its original breaks used worker path lengths. */
                if (relocated ? column + size > 72 : continued) {
                    fputs(" \\\n", tmp_dotd);
                    column = 0;
                }
                fputc(' ', tmp_dotd);
                ++column;
            }
            fwrite(word, 1, size, tmp_dotd);
            if (colon) fputc(':', tmp_dotd);
            column += size + colon;
            continued = 0;
        }
        continued = wrap;
        if (!wrap) {
            fputc('\n', tmp_dotd);
            column = 0;
            relocated = 0;
        }
    }
    ret = !feof(dotd) || ferror(dotd) || ferror(tmp_dotd);
    free(line);
    fclose(dotd);
    return fclose(tmp_dotd) || ret;
}

/* Go through arguments (in @p argv), and relevant environment variables, and
 * find out where the dependencies output should go.  Return that location in a
 * newly allocated string in @p dotd_fname.  @p needs_dotd is set to true if the
 * compilation command line and environment imply that a .d file must be
 * produced.  @p sets_dotd_target is set to true if there is a -MQ or -MT
 * option.  This is to be used on the client, so that the client knows where to
 * put the .d file it gets from the server. @p dotd_target is set only if
 * @needs_dotd is true and @sets_dotd_target is false and the target is given in
 * the DEPENDENCIES_OUTPUT environment variable.
 *
 * Note: -M is not handled here, because this option implies -E.
 *
 * TODO(manos): it does not support SUNPRO_DEPENDENCIES.
 */
int dcc_get_dotd_info(char **argv, char **dotd_fname,
                      int *needs_dotd, int *sets_dotd_target,
                      char **dotd_target)
{
    char *deps_output = 0;

    char *input_file;
    char *output_file;
    char **new_args = NULL;  /* will throw this away */
    int has_dash_o = 0;
    char *env_var = 0;
    int i, ret;
    char *a;

    *needs_dotd = 0;
    *sets_dotd_target = 0;
    *dotd_target = NULL;

    env_var = getenv("DEPENDENCIES_OUTPUT");

    if (env_var != NULL) {
        *needs_dotd = 1;
    }

    for (i = 0; (a = argv[i]); i++) {
        if (strncmp(a, "-MT", 3) == 0) {
            *sets_dotd_target = 1;
            if (!a[3]) ++i;
            continue;
        }
        if (strncmp(a, "-MQ", 3) == 0) {
            *sets_dotd_target = 1;
            if (!a[3]) ++i;
            continue;
        }
        /* Catch-all for all -MD, -MMD, etc, options.
         * -MQ and -MT do not imply a deps file is expected.
         */
        if (strncmp(a, "-M", 2) == 0) {
            *needs_dotd = 1;
        }
        if (strcmp(a, "-MF") == 0) {
            ++i;
            deps_output = argv[i];
        } else if (strncmp(a, "-MF", 3) == 0) {
            deps_output = argv[i] + 3;
        } else if (strncmp(a, "-o", 2) == 0) {
            has_dash_o = 1;
        }
    }

    /* TODO(csilvers): we also need to parse -Wp,-x,-y,-z, in the same
     * way we do gcc flags in the for loop above.  Note that the -Wp
     * flags are passed to cpp, with slightly different semantics than
     * gcc flags (eg -Wp,-MD takes a filename argument, while -MD does
     * not).
     */

    if (deps_output) {
        *dotd_fname = strdup(deps_output);
        if (*dotd_fname == NULL) {
            return EXIT_OUT_OF_MEMORY;
        } else {
            return 0;
        }
    }

    /* ok, so there is no explicit setting of the deps filename. */
    deps_output = env_var;
    if (deps_output) {
        char *space;
        *dotd_fname = strdup(deps_output);
        if (*dotd_fname == NULL) {
            return EXIT_OUT_OF_MEMORY;
        }
        space = strchr(*dotd_fname, ' ');
        if (space != NULL) {
            *space = '\0';
            *dotd_target = space + 1;
        }

        return 0;
    }

    /* and it's not set explicitly in the variable */

    { /* Call dcc_scan_args to find the input/output files in order to calculate
         a name for the .d file.*/

        char *extension;
        char *tmp_dotd_fname;
        /* This only derives filenames. Native flags are resolved by the
         * compilation path, including the remote frontend's driver. */
        ret = dcc_scan_args(argv, &input_file, &output_file, &new_args, 1);
        if (ret) {
          dcc_free_argv(new_args);
          return ret;
        }
        /* if .o is set, just append .d.
         * otherwise, take the basename of the input, and set the suffix to .d */
        if (has_dash_o)
          tmp_dotd_fname = strdup(output_file);
        else
          tmp_dotd_fname = strdup(input_file);
        dcc_free_argv(new_args);
        if (tmp_dotd_fname == NULL) return EXIT_OUT_OF_MEMORY;
        extension = dcc_find_extension(tmp_dotd_fname);
        /* Whether derived from input or output filename, we peel the extension
           off (if it exists). */
        if (extension) {
          /* dcc_find_extension guarantees that there is space for 'd'. */
          extension[1] = 'd';
          extension[2] = '\0';
          *dotd_fname = tmp_dotd_fname;
        }
        else { /* There is no extension (or name ends with a "."). */
          if (tmp_dotd_fname[strlen(tmp_dotd_fname) - 1] == '.')
            checked_asprintf(dotd_fname, "%s%s", tmp_dotd_fname, "d");
          else
            checked_asprintf(dotd_fname, "%s%s", tmp_dotd_fname, ".d");
          if (*dotd_fname == NULL) {
            return EXIT_OUT_OF_MEMORY;
          }
          free(tmp_dotd_fname);
        }
        return 0;
    }
}
