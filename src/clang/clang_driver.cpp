/* Compiler-owned argument classification and auxiliary inputs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Options/Options.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Tool.h>
#include <clang/Driver/ToolChain.h>
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
#include "frontend_identity.h"
#include "plan_memo.h"

namespace {
// Own the driver's plan while callers borrow its cc1 arguments. Parsing the
// invocation supplies input roots and auxiliary files; it must not regenerate
// or otherwise change the command the actual compiler selected.
class PlanFiles : public llvm::vfs::ProxyFileSystem {
    int fd;
    bool templated;
    size_t bytes = 4;
    std::set<std::string> records;
    void observe(unsigned op, const llvm::Twine &path, std::error_code error, llvm::StringRef target = {}) {
        llvm::SmallString<256> name;
        path.toVector(name);
        // A derived path consulted by the driver is an input to planning.
        if (templated && name.str().contains(DCC_PLAN_MARKER)) dynamic_path = true;
        if (fd < 0) return;
        if (getUnderlyingFS().makeAbsolute(name)) { fd = -1; return; }
        dcc_plan_dependency item{};
        item.operation = op;
        item.error = error.value();
        if (!error && stat(name.c_str(), &item.metadata)) { fd = -1; return; }
        item.length = name.size() + 1;
        item.target_length = target.empty() ? 0 : target.size() + 1;
        std::string entry(reinterpret_cast<const char *>(&item), sizeof(item));
        entry.append(name.c_str(), item.length);
        if (item.target_length) { entry.append(target.data(), target.size()); entry += '\0'; }
        if (!records.insert(entry).second) return;
        bytes += entry.size();
        if (bytes > DCC_PLAN_JOURNAL_LIMIT || write(fd, entry.data(), entry.size()) != (ssize_t)entry.size()) fd = -1;
    }
public:
    bool dynamic_path = false;
    PlanFiles(int journal, bool templated) : ProxyFileSystem(llvm::vfs::getRealFileSystem()), fd(journal), templated(templated) {}
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &p) override {
        auto value = getUnderlyingFS().status(p);
        observe(DCC_PLAN_STAT, p, value.getError());
        return value;
    }
    bool exists(const llvm::Twine &p) override { return bool(status(p)); }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &p) override {
        auto value = getUnderlyingFS().openFileForRead(p);
        observe(DCC_PLAN_OPEN, p, value.getError());
        return value;
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &p, std::error_code &ec) override {
        auto value = getUnderlyingFS().dir_begin(p, ec);
        observe(DCC_PLAN_DIRECTORY, p, ec);
        return value;
    }
    std::error_code getRealPath(const llvm::Twine &p, llvm::SmallVectorImpl<char> &out) override {
        auto ec = getUnderlyingFS().getRealPath(p, out);
        observe(DCC_PLAN_REALPATH, p, ec, {out.data(), out.size()});
        return ec;
    }
    void finish() {
        const unsigned char done[] = {1, 1, 0, 0};
        if (fd >= 0 && pwrite(fd, done, sizeof(done), 0) != sizeof(done)) fd = -1;
    }
};

class FrontendPlan {
    std::vector<std::string> command;
    llvm::SmallVector<const char *> argv;
    std::unique_ptr<clang::DiagnosticOptions> diagnostic_options;
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics;
    std::unique_ptr<clang::driver::Driver> driver;
public:
    std::unique_ptr<clang::driver::Compilation> compilation;
    llvm::ArrayRef<const char *> cc1;
    std::unique_ptr<clang::CompilerInvocation> compiler;
    explicit FrontendPlan(char **input, bool frontend = true,
                          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> files = llvm::vfs::getRealFileSystem(), bool check_inputs = true) {
        for (char **p = input; *p; ++p) command.emplace_back(*p);
        if (command.empty()) return;
        const auto invoked_as = command.front();
        clang::tooling::addTargetAndModeForProgramName(command, invoked_as);
        auto executable = llvm::sys::findProgramByName(command.front());
        if (executable) command.front() = *executable;
        for (const auto &arg : command) argv.push_back(arg.c_str());
        diagnostic_options = clang::CreateAndPopulateDiagOpts(argv);
        clang::TextDiagnosticPrinter *printer = nullptr;
        if (!frontend) {
            printer = new clang::TextDiagnosticPrinter(llvm::errs(), *diagnostic_options);
            printer->setPrefix(llvm::sys::path::filename(argv[0]).str());
        }
        diagnostics = clang::CompilerInstance::createDiagnostics(
            *files, *diagnostic_options, printer);
        if (argv.size() >= 2 && llvm::StringRef(argv[1]) == "-cc1") {
            cc1 = llvm::ArrayRef(argv).drop_front();
        } else {
            driver = std::make_unique<clang::driver::Driver>(argv[0],
                llvm::sys::getDefaultTargetTriple(), *diagnostics, "clang LLVM compiler", files);
            driver->setCheckInputsExist(check_inputs);
            compilation.reset(driver->BuildCompilation(argv));
            if (!compilation || compilation->containsError() || diagnostics->hasErrorOccurred()) return;
            if (!frontend) return;
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
    llvm::opt::InputArgList parseArguments(llvm::ArrayRef<const char *> raw, bool &error) const {
        if (driver) return driver->ParseArgStrings(raw, true, error);
        unsigned index = 0, count = 0;
        auto args = clang::getDriverOptTable().ParseArgs(
            raw, index, count, llvm::opt::Visibility(clang::options::CC1Option));
        error = count != 0;
        return args;
    }
    bool valid() const {
        return compilation && !compilation->containsError() && !diagnostics->hasErrorOccurred();
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

extern "C" int dcc_clang_frontend(char **argv, char ***cc1, char ***roots, int journal, bool templated) {
    *cc1 = *roots = nullptr;
    llvm::IntrusiveRefCntPtr<PlanFiles> files(new PlanFiles(journal, templated));
    FrontendPlan plan(argv, true, files, !templated);
    files->finish();
    if (files->dynamic_path || !plan.compiler) return -1;
    auto &invocation = *plan.compiler;
    const auto &inputs = invocation.getFrontendOpts().Inputs;
    if (inputs.size() != 1 || !inputs[0].isFile()) return -1;
    std::unique_ptr<char, decltype(&free)> credentials(dcc_frontend_identity(), free);
    if (!credentials) return -1;
    std::string identity(credentials.get());
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

/* An empty command denotes a valid local driver job. Clang owns linker
 * selection, including config files, response files and toolchain defaults. */
extern "C" int dcc_clang_link_plan(char **argv, char ***command) {
    *command = nullptr;
    FrontendPlan plan(argv, false);
    if (!plan.valid()) return 1;
    std::vector<std::string> result;
    auto &jobs = plan.compilation->getJobs();
    if (jobs.size() == 1) {
        const auto &job = *jobs.begin();
        // GetLinkerPath's LLD flag is false for explicit paths, including a
        // perfectly usable LLD from another installation. Only a positively
        // identified BFD job bypasses the link coordinator. Other commands
        // retain the coordinator's existing capability policy.
        auto bfd = job.getCreator().getToolChain().GetProgramPath("ld.bfd");
        if (!llvm::sys::fs::equivalent(job.getExecutable(), bfd)) {
            result.emplace_back(job.getExecutable());
            for (const char *arg : job.getArguments()) result.emplace_back(arg);
            if (plan.compilation->getArgs().hasArg(clang::options::OPT_v))
                job.Print(llvm::errs(), "\n", false);
        }
    }
    *command = copy(result);
    return *command ? 0 : 1;
}

extern "C" int dcc_clang_inputs(char **argv, char ***normalized, char ***files) {
    FrontendPlan plan(argv);
    if (!plan.compiler) return -1;
    std::vector<const char *> raw;
    for (unsigned i = 1; argv[i]; ++i) raw.push_back(argv[i]);
    std::set<std::string> used;
    for (const auto &input : plan.compiler->getFrontendOpts().Inputs)
        if (input.isFile()) used.insert(absolute(input.getFile()));
    // A source-and-link invocation also consumes objects and archives in
    // later driver jobs. Only inputs unused by the entire plan may be removed.
    if (plan.compilation)
        for (const auto &job : plan.compilation->getJobs())
            for (const auto &input : job.getInputInfos())
                if (input.isFilename()) used.insert(absolute(input.getFilename()));
    // The shared table also contains other driver dialects: DXC's -E takes
    // an entry-point operand, whereas Clang's -E requests preprocessing.
    // Use the same dialect as the driver that selected the actual inputs.
    bool error = false;
    auto args = plan.parseArguments(raw, error);
    if (error) return -1;
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
