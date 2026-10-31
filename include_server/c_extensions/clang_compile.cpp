/* Preserve the client's source identity during code generation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/ScopeExit.h>
#include "../../src/clang_inputs.h"
#include "remote_files.h"
#include "clang_compiler.h"
#include "compiler_command.h"
#ifdef DCC_CLANG_COMPILE_MODULE
#include "clang_codegen.h"
#endif

namespace {
const dcc_file_cache_api *file_cache;
const dcc_argv_file_api *command_api;
// The driver sees the existing worker paths. Only the frontend uses the
// client's main-file identity, including quoted-include lookup from its parent.
// File contents and metadata remain owned by the physical filesystem.
class ClientFiles : public llvm::vfs::ProxyFileSystem {
    std::string root, cwd, original, physical;
    bool pump;
public:
    ClientFiles(const char *root, const char *cwd, bool pump)
        : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()), root(root), cwd(cwd), pump(pump) {}
    void activate(llvm::StringRef name, llvm::StringRef input) {
        original = name.str();
        physical = input.str();
    }
    std::string translate(const llvm::Twine &path) const {
        auto name = path.str();
        if (!original.empty() && name == original) return physical;
        // FileManager resolves the main file's directory before opening it.
        // A preprocessed job carries only the source image, not that tree.
        if (!pump && !original.empty() &&
            name == llvm::sys::path::parent_path(original))
            return llvm::sys::path::parent_path(physical).str();
        if (!root.empty() && name != root && !llvm::StringRef(name).starts_with(root + "/")) {
            std::string absolute = llvm::sys::path::is_absolute(name) ? name : cwd + "/" + name;
            llvm::SmallString<256> normalized(absolute);
            llvm::sys::path::remove_dots(normalized, true);
            auto mapped = root + normalized.str().str();
            // The driver also reads its installed toolchain configuration.
            // Pump frontend lookup is restricted to the client's input tree.
            if ((pump && !original.empty()) || getUnderlyingFS().exists(mapped)) return mapped;
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
void dcc_clang_compile_init() {
    llvm::InitializeAllTargetInfos(); llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs(); llvm::InitializeAllAsmParsers(); llvm::InitializeAllAsmPrinters();
}

// Called in a dedicated compiler child, never in the long-lived include server.
PyObject *dcc_clang_compile(PyObject *, PyObject *arguments) {
    PyObject *argv;
    const char *original, *root, *cwd;
    int pump;
    if (!PyArg_ParseTuple(arguments, "Osssp", &argv, &original, &root, &cwd, &pump)) return nullptr;
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
    const std::string invoked_as = command.front();
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    clang::tooling::addTargetAndModeForProgramName(command, invoked_as);
    {
        llvm::IntrusiveRefCntPtr<ClientFiles> files(new ClientFiles(root, cwd, pump));
        llvm::IntrusiveRefCntPtr<clang::FileManager> manager(
            new clang::FileManager(clang::FileSystemOptions(), files));
        Compile action(*files, original, cwd);
        clang::tooling::ToolInvocation invocation(command, &action, manager.get(),
            std::make_shared<clang::PCHContainerOperations>());
        ok = invocation.run();
    }
    llvm::outs().flush(); llvm::errs().flush();
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    return PyLong_FromLong(ok ? 0 : 1);
}

static int compile_file(int request, int fd) {
    CompilerCommand command(*command_api);
    if (!command.read(request, 1)) {
        llvm::errs() << "distcc: invalid complete compiler command file\n";
        return 1;
    }
    llvm::SmallVector<const char *> argv;
    for (char **p = command[0]; *p; ++p) argv.push_back(*p);
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
    compiler->createDiagnostics();
    if (clang::CompilerInvocation::CreateFromArgs(*invocation, args, compiler->getDiagnostics())) {
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
        llvm::IntrusiveRefCntPtr<RemoteFiles> files(new RemoteFiles(dup(fd), cwd, *file_cache, client_identity,
            llvm::sys::path::parent_path(root).str()));
        compiler->setFileManager(new clang::FileManager(invocation->getFileSystemOpts(), files));
        compiler->createSourceManager();
#ifdef DCC_CLANG_COMPILE_MODULE
        auto generated = dcc_clang_codegen(*compiler, [&](unsigned count) { return files->codegen_cpus(count); });
        ok = generated ? *generated : clang::ExecuteCompilerInvocation(compiler.get());
#else
        ok = clang::ExecuteCompilerInvocation(compiler.get());
#endif
    }
    llvm::outs().flush(); llvm::errs().flush();
    return ok ? 0 : 1;
}

#ifdef DCC_CLANG_COMPILE_MODULE
#include "native_worker.h"
namespace {
PyObject *compile_request(PyObject *, PyObject *arguments) {
    int request, fd;
    if (!PyArg_ParseTuple(arguments, "ii", &request, &fd)) return nullptr;
    int status;
    Py_BEGIN_ALLOW_THREADS
    status = compile_file(request, fd);
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    return PyLong_FromLong(status);
}
PyObject *serve(PyObject *, PyObject *arguments) {
    int descriptor;
    if (!PyArg_ParseTuple(arguments, "i", &descriptor)) return nullptr;
    int status;
    Py_BEGIN_ALLOW_THREADS
    status = dcc_native_worker(descriptor, [](int request, bool preparing) {
        return preparing ? 1 : compile_file(request, STDIN_FILENO);
    });
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    return PyLong_FromLong(status);
}
PyMethodDef methods[] = {
    {"compile", dcc_clang_compile, METH_VARARGS, "Compile with the client's source identity."},
    {"compile_file", compile_request, METH_VARARGS, "Consume a remote command file and filesystem channel."},
    {"serve", serve, METH_VARARGS, "Own descriptor dispatch and isolated compiler children."},
    {nullptr, nullptr, 0, nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "clang_compile", nullptr, -1,
                      methods, nullptr, nullptr, nullptr, nullptr};
}
PyMODINIT_FUNC PyInit_clang_compile() {
    command_api = CompilerCommand::load();
    if (!command_api) return nullptr;
    PyObject *cache_module = PyImport_ImportModule("include_server.distcc_pump_c_extensions");
    if (!cache_module) return nullptr;
    PyObject *capsule = PyObject_GetAttrString(cache_module, "file_cache");
    Py_DECREF(cache_module);
    if (!capsule) return nullptr;
    // This capsule exposes a static C function table, independent of Python
    // object lifetime. Resolve it once in the resident worker before forking.
    file_cache = static_cast<const dcc_file_cache_api *>(PyCapsule_GetPointer(capsule, "distcc.file_cache"));
    Py_DECREF(capsule);
    if (!file_cache) return nullptr;
    dcc_clang_compile_init();
    dcc_clang_codegen_init();
    return PyModule_Create(&module);
}
#endif
