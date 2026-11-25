/* Keep this preload safe in wrappers with immediate ELF binding. Only the
 * sibling module calls LLVM, after rustc has loaded its own LLVM runtime. */
#define LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING 1
#include "rust/cgu_hooks.h"
#include "exitcode.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm-c/TargetMachine.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

using namespace llvm;
namespace {
const cgu::Hooks *hooks;
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
    [[gnu::used, gnu::retain, gnu::section("dcc_hook_entries")]] static const void *const id##_entry =       \
        __start_dcc_hook_##id;                                                                               \
    [[gnu::section("dcc_hook_" #id)]]
extern "C" [[gnu::visibility("hidden")]] const void *const __start_dcc_hook_entries[], *const __stop_dcc_hook_entries[];
void *original(const void *hook) {
    Dl_info info{};
    // GNU ld may also expose a zero-size __start symbol at the first byte.
    // Query inside the function so dladdr selects the interposed ELF symbol.
    if (!dladdr(static_cast<const char *>(hook) + 1, &info) || !info.dli_sname)
        fatal("cannot resolve LLVM hook symbol");
    auto fn = dlsym(RTLD_NEXT, info.dli_sname);
    if (!fn)
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
    return dladdr(pc, &info) && info.dli_fname && strstr(info.dli_fname, "librustc_driver");
}

__attribute__((constructor)) void compiler_environment() {
    const char *probe = getenv("DISTCC_RUST_CGU_PROBE");
    const char *client = getenv("DISTCC_RUST_CGU_CLIENT");
    if (!probe && !client)
        return;
    if (!dl_iterate_phdr([](dl_phdr_info *info, size_t, void *) {
            return int(strstr(info->dlpi_name, "librustc_driver") != nullptr);
        }, nullptr))
        return;
    if (probe) {
        char *end;
        long fd = strtol(probe, &end, 10);
        if (!*probe || *end || fd < 3 || fd > INT_MAX)
            fatal("invalid probe descriptor");
        probe_fd = fd;
    }
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&compiler_environment), &info) || !info.dli_fname)
        fatal("cannot locate CGU interposer");
    std::string path = info.dli_fname;
    auto slash = path.rfind('/');
    auto directory = slash == std::string::npos ? "./" : path.substr(0, slash + 1);
    auto module = dlopen((directory + "distcc-rust-cgu-llvm.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        const char *err = dlerror();
        fatal(err ? err : "cannot load CGU LLVM module");
    }
    auto initialize = reinterpret_cast<decltype(&cgu::dcc_cgu_initialize)>(dlsym(module, "dcc_cgu_initialize"));
    if (!initialize) {
        const char *err = dlerror();
        fatal(err ? err : "cannot resolve dcc_cgu_initialize");
    }
    for (auto entry = __start_dcc_hook_entries; entry != __stop_dcc_hook_entries; ++entry)
        original(*entry);
    auto api = initialize(probe ? nullptr : client);
    if (!api)
        fatal("incompatible linked LLVM ABI");
    if (probe) {
        dprintf(probe_fd, "ok\n");
        close(probe_fd);
        probe_fd = -1;
        unsetenv("DISTCC_RUST_CGU_PROBE");
    } else {
        hooks = api;
    }
}
} // namespace
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
// Forward its original storage so the preload never needs PGOOptions' LLVM
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
