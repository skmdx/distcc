/* LLVM Rust CGU boundaries. rustc retains its local pass managers, ThinLTO
 * summary writer, object streams and archive writer throughout the job. */
#include "rust/cgu.h"
#include "exitcode.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/Transforms/IPO/ThinLTOBitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm-c/TargetMachine.h>
#include <atomic>
#include <mutex>
#include <map>
#include <llvm/Support/Endian.h>
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
[[noreturn]] void fatal(const Twine &message);
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
        fatal(Twine("missing LLVM entry point ") + info.dli_sname);
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
    bool custom = false;
};
thread_local Builder builder;
struct Emission {
    legacy::PassManagerBase *pm = nullptr;
    TargetMachine *tm = nullptr;
    raw_fd_ostream *object = nullptr;
};
thread_local Emission emission;
thread_local bool no_builtins;
std::vector<std::string> compiler_flags;
bool object_only;
bool plugin_lto;
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
    if (!cgu::matching_llvm()) {
        fprintf(stderr, "distcc-rust-cgu: incompatible linked LLVM ABI\n");
        _exit(EXIT_DISTCC_FAILED);
    }
    for (auto entry = __start_dcc_hook_entries; entry != __stop_dcc_hook_entries; ++entry)
        original(*entry);
    transport = strdup(client);
    object_only =
        getenv("DISTCC_RUST_CGU_OBJECT_ONLY") && !strcmp(getenv("DISTCC_RUST_CGU_OBJECT_ONLY"), "1");
    unsetenv("DISTCC_RUST_CGU_OBJECT_ONLY");
    plugin_lto = getenv("DISTCC_RUST_CGU_PLUGIN_LTO") && !strcmp(getenv("DISTCC_RUST_CGU_PLUGIN_LTO"), "1");
    unsetenv("DISTCC_RUST_CGU_PLUGIN_LTO");
    const char *previous = getenv("DISTCC_RUST_CGU_PREVIOUS");
    if (previous && *previous)
        setenv("LD_PRELOAD", previous, 1);
    else
        unsetenv("LD_PRELOAD");
    unsetenv("DISTCC_RUST_CGU_PREVIOUS");
    unsetenv("DISTCC_RUST_CGU_CLIENT");
}
bool supported(const TargetMachine &tm) {
    return tm.Options.MCOptions.SplitDwarfFile.empty() && !tm.getPGOOption() &&
           tm.getCodeModel() == CodeModel::Small;
}
[[noreturn]] void fatal(const Twine &message) {
    errs() << "distcc-rust-cgu: " << message << '\n';
    _exit(EXIT_DISTCC_FAILED);
}
struct Reply {
    std::unique_ptr<MemoryBuffer> buffer;
    StringRef object, bitcode;
    Reply(std::unique_ptr<MemoryBuffer> b, bool ir_only) : buffer(std::move(b)) {
        auto data = buffer->getBuffer();
        if (data.size() < 8)
            fatal("truncated CGU reply");
        auto length = support::endian::read64le(data.data());
        data = data.drop_front(8);
        if (length > data.size() || (ir_only ? length != 0 || data.empty() : length == 0))
            fatal("invalid object size in CGU reply");
        object = data.take_front(length);
        bitcode = data.drop_front(length);
    }
};
// Smaller modules cost less to emit in-process than to serialize and schedule.
constexpr unsigned min_remote_instructions = 128;
bool eligible(Module &m) {
    return transport && StringRef(m.getModuleIdentifier()).contains("cgu.") &&
           m.getInstructionCount() >= min_remote_instructions;
}
std::optional<Reply> dispatch(Module &m, cgu::Object spec) {
    const bool ir_only = spec.get("ir-only") != nullptr;
    File in("rust-cgu-input"), out("rust-cgu-result");
    if (in.fd < 0 || out.fd < 0)
        fatal(strerror(errno));
    auto kind = cgu::string(spec, "kind").str();
    auto target = cgu::string(spec, "backend").str();
    spec["id"] = m.getModuleIdentifier();
    if (!spec.get("no-builtins"))
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
            fatal(stream.error().message());
    }
    if (fcntl(in.fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
        fatal(strerror(errno));
    const char *args[] = {transport, "--rust-cgu", cgu::abi, target.c_str(), kind.c_str(), nullptr};
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc)
        fatal(strerror(rc));
    rc = posix_spawn_file_actions_adddup2(&actions, in.fd, STDIN_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, out.fd, STDOUT_FILENO);
    pid_t pid;
    if (!rc)
        rc = posix_spawn(&pid, transport, &actions, nullptr, const_cast<char **>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc)
        fatal(strerror(rc));
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            fatal(strerror(errno));
    if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_LOCAL_COMPILE)
        return std::nullopt;
    if (status)
        fatal(Twine("backend failed for ") + m.getModuleIdentifier());
    struct stat st;
    if (fstat(out.fd, &st))
        fatal(strerror(errno));
    auto bytes = MemoryBuffer::getOpenFile(out.fd, m.getModuleIdentifier(), st.st_size, false);
    if (!bytes)
        fatal(bytes.getError().message());
    if (getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
        errs() << "distcc-rust-cgu: completed " + kind + " " + m.getModuleIdentifier() + "\n";
    return Reply(std::move(*bytes), ir_only);
}
// rustc can hand a module to a different codegen thread. Its identity owns
// the pending work until its object stream is opened.
struct Pending {
    cgu::Object spec;
    std::optional<Reply> reply;
    TargetMachine *tm;
    void run_locally(Module &m) {
        PipelineTuningOptions tuning;
        auto opts = tm->Options;
        cgu::configure(spec, opts, tuning);
        TargetLibraryInfoImpl tli(m.getTargetTriple());
        if (cgu::integer(spec, "no-builtins"))
            tli.disableAllFunctions();
        cgu::run_pipeline(m, *tm, tuning, tli, spec);
    }
    bool materialize(Module &m, ModuleAnalysisManager &am) {
        spec["return-ir"] = 1;
        reply = dispatch(m, spec);
        if (!reply)
            return false;
        auto ir = parseBitcodeFile(MemoryBufferRef(reply->bitcode, m.getModuleIdentifier()), m.getContext());
        if (!ir)
            fatal(toString(ir.takeError()));
        am.invalidate(m, PreservedAnalyses::none());
        m = std::move(**ir);
        return true;
    }
};
std::mutex pending_lock;
std::map<Module *, Pending> modules;
auto take(Module &m) {
    std::lock_guard<std::mutex> guard(pending_lock);
    return modules.extract(&m);
}
void remember(Module &m, Pending work) {
    std::lock_guard<std::mutex> guard(pending_lock);
    if (!modules.try_emplace(&m, std::move(work)).second)
        fatal("duplicate module submission");
}
struct RemotePass : PassInfoMixin<RemotePass> {
    static bool isRequired() { return true; }
    ModulePassManager local;
    TargetMachine *tm;
    bool frontend = false;
    cgu::Object spec;
    RemotePass(ModulePassManager &&p, TargetMachine *machine, cgu::Object s)
        : local(std::move(p)), tm(machine), spec(std::move(s)) {}
    PreservedAnalyses run(Module &m, ModuleAnalysisManager &am) {
        if (frontend || (!spec.get("tail") && !eligible(m)))
            return local.run(m, am);
        // These final pipelines consume an already-resolved module. Rust has
        // retained its pre-link bitcode separately and may still attach it as
        // module assembly immediately before native emission.
        bool late = spec.get("tail") != nullptr;
        spec["no-builtins"] = int(no_builtins);
        if (late)
            spec["initial-asm"] = int64_t(cgu::assembly_size(m));
        spec["return-ir"] = int(!late);
        if (plugin_lto)
            spec["ir-only"] = 1;
        Pending work{std::move(spec), {}, tm};
        if (!late && !work.materialize(m, am))
            return local.run(m, am);
        if (!plugin_lto)
            remember(m, std::move(work));
        return late ? PreservedAnalyses::all() : PreservedAnalyses::none();
    }
};
// LLVM flattens nested module managers as they are composed. At the
// outer rustc boundary the passes after our marker are the actual compiler
// continuation. Preserve their order on the worker, including named LLVM
// transformations added by rustc. A pass owning an untransportable stream or
// a private callback keeps the original manager on the frontend.
struct ModulePasses : ModulePassManager {
    static auto &list(ModulePassManager &pm) { return pm.*(&ModulePasses::Passes); }
};
void capture_tail(ModulePassManager &pm, Module &m) {
    if (!eligible(m) || !builder.pb)
        return;
    auto &passes = ModulePasses::list(pm);
    auto marker = llvm::find_if(passes, [](const auto &p) { return p->name() == RemotePass::name(); });
    if (marker == passes.end())
        return;
    using Model = detail::PassModel<Module, RemotePass, ModuleAnalysisManager>;
    auto &remote = static_cast<Model &>(**marker).Pass;
    if (builder.custom || (!plugin_lto && llvm::any_of(make_range(std::next(marker), passes.end()), [](const auto &p) {
            return p->name() == ThinLTOBitcodeWriterPass::name() || p->name() == BitcodeWriterPass::name();
        }))) {
        remote.frontend = true; // rustc needs this pre-link module for its own ThinLTO session.
        return;
    }
    // The linker plugin consumes bitcode. Return optimized IR at the marker
    // and leave rustc's writer and any subsequent passes on the frontend.
    if (plugin_lto || !object_only || !m.getTargetTriple().isOSBinFormatELF())
        return;
    auto *pic = builder.pb->getPassInstrumentationCallbacks();
    bool has_names = true;
    std::string tail;
    raw_string_ostream out(tail);
    for (auto it = std::next(marker); it != passes.end(); ++it) {
        if (it != std::next(marker))
            out << ',';
        (*it)->printPipeline(out, [&](StringRef type) {
            auto name = pic->getPassNameForClassName(type);
            if (name.empty())
                has_names = false;
            return name;
        });
    }
    if (!has_names)
        return;
    ModulePassManager portable;
    if (!tail.empty()) {
        if (auto err = builder.pb->parsePassPipeline(portable, tail)) {
            logAllUnhandledErrors(std::move(err), errs(), "distcc-rust-cgu: frontend continuation: ");
            return;
        }
    }
    remote.spec["tail"] = std::move(tail);
    passes.erase(std::next(marker), passes.end());
}

ModulePassManager wrap_pipeline(PassBuilder *pb, ModulePassManager passes, StringRef kind, OptimizationLevel level,
                       ThinOrFullLTOPhase phase) {
    if (!transport || pb != builder.pb || !builder.tm || !supported(*builder.tm))
        return passes;
    auto spec = cgu::describe(*builder.tm, builder.tuning);
    spec["kind"] = kind;
    spec["level"] = int(cgu::encode(level));
    spec["phase"] = int(phase);
    ModulePassManager extensions;
    pb->invokePipelineStartEPCallbacks(extensions, level);
    pb->invokeOptimizerLastEPCallbacks(extensions, level, phase);
    if (!extensions.isEmpty())
        return passes;
    ModulePassManager result;
    result.addPass(RemotePass(std::move(passes), builder.tm, std::move(spec)));
    return result;
}
} // namespace
// The header declares extern template class PassManager<Module>, precluding
// a specialization of run() here. The exported signature has no std types.
PreservedAnalyses module_passes(ModulePassManager *, Module &, ModuleAnalysisManager &) asm(
    "_ZN4llvm11PassManagerINS_6ModuleENS_15AnalysisManagerIS1_JEEEJEE3runERS1_RS3_");
CGU_HOOK(module_passes)
PreservedAnalyses module_passes(ModulePassManager *pm, Module &m, ModuleAnalysisManager &am) {
    if (transport && rust_caller(__builtin_return_address(0))) {
        // A compiler-side bitcode writer (e.g. save-temps or linker-plugin
        // LTO) is allowed to observe the IR before opening an object stream.
        auto node = take(m);
        if (!node.empty()) {
            auto &work = node.mapped();
            if (work.reply || work.materialize(m, am))
                remember(m, std::move(work));
            else
                work.run_locally(m);
        } else {
            capture_tail(*pm, m);
        }
    }
    static auto next = next_function(module_passes_entry, &module_passes);
    return next(pm, m, am);
}
// A compiler-supplied pass may own observations in the local LLVMContext.
CGU_HOOK(custom_pipeline)
Error PassBuilder::parsePassPipeline(ModulePassManager &pm, StringRef pipeline) {
    if (this == builder.pb && rust_caller(__builtin_return_address(0)))
        builder.custom = true;
    using Parse = Error (PassBuilder::*)(ModulePassManager &, StringRef);
    static auto next = next_method(custom_pipeline_entry, static_cast<Parse>(&PassBuilder::parsePassPipeline));
    return next(this, pm, pipeline);
}
extern "C" void destroy_module(Module *) asm("_ZN4llvm6ModuleD1Ev");
CGU_HOOK(destroy_module)
void destroy_module(Module *m) {
    if (transport)
        take(*m);
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
extern "C" void pb_ctor(PassBuilder *, TargetMachine *, PipelineTuningOptions, std::optional<PGOOptions>,
                        PassInstrumentationCallbacks *, IntrusiveRefCntPtr<vfs::FileSystem>) asm(
    "_ZN4llvm11PassBuilderC1EPNS_13TargetMachineENS_21PipelineTuningOptionsE" STD_TYPE("8optionalINS_10PGOOptionsEE")
    "PNS_28PassInstrumentationCallbacksENS_18IntrusiveRefCntPtrINS_3vfs10FileSystemEEE");
CGU_HOOK(pb_ctor)
void pb_ctor(PassBuilder *self, TargetMachine *tm, PipelineTuningOptions pto, std::optional<PGOOptions> pgo,
             PassInstrumentationCallbacks *pic, IntrusiveRefCntPtr<vfs::FileSystem> fs) {
    if (rust_caller(__builtin_return_address(0))) {
        backend_started();
        builder = {self, !pgo && pic ? tm : nullptr, pto};
        no_builtins = false;
    }
    static auto next = next_function(pb_ctor_entry, &pb_ctor);
    next(self, tm, pto, std::move(pgo), pic, std::move(fs));
}
#define PIPELINE(fn, kind)                                                                                   \
    CGU_HOOK(fn)                                                                                             \
    ModulePassManager PassBuilder::fn(OptimizationLevel level, ThinOrFullLTOPhase phase) {                   \
        static auto next = next_method(fn##_entry, &PassBuilder::fn);                                        \
        auto p = next(this, level, phase);                                                                   \
        if (!rust_caller(__builtin_return_address(0)))                                                       \
            return p;                                                                                        \
        return wrap_pipeline(this, std::move(p), kind, level, phase);                                                 \
    }
PIPELINE(buildPerModuleDefaultPipeline, "per-module")
PIPELINE(buildO0DefaultPipeline, "O0")
PIPELINE(buildModuleOptimizationPipeline, "module-opt")
CGU_HOOK(prelink)
ModulePassManager PassBuilder::buildThinLTOPreLinkDefaultPipeline(OptimizationLevel level) {
    static auto next = next_method(prelink_entry, &PassBuilder::buildThinLTOPreLinkDefaultPipeline);
    auto passes = next(this, level);
    if (!plugin_lto || !rust_caller(__builtin_return_address(0)))
        return passes;
    return wrap_pipeline(this, std::move(passes), "thin-prelink", level, ThinOrFullLTOPhase::ThinLTOPreLink);
}
CGU_HOOK(thin)
ModulePassManager PassBuilder::buildThinLTODefaultPipeline(OptimizationLevel level, const ModuleSummaryIndex *index) {
    static auto next = next_method(thin_entry, &PassBuilder::buildThinLTODefaultPipeline);
    auto p = next(this, level, index);
    if (index || !rust_caller(__builtin_return_address(0)))
        return p;
    return wrap_pipeline(this, std::move(p), "thin-postlink", level, ThinOrFullLTOPhase::ThinLTOPostLink);
}
CGU_HOOK(llvm_flags)
bool cl::ParseCommandLineOptions(int n, const char *const *argv, StringRef desc, raw_ostream *err,
                                 vfs::FileSystem *fs, const char *env, bool dash) {
    if (rust_caller(__builtin_return_address(0))) {
        compiler_flags.assign(argv + 1, argv + n);
    }
    static auto next = next_function(llvm_flags_entry, &cl::ParseCommandLineOptions);
    return next(n, argv, desc, err, fs, env, dash);
}
CGU_HOOK(disable_builtins)
void TargetLibraryInfoImpl::disableAllFunctions() {
    if (rust_caller(__builtin_return_address(0)))
        no_builtins = true;
    static auto next = next_method(disable_builtins_entry, &TargetLibraryInfoImpl::disableAllFunctions);
    next(this);
}
CGU_HOOK(analysis)
void LLVMAddAnalysisPasses(LLVMTargetMachineRef tm, LLVMPassManagerRef pm) {
    if (rust_caller(__builtin_return_address(0))) {
        backend_started();
        emission = {unwrap(pm), reinterpret_cast<TargetMachine *>(tm), nullptr};
        no_builtins = false;
    }
    static auto next = next_function(analysis_entry, &LLVMAddAnalysisPasses);
    next(tm, pm);
}
extern "C" void open_output(raw_fd_ostream *, StringRef, std::error_code &, sys::fs::OpenFlags) asm(
    "_ZN4llvm14raw_fd_ostreamC1ENS_9StringRefER" STD_TYPE("10error_code") "NS_3sys2fs9OpenFlagsE");
CGU_HOOK(open_output)
void open_output(raw_fd_ostream *os, StringRef path, std::error_code &ec, sys::fs::OpenFlags flags) {
    static auto next = next_function(open_output_entry, &open_output);
    next(os, path, ec, flags);
    if (emission.pm && rust_caller(__builtin_return_address(0)))
        emission.object = !ec && path.ends_with(".rcgu.o") ? os : nullptr;
}
CGU_HOOK(emit)
bool legacy::PassManager::run(Module &m) {
    auto pending = emission;
    emission = {};
    if (this == pending.pm && pending.object) {
        auto write_object = [&](const Reply &result) {
            *pending.object << result.object;
            if (getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
                errs() << "distcc-rust-cgu: emitted " + m.getModuleIdentifier() + "\n";
            return true;
        };
        auto node = take(m);
        Pending *work = node.empty() ? nullptr : &node.mapped();
        if (work && work->reply) {
            SmallVector<char, 0> final_ir;
            raw_svector_ostream stream(final_ir);
            WriteBitcodeToFile(m, stream, true);
            if (StringRef(final_ir.data(), final_ir.size()) == work->reply->bitcode)
                return write_object(*work->reply);
            // The compiler added target sections after materializing LLVM IR.
            // Its emitter owns those final edits. Never resubmit the module.
        } else if (work) {
            if (auto reply = dispatch(m, work->spec))
                return write_object(*reply);
            work->run_locally(m);
        } else if (eligible(m) && supported(*pending.tm)) {
            auto spec = cgu::describe(*pending.tm, PipelineTuningOptions());
            spec["kind"] = "object";
            if (auto reply = dispatch(m, std::move(spec)))
                return write_object(*reply);
        }
    }
    static auto next = next_method(emit_entry, &legacy::PassManager::run);
    return next(this, m);
}
