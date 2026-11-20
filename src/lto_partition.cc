#include "lto_partition.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ModuleSymbolTable.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <algorithm>
#include <memory>
#include <vector>

using namespace llvm;

// Function asm is an operand template, not module asm that can be fed to
// CollectAsmSymbols. Accept instruction-only AT&T templates on ELF x86.
// Operands may refer to IR operands, literal numbers, or a numeric label
// resolved within this template. Named definitions and cross-template
// references lack IR ownership and stay in one object.
static bool independentAsm(const Module &module, const InlineAsm &assembly) {
  StringRef text = assembly.getAsmString().trim();
  if (text.empty()) return true;
  if (!module.getTargetTriple().isX86() || !module.getTargetTriple().isOSBinFormatELF() ||
      assembly.getDialect() != InlineAsm::AD_ATT || !module.getModuleInlineAsm().empty())
    return false;
  unsigned defined = 0, forward = 0;
  while (!text.empty()) {
    size_t end = text.find_first_of(";\n");
    auto line = text.take_front(end).trim();
    text = end == StringRef::npos ? StringRef() : text.drop_front(end + 1);
    if (line.size() >= 2 && isDigit(line[0]) && line[1] == ':') {
      unsigned label = 1u << (line[0] - '0');
      defined |= label;
      forward &= ~label;
      line = line.drop_front(2).trim();
    }
    if (line.empty()) continue;
    auto name = line.take_until([](char c) { return c == ' ' || c == '\t'; });
    auto args = line.drop_front(name.size()).trim();
    if (name == ".byte") {
      do {
        auto [value, rest] = args.split(',');
        unsigned byte;
        if (value.trim().getAsInteger(0, byte) || byte > 255) return false;
        args = rest.trim();
      } while (!args.empty());
    } else {
      if (!llvm::all_of(name, [](char c) { return isAlnum(c); })) return false;
      if (args.size() == 2 && isDigit(args[0]) && (args[1] == 'b' || args[1] == 'f')) {
        unsigned label = 1u << (args[0] - '0');
        if (args[1] == 'f') forward |= label;
        else if (!(defined & label)) return false;
      } else if (args.find_first_not_of("0123456789(),$*+- \t") != StringRef::npos)
        return false;
    }
  }
  return forward == 0;
}

Expected<std::vector<std::string>>
dcc_split_module(Module &module, unsigned count,
                 function_ref<Error(Module &, bool)> consume) {
  auto definitions = llvm::count_if(module, [](const Function &function) {
    return !function.isDeclarationForLinker();
  });
  count = std::min<unsigned>(count, definitions);
  // Let LLVM's assembler interpret directives. Strong global declarations and
  // definitions survive the relocatable merge with assembly in partition zero.
  // Local assembly definitions and per-object weak/versioned references lack
  // the IR ownership information needed to move their users between objects.
  bool keepTogether = false;
  // Function assembly may also define symbols referenced elsewhere in this
  // module. LLVM IR does not describe those edges; splitting can turn a local
  // assembler definition into an unresolved reference in another object.
  for (const auto &function : module)
    for (const auto &block : function)
      for (const auto &instruction : block)
        if (const auto *call = dyn_cast<CallBase>(&instruction))
          if (const auto *assembly = dyn_cast<InlineAsm>(call->getCalledOperand()))
            if (!independentAsm(module, *assembly)) keepTogether = true;
  if (!module.getModuleInlineAsm().empty()) {
    InitializeAllTargetInfos();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    ModuleSymbolTable::CollectAsmSymbols(module,
        [&](StringRef, object::BasicSymbolRef::Flags flags) {
          using Symbol = object::BasicSymbolRef;
          if (!(flags & Symbol::SF_Undefined) && !(flags & Symbol::SF_Global))
            keepTogether = true;
          if ((flags & Symbol::SF_Undefined) && (flags & Symbol::SF_Weak))
            keepTogether = true;
        });
    ModuleSymbolTable::CollectAsmSymvers(module, [&](StringRef, StringRef) {
      keepTogether = true;
    });
  }

  // Explicit sections can be linker-assembled arrays whose declaration order
  // is observable (for example ordered option tables). Independent objects
  // cannot preserve that order through the relocatable merge.
  for (const auto &global : module.globals())
    if (global.hasSection() && global.getSection() != "llvm.metadata" &&
        !(module.getTargetTriple().isOSBinFormatELF() &&
          llvm::is_contained({StringRef(".rodata"), StringRef(".data"), StringRef(".bss"),
                              StringRef(".tdata"), StringRef(".tbss")}, global.getSection())))
      keepTogether = true;

  auto emit = [&](Module &part, bool last) -> Error {
    if (verifyModule(part, &errs()))
      return createStringError("invalid LLVM partition");
    return consume(part, last);
  };
  std::vector<std::string> names;
  if (count < 2 || keepTogether) {
    if (auto error = emit(module, true))
      return std::move(error);
  } else {
    // AsmPrinter normally uses a local alias for these definitions. After
    // splitting, a caller only sees a declaration, so that alias is no longer
    // available (and i386 emits an invalid PC32 relocation for a shared DSO).
    // Keep the definition internal and export its original name as an alias.
    // LLVM then preserves local binding across parts, and the usual merge
    // restores the promoted internal name without hiding the public symbol.
    if (Triple(module.getTargetTriple()).isOSBinFormatELF() &&
        module.getPICLevel() != PICLevel::NotPIC &&
        module.getPIELevel() == PIELevel::Default) {
      std::vector<GlobalValue *> definitions;
      for (auto &global : module.global_values())
        if (global.isDSOLocal() && global.canBenefitFromLocalAlias())
          definitions.push_back(&global);
      for (auto *global : definitions) {
        std::string name = global->getName().str();
        global->setName(name + ".distcc.local");
        auto *alias = GlobalAlias::create(name, global);
        alias->setDSOLocal(true);
        alias->setThreadLocalMode(global->getThreadLocalMode());
        global->setLinkage(GlobalValue::InternalLinkage);
      }
    }
    std::vector<GlobalValue *> locals;
    for (auto &global : module.global_values())
      if (global.hasLocalLinkage())
        locals.push_back(&global);
    SmallVector<GlobalValue *, 16> used, compilerUsed;
    collectUsedGlobalVariables(module, used, false);
    collectUsedGlobalVariables(module, compilerUsed, true);
    Error failure = Error::success();
    unsigned index = 0;
    dccSplitModule(module, count, [&](std::unique_ptr<Module> part) {
      // SplitModule hashes the retention arrays independently of their
      // definitions. Rebuild each list beside the definitions it protects,
      // so optimization and ELF section GC retain the same input objects.
      auto retain = [&](StringRef name, ArrayRef<GlobalValue *> original,
                        bool compilerOnly) {
        if (auto *list = part->getNamedGlobal(name)) list->eraseFromParent();
        SmallVector<GlobalValue *, 16> values;
        for (auto *global : original) {
          auto *copy = part->getNamedValue(global->getName());
          assert(copy && "split module lost a retained global");
          if (!copy->isDeclarationForLinker()) values.push_back(copy);
        }
        if (compilerOnly) appendToCompilerUsed(*part, values);
        else appendToUsed(*part, values);
      };
      retain("llvm.used", used, false);
      retain("llvm.compiler.used", compilerUsed, true);
      if (!failure)
        failure = emit(*part, ++index == count);
    }, false, true);
    if (failure)
      return std::move(failure);
    for (auto *local : locals) {
      SmallString<128> name;
      Mangler::getNameWithPrefix(name, local->getName(), module.getDataLayout());
      names.push_back(name.str().str());
    }
  }
  return names;
}

Expected<json::Value> dcc_partition_module(Module &module, unsigned count,
                                         StringRef prefix) {
  json::Array parts;
  auto names = dcc_split_module(module, count, [&](Module &part, bool) -> Error {
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
  });
  if (!names)
    return names.takeError();
  json::Array locals;
  for (auto &name : *names) locals.push_back(std::move(name));
  return json::Value(json::Object{{"parts", std::move(parts)},
                                  {"locals", std::move(locals)}});
}
