/* LLVM 23 Rust CGU boundaries. rustc retains its local pass managers, ThinLTO
 * summary writer, object streams and archive writer throughout the job. */
#include "rust_cgu.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <atomic>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace llvm;
namespace {
template <typename Fn> Fn original(Fn hook) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(hook), &info) || !info.dli_sname)
        std::abort();
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, info.dli_sname));
    if (!fn)
        std::abort();
    return fn;
}
bool rust_caller(void *pc) {
    Dl_info info{};
    return dladdr(pc, &info) && info.dli_fname && strstr(info.dli_fname, "librustc_driver");
}
struct File {
    int fd;
    explicit File(const char *name) : fd(memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING)) {}
    ~File() {
        if (fd >= 0)
            close(fd);
    }
};
struct Builder {
    PassBuilder *pb = nullptr;
    TargetMachine *tm = nullptr;
    PipelineTuningOptions tuning;
};
thread_local Builder builder;
struct Emission {
    legacy::PassManager *pm = nullptr;
    TargetMachine *tm = nullptr;
    raw_fd_ostream *object = nullptr;
};
thread_local Emission emission;
thread_local bool no_builtins;
std::vector<std::string> compiler_flags;
bool flags_supported;
const char *transport;
std::atomic<int> frontend_pipe{-1};
void backend_started() {
    if (frontend_pipe.load(std::memory_order_relaxed) < 0)
        return;
    int fd = frontend_pipe.exchange(-1);
    if (fd >= 0)
        close(fd);
}
__attribute__((constructor)) void compiler_environment() {
    const char *client = getenv("DISTCC_RUST_CGU_CLIENT");
    if (!client)
        return;
    int driver =
        dl_iterate_phdr([](dl_phdr_info *info, size_t,
                           void *) { return int(strstr(info->dlpi_name, "librustc_driver") != nullptr); },
                        nullptr);
    if (!driver)
        return; // A compiler wrapper may exec rustc afterwards.
    if (const char *started = getenv("DISTCC_RUST_CGU_STARTED")) {
        char *end;
        long fd = strtol(started, &end, 10);
        if (*started && !*end && fd >= 3 && fd <= INT_MAX && !fcntl(fd, F_SETFD, FD_CLOEXEC))
            frontend_pipe.store(fd);
        unsetenv("DISTCC_RUST_CGU_STARTED");
    }
    if (cgu::matching_llvm())
        transport = strdup(client);
    const char *previous = getenv("DISTCC_RUST_CGU_PREVIOUS");
    if (previous && *previous)
        setenv("LD_PRELOAD", previous, 1);
    else
        unsetenv("LD_PRELOAD");
    unsetenv("DISTCC_RUST_CGU_PREVIOUS");
    unsetenv("DISTCC_RUST_CGU_CLIENT");
}
bool supported(const TargetMachine &tm) {
    return flags_supported && tm.Options.MCOptions.SplitDwarfFile.empty() && !tm.getPGOOption() &&
           tm.getCodeModel() == CodeModel::Small;
}
std::unique_ptr<MemoryBuffer> dispatch(Module &m, cgu::Object spec) {
    const char *client = transport;
    if (!client || !StringRef(m.getModuleIdentifier()).contains("cgu.") || m.getInstructionCount() < 128)
        return {};
    File in("rust-cgu-input"), out("rust-cgu-result");
    if (in.fd < 0 || out.fd < 0)
        return {};
    auto kind = cgu::string(spec, "kind").str();
    auto target = cgu::string(spec, "backend").str();
    spec["id"] = m.getModuleIdentifier();
    spec["no-builtins"] = int(no_builtins);
    json::Array flags;
    for (auto &flag : compiler_flags)
        flags.push_back(flag);
    spec["flags"] = std::move(flags);
    {
        raw_fd_ostream stream(in.fd, false);
        stream << json::Value(std::move(spec)) << '\n';
        WriteBitcodeToFile(m, stream, true);
        stream.flush();
        if (stream.has_error())
            return {};
    }
    if (fcntl(in.fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
        return {};
    // Each parallel CGU owns a broker session. posix_spawn never forks a
    // mutable C++ runtime into a child that must perform work before exec.
    const char *args[] = {client, "--rust-cgu", LLVM_VERSION_STRING, target.c_str(), kind.c_str(), nullptr};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions))
        return {};
    int rc = posix_spawn_file_actions_adddup2(&actions, in.fd, STDIN_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, out.fd, STDOUT_FILENO);
    pid_t pid;
    if (!rc)
        rc = posix_spawn(&pid, client, &actions, nullptr, const_cast<char **>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc)
        return {};
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return {};
    struct stat st;
    if (status || fstat(out.fd, &st) || !st.st_size)
        return {};
    auto result = MemoryBuffer::getOpenFile(out.fd, m.getModuleIdentifier(), st.st_size, false);
    return result ? std::move(*result) : nullptr;
}
struct RemotePass : RequiredPassInfoMixin<RemotePass> {
    ModulePassManager local;
    cgu::Object spec;
    RemotePass(ModulePassManager &&p, cgu::Object s) : local(std::move(p)), spec(std::move(s)) {}
    PreservedAnalyses run(Module &m, ModuleAnalysisManager &am) {
        auto buffer = dispatch(m, spec);
        if (buffer) {
            auto updated = parseBitcodeFile(buffer->getMemBufferRef(), m.getContext());
            if (updated) {
                am.invalidate(m, PreservedAnalyses::none());
                m = std::move(**updated);
                return PreservedAnalyses::none();
            }
            consumeError(updated.takeError());
        }
        return local.run(m, am);
    }
};
ModulePassManager wrap(PassBuilder *pb, ModulePassManager passes, StringRef kind, OptimizationLevel level,
                       ThinOrFullLTOPhase phase) {
    if (!transport || pb != builder.pb || !builder.tm || !supported(*builder.tm))
        return passes;
    auto spec = cgu::describe(*builder.tm, builder.tuning);
    spec["kind"] = kind;
    spec["level"] = int(level);
    spec["phase"] = int(phase);
    spec["pipeline"] = cgu::pipeline_name(passes, *pb->getPassInstrumentationCallbacks());
    ModulePassManager result;
    result.addPass(RemotePass(std::move(passes), std::move(spec)));
    return result;
}
} // namespace
// Use free-function ABI declarations for constructor entry points. Calling a
// replacement C++ constructor would initialize the LLVM object's members twice.
#define PB_CTOR                                                                                              \
    "_ZN4llvm11PassBuilderC1EPNS_13TargetMachineENS_21PipelineTuningOptionsESt8optionalINS_"                 \
    "10PGOOptionsEEPNS_28PassInstrumentationCallbacksENS_18IntrusiveRefCntPtrINS_3vfs10FileSystemEEE"
extern "C" void pb_ctor(PassBuilder *, TargetMachine *, PipelineTuningOptions, std::optional<PGOOptions>,
                        PassInstrumentationCallbacks *, IntrusiveRefCntPtr<vfs::FileSystem>) asm(PB_CTOR);
void pb_ctor(PassBuilder *self, TargetMachine *tm, PipelineTuningOptions pto, std::optional<PGOOptions> pgo,
             PassInstrumentationCallbacks *pic, IntrusiveRefCntPtr<vfs::FileSystem> fs) {
    if (rust_caller(__builtin_return_address(0))) {
        backend_started();
        builder = {self, !pgo && pic ? tm : nullptr, pto};
        no_builtins = false;
    }
    static auto next = original(&pb_ctor);
    next(self, tm, pto, std::move(pgo), pic, std::move(fs));
}
#define PIPELINE(fn, symbol, kind)                                                                           \
    ModulePassManager fn(PassBuilder *, OptimizationLevel, ThinOrFullLTOPhase) asm(symbol);       \
    ModulePassManager fn(PassBuilder *pb, OptimizationLevel level, ThinOrFullLTOPhase phase) {               \
        static auto next = original(&fn);                                                                    \
        auto p = next(pb, level, phase);                                                                     \
        if (!rust_caller(__builtin_return_address(0)))                                                       \
            return p;                                                                                        \
        return wrap(pb, std::move(p), kind, level, phase);                                                   \
    }
#define LEVEL_PHASE "ENS_17OptimizationLevelENS_18ThinOrFullLTOPhaseE"
PIPELINE(per_module, "_ZN4llvm11PassBuilder29buildPerModuleDefaultPipeline" LEVEL_PHASE, "per-module")
PIPELINE(o0, "_ZN4llvm11PassBuilder22buildO0DefaultPipeline" LEVEL_PHASE, "O0")
PIPELINE(module_opt, "_ZN4llvm11PassBuilder31buildModuleOptimizationPipeline" LEVEL_PHASE, "module-opt")
ModulePassManager thin(PassBuilder *, OptimizationLevel, const ModuleSummaryIndex *) asm(
    "_ZN4llvm11PassBuilder27buildThinLTODefaultPipelineENS_17OptimizationLevelEPKNS_18ModuleSummaryIndexE");
ModulePassManager thin(PassBuilder *pb, OptimizationLevel level, const ModuleSummaryIndex *index) {
    static auto next = original(&thin);
    auto p = next(pb, level, index);
    if (index || !rust_caller(__builtin_return_address(0)))
        return p;
    return wrap(pb, std::move(p), "thin-postlink", level, ThinOrFullLTOPhase::ThinLTOPostLink);
}
extern "C" bool
llvm_flags(int, const char *const *, StringRef, raw_ostream *, vfs::FileSystem *, const char *, bool) asm(
    "_ZN4llvm2cl23ParseCommandLineOptionsEiPKPKcNS_9StringRefEPNS_11raw_ostreamEPNS_3vfs10FileSystemES2_b");
bool llvm_flags(int n, const char *const *argv, StringRef desc, raw_ostream *err, vfs::FileSystem *fs,
                const char *env, bool dash) {
    if (rust_caller(__builtin_return_address(0))) {
        compiler_flags.assign(argv + 1, argv + n);
        flags_supported = std::all_of(compiler_flags.begin(), compiler_flags.end(), cgu::supported_flag);
    }
    static auto next = original(&llvm_flags);
    return next(n, argv, desc, err, fs, env, dash);
}
extern "C" void
disable_builtins(TargetLibraryInfoImpl *) asm("_ZN4llvm21TargetLibraryInfoImpl19disableAllFunctionsEv");
void disable_builtins(TargetLibraryInfoImpl *info) {
    if (rust_caller(__builtin_return_address(0)))
        no_builtins = true;
    static auto next = original(&disable_builtins);
    next(info);
}
extern "C" void analysis(TargetMachine *, legacy::PassManager *) asm("LLVMAddAnalysisPasses");
void analysis(TargetMachine *tm, legacy::PassManager *pm) {
    if (rust_caller(__builtin_return_address(0))) {
        backend_started();
        emission = {pm, tm, nullptr};
        no_builtins = false;
    }
    static auto next = original(&analysis);
    next(tm, pm);
}
extern "C" void open_output(raw_fd_ostream *, StringRef, std::error_code &, sys::fs::OpenFlags) asm(
    "_ZN4llvm14raw_fd_ostreamC1ENS_9StringRefERSt10error_codeNS_3sys2fs9OpenFlagsE");
void open_output(raw_fd_ostream *os, StringRef path, std::error_code &ec, sys::fs::OpenFlags flags) {
    static auto next = original(&open_output);
    next(os, path, ec, flags);
    if (emission.pm && rust_caller(__builtin_return_address(0)))
        emission.object = !ec && path.ends_with(".rcgu.o") ? os : nullptr;
}
extern "C" bool emit(legacy::PassManager *, Module &) asm("_ZN4llvm6legacy11PassManager3runERNS_6ModuleE");
bool emit(legacy::PassManager *pm, Module &m) {
    auto pending = emission;
    emission = {};
    if (pm == pending.pm && pending.object && supported(*pending.tm)) {
        auto spec = cgu::describe(*pending.tm, PipelineTuningOptions());
        spec["kind"] = "object";
        if (auto bytes = dispatch(m, std::move(spec))) {
            *pending.object << bytes->getBuffer();
            return true;
        }
    }
    static auto next = original(&emit);
    return next(pm, m);
}
