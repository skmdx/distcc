/* Retain GCC's loaded image, never its per-invocation compiler state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include "native_worker.h"
extern "C" {
#include "argv_file.h"
}

namespace {
using main_function = int (*)(int, char **, char **);
main_function compiler_main;

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
