/* Retain GCC's loaded image, never its per-invocation compiler state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <spawn.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include "native_worker.h"
extern "C" {
#include "argv_file.h"
}

namespace {
using main_function = int (*)(int, char **, char **);
main_function compiler_main;
std::string capture_program;

bool capturing(const char *file, char *const argv[])
{
    return !capture_program.empty() && capture_program == file && argv[0] &&
        argv[1] && !strcmp(argv[1], "--gcc-plan-capture");
}
int prepare_relay(char *const argv[], char *const environment[])
{
    // GCC still owns this child and waits normally. Its generated response
    // files stay live until planning finishes, then GCC removes them itself.
    if (ftruncate(3, 0) || lseek(3, 0, SEEK_SET) < 0) return 1;
    char *cwd = getcwd(nullptr, 0);
    if (!cwd) return 1;
    char *directory[] = {cwd, nullptr};
    int status = 0;
    for (char *const *group : {static_cast<char *const *>(directory), argv + 2, environment}) {
        char *data = nullptr;
        size_t size = 0;
        status = dcc_argv_encode("ARGC", "ARGV", const_cast<char **>(group), &data, &size);
        size_t offset = 0;
        while (!status && offset < size) {
            ssize_t n = write(3, data + offset, size - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) status = 1;
            else offset += n;
        }
        free(data);
        if (status) break;
    }
    free(cwd);
    return status;
}
int wait_for_plan(void *)
{
    // Runs on a separate stack while the driver is suspended. Only IPC and
    // exit are allowed here: no allocator, stdio or compiler-state mutation.
    uint32_t result = 1;
    ssize_t n = -1;
    if (send(4, "P", 1, MSG_NOSIGNAL) == 1)
        do { n = recv(4, &result, sizeof(result), MSG_TRUNC); } while (n < 0 && errno == EINTR);
    return n == sizeof(result) && result <= 255 ? result : 1;
}
[[noreturn]] void relay(char *const argv[], char *const environment[])
{
    _exit(prepare_relay(argv, environment) ? 1 : wait_for_plan(nullptr));
}
int spawn_relay(pid_t *pid, char *const argv[], char *const environment[])
{
    if (prepare_relay(argv, environment)) return EIO;
    // Like posix_spawn, share the address space only until this tiny child
    // exits. Forking the loaded driver copied page tables and tore them down
    // for an invocation that only sends a notification and waits for status.
    // The platform's thread-stack minimum supplies a separate aligned stack.
    long minimum = sysconf(_SC_THREAD_STACK_MIN);
    if (minimum <= 0) return EINVAL;
    constexpr size_t alignment = alignof(max_align_t);
    size_t size = (size_t(minimum) + alignment - 1) & ~(alignment - 1);
    char *stack = static_cast<char *>(malloc(size));
    if (!stack) return ENOMEM;
    sigset_t blocked, previous;
    sigfillset(&blocked);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) { int error = errno; free(stack); return error; }
    // Block inherited handlers while sharing memory. The existing owner uses
    // SIGKILL on cancellation, so both the suspended driver and child stop.
    pid_t child = clone(wait_for_plan, stack + size, CLONE_VM | CLONE_VFORK | SIGCHLD, nullptr);
    int error = errno;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    free(stack); // CLONE_VFORK returns only after the child releases this memory.
    if (child < 0) return error;
    *pid = child;
    return 0;
}

int execute(int fd, bool preparing)
{
    dcc_argv_file command{};
    if (!preparing || dcc_argv_file_read(fd, 3, &command)) return 1;
    char **directory = command.groups[0], **argv = command.groups[1];
    if (!directory[0] || directory[1] || !argv[0] || chdir(directory[0])) return 1;
    // The driver parsed every option. Only our private output descriptor
    // changes when the command crosses SCM_RIGHTS into this child.
    constexpr char option[] = "-fplugin-arg-distcc_gcc_scan-plan-fd=";
    bool output = false;
    int argc = 0;
    for (; argv[argc]; ++argc) if (!strncmp(argv[argc], option, sizeof(option) - 1)) {
        argv[argc] = const_cast<char *>("-fplugin-arg-distcc_gcc_scan-plan-fd=3");
        output = true;
    }
    constexpr char suffix[] = ",--gcc-plan-capture";
    for (int i = 0; i + 1 < argc; ++i) if (!strcmp(argv[i], "-wrapper")) {
        const char *wrapper = argv[i + 1];
        size_t n = strlen(wrapper), tail = sizeof(suffix) - 1;
        if (n > tail && !strcmp(wrapper + n - tail, suffix))
            capture_program.assign(wrapper, n - tail);
    }
    if (!output || ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) return 1;
    environ = command.groups[2];
    // main has never run in the parent. GCC parses the complete fresh argv
    // and environment, observes current paths, and loads its plugins anew.
    return compiler_main(argc, argv, environ);
}

int serve(int, char **, char **)
{
    const char *value = getenv("DISTCC_GCC_PLAN_CONTROL");
    char *end;
    long control = value ? strtol(value, &end, 10) : -1;
    if (!value || !*value || *end || control < 3 || control > 0x7fffffff) return 1;
    return dcc_native_worker(control, execute);
}
}

// GCC's libiberty can use posix_spawn or fork/exec, depending on its build.
// Only the private capture wrapper is intercepted; user wrappers keep their
// ordinary spawn attributes, file actions and execution semantics.
#define DCC_EXPORT extern "C" __attribute__((visibility("default")))
DCC_EXPORT int posix_spawn(pid_t *pid, const char *file,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[])
{
    if (capturing(file, argv)) return spawn_relay(pid, argv, environment);
    auto next = reinterpret_cast<decltype(&posix_spawn)>(dlsym(RTLD_NEXT, "posix_spawn"));
    return next ? next(pid, file, actions, attributes, argv, environment) : ENOSYS;
}
DCC_EXPORT int posix_spawnp(pid_t *pid, const char *file,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
    char *const argv[], char *const environment[])
{
    if (capturing(file, argv)) return spawn_relay(pid, argv, environment);
    auto next = reinterpret_cast<decltype(&posix_spawnp)>(dlsym(RTLD_NEXT, "posix_spawnp"));
    return next ? next(pid, file, actions, attributes, argv, environment) : ENOSYS;
}
DCC_EXPORT int execv(const char *file, char *const argv[])
{
    if (capturing(file, argv)) relay(argv, environ);
    auto next = reinterpret_cast<decltype(&execv)>(dlsym(RTLD_NEXT, "execv"));
    if (next) return next(file, argv);
    errno = ENOSYS; return -1;
}
DCC_EXPORT int execvp(const char *file, char *const argv[])
{
    if (capturing(file, argv)) relay(argv, environ);
    auto next = reinterpret_cast<decltype(&execvp)>(dlsym(RTLD_NEXT, "execvp"));
    if (next) return next(file, argv);
    errno = ENOSYS; return -1;
}

// glibc performs ELF constructors before calling this replacement main.
// The supported x86_64/aarch64 glibc entry ABI passes the original main here.
extern "C" __attribute__((visibility("default")))
int __libc_start_main(main_function main, int argc, char **argv,
                      void (*init)(), void (*fini)(), void (*rtld_fini)(), void *stack)
{
    using start_function = int (*)(main_function, int, char **,
                                   void (*)(), void (*)(), void (*)(), void *);
    auto start = reinterpret_cast<start_function>(dlsym(RTLD_NEXT, "__libc_start_main"));
    if (!start) _exit(1);
    compiler_main = main;
    return start(getenv("DISTCC_GCC_PLAN_CONTROL") ? serve : main,
                 argc, argv, init, fini, rtld_fini, stack);
}
