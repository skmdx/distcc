/* Resolve native CPU options using the client's actual compiler driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "distcc.h"
#include "exec.h"
#include "exitcode.h"
#include "trace.h"

static int native_option(const char *arg)
{
    return !strcmp(arg, "-march=native") || !strcmp(arg, "-mtune=native") ||
           !strcmp(arg, "-mcpu=native");
}

/* Driver -### output is quoted argv, not shell code. Never execute it.
 * Decode a single command, rejecting incomplete quotes or escapes.
 */
static char **driver_words(char *line)
{
    char **words = calloc(strlen(line) + 1, sizeof(char *));
    char *read = line, *write = line;
    int count = 0;
    if (!words)
        return NULL;
    while (*read) {
        int quote = 0;
        while (isspace((unsigned char)*read)) read++;
        if (!*read) break;
        words[count++] = write;
        while (*read && (quote || !isspace((unsigned char)*read))) {
            if (*read == '\\' && quote != '\'') {
                if (!*++read) goto invalid;
                *write++ = *read++;
            } else if (*read == quote) {
                quote = 0;
                read++;
            } else if (!quote && (*read == '"' || *read == '\'')) {
                quote = *read++;
            } else {
                *write++ = *read++;
            }
        }
        if (quote) goto invalid;
        if (*read) read++;
        *write++ = '\0';
    }
    return words;
invalid:
    free(words);
    return NULL;
}

static int add_arg(char **args, int *count, const char *value)
{
    if (!(args[*count] = strdup(value)))
        return EXIT_OUT_OF_MEMORY;
    ++*count;
    return 0;
}

/* On failure argv is untouched, so the ordinary scanner keeps native local.
 * Probe the complete invocation: option ordering, explicit feature overrides,
 * target mode and compiler configuration all affect the expansion.
 */
int dcc_expand_native(char ***argvp)
{
    char **argv = *argvp, **probe = NULL, **frontend = NULL, **expanded = NULL;
    char *file = NULL, *text = NULL, *line, *next;
    int argc = dcc_argv_len(argv), ret = EXIT_DISTCC_FAILED, status;
    int i, n = 0, clang = 0, found = 0, cpu = 0, native = 0;
    pid_t pid;
    struct stat st;

    for (i = 1; argv[i]; i++) native |= native_option(argv[i]);
    if (!native) return 0;
    if (dcc_copy_argv(argv, &probe, 1)) goto out;
    probe[argc + 1] = NULL;
    if (!(probe[argc] = strdup("-###")) ||
        dcc_make_tmpnam("distcc_native", ".txt", &file)) goto out;
    if (dcc_spawn_child(probe, &pid, "/dev/null", "/dev/null", file) ||
        dcc_collect_child("native option expansion", pid, &status, timeout_null_fd) || status ||
        stat(file, &st) || st.st_size > 1024 * 1024 ||
        dcc_load_file_string(file, &text))
        goto out;

    for (line = text; line && *line; line = next) {
        char **words;
        const char *base;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (!isspace((unsigned char)*line)) continue;
        words = driver_words(line);
        if (!words) goto out;
        if (!words[0]) { free(words); continue; }
        base = strrchr(words[0], '/');
        base = base ? base + 1 : words[0];
        if (!strcmp(base, "cc1") || !strcmp(base, "cc1plus") ||
            (words[1] && !strcmp(words[1], "-cc1"))) {
            if (found++) { free(words); goto out; }
            frontend = words;
            clang = words[1] && !strcmp(words[1], "-cc1");
        } else {
            free(words);
        }
    }
    if (!frontend) goto out;
    expanded = calloc(argc + 4 * dcc_argv_len(frontend) + 1, sizeof(char *));
    if (!expanded) goto out;
    for (i = 0; argv[i]; i++) {
        if (native_option(argv[i]) || (!clang && i && !strncmp(argv[i], "-m", 2)))
            continue;
        if (clang && !strcmp(argv[i], "-fno-integrated-as")) goto out;
        if (add_arg(expanded, &n, argv[i])) goto out;
    }
    for (i = 1; frontend[i]; i++) {
        const char *arg = frontend[i];
        if (native_option(arg)) goto out;
        if (clang) {
            if (strcmp(arg, "-target-cpu") && strcmp(arg, "-tune-cpu") &&
                strcmp(arg, "-target-feature")) continue;
            if (!frontend[i + 1] || !strcmp(frontend[i + 1], "native")) goto out;
            cpu |= !strcmp(arg, "-target-cpu");
            if (add_arg(expanded, &n, "-Xclang") || add_arg(expanded, &n, arg) ||
                add_arg(expanded, &n, "-Xclang") || add_arg(expanded, &n, frontend[++i]))
                goto out;
        } else if (!strncmp(arg, "-m", 2)) {
            cpu |= !strncmp(arg, "-march=", 7) || !strncmp(arg, "-mtune=", 7) ||
                   !strncmp(arg, "-mcpu=", 6);
            if (add_arg(expanded, &n, arg)) goto out;
        } else if (!strcmp(arg, "--param")) {
            if (!frontend[i + 1] || add_arg(expanded, &n, arg) ||
                add_arg(expanded, &n, frontend[++i])) goto out;
        } else if (!strncmp(arg, "--param=", 8)) {
            if (add_arg(expanded, &n, arg)) goto out;
        }
    }
    if (!cpu) goto out;
    dcc_trace_argv("expanded native options", expanded);
    dcc_free_argv(argv);
    *argvp = expanded;
    expanded = NULL;
    ret = 0;
out:
    if (ret) rs_log_info("native option expansion unavailable; compiling locally");
    if (probe) dcc_free_argv(probe);
    if (expanded) dcc_free_argv(expanded);
    free(frontend);
    free(text);
    if (file) unlink(file);
    free(file);
    return ret;
}
