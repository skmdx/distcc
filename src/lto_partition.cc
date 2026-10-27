#include "lto_partition.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ModuleSymbolTable.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>
#include <vector>

using namespace llvm;

Expected<json::Value> dcc_partition_module(Module &module, unsigned count,
                                         StringRef prefix) {
  auto definitions = llvm::count_if(module, [](const Function &function) {
    return !function.isDeclarationForLinker();
  });
  count = std::min<unsigned>(count, definitions);
  // Let LLVM's assembler interpret directives. Strong global declarations and
  // definitions survive the relocatable merge with assembly in partition zero.
  // Local assembly definitions and per-object weak/versioned references lack
  // the IR ownership information needed to move their users between objects.
  bool keepAssemblyTogether = false;
  if (!module.getModuleInlineAsm().empty()) {
    InitializeAllTargetInfos();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    ModuleSymbolTable::CollectAsmSymbols(module,
        [&](StringRef, object::BasicSymbolRef::Flags flags) {
          using Symbol = object::BasicSymbolRef;
          if (!(flags & Symbol::SF_Undefined) && !(flags & Symbol::SF_Global))
            keepAssemblyTogether = true;
          if ((flags & Symbol::SF_Undefined) && (flags & Symbol::SF_Weak))
            keepAssemblyTogether = true;
        });
    ModuleSymbolTable::CollectAsmSymvers(module, [&](StringRef, StringRef) {
      keepAssemblyTogether = true;
    });
  }

  json::Array parts;
  auto write = [&](const Module &part) -> Error {
    if (verifyModule(part, &errs()))
      return createStringError("invalid LLVM partition");
    auto path = (prefix + Twine(parts.size()) + ".bc").str();
    std::error_code error;
    raw_fd_ostream output(path, error, sys::fs::OF_None);
    if (error)
      return errorCodeToError(error);
    WriteBitcodeToFile(part, output);
    output.close();
    if (output.has_error())
      return createStringError("cannot write LLVM partition");
    int64_t instructions = 0;
    for (auto &function : part)
      instructions += function.getInstructionCount();
    parts.push_back(json::Object{{"path", std::move(path)},
                                 {"instructions", instructions}});
    return Error::success();
  };
  json::Array names;
  if (count < 2 || keepAssemblyTogether) {
    if (auto error = write(module))
      return std::move(error);
  } else {
    std::vector<GlobalValue *> locals;
    for (auto &global : module.global_values())
      if (global.hasLocalLinkage())
        locals.push_back(&global);
    Error failure = Error::success();
    dccSplitModule(module, count, [&](std::unique_ptr<Module> part) {
      if (!failure)
        failure = write(*part);
    }, false, true);
    if (failure)
      return std::move(failure);
    for (auto *local : locals) {
      SmallString<128> name;
      Mangler::getNameWithPrefix(name, local->getName(), module.getDataLayout());
      names.push_back(name.str().str());
    }
  }
  return json::Value(json::Object{{"parts", std::move(parts)},
                                  {"locals", std::move(names)}});
}
