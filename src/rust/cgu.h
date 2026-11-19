/* Same-LLVM backend contract. Only scalar values and UTF-8 strings cross the wire. */
#pragma once
#include <llvm-c/Core.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/JSON.h>
#include <llvm/Target/TargetMachine.h>
#include <stdexcept>

namespace cgu {
using namespace llvm;
using Object = json::Object;
#define DCC_STRING_1(x) #x
#define DCC_STRING(x) DCC_STRING_1(x)
inline constexpr char abi[] = DCC_STRING(LLVM_VERSION_MAJOR) "." DCC_STRING(LLVM_VERSION_MINOR);
inline bool matching_llvm() {
    unsigned major, minor, patch;
    LLVMGetVersion(&major, &minor, &patch);
    return major == LLVM_VERSION_MAJOR && minor == LLVM_VERSION_MINOR;
}
inline const json::Value &field(const Object &data, StringRef key) {
    if (auto *v = data.get(key))
        return *v;
    throw std::runtime_error(("missing LLVM setting " + key).str());
}
inline StringRef string(const Object &data, StringRef key) {
    if (auto value = field(data, key).getAsString())
        return *value;
    throw std::runtime_error("invalid LLVM string setting");
}
inline int integer(const Object &data, StringRef key) {
    if (auto value = field(data, key).getAsInteger()) {
        if (*value >= INT_MIN && *value <= INT_MAX)
            return static_cast<int>(*value);
    }
    throw std::runtime_error("invalid LLVM integer setting");
}
// rustc starts with InitTargetOptionsFromCodeGenFlags, then supplies these
// explicit overrides in LLVMRustCreateTargetMachine. The worker starts from
// the same clang/LLVM command line before restoring these values.
// clang-format off
#define DCC_CGU_TARGET_FIELDS(X) \
    X(FloatABIType) X(DataSections) X(FunctionSections) X(UniqueSectionNames) \
    X(MCOptions.AsmVerbose) X(MCOptions.PreserveAsmComments) X(MCOptions.CompressDebugSections) \
    X(MCOptions.X86RelaxRelocations) X(UseInitArray) X(EmulatedTLS) X(TrapUnreachable) \
    X(NoTrapAfterNoreturn) X(ThreadModel) X(ExceptionModel) X(EmitStackSizeSection) \
    X(EnableFastISel) X(EnableGlobalISel)
#define DCC_CGU_TUNING_FIELDS(X) \
    X(LoopInterleaving) X(LoopVectorization) X(SLPVectorization) X(LoopUnrolling) \
    X(LoopInterchange) X(LoopFusion) X(ForgetAllSCEVInLoopUnroll) X(LicmMssaOptCap) \
    X(LicmMssaNoAccForPromotionCap) X(CallGraphProfile) X(UnifiedLTO) X(MergeFunctions) \
    X(InlinerThreshold) X(EagerlyInvalidateAnalyses) X(DevirtualizeSpeculatively)
// clang-format on

inline Object describe(const TargetMachine &tm, const PipelineTuningOptions &pto) {
    const auto &opts = tm.Options;
    Object obj{{"llvm", abi},
               {"triple", tm.getTargetTriple().str()},
               {"backend", tm.getTarget().getName()},
               {"cpu", tm.getTargetCPU()},
               {"features", tm.getTargetFeatureString()},
               {"reloc", int(tm.getRelocationModel())},
               {"model", int(tm.getCodeModel())},
               {"cgopt", int(tm.getOptLevel())},
               {"abi", opts.MCOptions.ABIName},
               {"object", opts.ObjectFilenameForDebug}};
#define SAVE_TARGET(F) obj[#F] = int(opts.F);
#define SAVE_TUNING(F) obj[#F] = int(pto.F);
    DCC_CGU_TARGET_FIELDS(SAVE_TARGET)
    DCC_CGU_TUNING_FIELDS(SAVE_TUNING)
#undef SAVE_TARGET
#undef SAVE_TUNING
    return obj;
}
inline void configure(const Object &obj, TargetOptions &opts, PipelineTuningOptions &pto) {
#define LOAD_TARGET(F) opts.F = static_cast<decltype(opts.F)>(integer(obj, #F));
#define LOAD_TUNING(F) pto.F = static_cast<decltype(pto.F)>(integer(obj, #F));
    DCC_CGU_TARGET_FIELDS(LOAD_TARGET)
    DCC_CGU_TUNING_FIELDS(LOAD_TUNING)
#undef LOAD_TARGET
#undef LOAD_TUNING
    opts.MCOptions.ABIName = string(obj, "abi").str();
    opts.ObjectFilenameForDebug = string(obj, "object").str();
}
inline const OptimizationLevel levels[] = {
    OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::O3,
#if LLVM_VERSION_MAJOR < 23
    OptimizationLevel::Os, OptimizationLevel::Oz,
#endif
};
inline OptimizationLevel optimization(unsigned index) {
    if (index >= std::size(levels))
        throw std::runtime_error("invalid optimization level");
    return levels[index];
}
inline unsigned encode(OptimizationLevel level) {
    auto it = llvm::find(levels, level);
    if (it == std::end(levels))
        throw std::runtime_error("unrecognized optimization level");
    return it - std::begin(levels);
}
inline ModulePassManager pipeline(PassBuilder &pb, StringRef kind, int level, int phase) {
    if (phase < 0 || phase > int(ThinOrFullLTOPhase::FullLTOPostLink))
        throw std::runtime_error("invalid LLVM pipeline");
    auto opt = cgu::optimization(level);
    auto lto = static_cast<ThinOrFullLTOPhase>(phase);
    if (kind == "O0")
        return pb.buildO0DefaultPipeline(opt, lto);
    if (kind == "per-module")
        return pb.buildPerModuleDefaultPipeline(opt, lto);
    if (kind == "module-opt")
        return pb.buildModuleOptimizationPipeline(opt, lto);
    if (kind == "thin-postlink")
        return pb.buildThinLTODefaultPipeline(opt, nullptr);
    throw std::runtime_error("unknown CGU pipeline");
}

// The compiler can append target assembly after optimization (including its
// embedded input bitcode). Keep those fragments outside the optimizer.
inline size_t assembly_size(const Module &m) {
#if LLVM_VERSION_MAJOR >= 23
    size_t bytes = 0;
    for (const auto &fragment : m.getModuleInlineAsm())
        bytes += fragment.Asm.size();
    return bytes;
#else
    return m.getModuleInlineAsm().size();
#endif
}
inline auto separate_final_assembly(Module &m, size_t initial) {
    if (initial > assembly_size(m))
        throw std::runtime_error("CGU assembly boundary exceeds module");
#if LLVM_VERSION_MAJOR >= 23
    SmallVector<Module::GlobalAsmFragment, 0> before, after;
    for (const auto &f : m.getModuleInlineAsm()) {
        size_t split = std::min(initial, f.Asm.size());
        if (split)
            before.emplace_back(f.Asm.substr(0, split), f.Props);
        if (split < f.Asm.size())
            after.emplace_back(f.Asm.substr(split), f.Props);
        initial -= split;
    }
    m.setModuleInlineAsm(before);
    return [after = std::move(after)](Module &m) {
        for (const auto &f : after)
            m.appendModuleInlineAsm(f);
    };
#else
    auto after = m.getModuleInlineAsm().substr(initial);
    if (!after.empty())
        m.setModuleInlineAsm(m.getModuleInlineAsm().substr(0, initial));
    return [after = std::move(after)](Module &m) { m.appendModuleInlineAsm(after); };
#endif
}

inline void run_pipeline(Module &m, TargetMachine &tm, const PipelineTuningOptions &tuning,
                         const TargetLibraryInfoImpl &tli, const Object &spec) {
    PassInstrumentationCallbacks pic;
    PassBuilder pb(&tm, tuning, std::nullopt, &pic);
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    fam.registerPass([&] { return pb.buildDefaultAAPipeline(); });
    fam.registerPass([&] { return TargetLibraryAnalysis(tli); });
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    auto passes = pipeline(pb, string(spec, "kind"), integer(spec, "level"), integer(spec, "phase"));
    if (auto tail = spec.getString("tail"); tail && !tail->empty()) {
        if (auto error = pb.parsePassPipeline(passes, *tail))
            throw std::runtime_error(toString(std::move(error)));
    }
    if (auto bytes = spec.getInteger("initial-asm")) {
        auto finish = separate_final_assembly(m, *bytes);
        passes.run(m, mam);
        finish(m);
    } else {
        passes.run(m, mam);
    }
}
} // namespace cgu
