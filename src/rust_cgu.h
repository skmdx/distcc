/* Same-LLVM backend contract. Only scalar values and UTF-8 strings cross the wire. */
#pragma once
#include <llvm-c/Core.h>
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
inline bool matching_llvm() {
    unsigned major, minor, patch;
    LLVMGetVersion(&major, &minor, &patch);
    return major == LLVM_VERSION_MAJOR && minor == LLVM_VERSION_MINOR && patch == LLVM_VERSION_PATCH;
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
#define DCC_CGU_TARGET_FIELDS(X)                                                                             \
    X(FloatABIType)                                                                                          \
    X(DataSections) X(FunctionSections) X(UniqueSectionNames) X(MCOptions.AsmVerbose)                        \
        X(MCOptions.PreserveAsmComments) X(MCOptions.CompressDebugSections) X(MCOptions.X86RelaxRelocations) \
            X(UseInitArray) X(EmulatedTLS) X(TrapUnreachable) X(NoTrapAfterNoreturn) X(ThreadModel)          \
                X(ExceptionModel) X(EmitStackSizeSection) X(EnableFastISel) X(EnableGlobalISel)
#define DCC_CGU_TUNING_FIELDS(X)                                                                             \
    X(LoopInterleaving)                                                                                      \
    X(LoopVectorization) X(SLPVectorization) X(LoopUnrolling) X(LoopInterchange) X(LoopFusion)               \
        X(ForgetAllSCEVInLoopUnroll) X(LicmMssaOptCap) X(LicmMssaNoAccForPromotionCap) X(CallGraphProfile)   \
            X(UnifiedLTO) X(MergeFunctions) X(InlinerThreshold) X(EagerlyInvalidateAnalyses)                 \
                X(DevirtualizeSpeculatively)

inline Object describe(const TargetMachine &tm, const PipelineTuningOptions &pto) {
    const auto &opts = tm.Options;
    Object obj{{"llvm", LLVM_VERSION_STRING},
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
inline std::string pipeline_name(ModulePassManager &passes, PassInstrumentationCallbacks &pic) {
    std::string name;
    raw_string_ostream out(name);
    passes.printPipeline(out, [&](StringRef id) { return pic.getPassNameForClassName(id); });
    return name;
}
inline bool supported_flag(StringRef arg) {
    // LLVM command line parsing can run plugins. Accept rustc's builtin
    // scalar defaults only. A different option set runs in the compiler.
    return arg == "-generate-arange-section" || arg == "-mergefunc-use-aliases" ||
           arg == "-preserve-alignment-assumptions-during-inlining=false" ||
           arg == "-import-cold-multiplier=0.1";
}
} // namespace cgu
