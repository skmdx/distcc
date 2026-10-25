/* Prepare Clang's bundled, preprocessed AMDGPU inputs for ordinary TCP jobs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "distcc.h"
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

/* ROCm's driver discovers a directory, including libraries not selected by
 * this particular GPU. Ship its bitcode files through the existing shared
 * input cache, so workers need neither the client's ROCm install nor headers. */
static int device_libraries(const char *file, char **directory, char ***files)
{
    char *path = realpath(file, NULL), *slash;
    DIR *dir;
    struct dirent *entry;
    int ret = EXIT_IO_ERROR;
    if (!path) return ret;
    slash = strrchr(path, '/');
    if (!slash) { free(path); return ret; }
    *slash = '\0';
    if (*directory) {
        ret = strcmp(*directory, path) ? EXIT_DISTCC_FAILED : 0;
        free(path);
        return ret;
    }
    *directory = path;
    if (!(dir = opendir(path))) return ret;
    ret = 0;
    while ((entry = readdir(dir))) {
        char *name;
        struct stat st;
        if (!str_endswith(".bc", entry->d_name)) continue;
        if (asprintf(&name, "%s/%s", path, entry->d_name) < 0) {
            ret = EXIT_OUT_OF_MEMORY;
            break;
        }
        if (!stat(name, &st) && S_ISREG(st.st_mode)) ret = append(files, name);
        free(name);
        if (ret) break;
    }
    closedir(dir);
    return ret;
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

int dcc_prepare_offload(char ***argvp, char ***files)
{
    char **argv = *argvp, **expanded = NULL, **inputs = NULL, **host = NULL, **gpus = NULL;
    char *text = NULL, *directory = NULL, *compiler = NULL, *cuid = NULL, *line, *next;
    int ret = EXIT_DISTCC_FAILED, native = dcc_native_requested(argv);
    int hip = !strcmp(dcc_offload_language(argv), "hip-cpp-output");
    int gpu_native = dcc_argv_search(argv, "--offload-arch=native");
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
            if (gpu_native) {
                const char *cpu = option(words, "-target-cpu");
                char *flag = NULL;
                if (!cpu || asprintf(&flag, "--offload-arch=%s", cpu) < 0) { free(words); goto out; }
                int error = append(&gpus, flag);
                free(flag);
                if (error) { free(words); goto out; }
            }
            for (int i = 1; words[i] && words[i + 1]; ++i) {
                if (!strcmp(words[i], "-mlink-builtin-bitcode")) {
                    if (device_libraries(words[++i], &directory, &inputs)) {
                        free(words);
                        goto out;
                    }
                }
            }
            free(words);
        } else {
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
        if (directory && !strncmp(argv[i], "--rocm-device-lib-path=", 23)) continue;
        if (directory && !strcmp(argv[i], "--rocm-device-lib-path") && argv[i + 1]) { ++i; continue; }
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
    if (directory) {
        char *flag;
        if (asprintf(&flag, "--rocm-device-lib-path=%s", directory) < 0) goto out;
        int error = append(&expanded, flag);
        free(flag);
        if (error) goto out;
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
    free(directory);
    free(text);
    return ret;
}
