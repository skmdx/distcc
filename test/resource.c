/* Admission parsing and cgroup reads against complete kernel-style records. */
#include <config.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static int fixture_open(const char *, int);
static time_t fixture_now = 1;
static time_t fixture_time(time_t *value)
{
    if (value) *value = fixture_now;
    return fixture_now;
}
#define open fixture_open
#define time fixture_time
#include "../src/resource.c"
#undef open
#undef time

static int limited, missing, own_reads, parent_reads;
static unsigned available_mib;

static int fixture_open(const char *path, int flags)
{
    const char *text = NULL;
    char memory[128];
    assert(flags == (O_RDONLY | O_CLOEXEC));
    if (available_mib && !strcmp(path, "/proc/meminfo")) {
        snprintf(memory, sizeof(memory), "MemTotal: 3831808 kB\nMemAvailable: %u kB\n", available_mib * 1024);
        text = memory;
    } else if (available_mib && strstr(path, "/memory.max")) text = "max\n";
    else if (available_mib && strstr(path, "/memory.current")) text = "0\n";
    else if (available_mib && strstr(path, "/memory.stat")) text = "inactive_file 0\nshmem 0\n";
    else if (!strcmp(path, "/proc/self/cgroup")) text = "0::/parent/child\n";
    else if (!strcmp(path, "/proc/meminfo")) text = "MemTotal: 2000 kB\nMemAvailable: 1000 kB\n";
    else if (!strcmp(path, "/proc/pressure/memory")) text = "some avg10=2.00 avg60=1.00 total=1\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/child/memory.current")) text = missing ? NULL : "3000\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/child/memory.stat")) {
        ++own_reads; text = "anon 800\nshmem 400\ninactive_file 1000\n";
    } else if (!strcmp(path, "/sys/fs/cgroup/parent/child/memory.max")) text = "9000\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/memory.max")) text = limited ? "12000\n" : "max\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/memory.current")) text = "10000\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/memory.stat")) {
        ++parent_reads; text = "inactive_file 2000\n";
    } else if (!strcmp(path, "/sys/fs/cgroup/parent/cpu.max")) text = "150000 100000\n";
    else if (!strcmp(path, "/sys/fs/cgroup/parent/memory.pressure")) text = "some avg10=5.00 avg60=1.00 total=1\n";
    else if (strstr(path, "/memory.max")) text = "max\n";
    else if (strstr(path, "/cpu.max")) text = "max 100000\n";
    if (!text) { errno = ENOENT; return -1; }
    int fd = memfd_create("resource-fixture", MFD_CLOEXEC);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!lseek(fd, 0, SEEK_SET));
    return fd;
}

int main(void)
{
    for (int mode = 0; mode < 4; ++mode) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            if (mode == 3) {
                assert(!unsetenv("DISTCC_JOB_MEMORY"));
                available_mib = 2320;
                assert(dcc_resource_slots(4) == 2);
                assert(dcc_resource_local_slots(4) == 4 && dcc_resource_local_slots(2) == 2);
                available_mib = 2288; ++fixture_now;
                assert(dcc_resource_slots(4) == 1);
                assert(dcc_resource_local_slots(4) == 4 && dcc_resource_local_slots(2) == 2);
                assert(!setenv("DISTCC_JOB_MEMORY", "1024", 1));
                assert(dcc_resource_local_slots(4) == 1);
                assert(!setenv("DISTCC_JOB_MEMORY", "512", 1));
                assert(dcc_resource_local_slots(4) == 4);
                assert(!setenv("DISTCC_JOB_MEMORY", "", 1));
                assert(dcc_resource_local_slots(4) == 4);
                const unsigned thresholds[][2] = {{1088,4},{1087,3},{320,1},{319,0}};
                for (unsigned i = 0; i < sizeof(thresholds)/sizeof(*thresholds); ++i) {
                    available_mib = thresholds[i][0]; ++fixture_now;
                    assert(dcc_resource_local_slots(4) == (int)thresholds[i][1]);
                }
                available_mib = 2288; ++fixture_now;
                assert(!setenv("DISTCC_MEMORY_LEARNING", "1", 1));
                dcc_resource_observe(1, 2048ULL * 1024 * 1024, 1);
                assert(dcc_resource_local_slots(4) == 4); /* Backend peaks do not govern client slots. */
                pressure = 12;
                assert(!dcc_resource_local_slots(4));
                pressure = 0;
                assert(dcc_resource_local_slots(4) == 4);
                _exit(0);
            }
            limited = mode == 1; missing = mode == 2;
            sample_resources();
            assert(available == (missing ? 1024000 : limited ? 4000 : 7000));
            assert(budget == (missing ? 1024000 : limited ? 5600 : 8600));
            assert(processors >= 1 && processors <= 2 && pressure == 5);
            assert(own_reads == !missing && parent_reads == limited);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    return 0;
}
