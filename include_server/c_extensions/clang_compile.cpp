/* Preserve the client's source identity during code generation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>

namespace {
// The driver sees the existing worker paths. Only the frontend uses the
// client's main-file identity, including quoted-include lookup from its parent.
// File contents and metadata remain owned by the physical filesystem.
class ClientFiles : public llvm::vfs::ProxyFileSystem {
    std::string root, original, physical;
public:
    explicit ClientFiles(const char *root)
        : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()), root(root) {}
    void activate(llvm::StringRef name, llvm::StringRef input) {
        original = name.str();
        physical = input.str();
    }
    std::string translate(const llvm::Twine &path) const {
        auto name = path.str();
        if (original.empty()) return name;
        if (name == original) return physical;
        if (!root.empty() && llvm::sys::path::is_absolute(name) &&
            name != root && !llvm::StringRef(name).starts_with(root + "/"))
            return root + name;
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
    std::string original;
public:
    Compile(ClientFiles &files, const char *original) : files(files), original(original) {}
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
        clang::CompilerInstance compiler(invocation, std::move(pch));
        compiler.setFileManager(manager);
        compiler.createDiagnostics(diagnostics, false);
        compiler.createSourceManager();
        return clang::ExecuteCompilerInvocation(&compiler);
    }
};
}

// Called in a dedicated compiler child, never in the long-lived include server.
PyObject *dcc_clang_compile(PyObject *, PyObject *arguments) {
    PyObject *argv;
    const char *original, *root;
    if (!PyArg_ParseTuple(arguments, "Oss", &argv, &original, &root)) return nullptr;
    PyObject *sequence = PySequence_Fast(argv, "argv must be a sequence");
    if (!sequence) return nullptr;
    std::vector<std::string> command;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(sequence); ++i) {
        const char *value = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(sequence, i));
        if (!value) { Py_DECREF(sequence); return nullptr; }
        command.emplace_back(value);
    }
    Py_DECREF(sequence);
    if (command.empty() || !*original)
        return PyErr_Format(PyExc_ValueError, "missing compiler command or source identity");
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    {
        llvm::IntrusiveRefCntPtr<ClientFiles> files(new ClientFiles(root));
        llvm::IntrusiveRefCntPtr<clang::FileManager> manager(
            new clang::FileManager(clang::FileSystemOptions(), files));
        Compile action(*files, original);
        clang::tooling::ToolInvocation invocation(command, &action, manager.get(),
            std::make_shared<clang::PCHContainerOperations>());
        ok = invocation.run();
    }
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    return PyLong_FromLong(ok ? 0 : 1);
}
