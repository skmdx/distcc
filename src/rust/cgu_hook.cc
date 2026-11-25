/* Host the selected rustc driver. Only the sibling module imports LLVM. */
#define LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING 1
#include "rust/cgu_hooks.h"
#include "exitcode.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm-c/TargetMachine.h>
#include <dlfcn.h>
#include <filesystem>
#include <link.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace llvm;
namespace {
const cgu::Hooks *hooks;
void *llvm_handle;
const void *driver_base;
int probe_fd = -1;

[[noreturn]] void fatal(const std::string &reason) {
    if (probe_fd >= 0) {
        dprintf(probe_fd, "err:%s\n", reason.c_str());
        _exit(0);
    }
    fprintf(stderr, "distcc-rust-cgu: %s\n", reason.c_str());
    _exit(EXIT_DISTCC_FAILED);
}
// Each ELF section contains one interposed function. Its linker-provided start
// identifies the export even for members, constructors and overloaded methods.
// The same sections identify the hooks to the cross-build symbol check.
#define CGU_HOOK(id)                                                                                         \
    extern "C" [[gnu::visibility("hidden")]] const char __start_dcc_hook_##id[];                             \
    [[gnu::used, gnu::retain, gnu::section("dcc_cgu_entries")]] static const void *const id##_entry =        \
        __start_dcc_hook_##id;                                                                               \
    [[gnu::visibility("default"), gnu::section("dcc_hook_" #id)]]
extern "C" [[gnu::visibility("hidden")]] const void *const __start_dcc_cgu_entries[], *const __stop_dcc_cgu_entries[];
void *original(const void *hook) {
    Dl_info info{};
    // GNU ld may also expose a zero-size __start symbol at the first byte.
    // Query inside the function so dladdr selects the interposed ELF symbol.
    if (!dladdr(static_cast<const char *>(hook) + 1, &info) || !info.dli_sname)
        fatal("cannot resolve LLVM hook symbol");
    auto fn = dlsym(llvm_handle, info.dli_sname);
    if (!fn || fn == hook)
        fatal(std::string("missing LLVM entry point ") + info.dli_sname);
    return fn;
}
template <typename Fn> Fn next_function(const void *hook, Fn) {
    return reinterpret_cast<Fn>(original(hook));
}
template <typename R, typename C, typename... A>
auto next_method(const void *hook, R (C::*)(A...)) {
    return reinterpret_cast<R (*)(C *, A...)>(original(hook));
}
bool rust_caller(void *pc) {
    Dl_info info{};
    return dladdr(pc, &info) && info.dli_fbase == driver_base;
}

std::string take_env(const char *key) {
    const char *value = getenv(key);
    std::string saved = value ? value : "";
    unsetenv(key);
    return saved;
}
void *load_library(const char *path, int flags) {
    auto handle = dlopen(path, flags);
    if (!handle) {
        const char *error = dlerror();
        fatal(error ? error : std::string("cannot load ") + path);
    }
    return handle;
}
void *symbol(void *handle, const char *name) {
    auto fn = dlsym(handle, name);
    if (!fn)
        fatal(std::string("missing entry point ") + name);
    return fn;
}
} // namespace

int main(int, char **) try {
    auto driver = take_env("DISTCC_RUST_CGU_DRIVER");
    auto entry_name = take_env("DISTCC_RUST_CGU_ENTRY");
    auto probe = take_env("DISTCC_RUST_CGU_PROBE");
    auto client = take_env("DISTCC_RUST_CGU_CLIENT");
    auto previous = take_env("DISTCC_RUST_CGU_PREV_TUNABLES");
    if (!probe.empty()) {
        char *end;
        long fd = strtol(probe.c_str(), &end, 10);
        if (*end || fd < 3 || fd > INT_MAX)
            fatal("invalid probe descriptor");
        probe_fd = fd;
    }
    if (previous.empty() ? unsetenv("GLIBC_TUNABLES") : setenv("GLIBC_TUNABLES", previous.c_str(), 1))
        fatal("cannot restore GLIBC_TUNABLES");
    if (driver.empty() || entry_name.empty() || (probe_fd < 0 && client.empty()))
        fatal("missing rustc driver configuration");
    stack_t stack{};
    long min_stack = SIGSTKSZ;
    stack.ss_size = min_stack > 65536 ? size_t(min_stack) : 65536;
    stack.ss_sp = mmap(nullptr, stack.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack.ss_sp == MAP_FAILED || sigaltstack(&stack, nullptr) || signal(SIGPIPE, SIG_DFL) == SIG_ERR)
        fatal("cannot initialize rustc signal handling");

    auto drv = load_library(driver.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    auto entry = reinterpret_cast<uint8_t (*)()>(symbol(drv, entry_name.c_str()));
    Dl_info owner{};
    if (!dladdr(reinterpret_cast<void *>(entry), &owner))
        fatal("cannot locate rustc driver entry");
    driver_base = owner.dli_fbase;
    const char *llvm_path = nullptr;
    int multiple = dl_iterate_phdr([](dl_phdr_info *info, size_t, void *result) {
        if (!info->dlpi_name || !*info->dlpi_name)
            return 0;
        const char *base = strrchr(info->dlpi_name, '/');
        if (strncmp(base ? base + 1 : info->dlpi_name, "libLLVM", 7))
            return 0;
        auto &found = *static_cast<const char **>(result);
        if (found)
            return 1;
        found = info->dlpi_name;
        return 0;
    }, &llvm_path);
    if (multiple)
        fatal("multiple LLVM libraries in rustc driver process");
    if (!llvm_path)
        fatal("rustc driver did not load LLVM");
    llvm_handle = load_library(llvm_path, RTLD_LAZY | RTLD_NOLOAD);
    auto sibling = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "distcc-rust-cgu-llvm.so";
    auto module = load_library(sibling.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto initialize = reinterpret_cast<decltype(&cgu::dcc_cgu_initialize)>(symbol(module, "dcc_cgu_initialize"));
    for (auto hook = __start_dcc_cgu_entries; hook != __stop_dcc_cgu_entries; ++hook)
        original(*hook);
    auto api = initialize(probe_fd >= 0 ? nullptr : client.c_str());
    if (!api)
        fatal("incompatible linked LLVM ABI");
    if (probe_fd >= 0) {
        dprintf(probe_fd, "ok\n");
        close(probe_fd);
        probe_fd = -1;
    } else {
        hooks = api;
    }
    return entry();
} catch (const std::exception &e) {
    fatal(e.what());
} catch (...) {
    fatal("rustc driver unwound across its entry point");
}
// The header declares extern template class PassManager<Module>, precluding
// a specialization of run() here. The exported signature has no std types.
PreservedAnalyses module_passes(ModulePassManager *, Module &, ModuleAnalysisManager &) asm(
    "_ZN4llvm11PassManagerINS_6ModuleENS_15AnalysisManagerIS1_JEEEJEE3runERS1_RS3_");
CGU_HOOK(module_passes)
PreservedAnalyses module_passes(ModulePassManager *pm, Module &m, ModuleAnalysisManager &am) {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->module(*pm, m, am);
    static auto next = next_function(module_passes_entry, &module_passes);
    return next(pm, m, am);
}
// A compiler-supplied pass may own observations in the local LLVMContext.
CGU_HOOK(custom_pipeline)
Error PassBuilder::parsePassPipeline(ModulePassManager &pm, StringRef pipeline) {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->custom(this);
    using Parse = Error (PassBuilder::*)(ModulePassManager &, StringRef);
    static auto next = next_method(custom_pipeline_entry, static_cast<Parse>(&PassBuilder::parsePassPipeline));
    return next(this, pm, pipeline);
}
extern "C" void destroy_module(Module *) asm("_ZN4llvm6ModuleD1Ev");
CGU_HOOK(destroy_module)
void destroy_module(Module *m) {
    // LLVMDisposeModule can destroy a tracked module from inside libLLVM.
    if (hooks)
        hooks->destroy(*m);
    static auto next = next_function(destroy_module_entry, &destroy_module);
    next(m);
}
// Use free-function ABI declarations for constructor entry points. Calling a
// replacement C++ constructor would initialize the LLVM object's members twice.
#ifdef _LIBCPP_VERSION
#include "cgu-libcpp.h"
#define STD_TYPE(name) "NSt" DCC_LIBCPP_NAMESPACE name "E"
#else
#define STD_TYPE(name) "St" name
#endif
// The Itanium C++ ABI passes this non-trivial value by invisible reference.
// Forward its original storage so the driver host never needs PGOOptions' LLVM
// copy constructor or destructor. The caller owns destruction.
static_assert(!std::is_trivially_destructible_v<std::optional<PGOOptions>>);
extern "C" void pb_ctor(PassBuilder *, TargetMachine *, PipelineTuningOptions, std::optional<PGOOptions> &,
                        PassInstrumentationCallbacks *, IntrusiveRefCntPtr<vfs::FileSystem>) asm(
    "_ZN4llvm11PassBuilderC1EPNS_13TargetMachineENS_21PipelineTuningOptionsE" STD_TYPE("8optionalINS_10PGOOptionsEE")
    "PNS_28PassInstrumentationCallbacksENS_18IntrusiveRefCntPtrINS_3vfs10FileSystemEEE");
CGU_HOOK(pb_ctor)
void pb_ctor(PassBuilder *self, TargetMachine *tm, PipelineTuningOptions pto, std::optional<PGOOptions> &pgo,
             PassInstrumentationCallbacks *pic, IntrusiveRefCntPtr<vfs::FileSystem> fs) {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->builder(self, !pgo && pic ? tm : nullptr, pto);
    static auto next = next_function(pb_ctor_entry, &pb_ctor);
    next(self, tm, pto, pgo, pic, std::move(fs));
}
#define PIPELINE(fn, kind)                                                                                   \
    CGU_HOOK(fn)                                                                                             \
    ModulePassManager PassBuilder::fn(OptimizationLevel level, ThinOrFullLTOPhase phase) {                   \
        static auto next = next_method(fn##_entry, &PassBuilder::fn);                                        \
        auto build = [&] { return next(this, level, phase); };                                               \
        if (hooks && rust_caller(__builtin_return_address(0)))                                               \
            return hooks->pipeline(this, build, kind, level, phase);                                         \
        return build();                                                                                      \
    }
PIPELINE(buildPerModuleDefaultPipeline, "per-module")
PIPELINE(buildO0DefaultPipeline, "O0")
PIPELINE(buildModuleOptimizationPipeline, "module-opt")
CGU_HOOK(prelink)
ModulePassManager PassBuilder::buildThinLTOPreLinkDefaultPipeline(OptimizationLevel level) {
    static auto next = next_method(prelink_entry, &PassBuilder::buildThinLTOPreLinkDefaultPipeline);
    auto build = [&] { return next(this, level); };
    if (hooks && rust_caller(__builtin_return_address(0)))
        return hooks->pipeline(this, build, "thin-prelink", level, ThinOrFullLTOPhase::ThinLTOPreLink);
    return build();
}
CGU_HOOK(thin)
ModulePassManager PassBuilder::buildThinLTODefaultPipeline(OptimizationLevel level, const ModuleSummaryIndex *index) {
    static auto next = next_method(thin_entry, &PassBuilder::buildThinLTODefaultPipeline);
    auto build = [&] { return next(this, level, index); };
    if (hooks && !index && rust_caller(__builtin_return_address(0)))
        return hooks->pipeline(this, build, "thin-postlink", level, ThinOrFullLTOPhase::ThinLTOPostLink);
    return build();
}
CGU_HOOK(llvm_flags)
bool cl::ParseCommandLineOptions(int n, const char *const *argv, StringRef desc, raw_ostream *err,
                                 vfs::FileSystem *fs, const char *env, bool dash) {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->flags(n, argv);
    static auto next = next_function(llvm_flags_entry, &cl::ParseCommandLineOptions);
    return next(n, argv, desc, err, fs, env, dash);
}
CGU_HOOK(disable_builtins)
void TargetLibraryInfoImpl::disableAllFunctions() {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->disable_builtins();
    static auto next = next_method(disable_builtins_entry, &TargetLibraryInfoImpl::disableAllFunctions);
    next(this);
}
CGU_HOOK(analysis)
void LLVMAddAnalysisPasses(LLVMTargetMachineRef tm, LLVMPassManagerRef pm) {
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->analysis(unwrap(pm), reinterpret_cast<TargetMachine *>(tm));
    static auto next = next_function(analysis_entry, &LLVMAddAnalysisPasses);
    next(tm, pm);
}
extern "C" void open_output(raw_fd_ostream *, StringRef, std::error_code &, sys::fs::OpenFlags) asm(
    "_ZN4llvm14raw_fd_ostreamC1ENS_9StringRefER" STD_TYPE("10error_code") "NS_3sys2fs9OpenFlagsE");
CGU_HOOK(open_output)
void open_output(raw_fd_ostream *os, StringRef path, std::error_code &ec, sys::fs::OpenFlags flags) {
    static auto next = next_function(open_output_entry, &open_output);
    next(os, path, ec, flags);
    if (hooks && rust_caller(__builtin_return_address(0)))
        hooks->output(!ec && path.ends_with(".rcgu.o") ? os : nullptr);
}
CGU_HOOK(emit)
bool legacy::PassManager::run(Module &m) {
    if (hooks && hooks->emit(this, m))
        return true;
    static auto next = next_method(emit_entry, &legacy::PassManager::run);
    return next(this, m);
}
