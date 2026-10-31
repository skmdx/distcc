/* Compiler ownership in a disposable job process.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_CLANG_COMPILER_H
#define DCC_CLANG_COMPILER_H

#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/BuryPointer.h>
#include <memory>

// The driver normally requests DisableFree because the process exits after
// compilation. Honor that contract in our isolated children as well. This
// owner must not be used by the long-lived analyzer or library-cache parent.
struct DccCompilerDeleter {
    void operator()(clang::CompilerInstance *compiler) const {
        if (!compiler->getFrontendOpts().DisableFree) { delete compiler; return; }
        // Serialized diagnostics and their streams must still be finalized.
        // ToolInvocation may lend a consumer; its caller retains ownership.
        if (compiler->hasDiagnostics()) compiler->getDiagnostics().takeClient().reset();
        llvm::BuryPointer(compiler);
    }
};
using DccChildCompiler = std::unique_ptr<clang::CompilerInstance, DccCompilerDeleter>;

#endif
