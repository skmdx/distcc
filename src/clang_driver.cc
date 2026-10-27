/* Compiler-owned argument classification and auxiliary inputs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Options/Options.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Option/Arg.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/ADT/SmallString.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "clang_inputs.h"

namespace {
class Capture : public clang::tooling::ToolAction {
public:
    std::shared_ptr<clang::CompilerInvocation> compiler;
    bool runInvocation(std::shared_ptr<clang::CompilerInvocation> invocation,
                       clang::FileManager *,
                       std::shared_ptr<clang::PCHContainerOperations>,
                       clang::DiagnosticConsumer *) override {
        compiler = std::move(invocation);
        return true;
    }
};

std::string absolute(llvm::StringRef name) {
    llvm::SmallString<256> path(name);
    if (llvm::sys::fs::make_absolute(path)) return {};
    llvm::sys::path::remove_dots(path, true);
    return path.str().str();
}

char **copy(const std::vector<std::string> &values) {
    auto result = static_cast<char **>(calloc(values.size() + 1, sizeof(char *)));
    if (!result) return nullptr;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!(result[i] = strdup(values[i].c_str()))) {
            for (size_t j = 0; j < i; ++j) free(result[j]);
            free(result);
            return nullptr;
        }
    }
    return result;
}
}

extern "C" int dcc_clang_expand_response_files(char **argv, char ***expanded) {
    llvm::SmallVector<const char *, 32> args;
    for (char **p = argv; *p; ++p) args.push_back(*p);
    llvm::BumpPtrAllocator allocator;
    llvm::cl::ExpansionContext context(allocator, llvm::cl::TokenizeGNUCommandLine);
    if (auto error = context.expandResponseFiles(args)) {
        llvm::consumeError(std::move(error));
        return -1;
    }
    std::vector<std::string> values;
    for (const char *arg : args) {
        // LLVM leaves unreadable response files untouched. Let the actual
        // compiler diagnose them locally instead of sending a partial argv.
        if (arg[0] == '@') return -1;
        values.emplace_back(arg);
    }
    *expanded = copy(values);
    return *expanded ? 0 : -1;
}

extern "C" int dcc_clang_inputs(char **argv, char ***normalized, char ***files) {
    std::vector<std::string> command;
    std::vector<const char *> raw;
    for (unsigned i = 0; argv[i]; ++i) {
        command.emplace_back(argv[i]);
        if (i) raw.push_back(argv[i]);
    }
    const std::string invoked_as = command.front();
    clang::tooling::addTargetAndModeForProgramName(command, invoked_as);
    auto executable = llvm::sys::findProgramByName(command.front());
    if (executable) command.front() = *executable;
    llvm::IntrusiveRefCntPtr<clang::FileManager> manager(
        new clang::FileManager(clang::FileSystemOptions()));
    Capture action;
    clang::tooling::ToolInvocation invocation(command, &action, manager.get(),
        std::make_shared<clang::PCHContainerOperations>());
    if (!invocation.run() || !action.compiler) return -1;
    std::set<std::string> used;
    for (const auto &input : action.compiler->getFrontendOpts().Inputs)
        if (input.isFile()) used.insert(absolute(input.getFile()));
    unsigned missing_index = 0, missing_count = 0;
    auto args = clang::getDriverOptTable().ParseArgs(raw, missing_index, missing_count);
    if (missing_count) return -1;
    std::set<unsigned> unused;
    for (const auto *arg : args.filtered(clang::options::OPT_INPUT))
        if (!used.count(absolute(arg->getValue()))) unused.insert(arg->getIndex() + 1);
    // Resolve directory/default profile selection on the client. The driver
    // otherwise tests the worker's physical directory before consulting VFS.
    const auto *profile = args.getLastArg(clang::options::OPT_fprofile_use,
        clang::options::OPT_fprofile_use_EQ, clang::options::OPT_fprofile_instr_use,
        clang::options::OPT_fprofile_instr_use_EQ);
    auto profile_path = action.compiler->getCodeGenOpts().ProfileInstrumentUsePath;
    std::vector<std::string> result, inputs;
    for (unsigned i = 0; argv[i]; ++i) {
        if (unused.count(i)) continue;
        if (profile && !profile_path.empty() && i == profile->getIndex() + 1)
            result.emplace_back("-fprofile-instr-use=" + absolute(profile_path));
        else result.emplace_back(argv[i]);
    }
    bool valid = true;
    dcc_clang_profile_inputs(action.compiler->getCodeGenOpts(), [&](std::string &path) {
        auto name = absolute(path);
        if (name.empty()) { valid = false; return; }
        if (std::find(inputs.begin(), inputs.end(), name) == inputs.end())
            inputs.push_back(std::move(name));
    });
    if (!valid) return -1;
    *normalized = copy(result);
    *files = copy(inputs);
    if (*normalized && *files) return 0;
    for (char **list : {*normalized, *files}) {
        if (!list) continue;
        for (char **p = list; *p; ++p) free(*p);
        free(list);
    }
    *normalized = *files = nullptr;
    return -1;
}
