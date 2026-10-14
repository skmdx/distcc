/* Compile-cost history is advisory: it never grants execution or memory slots.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <xxhash.h>
#include "workload.h"

void dcc_work_describe(struct dcc_work_hint *hint, char **argv, const char *input,
                       const char *prepared, int fd, int ready)
{
    *hint = (struct dcc_work_hint){0};
    struct stat st;
    const char *path = ready && prepared ? prepared : input;
    if (!input || (ready && fd >= 0 ? fstat(fd, &st) : stat(path, &st)) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 512LL * 1024 * 1024) return;
    unsigned kind = 1;
    const char *extension = strrchr(input, '.');
    if (strstr(argv[0], "++") || (extension &&
        (!strcmp(extension, ".cc") || !strcmp(extension, ".cpp") ||
         !strcmp(extension, ".cxx") || !strcmp(extension, ".C") || !strcmp(extension, ".ii")))) kind = 2;
    else if (extension && (!strcmp(extension, ".s") || !strcmp(extension, ".S"))) kind = 3;
    uint64_t key = 0;
    char *cwd = getcwd(NULL, 0);
    if (!cwd) return;
    key = XXH3_64bits(cwd, strlen(cwd));
    free(cwd);
    for (int i = 0; argv[i]; ++i) {
        /* Output paths do not change the cost of the translation unit. */
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-MF") ||
             !strcmp(argv[i], "-MT") || !strcmp(argv[i], "-MQ")) && argv[i + 1]) { ++i; continue; }
        if (!strcmp(argv[i], "-x") && argv[i + 1]) {
            if (strstr(argv[i + 1], "++")) kind = 2;
            else if (strstr(argv[i + 1], "assembler")) kind = 3;
            else kind = 1;
        }
        key = XXH3_64bits_withSeed(argv[i], strlen(argv[i]) + 1, key);
    }
    hint->bytes = st.st_size;
    hint->kind = kind + (ready ? 0 : 3);
    key = XXH3_64bits_withSeed(&hint->kind, sizeof(hint->kind), key);
    hint->key = key ? key : 1;
}

int dcc_work_valid(const struct dcc_work_hint *hint)
{
    return !hint->reserved && ((!hint->key && !hint->bytes && !hint->kind) ||
           (hint->key && hint->bytes && hint->bytes <= 512ULL * 1024 * 1024 &&
            hint->kind >= 1 && hint->kind <= DCC_WORK_CLASSES));
}

double dcc_work_predict(const struct dcc_work_model *model, const struct dcc_work_hint *hint)
{
    if (!hint->key) return 0;
    unsigned i = hint->key % DCC_WORK_HISTORY, kind = hint->kind - 1;
    if (model->history[i].key == hint->key) {
        double ratio = (double)hint->bytes / model->history[i].bytes;
        if (ratio < .25) ratio = .25;
        if (ratio > 4) ratio = 4;
        return model->history[i].seconds * ratio;
    }
    /* Cold estimates only bridge the first completed jobs in each class.
     * Raw sources and preprocessed inputs have independent learned rates. */
    double seconds_per_byte = (kind % 3 == 1 ? 1.0 : kind % 3 == 2 ? .0625 : .25) / 1048576;
    if (kind >= 3) seconds_per_byte *= 16;
    if (model->classes[kind].bytes > 0)
        seconds_per_byte = model->classes[kind].seconds / model->classes[kind].bytes;
    double seconds = hint->bytes * seconds_per_byte;
    return seconds > .01 ? seconds : .01;
}

void dcc_work_observe(struct dcc_work_model *model, const struct dcc_work_hint *hint, double seconds)
{
    if (!hint->key || !isfinite(seconds) || seconds < .001 || seconds > 86400) return;
    unsigned i = hint->key % DCC_WORK_HISTORY, kind = hint->kind - 1;
    if (model->history[i].key == hint->key && model->history[i].bytes == hint->bytes)
        seconds = .75 * model->history[i].seconds + .25 * seconds;
    model->history[i].key = hint->key;
    model->history[i].bytes = hint->bytes;
    model->history[i].seconds = seconds;
    double decay = model->classes[kind].bytes > 0 ? .875 : 0;
    model->classes[kind].bytes = decay * model->classes[kind].bytes + (1 - decay) * hint->bytes;
    model->classes[kind].seconds = decay * model->classes[kind].seconds + (1 - decay) * seconds;
}
