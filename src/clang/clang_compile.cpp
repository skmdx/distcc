/* Preserve the client's source identity during code generation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../native_worker.h"
#include <cstdlib>
#include <sys/prctl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/ScopeExit.h>
#include "clang_inputs.h"
#include "remote_files.h"
#include "clang_compiler.h"
#include "compiler_command.h"
#include "clang_codegen.h"
#include "lto_backend.h"
#include "clang_result.h"

namespace {
// The driver sees the existing worker paths. Only the frontend uses the
// client's main-file identity, including quoted-include lookup from its parent.
// File contents and metadata remain owned by the physical filesystem.
class ClientFiles : public llvm::vfs::ProxyFileSystem {
    std::string root, cwd, original, physical;
public:
    ClientFiles(const char *root, const char *cwd)
        : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()), root(root), cwd(cwd) {}
    void activate(llvm::StringRef name, llvm::StringRef input) {
        original = name.str();
        physical = input.str();
    }
    std::string translate(const llvm::Twine &path) const {
        auto name = path.str();
        if (!original.empty() && name == original) return physical;
        // FileManager resolves the main file's directory before opening it.
        // A preprocessed job carries only the source image, not that tree.
        if (!original.empty() &&
            name == llvm::sys::path::parent_path(original))
            return llvm::sys::path::parent_path(physical).str();
        if (!root.empty() && name != root && !llvm::StringRef(name).starts_with(root + "/")) {
            std::string absolute = llvm::sys::path::is_absolute(name) ? name : cwd + "/" + name;
            llvm::SmallString<256> normalized(absolute);
            llvm::sys::path::remove_dots(normalized, true);
            auto mapped = root + normalized.str().str();
            // The driver also reads its installed toolchain configuration.
            if (getUnderlyingFS().exists(mapped)) return mapped;
        }
        return name;
    }
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &path) override {
        auto info = getUnderlyingFS().status(translate(path));
        if (!info) return info.getError();
        return llvm::vfs::Status::copyWithNewName(*info, path);
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(
        const llvm::Twine &path) override {
        return llvm::vfs::File::getWithPath(
            getUnderlyingFS().openFileForRead(translate(path)), path);
    }
    std::error_code getRealPath(const llvm::Twine &path,
                               llvm::SmallVectorImpl<char> &output) override {
        auto error = getUnderlyingFS().getRealPath(translate(path), output);
        if (!error && !original.empty() && !root.empty() &&
            llvm::StringRef(output.data(), output.size()).starts_with(root + "/"))
            output.erase(output.begin(), output.begin() + root.size());
        return error;
    }
};

class Compile : public clang::tooling::ToolAction {
    ClientFiles &files;
    std::string original, cwd;
public:
    Compile(ClientFiles &files, const char *original, const char *cwd)
        : files(files), original(original), cwd(cwd) {}
    bool runInvocation(std::shared_ptr<clang::CompilerInvocation> invocation,
                       clang::FileManager *manager,
                       std::shared_ptr<clang::PCHContainerOperations> pch,
                       clang::DiagnosticConsumer *diagnostics) override {
        auto &inputs = invocation->getFrontendOpts().Inputs;
        if (inputs.size() != 1 || !inputs[0].isFile()) return false;
        files.activate(original, inputs[0].getFile());
        inputs[0] = clang::FrontendInputFile(original, inputs[0].getKind(),
                                             inputs[0].isSystem());
        invocation->getCodeGenOpts().MainFileName =
            llvm::sys::path::filename(original).str();
        dcc_clang_profile_inputs(invocation->getCodeGenOpts(), [&](std::string &path) {
            if (!llvm::sys::path::is_absolute(path)) path = cwd + "/" + path;
            llvm::SmallString<256> normalized(path);
            llvm::sys::path::remove_dots(normalized, true);
            path = files.translate(normalized);
        });
        DccChildCompiler compiler(new clang::CompilerInstance(invocation, std::move(pch)));
        compiler->setFileManager(manager);
        compiler->createDiagnostics(diagnostics, false);
        compiler->createSourceManager();
        return clang::ExecuteCompilerInvocation(compiler.get());
    }
};
}

// Import-time initialization is shared by the worker's forked jobs. Repeating
// target registration in every child also dirties otherwise shared pages.
static void dcc_clang_compile_init() {
    llvm::InitializeAllTargetInfos(); llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs(); llvm::InitializeAllAsmParsers(); llvm::InitializeAllAsmPrinters();
}

static int preprocessed(std::vector<std::string> command, const char *original,
                        const char *root, const char *cwd) {
    const std::string invoked_as = command.front();
    bool ok;
    clang::tooling::addTargetAndModeForProgramName(command, invoked_as);
    {
        llvm::IntrusiveRefCntPtr<ClientFiles> files(new ClientFiles(root, cwd));
        llvm::IntrusiveRefCntPtr<clang::FileManager> manager(
            new clang::FileManager(clang::FileSystemOptions(), files));
        Compile action(*files, original, cwd);
        clang::tooling::ToolInvocation invocation(command, &action, manager.get(),
            std::make_shared<clang::PCHContainerOperations>());
        ok = invocation.run();
    }
    llvm::outs().flush(); llvm::errs().flush();
    return ok ? 0 : 1;
}

static int remote(llvm::ArrayRef<const char *> argv, int fd) {
    if (argv.size() < 11 || llvm::StringRef(argv[4]) != "remote") {
        llvm::errs() << "distcc: invalid remote compiler command\n";
        return 1;
    }
    const char *root = argv[1], *output = argv[2], *cwd = argv[3],
               *dependencies = argv[6], *client_identity = argv[9];
    if (setenv("TMPDIR", root, 1)) { llvm::errs() << "distcc: " << strerror(errno) << '\n'; return 1; }
    auto args = llvm::ArrayRef(argv).drop_front(10);
    // Arguments borrow the decoded command file, owned until compilation ends.
    bool ok = false;
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    DccChildCompiler compiler(new clang::CompilerInstance(invocation, std::make_shared<clang::PCHContainerOperations>()));
    if (dcc_parse_compiler_invocation(*compiler, args)) {
        invocation->getFrontendOpts().OutputFile = output;
        auto &deps = invocation->getDependencyOutputOpts();
        if (!deps.OutputFile.empty()) deps.OutputFile = dependencies;
        // LLVM profile readers use physical paths outside the frontend VFS.
        dcc_clang_profile_inputs(invocation->getCodeGenOpts(), [&](std::string &path) {
            if (!llvm::sys::path::is_absolute(path)) path = std::string(cwd) + '/' + path;
            llvm::SmallString<256> normalized(path);
            llvm::sys::path::remove_dots(normalized, true);
            path = std::string(root) + normalized.str().str();
        });
        llvm::IntrusiveRefCntPtr<RemoteFiles> files(new RemoteFiles(dup(fd), cwd, dcc_file_cache_records, client_identity,
            llvm::sys::path::parent_path(root).str()));
        files->prepare(*invocation);
        auto finish = llvm::scope_exit([&] { files->finish(); });
        compiler->setFileManager(new clang::FileManager(invocation->getFileSystemOpts(), files));
        compiler->createSourceManager();
        dcc_result::Result result(*files, *compiler, cwd);
        auto generated = dcc_clang_codegen(*compiler, [&](unsigned count) { return files->codegen_cpus(count); },
                                           &result);
        ok = generated ? *generated : clang::ExecuteCompilerInvocation(compiler.get());
    }
    llvm::outs().flush(); llvm::errs().flush();
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    dcc_clang_compile_init();
    dcc_clang_codegen_init();
    if (argc == 3 && !strcmp(argv[1], "--lto-worker")) {
        if (prctl(PR_SET_DUMPABLE, 0) || !dcc_lto_init(false)) return 1;
        return dcc_native_worker(atoi(argv[2]), dcc_lto_remote);
    }
    if (argc == 4 && !strcmp(argv[1], "--native-worker")) {
        if (!dcc_lto_init(true)) return 1;
        return dcc_clang_native_worker(atoi(argv[2]));
    }
    if (argc == 4 && !strcmp(argv[1], "--worker"))
        return dcc_native_worker(atoi(argv[2]), [](int request, unsigned operation) {
            CompilerCommand command;
            if (operation != 'C' || !command.read(request, 1)) return 1;
            llvm::SmallVector<const char *> args;
            for (char **p = command[0]; *p; ++p) args.push_back(*p);
            return remote(args, STDIN_FILENO);
        });
    if (argc >= 6 && !strcmp(argv[4], "preprocessed"))
        return preprocessed({argv + 5, argv + argc}, argv[2], argv[1], argv[3]);
    if (argc >= 11 && !strcmp(argv[4], "remote"))
        return remote({argv, static_cast<size_t>(argc)}, atoi(argv[5]));
    return 1;
}
