/* Compile-cost history is advisory: it never grants execution or memory slots.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "workload.h"
#include "job.h"
#include "resource.h"

static void describe_source(const char *path, off_t size, struct dcc_source_features *source)
{
    if (size <= 0 || size > 512LL * 1024 * 1024) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return;
    /* Check the opened object as well: it may have replaced the pathname
     * described by the caller, and a pipe/device must not block hashing. */
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 512LL * 1024 * 1024) { close(fd); return; }
    uint32_t lines = 1;
    off_t total = 0;
    XXH3_state_t hash = {0};
    XXH3_64bits_reset(&hash);
    char buffer[65536];
    ssize_t length;
    while ((length = read(fd, buffer, sizeof(buffer))) != 0) {
        if (length < 0) { if (errno == EINTR) continue; break; }
        total += length;
        if (total > size) break;
        for (ssize_t i = 0; i < length; ++i) lines += buffer[i] == '\n';
        XXH3_64bits_update(&hash, buffer, length);
    }
    if (!length && total == size) {
        source->hash = XXH3_64bits_digest(&hash);
        source->lines = lines;
    }
    close(fd);
}

void dcc_work_describe(struct dcc_work_hint *hint, char **argv, const char *input,
                       const char *prepared, int fd, int ready, struct dcc_source_features *source)
{
    *hint = (struct dcc_work_hint){0};
    struct stat st;
    const char *path = ready && prepared ? prepared : input;
    if (!input || (ready && fd >= 0 ? fstat(fd, &st) : stat(path, &st)) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 512LL * 1024 * 1024) return;
    unsigned kind = 1;
    const char *extension = strrchr(input, '.');
    int gpu = extension && (!strcmp(extension, ".hip") || !strcmp(extension, ".hipi"));
    int host_only = 0, identity = 0;
    for (int i = 1; argv[i]; ++i)
        if (!strncmp(argv[i], "-cuid=", 6) && argv[i][6]) identity = 1;
    if (gpu || strstr(argv[0], "++") || (extension &&
        (!strcmp(extension, ".cc") || !strcmp(extension, ".cpp") ||
         !strcmp(extension, ".cxx") || !strcmp(extension, ".C") || !strcmp(extension, ".ii")))) kind = 2;
    else if (extension && (!strcmp(extension, ".s") || !strcmp(extension, ".S"))) kind = 3;
    uint64_t key = 0;
    char *cwd = getcwd(NULL, 0);
    if (!cwd) return;
    key = XXH3_64bits(cwd, strlen(cwd));
    uint64_t directory = key;
    free(cwd);
    for (int i = 0; argv[i]; ++i) {
        if (!strcmp(argv[i], "--offload-host-only") || !strcmp(argv[i], "--cuda-host-only")) host_only = 1;
        /* The compiler's ID identifies a HIP unit across temporary bundles. */
        if (identity && !strcmp(argv[i], input)) continue;
        /* Output paths do not change the cost of the translation unit. */
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-MF") ||
             !strcmp(argv[i], "-MT") || !strcmp(argv[i], "-MQ")) && argv[i + 1]) { ++i; continue; }
        const char *language = !strcmp(argv[i], "-x") ? argv[i + 1] :
                               !strncmp(argv[i], "-x", 2) ? argv[i] + 2 : NULL;
        if (language) {
            gpu = !strcmp(language, "hip") || !strcmp(language, "hip-cpp-output");
            if (gpu || strstr(language, "++")) kind = 2;
            else if (strstr(language, "assembler")) kind = 3;
            else kind = 1;
        }
        key = XXH3_64bits_withSeed(argv[i], strlen(argv[i]) + 1, key);
    }
    hint->bytes = st.st_size;
    int source_known = path == input && (!ready || fd < 0);
    if ((source_known || !stat(input, &st)) &&
        S_ISREG(st.st_mode) && st.st_size > 0 &&
        st.st_size <= 512LL * 1024 * 1024)
        hint->work_units = st.st_size;
    hint->kind = gpu && !host_only ? DCC_WORK_OFFLOAD : kind + (ready ? 0 : 3);
    hint->memory_class = 1 + (ready ? DCC_JOB_COMPILE : DCC_JOB_PUMP) * DCC_MEMORY_BUCKETS +
                         dcc_resource_bucket(hint->bytes);
    key = XXH3_64bits_withSeed(&hint->kind, sizeof(hint->kind), key);
    hint->key = key ? key : 1;
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    uint64_t family = XXH3_64bits_withSeed(name, strlen(name), kind);
    hint->scope_key = XXH3_64bits_withSeed(&directory, sizeof(directory), family);
    if (!gpu) {
        if (!source->hash) describe_source(input, hint->work_units, source);
        /* Count logical source lines during the existing content hash. A
         * generated literal and many declarations of equal size cost differently. */
        hint->work_units = source->lines;
        if (source->hash) {
            uint64_t phase = XXH3_64bits_withSeed(name, strlen(name), hint->kind);
            hint->family = XXH3_64bits_withSeed(&source->hash, sizeof(source->hash), phase);
        }
    }
}

int dcc_work_valid(const struct dcc_work_hint *hint)
{
    if (hint->memory_class > DCC_JOB_KIND_COUNT * DCC_MEMORY_BUCKETS ||
        (!!hint->key != !!hint->memory_class)) return 0;
    /* Backend snapshots include dependency artifacts and compiler data;
     * their size is not bounded by the C/C++ preprocessor input limit. */
    if (hint->kind == DCC_WORK_RUST || hint->kind == DCC_WORK_THINLTO ||
        hint->kind == DCC_WORK_OFFLOAD_LINK) return hint->key && hint->bytes;
    return hint->work_units <= 512U * 1024 * 1024 &&
           ((!hint->key && !hint->bytes && !hint->kind && !hint->work_units && !hint->family && !hint->scope_key) ||
           (hint->key && hint->bytes && hint->bytes <= 512ULL * 1024 * 1024 &&
            hint->kind >= 1 && hint->kind <= DCC_WORK_CLASSES));
}

/* A few colliding translation units must not evict each other on every build.
 * Four ways keep lookup bounded while retaining the existing entry limit. */
static unsigned history_slot(const struct dcc_work_model *model, const struct dcc_work_hint *hint)
{
    unsigned bucket = (hint->family ? hint->family : hint->key) % (DCC_WORK_HISTORY / DCC_WORK_WAYS);
    unsigned first = bucket * DCC_WORK_WAYS, empty = DCC_WORK_HISTORY;
    for (unsigned i = first; i < first + DCC_WORK_WAYS; ++i) {
        if (model->history[i].key == hint->key && model->history[i].family == hint->family) return i;
        if (!model->history[i].key && empty == DCC_WORK_HISTORY) empty = i;
    }
    return empty < DCC_WORK_HISTORY ? empty : first + model->next[bucket];
}

static double initial_rate(unsigned kind)
{
    if (kind + 1 >= DCC_WORK_RUST) return 0;
    /* Pump now measures the whole uncompressed tree, so a source-only
     * expansion multiplier would count the headers twice. */
    return kind % 3 == 1 ? 1 : kind % 3 == 2 ? .0625 : .25;
}

static const double rust_work_rate = .15;

static double computation_size(const struct dcc_work_hint *hint)
{
    /* LLVM instruction counts measure optimizer work directly; bitcode and
     * imported constant-data bytes remain the independent input-size feature. */
    if (hint->kind == DCC_WORK_THINLTO) return hint->work_units / 1048576.0;
    if (hint->kind == DCC_WORK_RUST) return log1p(hint->work_units / 65536.0);
    if (hint->kind < DCC_WORK_RUST) return hint->work_units / 16384.0;
    double size = hint->work_units / 1048576.0;
    /* Large source text can be generated tables rather than optimizer work. */
    return size < 1 ? size : 1;
}

static void learn_class(struct dcc_work_class *c, const struct dcc_work_hint *hint,
                        double seconds)
{
    if (!c->xx) {
        c->xx = c->yy = .001;
        c->xz = .001 * initial_rate(hint->kind - 1);
        if (hint->kind == DCC_WORK_RUST) c->yz = .001 * rust_work_rate;
    }
    double x = hint->bytes / 1048576.0, y = computation_size(hint);
    c->weight = .98 * c->weight + .02;
    c->x = .98 * c->x + .02 * x;
    c->y = .98 * c->y + .02 * y;
    c->z = .98 * c->z + .02 * seconds;
    c->xx = .98 * c->xx + .02 * x * x;
    c->xy = .98 * c->xy + .02 * x * y;
    c->yy = .98 * c->yy + .02 * y * y;
    c->xz = .98 * c->xz + .02 * x * seconds;
    c->yz = .98 * c->yz + .02 * y * seconds;
    /* Nonnegative affine fit: fixed frontend cost must not disappear when a
     * small source includes large headers. Enumerating the seven active sets
     * also handles collinear byte/work features without unstable subtraction.
     * Normalize equations by their diagonals so byte/instruction units do not
     * set the pivot threshold. Singular sets defer to smaller subsets. */
    double gram[3][3] = {{c->weight, c->x, c->y},
                         {c->x, c->xx, c->xy}, {c->y, c->xy, c->yy}};
    double rhs[3] = {c->z, c->xz, c->yz};
    double scale[3] = {c->weight, c->xx, c->yy};
    double best[3] = {0}, gain = 0;
    for (unsigned mask = 1; mask < 8; ++mask) {
        unsigned index[3], count = 0;
        double a[3][4] = {{0}}, coefficients[3] = {0};
        for (unsigned i = 0; i < 3; ++i) if (mask & (1U << i)) index[count++] = i;
        int valid = 1;
        for (unsigned i = 0; i < count; ++i)
            if (!(scale[index[i]] > 0)) valid = 0;
        if (!valid) continue;
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned j = 0; j < count; ++j)
                a[i][j] = gram[index[i]][index[j]] / scale[index[i]];
            a[i][count] = rhs[index[i]] / scale[index[i]];
        }
        for (unsigned k = 0; k < count; ++k) {
            if (a[k][k] < 1e-10) { valid = 0; break; }
            double pivot = a[k][k];
            for (unsigned j = k; j <= count; ++j) a[k][j] /= pivot;
            for (unsigned i = 0; i < count; ++i) if (i != k) {
                double factor = a[i][k];
                for (unsigned j = k; j <= count; ++j) a[i][j] -= factor * a[k][j];
            }
        }
        if (!valid) continue;
        for (unsigned i = 0; i < count; ++i) {
            coefficients[index[i]] = a[i][count];
            if (!isfinite(coefficients[index[i]]) || coefficients[index[i]] < 0) valid = 0;
        }
        if (!valid) continue;
        double improvement = 0;
        for (unsigned i = 0; i < 3; ++i) {
            improvement += 2 * coefficients[i] * rhs[i];
            for (unsigned j = 0; j < 3; ++j)
                improvement -= coefficients[i] * gram[i][j] * coefficients[j];
        }
        if (improvement > gain) {
            gain = improvement;
            memcpy(best, coefficients, sizeof(best));
        }
    }
    c->fixed = best[0]; c->input_rate = best[1]; c->work_rate = best[2];
}

double dcc_work_predict(const struct dcc_work_model *model, const struct dcc_work_hint *hint)
{
    if (!hint->key) return 0;
    unsigned i = history_slot(model, hint), kind = hint->kind - 1;
    if (model->history[i].key == hint->key && model->history[i].family == hint->family) {
        double ratio = (double)hint->bytes / model->history[i].bytes;
        if (ratio < .25) ratio = .25;
        if (ratio > 4) ratio = 4;
        return model->history[i].seconds * ratio;
    }
    /* A new ABI or build directory changes the exact command, but a completed
     * compile of the same source is a better prior than unrelated files.
     * Reuse only timing: compiler flags and artifact identity stay untouched. */
    if (hint->family) {
        unsigned first = i / DCC_WORK_WAYS * DCC_WORK_WAYS, count = 0;
        double sum = 0;
        for (unsigned j = first; j < first + DCC_WORK_WAYS; ++j)
            if (model->history[j].key && model->history[j].family == hint->family) {
                sum += model->history[j].seconds;
                ++count;
            }
        if (count) return sum / count;
    }
    /* Cold estimates only bridge the first completed jobs in each class.
     * Source trees and preprocessed inputs have independent learned rates. */
    const struct dcc_work_class *c = &model->classes[kind];
    /* Start file-based backends with occupancy-based placement and learn from execution,
     * rather than treating precompiled dependencies as C++ parsing work. */
    if (hint->kind == DCC_WORK_RUST && !c->xx) return .03 + rust_work_rate * computation_size(hint);
    if (hint->kind > DCC_WORK_RUST && !c->xx) return 0;
    double seconds = c->fixed + hint->bytes / 1048576.0 * (c->xx ? c->input_rate : initial_rate(kind))
                   + computation_size(hint) * c->work_rate;
    return seconds > .01 ? seconds : .01;
}

int dcc_work_peer(const struct dcc_work_model *model, const struct dcc_work_hint *hint)
{
    if (!hint->key) return -1;
    unsigned i = history_slot(model, hint);
    return model->history[i].key == hint->key && model->history[i].family == hint->family &&
           model->history[i].bytes == hint->bytes ? model->history[i].peer : -1;
}

void dcc_work_observe(struct dcc_work_model *model, const struct dcc_work_hint *hint, double seconds, int peer)
{
    if (!hint->key || !isfinite(seconds) || seconds < .001 || seconds > 86400) return;
    unsigned i = history_slot(model, hint), kind = hint->kind - 1;
    learn_class(&model->classes[kind], hint, seconds);
    if (model->history[i].key != hint->key || model->history[i].family != hint->family)
        model->next[i / DCC_WORK_WAYS] = (i + 1) % DCC_WORK_WAYS;
    if (model->history[i].key == hint->key && model->history[i].family == hint->family &&
        model->history[i].bytes == hint->bytes)
        seconds = .75 * model->history[i].seconds + .25 * seconds;
    model->history[i].key = hint->key;
    model->history[i].family = hint->family;
    model->history[i].bytes = hint->bytes;
    model->history[i].seconds = seconds;
    model->history[i].peer = peer;
}
