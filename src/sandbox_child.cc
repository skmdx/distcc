#include "sandbox_child.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <mntent.h>
#include <sched.h>
#include <seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static std::vector<sock_filter> compiler_filter;
// A client child can create files only in its disposable root. A daemon
// child can only read its read-only runtime and write its output/stdio FDs.
// Neither child needs process inspection, signalling, networking or exec.
bool dcc_restrict_compiler_child() {
    if (prctl(PR_SET_DUMPABLE, 0) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return false;
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct caps[2]{};
    if (syscall(SYS_capset, &header, caps)) return false;
    if (syscall(SYS_capget, &header, caps)) return false;
    for (const auto &word : caps)
        if (word.effective || word.permitted || word.inheritable) return false;
    sock_fprog program{static_cast<unsigned short>(compiler_filter.size()), compiler_filter.data()};
    return !prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

bool dcc_init_compiler_sandbox(bool client) {
    if (!client) {
        if (prctl(PR_SET_DUMPABLE, 0)) return false;
        // Reject an accidentally writable submount even if the namespace
        // root itself is read-only. Check once, before accepting descriptors.
        FILE *mounts = setmntent("/proc/self/mounts", "r");
        if (!mounts) return false;
        bool readonly = true;
        while (auto *mount = getmntent(mounts)) {
            struct stat info;
            if (!hasmntopt(mount, "ro") &&
                (stat(mount->mnt_dir, &info) || S_ISDIR(info.st_mode))) {
                fprintf(stderr, "distcc: expected read-only mount at %s\n", mount->mnt_dir);
                readonly = false;
                break;
            }
        }
        bool complete = !ferror(mounts);
        endmntent(mounts);
        if (!readonly || !complete || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
            prctl(PR_GET_SECCOMP) != SECCOMP_MODE_FILTER) return false;
    }
    // Compile the policy once in the resident image. Children only install it.
    auto rules = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (!rules) return false;
    int error = 0;
    for (auto name : {"read","readv","pread64","preadv","write","writev","pwrite64","pwritev",
          "fstat","newfstatat","stat","lstat","statx","statfs","fstatfs","close","close_range",
          "mmap","mmap2","mprotect","mremap","munmap","madvise","brk","getrandom","lseek","_llseek",
          "ftruncate","fsync","fdatasync","access","faccessat","faccessat2","getdents64","readlink",
          "readlinkat","getcwd","getpid","getppid","gettid","getuid","geteuid","getgid","getegid",
          "getgroups","dup","dup2","dup3","gettimeofday","clock_gettime",
          "clock_getres","clock_nanosleep","nanosleep","getrusage","getrlimit","getcpu",
          "sched_getaffinity","sched_yield","sysinfo","uname","rt_sigaction","rt_sigprocmask",
          "rt_sigreturn","sigaltstack","futex","futex_waitv","set_robust_list","rseq",
          "pipe","pipe2","poll","ppoll","exit","exit_group"}) {
        int nr = seccomp_syscall_resolve_name(name);
        if (nr >= 0) error |= seccomp_rule_add(rules, SCMP_ACT_ALLOW, nr, 0);
    }
    for (auto name : {"fcntl", "fcntl64"}) {
        int nr = seccomp_syscall_resolve_name(name);
        if (nr < 0) continue;
        for (unsigned cmd : {F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, F_SETFL})
            error |= seccomp_rule_add(rules, SCMP_ACT_ALLOW, nr, 1, SCMP_A1(SCMP_CMP_EQ, cmd));
    }
    // glibc retries clone3 using clone when the former is unavailable.
    error |= seccomp_rule_add(rules, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);
    error |= seccomp_rule_add(rules, SCMP_ACT_ALLOW, SCMP_SYS(clone), 1,
        SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_THREAD | CLONE_NEWUSER, CLONE_THREAD));
    error |= seccomp_rule_add(rules, SCMP_ACT_ALLOW, SCMP_SYS(prlimit64), 2,
        SCMP_A0(SCMP_CMP_EQ, 0), SCMP_A2(SCMP_CMP_EQ, 0));
    for (auto name : {"open","openat"}) {
        int nr = seccomp_syscall_resolve_name(name);
        if (nr < 0) continue;
        scmp_arg_cmp flags{unsigned(nr == SCMP_SYS(openat) ? 2 : 1), SCMP_CMP_MASKED_EQ,
            O_ACCMODE | O_CREAT | O_TRUNC | __O_TMPFILE, 0};
        error |= seccomp_rule_add_array(rules, SCMP_ACT_ALLOW, nr, client ? 0 : 1, &flags);
    }
    if (client)
        for (auto name : {"rename","renameat","renameat2","unlink","unlinkat","umask"}) {
            int nr = seccomp_syscall_resolve_name(name);
            if (nr >= 0) error |= seccomp_rule_add(rules, SCMP_ACT_ALLOW, nr, 0);
        }
    int fd = memfd_create("distcc-compiler-filter", MFD_CLOEXEC);
    if (!error && fd >= 0) error = seccomp_export_bpf(rules, fd);
    seccomp_release(rules);
    if (fd < 0) return false;
    struct stat st;
    if (!error && !fstat(fd, &st) && st.st_size > 0 &&
        st.st_size % sizeof(sock_filter) == 0 && st.st_size / sizeof(sock_filter) <= USHRT_MAX) {
        compiler_filter.resize(st.st_size / sizeof(sock_filter));
        error = pread(fd, compiler_filter.data(), st.st_size, 0) != st.st_size;
    } else error = 1;
    close(fd);
    return !error;
}
