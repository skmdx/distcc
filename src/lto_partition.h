#ifndef DISTCC_LTO_PARTITION_H
#define DISTCC_LTO_PARTITION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace llvm { class Module; }

// The caller owns the prepared module and will not resume its backend.
llvm::Expected<llvm::json::Value>
dcc_partition_module(llvm::Module &module, unsigned count, llvm::StringRef prefix);

#endif
