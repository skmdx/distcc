/* The client planner reads frozen indexes. Execution children own their inputs and outputs. */
#include "lto_backend.h"
#include "compiler_command.h"
#include "../fd_io.h"
#include <lld/Common/Driver.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/ModuleSummaryIndex.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/ObjCopy/CommonConfig.h>
#include <llvm/ObjCopy/ELF/ELFConfig.h>
#include <llvm/ObjCopy/ELF/ELFObjcopy.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <mntent.h>
#include <sched.h>
#include <seccomp.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "../job.h"
#include <sys/syscall.h>
#include <unistd.h>

LLD_HAS_DRIVER(elf)
using namespace llvm;
namespace {
std::vector<sock_filter> compiler_filter;
bool reserve_descriptors(rlim_t needed) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) return false;
    auto target = std::min(needed, limit.rlim_max);
    if (limit.rlim_cur >= target) return true;
    limit.rlim_cur = target;
    return setrlimit(RLIMIT_NOFILE, &limit) == 0;
}
// A client child can create files only in its disposable root. A daemon
// child can only read its read-only runtime and write its output/stdio FDs.
// Neither child needs process inspection, signalling, networking or exec.
bool restrict_job() {
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
}

bool dcc_lto_init(bool client) {
    if (!client) {
        if (!reserve_descriptors(DCC_JOB_INPUT_MAX)) return false;
        // Reject an accidentally writable submount even if the namespace
        // root itself is read-only. Check once, before accepting descriptors.
        FILE *mounts = setmntent("/proc/self/mounts", "r");
        if (!mounts) return false;
        bool readonly = true;
        while (auto *mount = getmntent(mounts)) {
            struct stat info;
            if (!hasmntopt(mount, "ro") &&
                (stat(mount->mnt_dir, &info) || S_ISDIR(info.st_mode))) {
                errs() << "distcc ThinLTO: expected read-only mount at " << mount->mnt_dir << '\n';
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

namespace {
bool enter_output(StringRef directory) {
    if (chdir(directory.str().c_str())) return false;
    // Open all inputs before giving up host credentials, including when a
    // root Portage install runs beneath a different user's private directory.
    if (chroot(".") && (errno != EPERM || unshare(CLONE_NEWUSER) || chroot("."))) return false;
    return !chdir("/") && restrict_job();
}

Error merge(const json::Array &parts, const json::Array &locals) {
    SmallVector<std::string> command{"ld.lld","-r","-o","merged.o"};
    for (const auto &part : parts) {
        auto name = part.getAsString();
        if (!name) return createStringError("invalid merge input");
        command.push_back(name->str());
    }
    SmallVector<const char *> args;
    for (auto &word : command) args.push_back(word.c_str());
    if (lld::lldMain(args, outs(), errs(), {{lld::Gnu, &lld::elf::link}}).retCode)
        return createStringError("relocatable link failed");
    if (locals.empty()) return Error::success();
    objcopy::CommonConfig config;
    for (const auto &local : locals) {
        auto name = local.getAsString();
        if (!name) return createStringError("invalid local symbol");
        if (auto e = config.SymbolsToLocalize.addMatcher(objcopy::NameOrPattern::create(
                *name, objcopy::MatchStyle::Literal, [](Error e) { return e; }))) return e;
    }
    auto binary = object::createBinary("merged.o");
    if (!binary) return binary.takeError();
    auto *elf = dyn_cast<object::ELFObjectFileBase>(binary->getBinary());
    if (!elf) return createStringError("expected a relocatable ELF object");
    std::error_code ec;
    raw_fd_ostream output("localized.o", ec);
    if (ec) return errorCodeToError(ec);
    if (Error e = objcopy::elf::executeObjcopyOnBinary(config, {}, *elf, output)) return e;
    output.close();
    return errorCodeToError(output.error());
}

int fail(StringRef message) {
    errs() << "distcc ThinLTO: " << message << '\n';
    return 1;
}
}

int dcc_lto_plan(const char *description, int output) {
    auto buffer = MemoryBuffer::getFile(description);
    if (!buffer) return fail(buffer.getError().message());
    auto request = json::parse((*buffer)->getBuffer());
    if (!request) return fail(toString(request.takeError()));
    auto *jobs = request->getAsArray();
    if (!jobs) return fail("expected ThinLTO input descriptions");
    json::Array weights;
    for (auto &value : *jobs) {
        auto *job = value.getAsObject();
        if (!job || !job->getString("source") || !job->getString("index"))
            return fail("missing ThinLTO source or index");
        auto source = *job->getString("source");
        auto index = getModuleSummaryIndexForFile(*job->getString("index"));
        if (!index) return fail(toString(index.takeError()));
        int64_t instructions = 0, functions = 0, largest = 0;
        for (auto &entry : **index) {
            for (auto &summary : entry.second.getSummaryList()) {
                auto *function = dyn_cast<FunctionSummary>(summary.get());
                if (!function || !(**index).isGlobalValueLive(function)) continue;
                bool primary = function->modulePath() == source;
                if (!primary && function->importType() != GlobalValueSummary::Definition) continue;
                instructions += function->instCount();
                largest = std::max<int64_t>(largest, function->instCount());
                if (primary && function->linkage() != GlobalValue::AvailableExternallyLinkage)
                    ++functions;
            }
        }
        weights.push_back(json::Object{{"instructions", instructions}, {"functions", functions},
                                       {"largest_function", largest}});
    }
    if (ftruncate(output, 0) || lseek(output, 0, SEEK_SET) < 0) return fail(strerror(errno));
    raw_fd_ostream stream(output, false);
    stream << json::Value(std::move(weights));
    stream.flush();
    auto error = stream.error();
    stream.clear_error();
    return error ? fail(error.message()) : 0;
}

int dcc_lto_local(const char *description, unsigned operation) {
    auto buffer = MemoryBuffer::getFile(description);
    if (!buffer) return fail(buffer.getError().message());
    auto data = json::parse((*buffer)->getBuffer());
    if (!data) return fail(toString(data.takeError()));
    auto *job = data->getAsObject();
    if (!job) return fail("expected job description");
    auto dir = sys::path::parent_path(description);
    close(3); close(STDIN_FILENO);
    if (operation == 'M') {
        auto *parts = job->getArray("parts"), *names = job->getArray("locals");
        if (!parts || !names) return fail("invalid merge job");
        if (!enter_output(dir)) return fail(strerror(errno));
        if (auto error = merge(*parts, *names)) return fail(toString(std::move(error)));
        return 0;
    }
    LtoInputs inputs;
    auto cwd = job->getString("cwd");
    auto count = job->getInteger("partitions");
    auto *files = job->getObject("files");
    auto *args = job->getArray("args");
    if (!cwd || !count || *count < 1 || *count > UINT_MAX || !args || !files)
        return fail("invalid partition job");
    inputs.cwd = cwd->str(); inputs.partitions = *count; inputs.manifest = "partitions.json";
    if (!reserve_descriptors(files->size() + 64)) return fail(strerror(errno));
    for (auto &item : *files) {
        auto path = item.second.getAsString();
        if (!path) return fail("invalid snapshot");
        int fd = open(path->str().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return fail(strerror(errno));
        inputs.files[item.first] = fd;
    }
    std::vector<std::string> words;
    SmallVector<const char *> command;
    for (const auto &arg : *args) {
        auto word = arg.getAsString();
        if (!word) return fail("invalid compiler argument");
        words.push_back(word->str());
    }
    for (auto &word : words) command.push_back(word.c_str());
    int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (sink < 0 || !enter_output(dir)) return fail(strerror(errno));
    return dcc_lto_compile(inputs, command, sink);
}

int dcc_lto_remote(int fd, unsigned operation) {
    CompilerCommand command;
    if (operation != 'T' || !command.read(fd, 3) || !command[0][0] || !command[0][1] || command[0][2])
        return fail("invalid backend request");
    LtoInputs inputs;
    inputs.cwd = command[0][0];
    inputs.output = command[0][1];
    char **name = command[2];
    while (*name) {
        int descriptors[DCC_IPC_FDS_MAX];
        unsigned received = DCC_IPC_FDS_MAX;
        unsigned char count = 0;
        if (dcc_recv_fds(0, &count, 1, descriptors, &received) != 1 || !count || count != received)
            return fail("invalid input descriptor batch");
        for (unsigned i = 0; i < count; ++i) {
            if (!*name || !inputs.files.try_emplace(*name++, descriptors[i]).second)
                return fail("unexpected input descriptor");
        }
    }
    if (dup2(5, STDIN_FILENO) < 0) return fail(strerror(errno));
    close(5);
    close(fd);
    SmallVector<const char *> args;
    for (char **p = command[1]; *p; ++p) args.push_back(*p);
    if (!restrict_job()) return fail("cannot confine compiler");
    int status = dcc_lto_compile(inputs, args, 4);
    close(4);
    return status;
}
