/* Compiler-owned argument classification and auxiliary inputs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Options/Options.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Option/Arg.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/TargetParser/Host.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>
#include "clang_inputs.h"

namespace {
// Own the driver's plan while callers borrow its cc1 arguments. Parsing the
// invocation supplies input roots and auxiliary files; it must not regenerate
// or otherwise change the command the actual compiler selected.
class FrontendPlan {
    std::vector<std::string> command;
    llvm::SmallVector<const char *> argv;
    std::unique_ptr<clang::DiagnosticOptions> diagnostic_options;
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics;
    std::unique_ptr<clang::driver::Driver> driver;
    std::unique_ptr<clang::driver::Compilation> compilation;
public:
    llvm::ArrayRef<const char *> cc1;
    std::unique_ptr<clang::CompilerInvocation> compiler;
    explicit FrontendPlan(char **input) {
        for (char **p = input; *p; ++p) command.emplace_back(*p);
        if (command.empty()) return;
        const auto invoked_as = command.front();
        clang::tooling::addTargetAndModeForProgramName(command, invoked_as);
        auto executable = llvm::sys::findProgramByName(command.front());
        if (executable) command.front() = *executable;
        for (const auto &arg : command) argv.push_back(arg.c_str());
        diagnostic_options = clang::CreateAndPopulateDiagOpts(argv);
        diagnostics = clang::CompilerInstance::createDiagnostics(
            *llvm::vfs::getRealFileSystem(), *diagnostic_options);
        if (argv.size() >= 2 && llvm::StringRef(argv[1]) == "-cc1") {
            cc1 = llvm::ArrayRef(argv).drop_front();
        } else {
            driver = std::make_unique<clang::driver::Driver>(argv[0],
                llvm::sys::getDefaultTargetTriple(), *diagnostics);
            compilation.reset(driver->BuildCompilation(argv));
            if (!compilation || compilation->containsError() || diagnostics->hasErrorOccurred()) return;
            auto args = clang::tooling::getCC1Arguments(diagnostics.get(), compilation.get());
            if (!args) return;
            cc1 = *args;
        }
        auto invocation = std::make_unique<clang::CompilerInvocation>();
        if (!clang::CompilerInvocation::CreateFromArgs(*invocation, cc1, *diagnostics, argv[0]) ||
            diagnostics->hasErrorOccurred()) return;
        compiler = std::move(invocation);
    }
    const std::string &executable() const { return command.front(); }
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

extern "C" int dcc_clang_frontend(char **argv, char ***cc1, char ***roots) {
    *cc1 = *roots = nullptr;
    FrontendPlan plan(argv);
    if (!plan.compiler) return -1;
    auto &invocation = *plan.compiler;
    const auto &inputs = invocation.getFrontendOpts().Inputs;
    if (inputs.size() != 1 || !inputs[0].isFile()) return -1;
    auto boot = llvm::MemoryBuffer::getFile("/proc/sys/kernel/random/boot_id");
    if (!boot) return -1;
    // Cache identities include the reader's credentials. Stat permission must
    // never let one user reuse bytes previously read by a different user.
    std::string identity = "distcc frontend files:" + (*boot)->getBuffer().str() +
        ':' + std::to_string(geteuid()) + ':' + std::to_string(getegid());
    int count = getgroups(0, nullptr);
    if (count < 0) return -1;
    std::vector<gid_t> groups(count);
    if (getgroups(count, groups.data()) != count) return -1;
    std::sort(groups.begin(), groups.end());
    for (auto group : groups) identity += ':' + std::to_string(group);
    std::vector<std::string> result{plan.executable(), inputs[0].getFile().str(), identity}, directories;
    for (auto arg : plan.cc1) result.emplace_back(arg);
    std::set<std::string> unique;
    for (const auto &entry : invocation.getHeaderSearchOpts().UserEntries)
        unique.insert(absolute(entry.Path));
    for (const auto &input : invocation.getFrontendOpts().Inputs)
        if (input.isFile()) unique.insert(llvm::sys::path::parent_path(absolute(input.getFile())).str());
    const auto &preprocessor = invocation.getPreprocessorOpts();
    for (const auto &paths : {preprocessor.Includes, preprocessor.MacroIncludes})
        for (const auto &path : paths)
            unique.insert(llvm::sys::path::parent_path(absolute(path)).str());
    if (!preprocessor.ImplicitPCHInclude.empty())
        unique.insert(llvm::sys::path::parent_path(absolute(preprocessor.ImplicitPCHInclude)).str());
    for (const auto &path : unique) if (!path.empty()) directories.push_back(path);
    *cc1 = copy(result); *roots = copy(directories);
    return *cc1 && *roots ? 0 : -1;
}

extern "C" int dcc_clang_inputs(char **argv, char ***normalized, char ***files) {
    FrontendPlan plan(argv);
    if (!plan.compiler) return -1;
    std::vector<const char *> raw;
    for (unsigned i = 1; argv[i]; ++i) raw.push_back(argv[i]);
    std::set<std::string> used;
    for (const auto &input : plan.compiler->getFrontendOpts().Inputs)
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
    auto profile_path = plan.compiler->getCodeGenOpts().ProfileInstrumentUsePath;
    std::vector<std::string> result, inputs;
    for (unsigned i = 0; argv[i]; ++i) {
        if (unused.count(i)) continue;
        if (profile && !profile_path.empty() && i == profile->getIndex() + 1)
            result.emplace_back("-fprofile-instr-use=" + absolute(profile_path));
        else result.emplace_back(argv[i]);
    }
    bool valid = true;
    dcc_clang_profile_inputs(plan.compiler->getCodeGenOpts(), [&](std::string &path) {
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
