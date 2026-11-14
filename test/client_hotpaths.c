/* Exercise actual client hot paths; --bench reports bounded local work. */
#include "frontend_fs.c"
#include "tcp_client.c"
#include "native_worker.h"
#include "frontend_identity.h"
#include "argv_file.h"
#include "broker.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

const char *rs_program_name = "client_hotpaths";

static double cpu(const struct rusage *r)
{
    return r->ru_utime.tv_sec + r->ru_stime.tv_sec +
           (r->ru_utime.tv_usec + r->ru_stime.tv_usec) / 1e6;
}

static void sample(const char *name, struct rusage *before)
{
    struct rusage after;
    assert(!getrusage(RUSAGE_SELF, &after));
    printf("%s cpu_seconds=%.6f minor_faults=%ld major_faults=%ld\n", name,
           cpu(&after) - cpu(before), after.ru_minflt - before->ru_minflt,
           after.ru_majflt - before->ru_majflt);
    *before = after;
}

static void chunk_contract(void)
{
    static unsigned char wire[DCC_TCP_CHUNK + 16U], snapshot[DCC_TCP_CHUNK + 16U];
    static unsigned char other[DCC_TCP_CHUNK + 16U], scratch[DCC_TCP_CHUNK + 16U];
    static unsigned char source[DCC_TCP_CHUNK + 37U], decoded[DCC_TCP_CHUNK];
    int fd = memfd_create("chunk-test", MFD_CLOEXEC);
    assert(fd >= 0);
    for (unsigned random = 0; random < 2; ++random) {
        uint32_t state = 19;
        for (unsigned i = 0; i < sizeof(source); ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            source[i] = random ? state : 'a';
        }
        assert(pwrite(fd, source, sizeof(source), 0) == sizeof(source));
        for (int immutable = 0; immutable < 2; ++immutable) {
            for (size_t offset = 0; offset < sizeof(source); offset += DCC_TCP_CHUNK) {
                size_t length = sizeof(source) - offset, size;
                if (length > DCC_TCP_CHUNK) length = DCC_TCP_CHUNK;
                int final = offset + length == sizeof(source);
                unsigned char *p = dcc_tcp_file_chunk(DCC_TCP_INPUT, 7, fd, offset,
                    length, final, immutable, wire, snapshot, &size);
                assert(p == wire || p == snapshot);
                assert(!!(dcc_tcp_get32(p) & DCC_TCP_FINAL) == final);
                assert(!!(dcc_tcp_get32(p) & DCC_TCP_COMPRESSED) == !random);
                if (size == 16) {
                    assert(immutable && random && p == wire);
                    /* sendfile supplies the immutable extent following the header. */
                    assert(pread(fd, p + 16, length, offset) == (ssize_t)length);
                    size += length;
                }
                struct dcc_tcp_frame frame = {.type = dcc_tcp_get32(p), .data = p + 8, .size = size - 8};
                const void *data;
                size_t plain;
                unsigned index;
                assert(!dcc_tcp_unpack(&frame, &index, decoded, &data, &plain));
                assert(index == 7 && plain == length && !memcmp(data, source + offset, plain));
                /* A second caller must not overwrite the first caller's queued bytes. */
                memcpy(other, p, size);
                size_t next_size;
                assert(dcc_tcp_file_chunk(DCC_TCP_INPUT, 9, fd, 0, 1, 1, 1,
                                         scratch, snapshot == p ? wire : snapshot, &next_size));
                assert(!memcmp(other, p, size));
            }
        }
    }
    size_t size;
    unsigned char *held = dcc_tcp_file_chunk(DCC_TCP_INPUT, 0, fd, 0,
        DCC_TCP_CHUNK, 0, 0, wire, snapshot, &size);
    assert(held == snapshot && size == DCC_TCP_CHUNK + 16U);
    assert(!ftruncate(fd, 0));
    assert(!memcmp(held + 16, source, DCC_TCP_CHUNK));
    assert(!dcc_tcp_file_chunk(DCC_TCP_INPUT, 0, fd, 0, 1, 1, 0, wire, snapshot, &size));
    assert(!dcc_tcp_file_chunk(DCC_TCP_INPUT, 0, fd, 0, 1, 1, 1, wire, snapshot, &size));
    assert(dcc_tcp_file_chunk(DCC_TCP_INPUT, 0, fd, 0, 0, 1, 0, wire, snapshot, &size));
    assert(size == 16);
    close(fd);
}

static void id_contract(void)
{
    uint64_t previous = transaction_id(), parent[32], child[32];
    int fds[2], status;
    assert(previous && !pipe(fds));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(fds[0]);
        for (unsigned i = 0; i < 32; ++i) child[i] = transaction_id();
        assert(write(fds[1], child, sizeof(child)) == sizeof(child));
        _exit(0);
    }
    close(fds[1]);
    for (unsigned i = 0; i < 32; ++i) {
        parent[i] = transaction_id();
        assert(parent[i] == previous + 1);
        previous = parent[i];
    }
    assert(read(fds[0], child, sizeof(child)) == sizeof(child));
    close(fds[0]);
    assert(waitpid(pid, &status, 0) == pid && status == 0);
    for (unsigned i = 0; i < 32; ++i) {
        assert(child[i] && (!i || child[i] == child[i - 1] + 1));
        for (unsigned j = 0; j < 32; ++j) assert(parent[i] != child[j]);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "--memo") || !strcmp(argv[1], "--worker")) {
        char *value = NULL;
        int worker = -1;
        int lease = !strcmp(argv[1], "--memo") ? dcc_broker_memo_get("retry", &value) :
                    dcc_broker_worker_get("retry", &worker);
        if (lease >= 0) { puts("owner"); close(lease); }
        else if (value) { puts(value); free(value); }
        else if (worker >= 0) {
            char byte;
            assert(read(worker, &byte, 1) == 1 && byte == 'W');
            puts("worker"); close(worker);
        } else { perror("memo_get"); return 1; }
        return 0;
    }
    chunk_contract();
    id_contract();
    char token[4] = {'A', 'R', 'G', 'C'};
    unsigned value = 42;
    assert(!dcc_parse_token_int("ARGCffffffff", token, &value) && value == UINT_MAX);
    assert(!dcc_parse_token_int("ARGCABCDEF01", token, &value) && value == 0xabcdef01U);
    assert(!dcc_parse_token_int("ARGC       1", token, &value) && value == 1);
    assert(!dcc_parse_token_int("ARGC ABCDEF1", token, &value) && value == 0xabcdef1U);
    assert(dcc_parse_token_int("ARGC        ", token, &value));
    assert(dcc_parse_token_int("ARGV00000001", token, &value));
    for (unsigned i = 4; i < 12; ++i) {
        for (const char *bad = "gG-+/\t\n"; *bad; ++bad) {
            char bytes[12];
            memcpy(bytes, "ARGC00000001", sizeof(bytes));
            bytes[i] = *bad;
            assert(dcc_parse_token_int(bytes, token, &value));
        }
        if (i > 4) {
            char bytes[12];
            memcpy(bytes, "ARGC00000001", sizeof(bytes));
            bytes[i] = ' ';
            assert(dcc_parse_token_int(bytes, token, &value));
        }
    }
    char strings[][10] = {"cc1", "", "a b", "-DVALUE=1"};
    char *arguments[] = {strings[0], strings[1], strings[2], strings[3], NULL}, *encoded;
    size_t length;
    assert(!dcc_argv_encode(token, "ARGV", arguments, &encoded, &length));
    assert(!memcmp(encoded, "ARGC00000004ARGV00000003cc1ARGV00000000", 39));
    int command = memfd_create("argv-test", MFD_CLOEXEC);
    assert(command >= 0 && write(command, encoded, length) == (ssize_t)length);
    assert(!lseek(command, 0, SEEK_SET));
    struct dcc_argv_file decoded;
    assert(!dcc_argv_file_read(command, 1, &decoded));
    for (int i = 0; arguments[i]; ++i) assert(!strcmp(arguments[i], decoded.groups[0][i]));
    assert(!decoded.groups[0][4]);
    dcc_argv_file_close(&decoded);
    close(command); free(encoded);
    int bench = !strcmp(argv[1], "--bench");
    char key[80], previous[80];
    static const char *const omitted[] = {"VOLATILE", NULL};
    assert(!setenv("STABLE", "first", 1) && !setenv("VOLATILE", "first", 1));
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    strcpy(previous, key);
    assert(!setenv("VOLATILE", "next", 1));
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    assert(!strcmp(key, previous));
    assert(!setenv("STABLE", "next", 1));
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    assert(strcmp(key, previous));
    char **saved = environ;
    char stable[] = "STABLE=next", variable[] = "VOLATILE=different";
    char *alternate[] = {stable, variable, NULL};
    environ = alternate;
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, previous, sizeof(previous)));
    char *reordered[] = {alternate[1], alternate[0], NULL};
    environ = reordered;
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    assert(!strcmp(key, previous));
    stable[7] = 'b';
    assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    assert(strcmp(key, previous)); /* putenv-style mutation in the same array. */
    environ = saved;
    char *authority = dcc_frontend_identity(), *same_authority = dcc_frontend_identity();
    assert(authority && same_authority && authority != same_authority && !strcmp(authority, same_authority));
    free(authority); free(same_authority);

    struct dcc_job job = {.cwd = (char *)"/source"};
    assert(!dcc_frontend_fs_add_root(&job, "inc/../include//."));
    assert(!strcmp(job.fs_roots[0], "/source/include"));
    assert(allowed(&job, "/source/./include/a/../b.h") == (PATH_CONTENTS|PATH_METADATA));
    assert(!allowed(&job, "/source/include/../../secret"));
    assert(!allowed(&job, "/source/include-more/header"));
    assert(allowed(&job, "/source") == PATH_METADATA);
    assert(!dcc_frontend_fs_add_root(&job, "/source/include/./sub"));
    assert(!job.fs_roots[1]);

    assert(!setenv("DISTCC_HOSTS", "--localslots_cpp=2 localhost/4", 1));
    dcc_read_localslots_configuration();
    assert(dcc_hostdef_local_cpp->slots_specified && dcc_hostdef_local_cpp->n_slots == 2);
    assert(!setenv("DISTCC_HOSTS", "--localslots_cpp=3 localhost/4", 1));
    dcc_read_localslots_configuration();
    assert(dcc_hostdef_local_cpp->n_slots == 3);
    if (!bench) {
        assert(!setenv("DISTCC_HOSTS", "localhost/4", 1));
        dcc_read_localslots_configuration();
        assert(!dcc_hostdef_local_cpp->slots_specified);
        for (int i = 0; i < 2; ++i) {
            const char *directory = i ? "two" : "one";
            assert(!mkdir(directory, 0700));
            char path[32];
            snprintf(path, sizeof(path), "%s/hosts", directory);
            FILE *file = fopen(path, "w");
            assert(file);
            fprintf(file, "--localslots_cpp=%d localhost/4\n", i + 5);
            assert(!fclose(file));
            assert(!unsetenv("DISTCC_HOSTS") && !setenv("DISTCC_DIR", directory, 1));
            dcc_read_localslots_configuration();
            assert(dcc_hostdef_local_cpp->n_slots == i + 5);
        }
    }
    struct rusage before;
    assert(!getrusage(RUSAGE_SELF, &before));
    for (int i = 0; i < (bench ? 2000 : 1); ++i)
        assert(!dcc_local_worker_key("/proc/self/exe", NULL, omitted, key, sizeof(key)));
    sample("identity", &before);
    size_t count = 0;
    while (saved[count]) ++count;
    char **relay = malloc((count + 2) * sizeof(*relay));
    assert(relay);
    memcpy(relay, saved, (count + 1) * sizeof(*relay));
    char volatile_binding[64];
    relay[count] = volatile_binding;
    relay[count + 1] = NULL;
    for (int i = 0; i < (bench ? 2000 : 1); ++i) {
        snprintf(volatile_binding, sizeof(volatile_binding), "VOLATILE=%d", i);
        environ = i % 2 ? relay : saved;
        assert(!dcc_local_worker_key(i % 2 ? "/bin/sh" : "/proc/self/exe",
                                    "/proc/self/exe", omitted, key, sizeof(key)));
    }
    environ = saved;
    free(relay);
    sample("identity_relay", &before);
    volatile int permissions = 0;
    for (int i = 0; i < (bench ? 200000 : 1); ++i)
        permissions += allowed(&job, "/source/./include/a/../b.h");
    assert(permissions > 0);
    sample("authorization", &before);
    for (int i = 0; i < (bench ? 2000 : 1); ++i) dcc_read_localslots_configuration();
    sample("localslots", &before);
    assert(!setenv("DISTCC_HOSTS", "--localslots_cpp=3 "
        "127.0.0.1:4200/4,tcp,stats=4300 127.0.0.1:4201/4,tcp,stats=4301 "
        "127.0.0.1:4202/4,tcp,stats=4302 127.0.0.1:4203/4,tcp,stats=4303", 1));
    for (int i = 0; i < (bench ? 2000 : 1); ++i) dcc_read_localslots_configuration();
    sample("localslots_four_hosts", &before);
    dcc_free_argv(job.fs_roots);
    return 0;
}
