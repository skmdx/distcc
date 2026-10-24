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
    if (!stat(input, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
        st.st_size <= 512LL * 1024 * 1024)
        hint->source_bytes = st.st_size;
    hint->kind = kind + (ready ? 0 : 3);
    key = XXH3_64bits_withSeed(&hint->kind, sizeof(hint->kind), key);
    hint->key = key ? key : 1;
}

int dcc_work_valid(const struct dcc_work_hint *hint)
{
    /* Rust snapshots include dependency artifacts and the compiler sysroot;
     * their size is not bounded by the C/C++ preprocessor input limit. */
    if (hint->kind == DCC_WORK_RUST) return hint->key && hint->bytes;
    return hint->source_bytes <= 512U * 1024 * 1024 &&
           ((!hint->key && !hint->bytes && !hint->kind && !hint->source_bytes) ||
           (hint->key && hint->bytes && hint->bytes <= 512ULL * 1024 * 1024 &&
            hint->kind >= 1 && hint->kind <= DCC_WORK_CLASSES));
}

/* A few colliding translation units must not evict each other on every build.
 * Four ways keep lookup bounded while retaining the existing entry limit. */
static unsigned history_slot(const struct dcc_work_model *model, uint64_t key)
{
    unsigned bucket = key % (DCC_WORK_HISTORY / DCC_WORK_WAYS);
    unsigned first = bucket * DCC_WORK_WAYS, empty = DCC_WORK_HISTORY;
    for (unsigned i = first; i < first + DCC_WORK_WAYS; ++i) {
        if (model->history[i].key == key) return i;
        if (!model->history[i].key && empty == DCC_WORK_HISTORY) empty = i;
    }
    return empty < DCC_WORK_HISTORY ? empty : first + model->next[bucket];
}

static double initial_rate(unsigned kind)
{
    if (kind + 1 == DCC_WORK_RUST) return 0;
    /* Pump now measures the whole uncompressed tree, so a source-only
     * expansion multiplier would count the headers twice. */
    return kind % 3 == 1 ? 1 : kind % 3 == 2 ? .0625 : .25;
}

static double source_size(const struct dcc_work_hint *hint)
{
    /* Very large sources can be generated tables rather than optimizer work.
     * Bound this feature's influence; input size still represents parsing. */
    double size = hint->source_bytes / 1048576.0;
    return size < 1 ? size : 1;
}

static void learn_class(struct dcc_work_class *c, const struct dcc_work_hint *hint,
                        double seconds)
{
    if (!c->xx) {
        c->xx = c->yy = .001;
        c->xz = .001 * initial_rate(hint->kind - 1);
    }
    double x = hint->bytes / 1048576.0, y = source_size(hint);
    c->xx = .98 * c->xx + .02 * x * x;
    c->xy = .98 * c->xy + .02 * x * y;
    c->yy = .98 * c->yy + .02 * y * y;
    c->xz = .98 * c->xz + .02 * x * seconds;
    c->yz = .98 * c->yz + .02 * y * seconds;
    /* Two-feature nonnegative least squares. If the unconstrained solution
     * crosses an axis, use the better one-feature fit instead. */
    double determinant = c->xx * c->yy - c->xy * c->xy;
    if (determinant > 1e-12) {
        double input = (c->xz * c->yy - c->yz * c->xy) / determinant;
        double source = (c->yz * c->xx - c->xz * c->xy) / determinant;
        if (input >= 0 && source >= 0) {
            c->input_rate = input;
            c->source_rate = source;
            return;
        }
    }
    if (c->yz <= 0 || c->xz * c->xz / c->xx >= c->yz * c->yz / c->yy) {
        c->input_rate = c->xz / c->xx;
        c->source_rate = 0;
    } else {
        c->input_rate = 0;
        c->source_rate = c->yz / c->yy;
    }
}

double dcc_work_predict(const struct dcc_work_model *model, const struct dcc_work_hint *hint)
{
    if (!hint->key) return 0;
    unsigned i = history_slot(model, hint->key), kind = hint->kind - 1;
    if (model->history[i].key == hint->key) {
        double ratio = (double)hint->bytes / model->history[i].bytes;
        if (ratio < .25) ratio = .25;
        if (ratio > 4) ratio = 4;
        return model->history[i].seconds * ratio;
    }
    /* Cold estimates only bridge the first completed jobs in each class.
     * Source trees and preprocessed inputs have independent learned rates. */
    const struct dcc_work_class *c = &model->classes[kind];
    /* Start Rust with occupancy-based placement and learn from execution,
     * rather than treating precompiled dependencies as C++ parsing work. */
    if (hint->kind == DCC_WORK_RUST && !c->xx) return 0;
    double seconds = hint->bytes / 1048576.0 * (c->xx ? c->input_rate : initial_rate(kind))
                   + source_size(hint) * c->source_rate;
    return seconds > .01 ? seconds : .01;
}

void dcc_work_observe(struct dcc_work_model *model, const struct dcc_work_hint *hint, double seconds)
{
    if (!hint->key || !isfinite(seconds) || seconds < .001 || seconds > 86400) return;
    unsigned i = history_slot(model, hint->key), kind = hint->kind - 1;
    learn_class(&model->classes[kind], hint, seconds);
    if (model->history[i].key != hint->key)
        model->next[i / DCC_WORK_WAYS] = (i + 1) % DCC_WORK_WAYS;
    if (model->history[i].key == hint->key && model->history[i].bytes == hint->bytes)
        seconds = .75 * model->history[i].seconds + .25 * seconds;
    model->history[i].key = hint->key;
    model->history[i].bytes = hint->bytes;
    model->history[i].seconds = seconds;
}
