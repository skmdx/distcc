#ifndef DISTCC_LTO_PARTITION_H
#define DISTCC_LTO_PARTITION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Module;
// Private build of LLVM's splitter, with instruction-weighted placement.
void dccSplitModule(Module &, unsigned,
                    function_ref<void(std::unique_ptr<Module>)>, bool, bool);
}

// Consume verified partitions while their IR is alive. The callback may fork
// before returning; the last partition can use the caller's execution lane.
// The original module is modified and must not resume its backend. The returned
// names identify promoted local symbols to restore after the relocatable merge.
llvm::Expected<std::vector<std::string>>
dcc_split_module(llvm::Module &, unsigned count,
                 llvm::function_ref<llvm::Error(llvm::Module &, bool last)>);

// File transport for backends that execute outside this process tree.
llvm::Expected<llvm::json::Value>
dcc_partition_module(llvm::Module &module, unsigned count, llvm::StringRef prefix);

#endif
