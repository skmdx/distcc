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


                /* "I have a bone to pick, and a few to break." */

/**
 * @file
 *
 * Functions for understanding and manipulating argument vectors.
 *
 * The few options explicitly handled by the client are processed in its
 * main().  At the moment, this is just --help and --version, so this function
 * never has to worry about them.
 *
 * We recognize two basic forms "distcc gcc ..." and "distcc ...", with no
 * explicit compiler name.  This second one is used if you have a Makefile
 * that can't manage two-word values for $CC; eventually it might support
 * putting a link to distcc on your path as 'gcc'.  We call this second one an
 * implicit compiler.
 *
 * We need to distinguish the two by working out whether the first argument
 * "looks like" a compiler name or not.  I think the two cases in which we
 * should assume it's implicit are "distcc -c hello.c" (starts with a hyphen),
 * and "distcc hello.c" (starts with a source filename.)
 *
 * In the case of implicit compilation "distcc --help" will always give you
 * distcc's help, not gcc's, and similarly for --version.  I don't see much
 * that we can do about that.
 *
 * @todo We don't need to run the full argument scanner on the server, only
 * something simple to recognize input and output files.  That would perhaps
 * make the function simpler, and also mean that if argument recognizer bugs
 * are fixed in the future, they only need to be fixed on the client, not on
 * the server.  An even better solution is to have the client tell the server
 * where to put the input and output files.
 *
 * @todo Perhaps make the argument parser driven by a data table.  (Would that
 * actually be clearer?)  Perhaps use regexps to recognize strings.
 *
 * @todo We could also detect options like "-x cpp-output" or "-x
 * assembler-with-cpp", because they should override language detection based
 * on extension.  I haven't seen anyone use them yet though.  In fact, since
 * we don't assemble remotely it is moot for the only reported case, the
 * Darwin C library.  We would also need to update the option when passing it
 * to the server.
 *
 * @todo Perhaps assume that assembly code will not use both #include and
 * .include, and therefore if we preprocess locally we can distribute the
 * compilation?  Assembling is so cheap that it's not necessarily worth
 * distributing.
 **/


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <dlfcn.h>

#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"
#include "snprintf.h"

/* Offload preprocessing is a compiler-generated bundle of target-specific
 * translation units. It must not go through the single-frontend pump scanner. */
const char *dcc_offload_language(char *const argv[])
{
    const char *language = NULL;
    int amdgpu = 0;
    if (!strstr(dcc_find_basename(argv[0]), "clang")) return NULL;
    for (int i = 1; argv[i]; ++i) {
        if (!strcmp(argv[i], "-x") && argv[i + 1]) language = argv[++i];
        else if (!strncmp(argv[i], "-x", 2) && argv[i][2]) language = argv[i] + 2;
        else if ((!strcmp(argv[i], "-target") || !strcmp(argv[i], "--target")) && argv[i + 1])
            amdgpu = !strncmp(argv[++i], "amdgcn", 6);
        else if (!strncmp(argv[i], "--target=", 9)) amdgpu = !strncmp(argv[i] + 9, "amdgcn", 6);
        else if (argv[i][0] != '-') {
            const char *ext = dcc_find_extension_const(argv[i]);
            if (!language && ext) {
                if (!strcmp(ext, ".hip") || !strcmp(ext, ".hipi")) language = "hip";
                else if (!strcmp(ext, ".cl") || !strcmp(ext, ".cli")) language = "cl";
                else if (!strcmp(ext, ".clcpp") || !strcmp(ext, ".clii")) language = "clcpp";
                else if (dcc_is_source(argv[i])) {
                    const char *preprocessed = dcc_preproc_exten(ext);
                    language = (strstr(dcc_find_basename(argv[0]), "++") ||
                                (preprocessed && !strcmp(preprocessed, ".ii"))) ? "c++" : "c";
                }
            }
        }
    }
    if (language && !strncmp(language, "hip", 3)) return "hip-cpp-output";
    if (!amdgpu) return NULL;
    if (language && !strncmp(language, "clcpp", 5)) return "clcpp-cpp-output";
    if (language && !strncmp(language, "cl", 2)) return "cl-cpp-output";
    return language && (!strncmp(language, "c++", 3)) ? "c++-cpp-output" : "cpp-output";
}

static void *dcc_clang_driver(void)
{
    const char *library = getenv("DISTCC_CLANG_LIBRARY");
    void *handle = dlopen(library && *library ? library : LIBDIR "/distcc/clang_driver.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) rs_log_warning("cannot load Clang driver: %s", dlerror());
    return handle;
}

int dcc_expand_response_files(char ***argv)
{
    char **p = *argv + 1;
    while (*p && **p != '@') ++p;
    if (!*p) return 0;
    void *handle = dcc_clang_driver();
    if (!handle) return EXIT_DISTCC_FAILED;
    int (*expand)(char **, char ***) = dlsym(handle, "dcc_clang_expand_response_files");
    char **expanded = NULL;
    int ret = expand ? expand(*argv, &expanded) : -1;
    dlclose(handle);
    if (ret) return EXIT_DISTCC_FAILED;
    dcc_free_argv(*argv);
    *argv = expanded;
    return 0;
}

int dcc_prepare_clang(char ***argv, char ***files)
{
    int needed = 0;
    if (dcc_offload_language(*argv)) return 0;
    if (!strstr(dcc_find_basename((*argv)[0]), "clang")) return 0;
    for (char **p = *argv + 1; *p; ++p)
        if (str_endswith(".a", *p) ||
            (str_startswith("-fprofile", *p) && !str_startswith("-fprofile-generate", *p)) ||
            str_startswith("-fauto-profile", *p) ||
            str_startswith("-fmemory-profile-use", *p)) needed = 1;
    if (!needed) return 0;
    void *handle = dcc_clang_driver();
    if (!handle) return EXIT_DISTCC_FAILED;
    int (*inputs)(char **, char ***, char ***) = dlsym(handle, "dcc_clang_inputs");
    char **normalized = NULL;
    int ret = inputs ? inputs(*argv, &normalized, files) : -1;
    dlclose(handle);
    if (ret) return EXIT_DISTCC_FAILED;
    dcc_free_argv(*argv);
    *argv = normalized;
    return 0;
}


int dcc_argv_append(char **argv, char *toadd)
{
    int l = dcc_argv_len(argv);
    argv[l] = toadd;
    argv[l+1] = NULL;           /* just make sure */
    return 0;
}

static void dcc_note_compiled(const char *input_file, const char *output_file)
{
    const char *input_base, *output_base;

    input_base = dcc_find_basename(input_file);
    output_base = dcc_find_basename(output_file);

    rs_log(RS_LOG_INFO|RS_LOG_NONAME,
           "compile from %s to %s", input_base, output_base);
}

/* These operands are passed to another compiler layer. They must not be
 * interpreted or rewritten as driver input, output or action options. */
int dcc_is_forward_option(const char *arg)
{
    return !strcmp(arg, "-Xclang") || !strcmp(arg, "-mllvm");
}

/**
 * Parse arguments, extract ones we care about, and also work out
 * whether it will be possible to distribute this invocation remotely.
 *
 * This is a little hard because the cc argument rules are pretty complex, but
 * the function still ought to be simpler than it already is.
 *
 * This code is called on both the client and the server, though they use the
 * results differently.
 *
 * This function makes a copy of the arguments, modified to ensure that
 * the arguments include '-o <filename>', except for -E output to stdout.
 * This is returned in *ret_newargv.
 * The copy is dynamically allocated and the caller is responsible for
 * deallocating it.
 *
 * @returns 0 if it's ok to distribute this compilation, or an error code.
 **/
int dcc_scan_args(char *argv[], char **input_file, char **output_file,
                  char ***ret_newargv, int allow_native)
{
    int seen_opt_c = 0, seen_opt_s = 0, seen_opt_e = 0;
    int seen_md = 0, seen_mf = 0;
    int i;
    char *a;
    int ret;

     /* allow for -o foo.o */
    if ((ret = dcc_copy_argv(argv, ret_newargv, 4)) != 0)
        return ret;
    argv = *ret_newargv;

    /* FIXME: new copy of argv is leaked */

    dcc_trace_argv("scanning arguments", argv);

    /* Things like "distcc -c hello.c" with an implied compiler are
     * handled earlier on by inserting a compiler name.  At this
     * point, argv[0] should always be a compiler name. */
    if (argv[0][0] == '-') {
        rs_log_error("unrecognized distcc option: %s", argv[0]);
        exit(EXIT_BAD_ARGUMENTS);
    }

    *input_file = *output_file = NULL;

    for (i = 0; (a = argv[i]); i++) {
        if (a[0] == '-') {
            if (!strcmp(a, "-E")) {
                seen_opt_e = 1;
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {
                seen_md = 1;
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || !strcmp(a, "-MT") ||
                       !strcmp(a, "-MQ")) {
                if (!strcmp(a, "-MF")) {
                    seen_mf = 1;
                    if (argv[i+1] && !strcmp(argv[i+1], "-"))
                        return EXIT_LOCAL_CPP;
                }
                /* As above but with extra argument. */
                i++;
            } else if (!strncmp(a, "-MF", 3) || !strncmp(a, "-MT", 3) ||
                       !strncmp(a, "-MQ", 3)) {
                if (!strncmp(a, "-MF", 3)) {
                    seen_mf = 1;
                    if (!strcmp(a + 3, "-")) return EXIT_LOCAL_CPP;
                }
                /* As above, without extra argument. */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                    produce a list of make-style dependencies on
                    header files, either to stdout or to a local file.
                    It implies -E, so only the preprocessor is run,
                    not the compiler.  There would be no point trying
                    to distribute it even if we could. */
                rs_trace("%s implies -E (maybe) and must be local", a);
                return EXIT_DISTCC_FAILED;
            } else if (dcc_is_forward_option(a)) {
                /* A frontend argument is not a driver option (e.g. -xop
                 * is a target feature, not the driver's -x language). */
                if (!argv[++i]) return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-march=native")) {
                if (allow_native) continue;
                rs_trace("-march=native generates code for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-mtune=native") || !strcmp(a, "-mcpu=native")) {
                if (allow_native) continue;
                rs_trace("-mtune=native optimizes for local machine; "
                         "must be local");
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-Wa,", a)) {
                /* Look for assembler options that would produce output
                 * files and must be local.
                 *
                 * Writing listings to stdout could be supported but it might
                 * be hard to parse reliably. */
                if (strstr(a, ",-a") || strstr(a, "--MD")) {
                    rs_trace("%s must be local", a);
                    return EXIT_DISTCC_FAILED;
                }
            } else if (!strcmp(a, "-S")) {
                seen_opt_s = 1;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")
		       || !strcmp(a, "--coverage")
		       || ((!strcmp(a, "-fprofile-generate") ||
                            str_startswith("-fprofile-generate=", a)) &&
                           !strstr(dcc_find_basename(argv[0]), "clang"))
		       || ((!strncmp(a, "-fprofile-use", 13) ||
                            !strncmp(a, "-fauto-profile", 14)) &&
                           !strstr(dcc_find_basename(argv[0]), "clang"))
		       || !strcmp(a, "-fprofile-correction")) {
                rs_log_info("compiler will emit/use profile info; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-frepo")) {
                rs_log_info("compiler will emit .rpo files; must be local");
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-fmodules-ts") || !strcmp(a, "-fmodules") ||
                       str_startswith("-fmodule-", a) || str_startswith("-fdeps-", a)) {
                /* Module scans and compilation exchange local mapper, BMI and
                 * dependency files beyond the object/depfile RPC contract. */
                rs_trace("%s requires local module artifacts", a);
                return EXIT_DISTCC_FAILED;
            } else if (str_startswith("-x", a)) {
                const char *language = a[2] ? a + 2 : argv[++i];
                if (!language ||
                    (!str_startswith("c", language) &&
                     !str_startswith("objective-c", language) &&
                     !str_startswith("go", language) &&
                     !str_startswith("hip", language))) {
                    rs_log_info("unsupported distributed language: %s", language ? language : "empty");
                    return EXIT_DISTCC_FAILED;
                }
            } else if (str_startswith("-dr", a)) {
                rs_log_info("gcc's debug option %s may write extra files; "
                            "running locally", a);
                return EXIT_DISTCC_FAILED;
            } else if (!strcmp(a, "-c")) {
                seen_opt_c = 1;
            } else if (!strcmp(a, "-o")) {
                /* Whatever follows must be the output */
                a = argv[++i];
                goto GOT_OUTPUT;
            } else if (str_startswith("-o", a)) {
                a += 2;         /* skip "-o" */
                goto GOT_OUTPUT;
            }
        } else {
            if (dcc_is_source(a)) {
                rs_trace("found input file \"%s\"", a);
                if (*input_file) {
                    rs_log_info("do we have two inputs?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *input_file = a;
            } else if (str_endswith(".o", a)) {
              GOT_OUTPUT:
                rs_trace("found object/output file \"%s\"", a);
                if (*output_file) {
                    rs_log_info("called for link?  i give up");
                    return EXIT_DISTCC_FAILED;
                }
                *output_file = a;
            }
        }
    }

    /* TODO: ccache has the heuristic of ignoring arguments that are not
     * extant files when looking for the input file; that's possibly
     * worthwhile.  Of course we can't do that on the server. */

    /* With -E -MD, -o names the dependency file unless -MF is explicit. */
    if (seen_opt_e && seen_md && !seen_mf)
        return EXIT_LOCAL_CPP;

    if (!seen_opt_c && !seen_opt_s && !seen_opt_e) {
        rs_log_info("compiler apparently called not for compile");
        return EXIT_DISTCC_FAILED;
    }

    if (!*input_file) {
        rs_log_info("no visible input file");
        return EXIT_DISTCC_FAILED;
    }

    if (dcc_source_needs_local(*input_file))
        return EXIT_DISTCC_FAILED;

    if (seen_opt_e && !*output_file) {
        static char stdout_name[] = "-";
        *output_file = stdout_name;
        dcc_note_compiled(*input_file, *output_file);
        return 0;
    }

    if (!*output_file) {
        /* This is a commandline like "gcc -c hello.c".  They want
         * hello.o, but they don't say so.  For example, the Ethereal
         * makefile does this.
         *
         * Note: this doesn't handle a.out, the other implied
         * filename, but that doesn't matter because it would already
         * be excluded by not having -c or -S.
         */
        char *ofile;

        /* -S takes precedence over -c, because it means "stop after
         * preprocessing" rather than "stop after compilation." */
        if (seen_opt_s) {
            if (dcc_output_from_source(*input_file, ".s", &ofile))
                return EXIT_DISTCC_FAILED;
        } else if (seen_opt_c) {
            if (dcc_output_from_source(*input_file, ".o", &ofile))
                return EXIT_DISTCC_FAILED;
        } else {
            rs_log_crit("this can't be happening(%d)!", __LINE__);
            return EXIT_DISTCC_FAILED;
        }
        rs_log_info("no visible output file, going to add \"-o %s\" at end",
                      ofile);
        dcc_argv_append(argv, strdup("-o"));
        dcc_argv_append(argv, ofile);
        *output_file = ofile;
    }

    dcc_note_compiled(*input_file, *output_file);

    if (!seen_opt_e && strcmp(*output_file, "-") == 0) {
        /* Different compilers may treat "-o -" as either "write to
         * stdout", or "write to a file called '-'".  We can't know,
         * so we just always run it locally.  Hopefully this is a
         * pretty rare case. */
        rs_log_info("output to stdout?  running locally");
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}



/**
 * Used to change "-c" or "-S" to "-E", so that we get preprocessed
 * source.
 **/
int dcc_set_action_opt(char **a, const char *new_c)
{
    int gotone = 0;

    for (; *a; a++)
        if (dcc_is_forward_option(*a) && a[1]) {
            a++;
        } else if (!strcmp(*a, "-c") || !strcmp(*a, "-S")) {
            *a = strdup(new_c);
            if (*a == NULL) {
                rs_log_error("strdup failed");
                exit(EXIT_OUT_OF_MEMORY);
            }
            gotone = 1;
            /* keep going; it's not impossible they wrote "gcc -c -c
             * -c hello.c" */
        }

    if (!gotone) {
        rs_log_error("failed to find -c or -S");
        return EXIT_DISTCC_FAILED;
    } else {
        return 0;
    }
}



/**
 * Change object file or suffix of -o to @p ofname
 * Frees the old value, if it exists.
 *
 * It's crucially important that in every case where an output file is
 * detected by dcc_scan_args(), it's also correctly identified here.
 * It might be better to make the code shared.
 **/
int dcc_set_output(char **a, char *ofname)
{
    int i;

    for (i = 0; a[i]; i++)
        if (dcc_is_forward_option(a[i]) && a[i+1]) {
            i++;
        } else if (0 == strcmp(a[i], "-o") && a[i+1] != NULL) {
            rs_trace("changed output from \"%s\" to \"%s\"", a[i+1], ofname);
            free(a[i+1]);
            a[i+1] = strdup(ofname);
            if (a[i+1] == NULL) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        } else if (0 == strncmp(a[i], "-o", 2)) {
            char *newptr;
            rs_trace("changed output from \"%s\" to \"%s\"", a[i]+2, ofname);
            free(a[i]);
            if (asprintf(&newptr, "-o%s", ofname) == -1) {
                rs_log_crit("failed to allocate space for output parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            a[i] = newptr;
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find \"-o\"");
    return EXIT_DISTCC_FAILED;
}

/**
 * Change input file to a copy of @p ifname; called on compiler.
 * Frees the old value.
 *
 * @todo Unify this with dcc_scan_args
 *
 * @todo Test this by making sure that when the modified arguments are
 * run through scan_args, the new ifname is identified as the input.
 **/
int dcc_set_input(char **a, char *ifname)
{
    int i;

    for (i =0; a[i]; i++)
        if (dcc_is_forward_option(a[i]) && a[i+1]) {
            i++;
        } else if (dcc_is_source(a[i])) {
            rs_trace("changed input from \"%s\" to \"%s\"", a[i], ifname);
            free(a[i]);
            a[i] = strdup(ifname);
            if (a[i] == NULL) {
                rs_log_crit("failed to allocate space for input parameter");
                return EXIT_OUT_OF_MEMORY;
            }
            dcc_trace_argv("command after", a);
            return 0;
        }

    rs_log_error("failed to find input file");
    return EXIT_DISTCC_FAILED;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Calculate how many extra arguments we'll need to convert
 * a "-Wp,..." option into regular gcc options.
 * Returns the number of extra arguments needed.
 */
static int count_extra_args(char *dash_Wp_option) {
    int extra_args = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;
        comma = strchr(opt, ',');
        if (str_startswith("-MD,", opt) ||
            str_startswith("-MMD,", opt))
        {
            char *filename = comma + 1;
            comma = strchr(filename, ',');
            extra_args += 3;  /* "-MD", "-MF", filename. */
        } else {
            extra_args++;
        }
    }
    return extra_args;
}

/* Subroutine of dcc_expand_preprocessor_options().
 * Convert a "-Wp,..." option into one or more regular gcc options.
 * Copy the resulting gcc options to dest_argv, which should be
 * pre-allocated by the caller.
 * Destructively modifies dash_Wp_option as it goes.
 * Returns 0 on success, nonzero for error (out of memory).
 */
static int copy_extra_args(char **dest_argv, char *dash_Wp_option,
                           int extra_args) {
    int i = 0;
    char *comma = dash_Wp_option + strlen("-Wp");
    while (comma != NULL) {
        char *opt = comma + 1;
        comma = strchr(opt, ',');
        if (comma) *comma = '\0';
        dest_argv[i] = strdup(opt);
        if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
        i++;
        if (strcmp(opt, "-MD") == 0 || strcmp(opt, "-MMD") == 0) {
            char *filename;
            if (!comma) {
                rs_log_warning("'-Wp,-MD' or '-Wp,-MMD' option is missing "
                               "filename argument");
                break;
            }
            filename = comma + 1;
            comma = strchr(filename, ',');
            if (comma) *comma = '\0';
            dest_argv[i] = strdup("-MF");
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
            dest_argv[i] = strdup(filename);
            if (!dest_argv[i]) return EXIT_OUT_OF_MEMORY;
            i++;
        }
    }
    assert(i == extra_args);
    return 0;
}


/*
 * Convert any "-Wp," options into regular gcc options.
 * We do this because it simplifies the command-line
 * option handling elsewhere; this is the only place
 * that needs to parse "-Wp," options.
 * Returns 0 on success, nonzero for error (out of memory).
 *
 * The argv array pointed to by argv_ptr when this function
 * is called must have been dynamically allocated.  It remains
 * the caller's responsibility to deallocate it.
 */
int dcc_expand_preprocessor_options(char ***argv_ptr) {
    int i, j, ret;
    char **argv = *argv_ptr;
    char **new_argv;
    int argc = dcc_argv_len(argv);
    for (i = 0; argv[i]; i++) {
        if (str_startswith("-Wp,", argv[i])) {
            /* First, calculate how many extra arguments we'll need. */
            int extra_args = count_extra_args(argv[i]);
            assert(extra_args >= 1);

            new_argv = calloc(argc + extra_args, sizeof(char *));
            if (!new_argv) {
                return EXIT_OUT_OF_MEMORY;
            }
            for (j = 0; j < i; j++) {
                new_argv[j] = argv[j];
            }
            if ((ret = copy_extra_args(new_argv + i, argv[i],
                                       extra_args)) != 0) {
                free(new_argv);
                return ret;
            }
            for (j = i + 1; j <= argc; j++) {
                new_argv[j + extra_args - 1] = argv[j];
            }
            free(argv);
            *argv_ptr = argv = new_argv;
            i += extra_args - 1;
            argc += extra_args - 1;
        }
    }
    return 0;
}
