/* Compiler-backed include discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <clang/Basic/Version.h>
#include <clang/Basic/DiagnosticLex.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/DependencyDirectivesScanner.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <sys/stat.h>
#include <map>
#include <set>

PyObject *dcc_clang_compile(PyObject *, PyObject *);

namespace {
using namespace clang;

struct Stamp {
    struct stat value{};
    bool exists;
    explicit Stamp(const std::string &path) : exists(::stat(path.c_str(), &value) == 0) {}
    bool operator==(const Stamp &other) const {
        return exists == other.exists && (!exists ||
            (value.st_dev == other.value.st_dev && value.st_ino == other.value.st_ino &&
             value.st_mode == other.value.st_mode &&
             (S_ISDIR(value.st_mode) || (value.st_size == other.value.st_size &&
             value.st_mtim.tv_sec == other.value.st_mtim.tv_sec &&
             value.st_mtim.tv_nsec == other.value.st_mtim.tv_nsec &&
             value.st_ctim.tv_sec == other.value.st_ctim.tv_sec &&
             value.st_ctim.tv_nsec == other.value.st_ctim.tv_nsec))));
    }
};

class SnapshotFile : public llvm::vfs::File {
    llvm::vfs::Status info;
    const std::string &bytes;
public:
    SnapshotFile(llvm::vfs::Status info, const std::string &bytes) : info(info), bytes(bytes) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return info; }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(
        const llvm::Twine &name, int64_t = -1, bool = true, bool = false) override {
        // Inputs owns these immutable, NUL-terminated strings until the
        // invocation and all of its SourceManager buffers have been destroyed.
        return llvm::MemoryBuffer::getMemBuffer(bytes, name.str());
    }
    std::error_code close() override { return {}; }
};

class Inputs : public llvm::vfs::ProxyFileSystem {
    std::string cwd;
    size_t bytes = 0;
public:
    std::map<std::string, Stamp> stamps;
    std::map<std::string, std::string> contents;
    bool changed = false;
    std::error_code cwd_error;
    explicit Inputs(const std::string &cwd)
        : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()), cwd(cwd) {
        cwd_error = setCurrentWorkingDirectory(cwd);
    }
    std::string path(const llvm::Twine &name) const {
        auto result = name.str();
        return !result.empty() && result[0] == '/' ? result : cwd + '/' + result;
    }
    void observe(const std::string &name) {
        auto it = stamps.find(name);
        if (it == stamps.end()) stamps.emplace(name, Stamp(name));
        else if (!(it->second == Stamp(name))) changed = true;
    }
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &name) override {
        observe(path(name));
        auto result = getUnderlyingFS().status(name);
        return result;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &name) override {
        auto key = path(name);
        observe(key);
        auto file = getUnderlyingFS().openFileForRead(name);
        if (!file) return file.getError();
        auto info = (*file)->status();
        if (!info) return info.getError();
        auto it = contents.find(key);
        if (it == contents.end()) {
            // Bound a single request independently of the surrounding build.
            if (info->getSize() > 64 * 1024 * 1024 - bytes)
                return std::make_error_code(std::errc::file_too_large);
            auto buffer = (*file)->getBuffer(name);
            if (!buffer) return buffer.getError();
            bytes += (*buffer)->getBufferSize();
            it = contents.emplace(key, (*buffer)->getBuffer().str()).first;
            observe(key);
        }
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*info, it->second));
    }
    bool validate() {
        for (const auto &entry : stamps)
            if (!(entry.second == Stamp(entry.first))) changed = true;
        return !changed;
    }
};

struct Result {
    bool unsupported = false;
    std::set<std::string> dependencies;
};

struct Callbacks : PPCallbacks {
    SourceManager &sources;
    Result &result;
    Callbacks(SourceManager &sources, Result &result)
        : sources(sources), result(result) {}
    void FileChanged(SourceLocation loc, FileChangeReason reason,
                     SrcMgr::CharacteristicKind, FileID) override {
        if (reason != EnterFile) return;
        if (auto file = sources.getFileEntryRefForID(sources.getFileID(loc)))
            result.dependencies.insert(file->getName().str());
    }
    void HasInclude(SourceLocation, llvm::StringRef, bool, OptionalFileEntryRef file,
                    SrcMgr::CharacteristicKind) override {
        if (file) result.dependencies.insert(file->getName().str());
    }
    void InclusionDirective(SourceLocation, const Token &, llvm::StringRef,
                            bool, CharSourceRange, OptionalFileEntryRef file,
                            llvm::StringRef, llvm::StringRef, const Module *, bool imported,
                            SrcMgr::CharacteristicKind) override {
        if (file) result.dependencies.insert(file->getName().str());
        if (imported) result.unsupported = true;
    }
    void EmbedDirective(SourceLocation, llvm::StringRef, bool, OptionalFileEntryRef,
                        const LexEmbedParametersResult &) override { result.unsupported = true; }
};

struct Action : PreprocessOnlyAction {
    Result &result;
    explicit Action(Result &result) : result(result) {}
    bool BeginSourceFileAction(CompilerInstance &instance) override {
        instance.getPreprocessor().addPPCallbacks(
            std::make_unique<Callbacks>(instance.getSourceManager(), result));
        return true;
    }
};

struct Diagnostics : DiagnosticConsumer {
    std::string error;
    bool unsafe = false;
    void HandleDiagnostic(DiagnosticsEngine::Level level, const Diagnostic &info) override {
        if (level >= DiagnosticsEngine::Error && info.getID() != diag::err_pp_hash_error)
            unsafe = true;
        if (level >= DiagnosticsEngine::Error && error.empty()) {
            llvm::SmallString<256> text;
            info.FormatDiagnostic(text);
            error = text.str();
        }
    }
};

bool requiresModules(llvm::StringRef source) {
    using namespace dependency_directives_scan;
    llvm::SmallVector<dependency_directives_scan::Token, 64> tokens;
    llvm::SmallVector<Directive, 16> directives;
    if (scanSourceForDependencyDirectives(source, tokens, directives)) return true;
    for (const auto &directive : directives)
        if (directive.Kind == decl_at_import || directive.Kind == cxx_module_decl ||
            directive.Kind == cxx_import_decl || directive.Kind == cxx_export_module_decl ||
            directive.Kind == cxx_export_import_decl) return true;
    return false;
}

PyObject *scan(PyObject *, PyObject *arguments) {
    PyObject *argv;
    const char *cwd;
    if (!PyArg_ParseTuple(arguments, "Os", &argv, &cwd)) return nullptr;
    PyObject *sequence = PySequence_Fast(argv, "argv must be a sequence");
    if (!sequence) return nullptr;
    std::vector<std::string> command;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(sequence); ++i) {
        const char *value = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(sequence, i));
        if (!value) { Py_DECREF(sequence); return nullptr; }
        command.emplace_back(value);
    }
    Py_DECREF(sequence);
    if (command.empty()) return PyErr_Format(PyExc_ValueError, "empty compiler command");
    const std::string invoked_as = command.front();
    tooling::addTargetAndModeForProgramName(command, invoked_as);
    llvm::IntrusiveRefCntPtr<Inputs> inputs(new Inputs(cwd));
    if (inputs->cwd_error)
        return PyErr_Format(PyExc_ValueError, "invalid working directory: %s", inputs->cwd_error.message().c_str());
    Result result;
    Diagnostics diagnostics;
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    {
        // Use the canonical preprocessor so its callbacks also cover probes,
        // unsupported directives and diagnostics. No AST or code generation.
        // FileManager is request-scoped: stale negative lookups cannot survive.
        FileSystemOptions options;
        options.WorkingDir = cwd;
        llvm::IntrusiveRefCntPtr<FileManager> manager(new FileManager(options, inputs));
        tooling::ToolInvocation invocation(command, std::make_unique<Action>(result), manager.get());
        invocation.setDiagnosticConsumer(&diagnostics);
        invocation.run();
        ok = !diagnostics.unsafe && !result.dependencies.empty();
        // Probes and guarded aliases can resolve without opening the file.
        // Preserve those names and bytes so the worker can repeat every lookup.
        for (const auto &path : result.dependencies)
            if (!inputs->contents.count(inputs->path(path)) &&
                !inputs->openFileForRead(path)) ok = false;
        for (const auto &path : result.dependencies) {
            auto it = inputs->contents.find(inputs->path(path));
            if (it != inputs->contents.end() && requiresModules(it->second))
                result.unsupported = true;
        }
        ok = inputs->validate() && ok && !result.unsupported;
    }
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    if (!ok) return PyErr_Format(PyExc_RuntimeError, "Clang dependency scan: %s",
        inputs->changed ? "input changed" : result.unsupported ? "unsupported preprocessor input" :
        diagnostics.error.empty() ? "unreadable input" : diagnostics.error.c_str());
    PyObject *files = PyDict_New(), *paths = PyDict_New(), *deps = PyList_New(0);
    if (!files || !paths || !deps) { Py_XDECREF(files); Py_XDECREF(paths); Py_XDECREF(deps); return nullptr; }
    bool success = true;
    for (const auto &entry : inputs->contents) {
        PyObject *key = PyUnicode_DecodeFSDefault(entry.first.c_str());
        PyObject *data = PyBytes_FromStringAndSize(entry.second.data(), entry.second.size());
        if (!key || !data || PyDict_SetItem(files, key, data) < 0) success = false;
        Py_XDECREF(key); Py_XDECREF(data);
        if (!success) break;
    }
    auto append = [&success](PyObject *list, const std::string &name) {
        if (!success) return;
        PyObject *value = PyUnicode_DecodeFSDefault(name.c_str());
        if (!value || PyList_Append(list, value) < 0) success = false;
        Py_XDECREF(value);
    };
    for (const auto &entry : inputs->stamps) {
        if (!success) break;
        const auto &st = entry.second.value;
        PyObject *key = PyUnicode_DecodeFSDefault(entry.first.c_str());
        // A directory lookup observes identity, not unrelated build outputs.
        // Negative file lookups still detect newly generated/shadowing headers.
        PyObject *value = !entry.second.exists ? Py_NewRef(Py_None) :
            S_ISDIR(st.st_mode) ? Py_BuildValue("(KKK)",
            (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
            (unsigned long long)st.st_mode) : Py_BuildValue("(KKKKLL)",
            (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
            (unsigned long long)st.st_mode, (unsigned long long)st.st_size,
            (long long)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec,
            (long long)st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec);
        if (!key || !value || PyDict_SetItem(paths, key, value) < 0) success = false;
        Py_XDECREF(key); Py_XDECREF(value);
    }
    for (const auto &entry : result.dependencies) append(deps, entry);
    if (!success) { Py_DECREF(files); Py_DECREF(paths); Py_DECREF(deps); return nullptr; }
    return Py_BuildValue("{s:N,s:N,s:N}", "files", files, "lookups", paths, "dependencies", deps);
}

PyMethodDef methods[] = {
    {"compile", dcc_clang_compile, METH_VARARGS, "Compile once with the client's source identity."},
    {"scan", scan, METH_VARARGS, "Scan using libclang-cpp; return immutable inputs and lookup paths."},
    {nullptr, nullptr, 0, nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "clang_scan", nullptr, -1, methods, nullptr, nullptr, nullptr, nullptr};
}

PyMODINIT_FUNC PyInit_clang_scan() {
    // Program-name target inference consults LLVM's registered targets,
    // including on the first scan in a fresh include-server process.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    PyObject *result = PyModule_Create(&module);
    if (result) {
        PyModule_AddStringConstant(result, "version", CLANG_VERSION_STRING);
        PyModule_AddStringConstant(result, "gcc_scan_dir", DCC_GCC_SCAN_DIR);
    }
    return result;
}
