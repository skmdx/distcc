/* Exercise actual client hot paths; --bench reports bounded local work. */
#include "frontend_fs.c"
#include "native_worker.h"
#include "hosts.h"
#include "frontend_identity.h"
#include "argv_file.h"
#include "broker.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/resource.h>

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
    char token[4] = {'A', 'R', 'G', 'C'};
    unsigned value = 42;
    assert(!dcc_parse_token_int("ARGCffffffff", token, &value) && value == UINT_MAX);
    assert(!dcc_parse_token_int("ARGCABCDEF01", token, &value) && value == 0xabcdef01U);
    assert(dcc_parse_token_int("ARGV00000001", token, &value));
    for (unsigned i = 4; i < 12; ++i) {
        for (const char *bad = "gG-+ /"; *bad; ++bad) {
            char bytes[12];
            memcpy(bytes, "ARGC00000001", sizeof(bytes));
            bytes[i] = *bad;
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
