/* Exercise the post-import partitioner on LLVM IR, including modules that
 * cannot be represented by a ThinLTO summary index. */
#include "../src/lto_partition.h"
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

int main(int argc, char **argv) {
    unsigned count;
    if (argc != 4 || llvm::StringRef(argv[2]).getAsInteger(10, count) || !count) return 1;
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto module = llvm::parseIRFile(argv[1], error, context);
    if (!module) {
        error.print(argv[0], llvm::errs());
        return 1;
    }
    auto parts = dcc_partition_module(*module, count, argv[3]);
    if (!parts) {
        llvm::logAllUnhandledErrors(parts.takeError(), llvm::errs());
        return 1;
    }
    llvm::outs() << std::move(*parts) << '\n';
}
