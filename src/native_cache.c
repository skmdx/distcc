/* Conservative, bounded fingerprints for native driver probes.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "distcc.h"
#include "broker.h"
#include "trace.h"

extern char **environ;

#if defined(__linux__) && defined(__SIZEOF_INT128__)

struct fingerprint { __uint128_t hash; int failed, files; };

static void append(struct fingerprint *key, const char *text)
{
    /* FNV-1a/128: a local memo fingerprint, not an authentication token.
     * Include the terminator so adjacent strings cannot alias. */
    do {
        key->hash ^= (unsigned char)*text;
        key->hash *= ((__uint128_t)0x1000000 << 64) | 0x13b;
    } while (*text++);
}

static int stamp(struct fingerprint *key, const char *path)
{
    struct stat st;
    char value[256];
    append(key, path);
    if (stat(path, &st)) {
        if (errno != ENOENT && errno != ENOTDIR) {
            rs_trace("native cache cannot stat %s: %s", path, strerror(errno));
            key->failed = 1;
        }
        append(key, "absent");
        return 0;
    }
    /* nsfs synthesizes timestamps on lookup; only its identity is stable. */
    if (!strcmp(path, "/proc/self/ns/mnt")) {
        snprintf(value, sizeof(value), "%llu/%llu", (unsigned long long)st.st_dev,
                 (unsigned long long)st.st_ino);
        append(key, value);
        return 1;
    }
    snprintf(value, sizeof(value), "%llu/%llu/%llu/%lld/%ld/%lld/%ld",
             (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
             (unsigned long long)st.st_size, (long long)st.st_mtim.tv_sec,
             st.st_mtim.tv_nsec, (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    append(key, value);
    return 1;
}

/* Track nested Clang response files too. Only simple relative @ paths are
 * accepted; unfamiliar configuration syntax uses an uncached driver probe. */
static void config_file(struct fingerprint *key, const char *path, int depth)
{
    FILE *file;
    char line[4096], nested[PATH_MAX], *word, *end, *slash;
    if (key->failed) return;
    if (depth > 8 || ++key->files > 4096) { key->failed = 1; return; }
    if (!stamp(key, path)) return;
    file = fopen(path, "r");
    if (!file) { rs_trace("native cache cannot read %s: %s", path, strerror(errno)); key->failed = 1; return; }
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) { key->failed = 1; break; }
        word = line + strspn(line, " \t");
        if (*word == '#') continue;
        if (strstr(word, "--config") || strstr(word, "-specs") ||
            strstr(word, "-B") || strchr(word, '$')) { key->failed = 1; break; }
        if (!strchr(word, '@')) continue;
        if (*word++ != '@') { key->failed = 1; break; }
        end = word + strcspn(word, " \t\r\n");
        if (strspn(end, " \t\r\n") != strlen(end) ||
            strpbrk(word, "\"'<>\\")) { key->failed = 1; break; }
        *end = 0;
        if (*word == '/') snprintf(nested, sizeof(nested), "%s", word);
        else {
            snprintf(nested, sizeof(nested), "%s", path);
            slash = strrchr(nested, '/');
            if (!slash || strlen(word) >= sizeof(nested) - (size_t)(slash + 1 - nested)) {
                key->failed = 1; break;
            }
            strcpy(slash + 1, word);
        }
        config_file(key, nested, depth + 1);
    }
    if (ferror(file)) key->failed = 1;
    fclose(file);
}

static void config_directory(struct fingerprint *key, const char *path, int depth, int gcc)
{
    struct dirent **entries;
    struct stat st;
    char child[PATH_MAX];
    int count, i;
    if (key->failed || !stamp(key, path)) return;
    count = scandir(path, &entries, NULL, alphasort);
    if (count < 0) { rs_trace("native cache cannot list %s: %s", path, strerror(errno)); key->failed = 1; return; }
    if (count > 512) key->failed = 1;
    for (i = 0; i < count; i++) {
        const char *name = entries[i]->d_name;
        if (!key->failed && strcmp(name, ".") && strcmp(name, "..") &&
            snprintf(child, sizeof(child), "%s/%s", path, name) < (int)sizeof(child)) {
            if (lstat(child, &st)) key->failed = 1;
            else if (S_ISDIR(st.st_mode) && depth)
                config_directory(key, child, depth - 1, gcc);
            else if (gcc && !strcmp(name, "specs")) key->failed = 1;
            else if (!gcc && strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".cfg"))
                config_file(key, child, 0);
            else if (!gcc && S_ISLNK(st.st_mode) && !stat(child, &st) && S_ISDIR(st.st_mode) && depth)
                key->failed = 1;
        }
        free(entries[i]);
    }
    free(entries);
}

char *dcc_native_cache_key(char *argv[], const char *input, const char *output)
{
    struct fingerprint *key;
    char path[PATH_MAX], resolved[PATH_MAX], *search = NULL, *part, *save, *slash;
    unsigned char elf[4];
    FILE *file;
    cpu_set_t affinity;
    int i, clang;
    if (!getenv("DISTCC_BROKER") || !*getenv("DISTCC_BROKER")) return NULL;
    if (strchr(argv[0], '/')) snprintf(path, sizeof(path), "%s", argv[0]);
    else {
        if (!(search = strdup(getenv("PATH") ? getenv("PATH") : ""))) return NULL;
        for (part = strtok_r(search, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
            if (snprintf(path, sizeof(path), "%s/%s", part, argv[0]) < (int)sizeof(path) &&
                !access(path, X_OK)) break;
        }
        free(search);
        if (!part) return NULL;
    }
    if (!realpath(path, resolved)) return NULL;
    /* Custom toolchains can have arbitrary built-in configuration roots. */
    if (strncmp(resolved, "/usr/", 5)) return NULL;
    file = fopen(resolved, "rb");
    if (!file) return NULL;
    i = fread(elf, 1, sizeof(elf), file) == sizeof(elf) && !memcmp(elf, "\177ELF", 4);
    fclose(file);
    if (!i) return NULL;
    key = calloc(1, sizeof(*key));
    if (!key) return NULL;
    key->hash = ((__uint128_t)0x6c62272e07bb0142ULL << 64) | 0x62b821756295c58dULL;
    append(key, "native-v2");
    stamp(key, resolved);
    stamp(key, "/proc/self/ns/mnt");
    stamp(key, "/");
    if (!getcwd(path, sizeof(path))) key->failed = 1;
    else append(key, path);
    clang = strstr(resolved, "clang") != NULL;
    for (i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        if (*arg == '@' || !strncmp(arg, "-B", 2) || strstr(arg, "specs") ||
            strstr(arg, "--config") || strstr(arg, "toolchain") ||
            strstr(arg, "--sysroot") || strstr(arg, "-isysroot") ||
            !strcmp(arg, "-Xclang") || strstr(arg, "plugin")) key->failed = 1;
        if (!strcmp(arg, "-o") && argv[i + 1]) { i++; append(key, "-o"); continue; }
        if (input && !strcmp(arg, input)) arg = dcc_find_extension_const(input);
        else if (output && !strncmp(arg, "-o", 2) && !strcmp(arg + 2, output)) arg = "-o";
        append(key, arg ? arg : "input");
    }
    for (i = 0; environ[i]; i++) {
        if (!strncmp(environ[i], "DISTCC_", 7) || !strncmp(environ[i], "_=" , 2) ||
            !strncmp(environ[i], "SANDBOX_LOG=", 12) ||
            !strncmp(environ[i], "SANDBOX_MESSAGE_P@TH=", 21) ||
            !strncmp(environ[i], "SANDBOX_DEBUG_LOG=", 18)) continue;
        if (!strncmp(environ[i], "LD_PRELOAD=", 11)) {
            /* Standard sandbox/allocator preloads do not change driver
             * options. Track their files; other injected code is uncached. */
            static const char *dirs[] = { "/usr/lib64", "/lib64", "/usr/lib", "/lib" };
            char *libraries = strdup(environ[i] + 11), *library, *next_library;
            unsigned directory;
            if (!libraries) { key->failed = 1; continue; }
            for (library = strtok_r(libraries, " :", &next_library); library;
                 library = strtok_r(NULL, " :", &next_library)) {
                if (strcmp(library, "libsandbox.so") && strcmp(library, "libjemalloc.so")) {
                    key->failed = 1;
                    break;
                }
                for (directory = 0; directory < sizeof(dirs) / sizeof(dirs[0]); directory++) {
                    snprintf(path, sizeof(path), "%s/%s", dirs[directory], library);
                    stamp(key, path);
                }
            }
            free(libraries);
        }
        if (!strncmp(environ[i], "GCC_EXEC_PREFIX=", 16) ||
            !strncmp(environ[i], "COMPILER_PATH=", 14) ||
            !strncmp(environ[i], "LD_LIBRARY_PATH=", 16) ||
            !strncmp(environ[i], "CCC_OVERRIDE_OPTIONS=", 21)) {
            rs_trace("native cache bypass: environment %.*s", (int)strcspn(environ[i], "="), environ[i]);
            key->failed = 1;
        }
        append(key, environ[i]);
    }
    if (sched_getaffinity(0, sizeof(affinity), &affinity)) key->failed = 1;
    else for (i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &affinity)) {
        snprintf(path, sizeof(path), "%d", i); append(key, path);
    }
    file = fopen("/proc/cpuinfo", "r");
    if (!file) key->failed = 1;
    else {
        /* Frequency and bogomips vary without changing native codegen. */
        char line[8192];
        while (fgets(line, sizeof(line), file))
            if (strncmp(line, "cpu MHz", 7) && strncmp(line, "bogomips", 8) &&
                strncmp(line, "BogoMIPS", 8)) append(key, line);
        if (ferror(file)) key->failed = 1;
        fclose(file);
    }
    if (clang) {
        config_directory(key, "/etc/clang", 2, 0);
        snprintf(path, sizeof(path), "%s/.config/clang", getenv("HOME") ? getenv("HOME") : "");
        config_directory(key, path, 0, 0);
        strcpy(path, resolved);
        slash = strrchr(path, '/'); *slash = 0;
        config_directory(key, path, 0, 0);
    } else config_directory(key, "/usr/lib/gcc", 2, 1);
    if (key->failed) { rs_trace("native cache bypass: unsupported configuration"); free(key); return NULL; }
    snprintf(path, sizeof(path), "native:%016llx%016llx",
             (unsigned long long)(key->hash >> 64), (unsigned long long)key->hash);
    search = strdup(path);
    rs_trace("native cache fingerprint %s", path);
    free(key);
    return search;
}

#else
char *dcc_native_cache_key(char *argv[], const char *input, const char *output)
{
    (void)argv; (void)input; (void)output;
    return NULL;
}
#endif
