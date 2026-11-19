/* Bounded job representations; no legacy RPC tokens or payload staging.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <search.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "job.h"
#include "rust/api.h"
#include "distcc.h"

const char *const dcc_gcc_locale_names[DCC_GCC_LOCALE_COUNT] = {
    "LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE", "LC_MESSAGES"
};

struct metadata {
    unsigned char *data;
    size_t size, offset, capacity;
    int error;
};
const char *dcc_job_tmp_top(const struct dcc_job *job)
{
    const char *tmp;
    if (dcc_job_tree(job)) {
        tmp = getenv("DISTCCD_RUST_TMPDIR");
        return tmp && *tmp ? tmp : "/var/tmp";
    }
    return dcc_get_tmp_top(&tmp) ? NULL : tmp;
}

int dcc_job_memfd(const char *name)
{
    return memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
}

int dcc_job_capture_stdin(struct dcc_job *job)
{
    int fd = dcc_job_memfd("distcc-stdin");
    if (fd < 0) return -1;
    job->stdin_fd = fd + 1;
    char bytes[65536];
    for (;;) {
        ssize_t size = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0 || (size && dcc_writex(fd, bytes, size))) return -1;
        if (!size) break;
    }
    return fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)
        ? -1 : 0;
}
static void put(struct metadata *m, const void *data, size_t size)
{
    size_t needed = m->size + size, capacity;
    unsigned char *next;
    if (m->error) return;
    if (size > DCC_JOB_METADATA_MAX - m->size) { m->error = 1; return; }
    if (needed > m->capacity) {
        capacity = m->capacity ? m->capacity : 4096;
        while (capacity < needed) capacity *= 2;
        if (!(next = realloc(m->data, capacity))) { m->error = 1; return; }
        m->data = next;
        m->capacity = capacity;
    }
    if (size) memcpy(m->data + m->size, data, size);
    m->size = needed;
}
static void number(struct metadata *m, uint64_t n)
{
    unsigned char bytes[10];
    unsigned length = 0;
    do {
        bytes[length++] = (n & 127) | (n >= 128 ? 128 : 0);
        n >>= 7;
    } while (n);
    put(m, bytes, length);
}
static void string(struct metadata *m, const char *s)
{
    size_t size = s ? strlen(s) : 0;
    number(m, size);
    put(m, s, size);
}
static const unsigned char *take(struct metadata *m, size_t size)
{
    const unsigned char *p;
    if (m->error || size > m->size - m->offset) { m->error = 1; return NULL; }
    p = m->data + m->offset;
    m->offset += size;
    return p;
}
static uint64_t get_number(struct metadata *m)
{
    uint64_t n = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        const unsigned char *p = take(m, 1);
        if (!p || (shift == 63 && *p > 1) || (shift && !*p)) break;
        n |= (uint64_t)(*p & 127) << shift;
        if (*p < 128) return n;
    }
    m->error = 1;
    return 0;
}
static char *get_string(struct metadata *m)
{
    uint64_t size = get_number(m);
    const unsigned char *p;
    char *s;
    if (m->error || size > DCC_JOB_METADATA_MAX) { m->error = 1; return NULL; }
    p = take(m, size);
    if (!p || memchr(p, 0, size)) { m->error = 1; return NULL; }
    s = malloc(size + 1);
    if (!s) { m->error = 1; return NULL; }
    memcpy(s, p, size);
    s[size] = 0;
    return s;
}
static int load(int fd, size_t offset, struct metadata *m)
{
    struct stat st;
    size_t done = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size < offset || st.st_size - offset == 0 ||
        st.st_size - offset > DCC_JOB_METADATA_MAX) return -1;
    m->size = st.st_size - offset;
    if (!(m->data = malloc(m->size))) return -1;
    while (done < m->size) {
        ssize_t n = pread(fd, m->data + done, m->size - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(m->data); m->data = NULL; return -1; }
        done += n;
    }
    return 0;
}
void dcc_job_free(struct dcc_job *job)
{
    unsigned i;
    if (!job) return;
    if (job->stdin_fd) close(job->stdin_fd - 1);
    if (job->argv) for (i = 0; job->argv[i]; ++i) free(job->argv[i]);
    free(job->argv);
    free(job->rust.crate_types); free(job->rust.linker); free(job->rust.compiler); free(job->rust.host); free(job->rust.cpu); free(job->rust.features);
    if (job->rust.directories) {
        for (char **p = job->rust.directories; *p; ++p) free(*p);
        free(job->rust.directories);
    }
    if (job->rust.environment) {
        for (char **p = job->rust.environment; *p; ++p) free(*p);
        free(job->rust.environment);
    }
    if (job->fs_roots) {
        for (i = 0; job->fs_roots[i]; ++i) free(job->fs_roots[i]);
        free(job->fs_roots);
    }
    for (i = 0; i < job->count; ++i) {
        free(job->inputs[i].path);
        free(job->inputs[i].target);
        if (job->inputs[i].fd >= 0) close(job->inputs[i].fd);
    }
    free(job->inputs);
    free(job->cwd);
    free(job);
}
void dcc_job_result_init(struct dcc_job_result *r)
{
    unsigned i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) r->fd[i] = -1;
}
void dcc_job_result_free(struct dcc_job_result *r)
{
    unsigned i;
    for (i = 0; i < r->named_count; ++i) {
        struct dcc_named_output *f = r->named + i;
        if (f->fd >= 0) close(f->fd);
        if (f->temporary) unlink(f->temporary);
        free(f->name); free(f->temporary); free(f->destination);
    }
    free(r->named); r->named = NULL; r->named_count = 0;
    for (i = 0; i < DCC_JOB_RESULT_COUNT; ++i) {
        if (r->fd[i] >= 0) { close(r->fd[i]); r->fd[i] = -1; }
        if (r->temporary[i]) { unlink(r->temporary[i]); free(r->temporary[i]); r->temporary[i] = NULL; }
    }
}
/* IDs are assigned on first use. Zero introduces the next parent. */
struct parent_name { const char *path; size_t length; unsigned id; };
static int parent_order(const void *aa, const void *bb)
{
    const struct parent_name *a = aa, *b = bb;
    if (a->length != b->length) return a->length < b->length ? -1 : 1;
    return memcmp(a->path, b->path, a->length);
}
static void keep_parent(void *parent) { (void)parent; }

static void strings(struct metadata *m, char **words)
{
    for (; words && *words; ++words) string(m, *words);
    string(m, "");
}

/* Each list contains nonempty path or NAME=value records, followed by the
 * empty string. The metadata parser owns all storage even on a short read. */
static int rust_environment(const char *entry)
{
    const char *eq = strchr(entry, '=');
    return eq && eq != entry && strncmp(entry, "LD_", 3) && strncmp(entry, "DYLD_", 5) &&
        strncmp(entry, "RUSTC_WRAPPER=", sizeof("RUSTC_WRAPPER=") - 1) &&
        strncmp(entry, "RUSTC_WORKSPACE_WRAPPER=", sizeof("RUSTC_WORKSPACE_WRAPPER=") - 1);
}
static int get_strings(struct metadata *m, char ***words, int (*accept)(const char *))
{
    unsigned count = 0, capacity = 0;
    for (;;) {
        char *s = get_string(m);
        if (!s) return -1;
        if (!*s) { free(s); return 0; }
        if (!accept(s)) { free(s); return -1; }
        if (count == capacity) {
            if (capacity == DCC_JOB_INPUT_MAX) { free(s); return -1; }
            capacity = capacity ? 2 * capacity : 8;
            char **next = realloc(*words, (capacity + 1) * sizeof(*next));
            if (!next) { free(s); return -1; }
            *words = next;
        }
        (*words)[count++] = s;
        (*words)[count] = NULL;
    }
}

unsigned char *dcc_job_encode_buffer(const struct dcc_job *job, size_t *size)
{
    struct metadata m = {0};
    unsigned argc = 0, i;
    if (!job || job->kind >= DCC_JOB_KIND_COUNT || job->count > DCC_JOB_INPUT_MAX ||
        (job->count && !job->inputs) || !job->argv || !job->cwd) return NULL;
    while (job->argv[argc]) if (++argc > DCC_JOB_INPUT_MAX) return NULL;
    void *tree = NULL;
    unsigned parents = 0;
    struct parent_name *names = calloc(job->count, sizeof(*names));
    if (job->count && !names) return NULL;
    number(&m, job->kind);
    number(&m, argc);
    number(&m, job->count);
    string(&m, job->cwd);
    for (i = 0; i < argc; ++i) string(&m, job->argv[i]);
    if (job->kind == DCC_JOB_RUST) {
        number(&m, job->rust.phase);
        number(&m, job->rust.pipeline);
        string(&m, job->rust.linker);
        string(&m, job->rust.crate_types);
        string(&m, job->rust.compiler);
        string(&m, job->rust.host);
        string(&m, job->rust.cpu);
        string(&m, job->rust.features);
        strings(&m, job->rust.directories);
        strings(&m, job->rust.environment);
    }
    for (i = 0; i < job->count; ++i) {
        const struct dcc_job_input *in = &job->inputs[i];
        number(&m, in->kind);
        number(&m, in->encoding);
        number(&m, in->cached);
        number(&m, in->wire_size);
        number(&m, in->plain_size);
        put(&m, in->digest, sizeof(in->digest));
        const char *slash = in->path ? strrchr(in->path, '/') : NULL;
        if (!slash || !slash[1]) { m.error = 1; break; }
        names[i] = (struct parent_name){ in->path, slash - in->path + 1, parents + 1 };
        struct parent_name **entry = tsearch(&names[i], &tree, parent_order);
        if (!entry) { m.error = 1; break; }
        if (*entry == &names[i]) {
            ++parents;
            number(&m, 0);
            number(&m, names[i].length);
            put(&m, in->path, names[i].length);
        } else number(&m, (*entry)->id);
        string(&m, slash + 1);
        string(&m, in->target);
    }
    if (tree) tdestroy(tree, keep_parent);
    free(names);
    if (m.error) { free(m.data); return NULL; }
    *size = m.size;
    return m.data;
}
static int absolute_path(const char *s)
{
    return s && s[0] == '/' && !strstr(s, "/../") && !strstr(s, "/./") &&
        !strstr(s, "//") && (strlen(s) < 3 || strcmp(s + strlen(s) - 3, "/..")) &&
        (strlen(s) < 2 || strcmp(s + strlen(s) - 2, "/."));
}
int dcc_job_decode_buffer(const void *data, size_t size, struct dcc_job **out)
{
    struct metadata m = { .data = (unsigned char *)data, .size = size };
    struct dcc_job *job = NULL;
    uint64_t kind, argc, count;
    char **parents = NULL;
    unsigned parent_count = 0;
    size_t path_bytes = 0;
    unsigned i;
    *out = NULL;
    if (!data || !size || size > DCC_JOB_METADATA_MAX) return -1;
    kind = get_number(&m);
    argc = get_number(&m);
    count = get_number(&m);
    if (m.error || kind >= DCC_JOB_KIND_COUNT || !argc || argc > DCC_JOB_INPUT_MAX ||
        count > DCC_JOB_INPUT_MAX || !(job = calloc(1, sizeof(*job)))) goto failed;
    job->kind = kind;
    if (!(job->argv = calloc(argc + 1, sizeof(*job->argv))) ||
        (count && !(job->inputs = calloc(count, sizeof(*job->inputs))))) goto failed;
    if (count && !(parents = calloc(count, sizeof(*parents)))) goto failed;
    job->count = count;
    for (i = 0; i < count; ++i) job->inputs[i].fd = -1;
    job->cwd = get_string(&m);
    if (!absolute_path(job->cwd)) goto failed;
    for (i = 0; i < argc; ++i) if (!(job->argv[i] = get_string(&m))) goto failed;
    if (kind == DCC_JOB_RUST_CGU && (argc != 2 || count != 1 || !*job->argv[1])) goto failed;
    if (kind == DCC_JOB_RUST) {
        uint64_t phase = get_number(&m), pipeline = get_number(&m);
        if (phase > DCC_RUST_OPTIMIZED || pipeline > 1 ||
            !(job->rust.linker = get_string(&m)) || strchr(job->rust.linker, '/') ||
            !strcmp(job->rust.linker, ".") || !strcmp(job->rust.linker, "..") ||
            !(job->rust.crate_types = get_string(&m)) ||
            !(job->rust.compiler = get_string(&m)) || !*job->rust.compiler ||
            !(job->rust.host = get_string(&m)) ||
            !(job->rust.cpu = get_string(&m)) || !(job->rust.features = get_string(&m)) ||
            get_strings(&m, &job->rust.directories, absolute_path) ||
            get_strings(&m, &job->rust.environment, rust_environment)) goto failed;
        /* Crate types have their own field. A second spelling in the raw
         * arguments would give the compiler a different command contract. */
        for (i = 0; i < argc; ++i)
            if (!strcmp(job->argv[i], "--crate-type") || !strncmp(job->argv[i], "--crate-type=", 13))
                goto failed;
        for (const char *p = job->rust.crate_types; *p;) {
            size_t n = strcspn(p, ",");
            if (n == sizeof("proc-macro") - 1 && !memcmp(p, "proc-macro", n)) goto failed;
            p += n;
            if (*p) ++p;
        }
        job->rust.phase = phase;
        job->rust.pipeline = pipeline;
    }
    for (i = 0; i < count; ++i) {
        struct dcc_job_input *in = &job->inputs[i];
        uint64_t type = get_number(&m), encoding = get_number(&m), cached = get_number(&m);
        const unsigned char *digest;
        in->wire_size = get_number(&m);
        in->plain_size = get_number(&m);
        digest = take(&m, sizeof(in->digest));
        if (m.error || type > DCC_JOB_READONLY || encoding > DCC_JOB_LZ4 || cached > DCC_CACHE_PUSH ||
            (cached == DCC_CACHE_PUSH && (job->kind != DCC_JOB_PUMP || in->plain_size > DCC_SOURCE_PUSH_MAX)) ||
            in->wire_size > INT64_MAX || in->plain_size > INT64_MAX) goto failed;
        in->kind = type;
        in->encoding = encoding;
        in->cached = cached;
        memcpy(in->digest, digest, sizeof(in->digest));
        uint64_t id = get_number(&m);
        if (id > parent_count || m.error) goto failed;
        if (!id) {
            char *parent = get_string(&m);
            parents[parent_count++] = parent;
            if (!absolute_path(parent) || parent[strlen(parent)-1] != '/') goto failed;
            id = parent_count;
        }
        char *base = get_string(&m);
        if (!base) goto failed;
        size_t length = strlen(parents[id-1]) + strlen(base) + 1;
        if (*base && !strchr(base, '/') && length <= DCC_JOB_METADATA_MAX - path_bytes) {
            path_bytes += length;
            in->path = malloc(length);
            if (in->path) snprintf(in->path, length, "%s%s", parents[id-1], base);
        }
        free(base);
        in->target = get_string(&m);
        if (!absolute_path(in->path) || !strcmp(in->path, "/") ||
            (kind == DCC_JOB_RUST && (in->kind != DCC_JOB_FILE ||
             !strcmp(in->path,DCC_RUST_ROOT) || !strncmp(in->path,DCC_RUST_ROOT "/",sizeof(DCC_RUST_ROOT)))) ||
            in->path[strlen(in->path) - 1] == '/' || !in->target ||
            (in->kind != DCC_JOB_SYMLINK && *in->target) ||
            (!dcc_job_file(in) && (in->cached || in->wire_size || in->plain_size)) ||
            (in->kind == DCC_JOB_SYMLINK && !*in->target) ||
            (dcc_job_file(in) && in->encoding == DCC_JOB_RAW && in->wire_size != in->plain_size)) goto failed;
    }
    if (m.error || m.offset != m.size) goto failed;
    *out = job;
failed:
    for (i = 0; i < parent_count; ++i) free(parents[i]);
    free(parents);
    if (!*out) dcc_job_free(job);
    return *out ? 0 : -1;
}
int dcc_job_decode(int fd, size_t offset, struct dcc_job **out)
{
    struct metadata m = {0};
    *out = NULL;
    if (load(fd, offset, &m)) return -1;
    int ret = dcc_job_decode_buffer(m.data, m.size, out);
    free(m.data);
    return ret;
}
