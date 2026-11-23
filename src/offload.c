/* Prepare Clang's bundled, preprocessed AMDGPU inputs for ordinary TCP jobs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "distcc.h"
#include "helpers.h"
#include "exitcode.h"
#include "trace.h"

static int append(char ***list, const char *value)
{
    int n = *list ? dcc_argv_len(*list) : 0;
    char **grown = realloc(*list, (n + 2) * sizeof(*grown));
    if (!grown) return EXIT_OUT_OF_MEMORY;
    *list = grown;
    grown[n] = NULL;
    grown[n + 1] = NULL;
    return (grown[n] = strdup(value)) ? 0 : EXIT_OUT_OF_MEMORY;
}

static const char *option(char **args, const char *name)
{
    for (int i = 1; args[i] && args[i + 1]; ++i)
        if (!strcmp(args[i], name)) return args[i + 1];
    return NULL;
}

/* Scope CPU options to the host action. Passing -Xclang globally would also
 * override the GPU's target-cpu/features and break device code generation. */
static int host_native(char ***args, char **host)
{
    const char *triple = option(host, "-triple");
    int x86 = triple && (!strncmp(triple, "x86", 3) || !strncmp(triple, "i386", 4));
    for (int i = 1; host[i] && host[i + 1]; ++i) {
        char *flag = NULL;
        if (!strcmp(host[i], "-target-cpu")) {
            if (asprintf(&flag, "%s%s", x86 ? "-march=" : "-mcpu=", host[++i]) < 0)
                return EXIT_OUT_OF_MEMORY;
        } else if (!strcmp(host[i], "-tune-cpu")) {
            if (asprintf(&flag, "-mtune=%s", host[++i]) < 0) return EXIT_OUT_OF_MEMORY;
        } else if (x86 && !strcmp(host[i], "-target-feature")) {
            const char *feature = host[++i];
            /* The triple owns the width; -m64 is not an Xarch_host option. */
            if (!strcmp(feature + 1, "64bit")) continue;
            if (asprintf(&flag, "-m%s%s", *feature == '-' ? "no-" : "", feature + 1) < 0)
                return EXIT_OUT_OF_MEMORY;
        }
        if (flag) {
            int ret = append(args, "-Xarch_host") || append(args, flag);
            free(flag);
            if (ret) return EXIT_OUT_OF_MEMORY;
        }
    }
    return 0;
}

int dcc_prepare_offload(char ***argvp, char ***files, int brokered, int *use_graph)
{
    char **argv = *argvp, **expanded = NULL, **inputs = NULL, **host = NULL, **gpus = NULL;
    char *text = NULL, *compiler = NULL, *cuid = NULL, *line, *next;
    int ret = EXIT_DISTCC_FAILED, native = dcc_native_requested(argv);
    int hip = !strcmp(dcc_offload_language(argv), "hip-cpp-output");
    int gpu_native = dcc_argv_search(argv, "--offload-arch=native");
    /* The coordinator owns its driver's temporary paths and consumes the
     * selected -mlink-builtin-bitcode operands directly from each cc1. */
    if ((hip || (brokered && !dcc_argv_search(argv, "-nogpulib"))) &&
        !dcc_argv_search(argv, "--cuda-host-only") && !dcc_argv_search(argv, "--offload-host-only")) {
        *use_graph = 1;
        return 0;
    }
    if (dcc_read_driver(argv, &text)) goto out;
    for (line = text; line && *line; line = next) {
        char **words;
        const char *triple;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (!isspace((unsigned char)*line)) continue;
        if (!(words = dcc_driver_words(line))) goto out;
        if (!words[0] || !words[1] || strcmp(words[1], "-cc1")) { free(words); continue; }
        if (!compiler) compiler = realpath(words[0], NULL);
        for (int i = 2; hip && !cuid && words[i]; ++i)
            if (!strncmp(words[i], "-cuid=", 6)) {
                if (!(cuid = strdup(words[i]))) { free(words); goto out; }
            }
        triple = option(words, "-triple");
        if (triple && !strncmp(triple, "amdgcn", 6)) {
            if (hip || option(words, "-mlink-builtin-bitcode")) {
                free(words);
                *use_graph = brokered || hip;
                ret = *use_graph ? 0 : EXIT_LOCAL_COMPILE;
                goto out;
            }
            if (gpu_native) {
                const char *cpu = option(words, "-target-cpu");
                char *flag = NULL;
                if (!cpu || asprintf(&flag, "--offload-arch=%s", cpu) < 0) { free(words); goto out; }
                int error = append(&gpus, flag);
                free(flag);
                if (error) { free(words); goto out; }
            }
            free(words);
        } else {
            const char *binary = option(words, "-fcuda-include-gpubinary");
            if (binary && access(binary, R_OK) == 0 && append(&inputs, binary)) {
                free(words);
                goto out;
            }
            free(host);
            host = words;
        }
    }
    if (!compiler) goto out;
    if (append(&expanded, compiler)) goto out;
    if (strstr(dcc_find_basename(argv[0]), "++") && append(&expanded, "--driver-mode=g++")) goto out;
    for (int i = 1; argv[i]; ++i) {
        if (gpu_native && !strcmp(argv[i], "--offload-arch=native")) continue;
        if (hip && !strcmp(argv[i], "-Xarch_host") && argv[i + 1] &&
            (!strcmp(argv[i + 1], "-march=native") || !strcmp(argv[i + 1], "-mcpu=native") ||
             !strcmp(argv[i + 1], "-mtune=native"))) { ++i; continue; }
        if (hip && (!strcmp(argv[i], "-march=native") || !strcmp(argv[i], "-mcpu=native") ||
                    !strcmp(argv[i], "-mtune=native"))) continue;
        if (append(&expanded, argv[i])) goto out;
    }
    for (int i = 0; gpus && gpus[i]; ++i)
        if (append(&expanded, gpus[i])) goto out;
    /* A preprocessed HIP bundle no longer makes the driver generate a
     * compilation-unit ID. Pass the original ID directly to both frontends,
     * including device-only actions and externalized static symbols. */
    if (cuid && (append(&expanded, "-Xclang") || append(&expanded, cuid))) goto out;
    if (hip && host) {
        const char *triple = option(host, "-triple");
        char *flag;
        if (!triple || asprintf(&flag, "--target=%s", triple) < 0) goto out;
        int error = append(&expanded, flag);
        free(flag);
        if (error || (native && host_native(&expanded, host))) goto out;
    }
    if (!hip && native) {
        char *input = NULL, *output = NULL, **scanned = NULL;
        ret = dcc_scan_args(expanded, &input, &output, &scanned, 1);
        if (!ret) ret = dcc_expand_native(&expanded, input, output);
        dcc_free_argv(scanned);
        if (ret) goto out;
    }
    dcc_trace_argv("prepared AMDGPU offload", expanded);
    dcc_free_argv(argv);
    *argvp = expanded;
    *files = inputs;
    expanded = inputs = NULL;
    ret = 0;
out:
    if (expanded) dcc_free_argv(expanded);
    if (inputs) dcc_free_argv(inputs);
    if (gpus) dcc_free_argv(gpus);
    free(host);
    free(compiler);
    free(cuid);
    free(text);
    return ret;
}

/* HIP preprocessing is target-specific. Let the coordinator execute the
 * client's driver plan before any generic single-output preprocessing. */
int dcc_offload_command(char **argv, const char *input, const char *output,
                        char ***result)
{
    char **helper = NULL;
    int ret = EXIT_OUT_OF_MEMORY;
    char path[PATH_MAX];
    if (append(&helper, dcc_helper_path("distcc-offload", path)) ||
        append(&helper, input) || append(&helper, output)) {
        ret = EXIT_OUT_OF_MEMORY;
        goto out;
    }
    for (int i = 0; argv[i]; ++i)
        if (append(&helper, argv[i])) { ret = EXIT_OUT_OF_MEMORY; goto out; }
    *result = helper;
    helper = NULL;
    ret = 0;
out:
    if (helper) dcc_free_argv(helper);
    return ret;
}
