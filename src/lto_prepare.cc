/* Stop the selected Clang ThinLTO backend after LLVM has resolved and imported
 * its inputs. This library is built and installed with that LLVM ABI. */
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "lto_partition.h"
#include <cstdlib>
#include <dlfcn.h>

llvm::Error llvm::lto::thinBackend(
    const Config &config, unsigned task, AddStreamFn addStream, Module &module,
    const ModuleSummaryIndex &index,
    const FunctionImporter::ImportMapTy &imports,
    const GVSummaryMapTy &globals,
    MapVector<StringRef, BitcodeModule> *modules, bool codegenOnly,
#if LLVM_VERSION_MAJOR >= 23
    ArrayRef<StringRef> libfuncs,
#endif
    AddStreamFn irStream, const std::vector<uint8_t> &args) {
  const char *destination = std::getenv("DISTCC_LTO_PARTITION_FILE");
  const char *requested = std::getenv("DISTCC_LTO_PARTITIONS");
  unsigned partitions;
  if (codegenOnly || !destination || !requested ||
      StringRef(requested).getAsInteger(10, partitions) || !partitions)
    report_fatal_error("distcc preparation requires a ThinLTO partition request");
  Dl_info symbol{};
  if (!dladdr(reinterpret_cast<void *>(&thinBackend), &symbol))
    report_fatal_error("cannot locate the ThinLTO backend symbol");
  auto original = reinterpret_cast<decltype(&thinBackend)>(
      dlsym(RTLD_NEXT, symbol.dli_sname));
  if (!original)
    report_fatal_error("cannot locate the installed ThinLTO backend");

  // Clang owns a mutable Config for this single invocation. Borrow only its
  // import hook, restoring it before returning. Interposing opt() instead is
  // insufficient: optimized LLVM builds inline it inside thinBackend().
  auto &hook = const_cast<Config &>(config).PostImportModuleHook;
  auto previous = std::move(hook);
  auto restore = scope_exit([&] { hook = std::move(previous); });
  hook = [&](unsigned task, const Module &prepared) {
    if (previous && !previous(task, prepared))
      return false;
    // Clang owns this mutable module. Returning false ends its backend, so
    // partitioning can externalize locals without changing any later pass.
    // Writing and reparsing the entire imported IR here doubles expensive
    // constant-data serialization; emit only the final partition inputs.
    auto result = dcc_partition_module(const_cast<Module &>(prepared), partitions,
                                       (Twine(destination) + ".part-").str());
    if (!result) {
      logAllUnhandledErrors(result.takeError(), errs(), "distcc-lto: ");
      report_fatal_error("cannot partition ThinLTO module");
    }
    std::error_code error;
    raw_fd_ostream output(destination, error, sys::fs::OF_None);
    if (error)
      report_fatal_error(Twine("cannot prepare ThinLTO module: ") + error.message());
    output << std::move(*result) << '\n';
    output.close();
    if (output.has_error())
      report_fatal_error("cannot finish ThinLTO module output");
    return false;
  };
  return original(config, task, addStream, module, index, imports, globals,
                  modules, codegenOnly,
#if LLVM_VERSION_MAJOR >= 23
                  libfuncs,
#endif
                  irStream, args);
}
