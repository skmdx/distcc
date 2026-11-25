/* Private interface between the ELF interposer and its sibling LLVM module. */
#pragma once
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/VirtualFileSystem.h>

namespace cgu {
struct Hooks {
    void (*module)(llvm::ModulePassManager &, llvm::Module &, llvm::ModuleAnalysisManager &);
    void (*custom)(llvm::PassBuilder *);
    void (*destroy)(llvm::Module &);
    void (*builder)(llvm::PassBuilder *, llvm::TargetMachine *, llvm::PipelineTuningOptions);
    llvm::ModulePassManager (*pipeline)(llvm::PassBuilder *, llvm::function_ref<llvm::ModulePassManager()>,
                                        const char *, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase);
    void (*flags)(int, const char *const *);
    void (*disable_builtins)();
    void (*analysis)(llvm::legacy::PassManagerBase *, llvm::TargetMachine *);
    void (*output)(llvm::raw_fd_ostream *);
    bool (*emit)(llvm::legacy::PassManager *, llvm::Module &);
};
extern "C" const Hooks *dcc_cgu_initialize(const char *client);
}
