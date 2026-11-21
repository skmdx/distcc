/* Compile-cost history is advisory: it never grants execution or memory slots.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include "source_scan.h"

#define SOURCE_LIMIT (512U * 1024 * 1024)

/* Unity generators put quoted implementation-file includes in a short stub.
 * Inspect its first buffer only. Headers and transitive includes belong to
 * the compiler, and preprocessed jobs already carry their expanded size. */
static void include_source(const char *path, const char *line, const char *end, unsigned *lookups,
                           int assembly, struct dcc_source_features *source)
{
    const uint32_t room = SOURCE_LIMIT - source->included_bytes;
    if (*lookups == 64 || !room) return;
    while (line < end && (*line == ' ' || *line == '\t')) ++line;
    if (end - line < 7 || memcmp(line, "include", 7)) return;
    line += 7;
    while (line < end && (*line == ' ' || *line == '\t')) ++line;
    if (line == end || *line++ != '"') return;
    const char *quote = line;
    while (quote < end && *quote != '"' && *quote != '\n' && *quote) ++quote;
    if (quote == end || *quote != '"' || quote == line) return;
    const char *slash = strrchr(path, '/');
    size_t length = quote - line, base = slash && *line != '/' ? (size_t)(slash + 1 - path) : 0;
    char name[PATH_MAX];
    if (base + length >= sizeof(name)) return;
    memcpy(name, path, base);
    memcpy(name + base, line, length);
    name[base + length] = 0;
    const char *ext = strrchr(name + base, '.');
    if (!ext || (strcmp(ext, ".c") && strcmp(ext, ".cc") && strcmp(ext, ".cpp") &&
                 strcmp(ext, ".cxx") && strcmp(ext, ".C"))) return;
    ++*lookups;
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    int fd = open(name, flags);
    if (fd < 0 && base) fd = open(name + base, flags);
    if (fd < 0) return;
    struct stat st;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0) {
        uint32_t extent = st.st_size < room ? (uint32_t)st.st_size : room;
        source->included_bytes += extent;
        struct source_scan scan = {0};
        char buffer[65536];
        for (uint32_t left = extent; left;) {
            ssize_t n = read(fd, buffer, left < sizeof(buffer) ? left : sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i)
                source->units += assembly ? buffer[i] == '\n' : source_byte(&scan, buffer[i]) != 0;
            left -= n;
        }
    }
    close(fd);
}

static void describe_source(const char *path, off_t size, int assembly, struct dcc_source_features *source)
{
    if (size <= 0 || size > SOURCE_LIMIT) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return;
    /* Check the opened object as well: it may have replaced the pathname
     * described by the caller, and a pipe/device must not block hashing. */
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > SOURCE_LIMIT) { close(fd); return; }
    struct dcc_source_features features = {.units = assembly};
    struct source_scan scan = {0};
    unsigned lookups = 0;
    off_t total = 0;
    XXH3_state_t hash = {0};
    XXH3_64bits_reset(&hash);
    char buffer[65536];
    ssize_t length;
    while ((length = read(fd, buffer, sizeof(buffer))) != 0) {
        if (length < 0) { if (errno == EINTR) continue; break; }
        if (total + length > size) break;
        for (ssize_t i = 0; i < length; ++i) {
            char token = !assembly || !total ? source_byte(&scan, buffer[i]) : 0;
            features.units += assembly ? buffer[i] == '\n' : token != 0;
            if (token == '#' && !total)
                include_source(path, buffer + i + 1, buffer + length, &lookups, assembly, &features);
        }
        total += length;
        XXH3_64bits_update(&hash, buffer, length);
    }
    if (!length && total == size) {
        features.hash = XXH3_64bits_digest(&hash);
        *source = features;
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
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > SOURCE_LIMIT) return;
    unsigned kind = 1;
    const char *extension = strrchr(input, '.');
    int gpu = extension && (!strcmp(extension, ".hip") || !strcmp(extension, ".hipi"));
    int host_only = 0, identity = 0;
    for (int i = 1; argv[i]; ++i)
        if (!strncmp(argv[i], "-cuid=", 6) && argv[i][6]) identity = 1;
    if (extension && (!strcmp(extension, ".s") || !strcmp(extension, ".S"))) kind = 3;
    else if (gpu || strstr(argv[0], "++") || (extension &&
        (!strcmp(extension, ".cc") || !strcmp(extension, ".cpp") ||
         !strcmp(extension, ".cxx") || !strcmp(extension, ".C") || !strcmp(extension, ".ii")))) kind = 2;
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
        st.st_size <= SOURCE_LIMIT)
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
    if (!gpu || host_only) {
        if (!source->hash) describe_source(input, hint->work_units, kind == 3, source);
        if (!ready) {
            hint->bytes += source->included_bytes;
            if (hint->bytes > SOURCE_LIMIT) hint->bytes = SOURCE_LIMIT;
        }
        hint->work_units = source->units < SOURCE_LIMIT ? source->units : SOURCE_LIMIT;
        if (source->hash) {
            uint64_t phase = XXH3_64bits_withSeed(name, strlen(name), hint->kind);
            hint->family = XXH3_64bits_withSeed(&source->hash, sizeof(source->hash), phase);
        }
    }
}

int dcc_work_valid(const struct dcc_work_hint *hint)
{
    if (!memchr(hint->rust_host_features, 0, sizeof(hint->rust_host_features)) ||
        (*hint->rust_host_features && hint->kind != DCC_WORK_RUST)) return 0;
    if (!memchr(hint->cgu_backend, 0, sizeof(hint->cgu_backend)) ||
        (*hint->cgu_backend && hint->kind != DCC_WORK_RUST_CGU) ||
        (hint->rust_compiler && hint->kind != DCC_WORK_RUST && hint->kind != DCC_WORK_RUST_CGU)) return 0;
    if (hint->memory_class > DCC_JOB_KIND_COUNT * DCC_MEMORY_BUCKETS ||
        (!!hint->key != !!hint->memory_class)) return 0;
    /* Backend snapshots include dependency artifacts and compiler data;
     * their size is not bounded by the C/C++ preprocessor input limit. */
    if (hint->kind == DCC_WORK_RUST || hint->kind == DCC_WORK_THINLTO ||
        hint->kind == DCC_WORK_OFFLOAD_LINK || hint->kind == DCC_WORK_RUST_CGU) return hint->key && hint->bytes;
    return hint->work_units <= SOURCE_LIMIT &&
           ((!hint->key && !hint->bytes && !hint->kind && !hint->work_units && !hint->family && !hint->scope_key) ||
           (hint->key && hint->bytes && hint->bytes <= SOURCE_LIMIT &&
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
    /* Pump now measures the whole uncompressed tree, so a source-only
     * expansion multiplier would count the headers twice. */
    static const double rate[] = {.0625, .25, 1}; /* assembly, C, C++ */
    return kind < DCC_WORK_RUST ? rate[kind % 3] : 0;
}

static int structural_source(unsigned kind)
{
    return kind < DCC_WORK_RUST && kind % 3 != 0;
}

static double initial_work_rate(unsigned kind)
{
    if (structural_source(kind)) return 1; /* seconds per Ki statement/directive */
    return kind == DCC_WORK_RUST ? .15 : 0;
}

static double computation_size(const struct dcc_work_hint *hint)
{
    double units = hint->work_units;
    if (structural_source(hint->kind)) return units / 1024;
    switch (hint->kind) {
    case 3: case 6: return units / 16384; /* assembly lines */
    case DCC_WORK_RUST: return log1p(units / 65536);
    case DCC_WORK_THINLTO: return units / 1048576; /* LLVM instructions */
    default: return fmin(units / 1048576, 1); /* offload / CGU artifact bytes */
    }
}

/* Leverage of [1,x,y] in the existing weighted design. Centering on the
 * intercept and eliminating x gives v' G^-1 v without a matrix inverse.
 * The slope priors keep both conditional variances strictly positive. */
static double leverage(const struct dcc_work_class *c, double x, double y)
{
    double mx = c->x / c->weight, my = c->y / c->weight;
    double xx = c->xx - c->x * mx, xy = c->xy - c->x * my;
    double yy = c->yy - c->y * my, slope = xy / xx;
    double dx = x - mx, dy = y - my - slope * dx;
    return 1 / c->weight + dx * dx / xx + dy * dy / (yy - slope * xy);
}

static void learn_class(struct dcc_work_class *c, const struct dcc_work_hint *hint,
                        double seconds)
{
    const double step = .02, keep = 1 - step;
    double x = hint->bytes / 1048576.0, y = computation_size(hint);
    int structural = structural_source(hint->kind);
    double w = step;
    double ridge = structural ? step : .001;
    double input_prior = initial_rate(hint->kind), work_prior = initial_work_rate(hint->kind);
    if (!c->xx) {
        c->input_rate = input_prior;
        c->work_rate = work_prior;
        c->xx = c->yy = ridge;
        c->xz = ridge * c->input_rate;
        c->yz = ridge * c->work_rate;
        if (structural) c->weight = ridge;
    }
    if (structural) {
        /* A new design row may supply at most as much information in its
         * direction as the accumulated design. Ordinary rows get full weight.
         * Bound the residual in either direction relative to the shorter
         * duration: preprocessing can discard an entire source body. */
        w = fmin(w, 1 / leverage(c, x, y));
        double fitted = fmax(.01, c->fixed + x*c->input_rate + y*c->work_rate);
        double scale = fmin(fitted, seconds);
        double residual = fabs(seconds - fitted);
        if (residual > scale) w *= scale / residual;
    }
    /* Keep one sample of slope prior through a run of uninformative probes.
     * Backends with measured work (e.g. LLVM instructions) retain their fit. */
    double anchor = structural ? step * ridge : 0;
    c->mass = keep * c->mass + step;
    c->mean += step / c->mass * (seconds - c->mean);
    c->weight = keep * c->weight + w;
    c->x = keep * c->x + w*x;
    c->y = keep * c->y + w*y;
    c->z = keep * c->z + w*seconds;
    c->xx = keep * c->xx + w*x*x + anchor;
    c->xy = keep * c->xy + w*x*y;
    c->yy = keep * c->yy + w*y*y + anchor;
    c->xz = keep * c->xz + w*x*seconds + anchor*input_prior;
    c->yz = keep * c->yz + w*y*seconds + anchor*work_prior;
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
    if (hint->kind > DCC_WORK_RUST && !c->xx) return 0;
    double fixed = !c->xx && hint->kind == DCC_WORK_RUST ? .03 : c->fixed;
    double seconds = fixed + hint->bytes / 1048576.0 * (c->xx ? c->input_rate : initial_rate(hint->kind))
                   + computation_size(hint) * (c->xx ? c->work_rate : initial_work_rate(hint->kind));
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
