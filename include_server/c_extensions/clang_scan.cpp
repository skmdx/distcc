/* Compiler-backed include discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <cerrno>
#include <optional>
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
#include <llvm/Support/CommandLine.h>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <set>

PyObject *dcc_clang_compile(PyObject *, PyObject *);

namespace {
using namespace clang;

struct Stamp {
    struct stat value{};
    bool exists;
    int error;
    explicit Stamp(const std::string &path) : exists(::stat(path.c_str(), &value) == 0),
                                            error(exists ? 0 : errno) {}
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

bool requiresModules(llvm::StringRef source);

struct Image {
    Stamp stamp;
    llvm::vfs::Status info;
    std::string bytes;
    mutable std::optional<bool> modules;
    Image(const Stamp &stamp, llvm::vfs::Status info, std::string bytes)
        : stamp(stamp), info(info), bytes(std::move(bytes)) {}
    bool hasModules() const {
        if (!modules) modules = requiresModules(bytes);
        return *modules;
    }
};
using Images = std::map<std::string, std::shared_ptr<const Image>>;
// Retain only the previous request's bytes, not its lookup results. Each scan
// rechecks identities and negative lookups; threads do not share mutable state.
thread_local Images previous_inputs;

// Python views retain their immutable image independently of the next scan's
// cache. The native cache owns no Python objects and can die without the GIL.
struct Snapshot {
    PyObject_HEAD
    std::shared_ptr<const Image> image;
};
PyTypeObject *snapshot_type;
int snapshot_buffer(PyObject *object, Py_buffer *view, int flags) {
    const auto &bytes = reinterpret_cast<Snapshot *>(object)->image->bytes;
    return PyBuffer_FillInfo(view, object, const_cast<char *>(bytes.data()), bytes.size(), 1, flags);
}
void snapshot_free(PyObject *object) {
    auto *type = Py_TYPE(object);
    reinterpret_cast<Snapshot *>(object)->image.~shared_ptr();
    type->tp_free(object);
    Py_DECREF(type);
}
PyObject *snapshot_view(const std::shared_ptr<const Image> &image) {
    auto *snapshot = reinterpret_cast<Snapshot *>(snapshot_type->tp_alloc(snapshot_type, 0));
    if (!snapshot) return nullptr;
    new (&snapshot->image) std::shared_ptr<const Image>(image);
    PyObject *view = PyMemoryView_FromObject(reinterpret_cast<PyObject *>(snapshot));
    Py_DECREF(snapshot);
    return view;
}

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
public:
    std::map<std::string, Stamp> stamps;
    Images contents;
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
    const Stamp &observe(const std::string &name) {
        // One observation per spelling and one validation at publication.
        // Rechecking at every FileManager status/open adds syscalls without
        // strengthening the final snapshot consistency check.
        return stamps.try_emplace(name, name).first->second;
    }
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &name) override {
        auto key = path(name);
        const auto &stamp = observe(key);
        if (!stamp.exists) return std::error_code(stamp.error, std::generic_category());
        auto previous = previous_inputs.find(key);
        if (previous != previous_inputs.end() && stamp == previous->second->stamp)
            return llvm::vfs::Status::copyWithNewName(previous->second->info, name);
        return getUnderlyingFS().status(name);
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &name) override {
        auto key = path(name);
        observe(key);
        auto it = contents.find(key);
        if (it == contents.end()) {
            auto previous = previous_inputs.find(key);
            std::shared_ptr<const Image> image;
            if (previous != previous_inputs.end() && stamps.at(key) == previous->second->stamp)
                image = previous->second;
            if (!image) {
                auto file = getUnderlyingFS().openFileForRead(name);
                if (!file) return file.getError();
                auto info = (*file)->status();
                if (!info) return info.getError();
                auto buffer = (*file)->getBuffer(name);
                if (!buffer) return buffer.getError();
                image = std::make_shared<Image>(stamps.at(key), *info, (*buffer)->getBuffer().str());
            }
            it = contents.emplace(key, std::move(image)).first;
        }
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(it->second->info, it->second->bytes));
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
            if (it != inputs->contents.end() && it->second->hasModules())
                result.unsupported = true;
        }
        ok = inputs->validate() && ok && !result.unsupported;
    }
    Py_END_ALLOW_THREADS
    previous_inputs = ok ? inputs->contents : Images{};
    if (PyErr_CheckSignals() < 0) return nullptr;
    if (!ok) return PyErr_Format(PyExc_RuntimeError, "Clang dependency scan: %s",
        inputs->changed ? "input changed" : result.unsupported ? "unsupported preprocessor input" :
        diagnostics.error.empty() ? "unreadable input" : diagnostics.error.c_str());
    PyObject *files = PyDict_New(), *paths = PyDict_New(), *deps = PyList_New(0);
    if (!files || !paths || !deps) { Py_XDECREF(files); Py_XDECREF(paths); Py_XDECREF(deps); return nullptr; }
    bool success = true;
    // Clang also opens code-generation data such as indexed PGO profiles.
    // Their transfer is owned by the job's auxiliary-input handling; only
    // preprocessor dependencies need Python snapshots or module scanning.
    for (const auto &path : result.dependencies) {
        const auto &entry = *inputs->contents.find(inputs->path(path));
        PyObject *key = PyUnicode_DecodeFSDefault(entry.first.c_str());
        PyObject *data = snapshot_view(entry.second);
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

PyObject *tokenize_command(PyObject *, PyObject *args) {
    const char *command;
    if (!PyArg_ParseTuple(args, "s", &command)) return nullptr;
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char *, 32> words;
    llvm::cl::TokenizeGNUCommandLine(command, saver, words);
    PyObject *result = PyList_New(words.size());
    if (!result) return nullptr;
    for (size_t i = 0; i < words.size(); ++i) {
        PyObject *word = PyUnicode_DecodeFSDefault(words[i]);
        if (!word) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, i, word);
    }
    return result;
}

PyMethodDef methods[] = {
    {"tokenize_command", tokenize_command, METH_VARARGS, "Decode Clang driver diagnostics with LLVM's argument parser."},
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
        PyType_Slot slots[] = {
            {Py_bf_getbuffer, reinterpret_cast<void *>(snapshot_buffer)},
            {Py_tp_dealloc, reinterpret_cast<void *>(snapshot_free)},
            {0, nullptr}};
        PyType_Spec spec = {"clang_scan._Snapshot", sizeof(Snapshot), 0,
                           Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, slots};
        PyObject *type = PyType_FromSpec(&spec);
        if (!type || PyModule_AddObjectRef(result, "_Snapshot", type) < 0) {
            Py_XDECREF(type);
            Py_DECREF(result);
            return nullptr;
        }
        snapshot_type = reinterpret_cast<PyTypeObject *>(type);
        Py_DECREF(type);
        PyModule_AddStringConstant(result, "version", CLANG_VERSION_STRING);
        PyModule_AddStringConstant(result, "gcc_scan_dir", DCC_GCC_SCAN_DIR);
    }
    return result;
}
