/* Compiler-backed include discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <cerrno>
#include <optional>
#include <clang/Basic/Version.h>
#include <clang/Basic/DiagnosticLex.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/PrecompiledPreamble.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Basic/CodeGenOptions.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Serialization/ASTWriter.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/DependencyDirectivesScanner.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <set>
#include <algorithm>

PyObject *dcc_clang_compile(PyObject *, PyObject *);
void dcc_clang_compile_init();

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
PyObject *input_changed_error;
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
    std::string changed_path;
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
            if (!(entry.second == Stamp(entry.first))) {
                changed = true;
                if (changed_path.empty()) changed_path = entry.first;
            }
        return !changed;
    }
};

struct Result {
    bool unsupported = false;
    bool contextual = false;
    std::set<std::string> dependencies;
};

struct Callbacks : PPCallbacks {
    SourceManager &sources;
    Result &result;
    Preprocessor *preamble;
    bool restored;
    Callbacks(SourceManager &sources, Result &result, Preprocessor *preamble = nullptr,
              bool restored = false)
        : sources(sources), result(result), preamble(preamble), restored(restored) {}
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
    void MacroExpands(const Token &token, const MacroDefinition &, SourceRange range,
                      const MacroArgs *) override {
        if (!preamble || restored) return;
        auto name = token.getIdentifierInfo()->getName();
        // Preambles are shared across translation units. These native builtins
        // can depend on the new main file or the time of the next invocation.
        if (name == "__BASE_FILE__" || name == "__DATE__" || name == "__TIME__" ||
            ((name == "__FILE__" || name == "__FILE_NAME__" || name == "__TIMESTAMP__") &&
             sources.isWrittenInMainFile(sources.getExpansionLoc(range.getBegin()))))
            result.contextual = true;
    }
    void PragmaDirective(SourceLocation location, PragmaIntroducerKind) override {
        if (!preamble) return;
        // Compiler-generated predefines have their own PCH handling.
        if (sources.isWrittenInBuiltinFile(location)) return;
        // Clang's PCH does not serialize its push_macro stack. Leave these
        // inputs on the full native preprocessing path. Observe and reinject
        // the native handler's first token, including expanded _Pragma forms.
        Token token;
        preamble->LexUnexpandedToken(token);
        auto *identifier = token.getIdentifierInfo();
        if (identifier && ((!restored && identifier->getName() == "push_macro") ||
                           identifier->getName() == "pop_macro"))
            result.contextual = true;
        preamble->EnterToken(token, true);
    }
};

template<class Base = PreprocessOnlyAction>
struct Action : Base {
    Result &result;
    std::optional<unsigned> counter;
    explicit Action(Result &result, std::optional<unsigned> counter = {})
        : result(result), counter(counter) {}
    bool BeginSourceFileAction(CompilerInstance &instance) override {
        if (counter) instance.getPreprocessor().setCounterValue(*counter);
        instance.getPreprocessor().addPPCallbacks(
            std::make_unique<Callbacks>(instance.getSourceManager(), result,
                counter ? &instance.getPreprocessor() : nullptr, counter.has_value()));
        return true;
    }
};

struct Diagnostics : DiagnosticConsumer {
    std::string error;
    bool unsafe = false;
    void clear() override {
        DiagnosticConsumer::clear();
        error.clear();
        unsafe = false;
    }
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

class BuildPreamble : public GeneratePCHAction {
public:
    Result result;
    unsigned counter = 0;
    std::shared_ptr<PCHBuffer> buffer = std::make_shared<PCHBuffer>();
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &compiler,
                                                 llvm::StringRef) override {
        return std::make_unique<PCHGenerator>(compiler.getPreprocessor(),
            compiler.getModuleCache(), "<distcc-preamble>", "", buffer,
            compiler.getCodeGenOpts(), compiler.getFrontendOpts().ModuleFileExtensions);
    }
    bool BeginSourceFileAction(CompilerInstance &compiler) override {
        if (!GeneratePCHAction::BeginSourceFileAction(compiler)) return false;
        compiler.getPreprocessor().addPPCallbacks(std::make_unique<Callbacks>(
            compiler.getSourceManager(), result, &compiler.getPreprocessor()));
        return true;
    }
    void ExecuteAction() override {
        auto &compiler = getCompilerInstance();
        auto &preprocessor = compiler.getPreprocessor();
        preprocessor.IgnorePragmas();
        preprocessor.EnterMainSourceFile();
        Token token;
        do { preprocessor.Lex(token); } while (token.isNot(tok::eof));
        counter = preprocessor.getCounterValue();
        // PCHGenerator's native preprocessor-only mode serializes macros,
        // source maps and header state without parsing declarations or bodies.
        compiler.getASTConsumer().HandleTranslationUnit(compiler.getASTContext());
    }
};

// Keep the current include cohort, just as previous_inputs retains the current
// file images. A worker never accumulates one PCH for every source it has seen.
struct PreambleCache {
    std::vector<std::string> options;
    std::string prefix;
    std::string directory;
    PreambleBounds bounds{0, false};
    llvm::SmallVector<char, 0> compiled;
    std::map<std::string, Stamp> stamps;
    Images contents;
    std::set<std::string> dependencies;
    bool attempted = false;
    unsigned counter = 0;
};
thread_local PreambleCache preamble_cache;

class Scan : public tooling::ToolAction {
    Inputs &inputs;
    Result &result;
    bool use_cache;
public:
    bool hit = false;
    bool built = false;
    bool module_syntax = false;
    Scan(Inputs &inputs, Result &result, bool use_cache)
        : inputs(inputs), result(result), use_cache(use_cache) {}
    bool runInvocation(std::shared_ptr<CompilerInvocation> invocation, FileManager *manager,
                       std::shared_ptr<PCHContainerOperations> containers,
                       DiagnosticConsumer *diagnostics) override {
        auto &frontend = invocation->getFrontendOpts();
        if (frontend.Inputs.size() != 1 || !frontend.Inputs[0].isFile()) return false;
        module_syntax = invocation->getLangOpts().CPlusPlusModules || invocation->getLangOpts().ObjC;
        auto execute = [&](std::shared_ptr<CompilerInvocation> request, FileManager *files,
                           FrontendAction &action) {
            CompilerInstance compiler(std::move(request), containers);
            compiler.setFileManager(files);
            compiler.createDiagnostics(diagnostics, false);
            compiler.createSourceManager();
            return compiler.ExecuteAction(action);
        };
        if (!use_cache) {
            Action<> action(result);
            return execute(invocation, manager, action);
        }
        auto source = frontend.Inputs[0].getFile().str();
        auto buffer = inputs.getBufferForFile(source);
        if (!buffer) return false;
        auto bounds = ComputePreambleBounds(invocation->getLangOpts(), **buffer, 0);
        auto prefix = (*buffer)->getBuffer().take_front(bounds.Size).str();
        auto directory = llvm::sys::path::parent_path(inputs.path(source)).str();
        CompilerInvocation normalized(*invocation);
        normalized.getFrontendOpts().Inputs = {
            FrontendInputFile("<main>", frontend.Inputs[0].getKind())};
        normalized.getFrontendOpts().OutputFile.clear();
        normalized.getCodeGenOpts().MainFileName.clear();
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        llvm::SmallVector<const char *, 64> arguments;
        normalized.generateCC1CommandLine(arguments, [&](const llvm::Twine &arg) {
            return saver.save(arg).data();
        });
        std::vector<std::string> options(arguments.begin(), arguments.end());
        options.push_back(inputs.path("."));
        auto &cache = preamble_cache;
        bool same_context = cache.options == options && cache.directory == directory;
        bool same = same_context && !cache.prefix.empty() &&
                    llvm::StringRef(prefix).starts_with(cache.prefix);
        if (same) { bounds = cache.bounds; prefix.resize(bounds.Size); }
        else if (same_context) {
            // Translation units often share initial includes followed by their
            // own header. Let Clang delimit their common complete directives.
            auto common = std::mismatch(prefix.begin(), prefix.end(),
                                        cache.prefix.begin(), cache.prefix.end()).first;
            unsigned lines = std::count(prefix.begin(), common, '\n');
            if (lines) {
                auto shared = ComputePreambleBounds(invocation->getLangOpts(), **buffer, lines);
                if (shared.Size && shared.Size <= static_cast<size_t>(common - prefix.begin())) {
                    bounds = shared;
                    prefix.resize(bounds.Size);
                    cache = {};
                    cache.options = options;
                    cache.directory = directory;
                    cache.prefix = prefix;
                    cache.bounds = bounds;
                    same = true;
                }
            }
        }
        if (same) {
            for (const auto &entry : cache.stamps)
                if (!(entry.second == inputs.observe(entry.first))) { same = false; break; }
        }
        if (!same) {
            cache = {};
            cache.options = std::move(options);
            cache.prefix = std::move(prefix);
            cache.directory = std::move(directory);
            cache.bounds = bounds;
        } else if (!cache.attempted && bounds.Size) {
            // Do not pay for a PCH on every one-off command. Build only after
            // observing the same preprocessing context in a second request.
            cache.attempted = true;
            BuildPreamble action;
            Diagnostics build_diagnostics;
            auto preparation = std::make_shared<CompilerInvocation>(*invocation);
            preparation->getFrontendOpts().ProgramAction = frontend::GeneratePCH;
            preparation->getFrontendOpts().OutputFile = "<distcc-preamble>";
            auto &preprocessing = preparation->getPreprocessorOpts();
            preprocessing.GeneratePreamble = true;
            preprocessing.addRemappedFile(source,
                llvm::MemoryBuffer::getMemBufferCopy(cache.prefix, source).release());
            CompilerInstance builder(preparation, containers);
            builder.setFileManager(new FileManager(manager->getFileSystemOpts(), &inputs));
            builder.createDiagnostics(&build_diagnostics, false);
            builder.createSourceManager();
            bool generated = builder.ExecuteAction(action);
            cache.stamps = inputs.stamps;
            cache.stamps.erase(inputs.path(source));
            if (generated && action.buffer->IsComplete && !build_diagnostics.unsafe &&
                !action.result.unsupported &&
                !action.result.contextual && inputs.validate()) {
                cache.compiled = std::move(action.buffer->Data);
                cache.contents = inputs.contents;
                cache.dependencies = std::move(action.result.dependencies);
                cache.counter = action.counter;
                // Each new main file has the same prefix but different body.
                cache.contents.erase(inputs.path(source));
                cache.dependencies.erase(source);
                built = true;
            } else cache.compiled.clear();
        }
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> filesystem(&inputs);
        if (!cache.compiled.empty()) {
            auto cached_invocation = std::make_shared<CompilerInvocation>(*invocation);
            inputs.contents.insert(cache.contents.begin(), cache.contents.end());
            result.dependencies.insert(cache.dependencies.begin(), cache.dependencies.end());
            auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            const char *pch_path = "/__distcc_preamble__.pch";
            memory->addFile(pch_path, 0, llvm::MemoryBuffer::getMemBuffer(
                llvm::StringRef(cache.compiled.data(), cache.compiled.size()), pch_path, false));
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(filesystem);
            overlay->pushOverlay(memory);
            filesystem = overlay;
            auto &preprocessing = cached_invocation->getPreprocessorOpts();
            preprocessing.addRemappedFile(source, buffer->release());
            preprocessing.PrecompiledPreambleBytes = {bounds.Size, bounds.PreambleEndsAtStartOfLine};
            preprocessing.DisablePCHOrModuleValidation = DisableValidationForModuleKind::PCH;
            preprocessing.UsePredefines = false;
            preprocessing.ImplicitPCHInclude = pch_path;
            hit = true;
            // Preamble reuse disables the PCHValidator listener which
            // normally restores __COUNTER__. Restore this native scalar too.
            Action<ReadPCHAndPreprocessAction> action(result, cache.counter);
            bool ok = execute(cached_invocation,
                new FileManager(manager->getFileSystemOpts(), filesystem), action);
            if (!result.contextual) return ok;
            // Even builtin predefines can leave a push_macro stack. If the
            // remaining source pops a macro, replay with the full native state.
            hit = false;
            result = {};
            diagnostics->clear();
        }
        Action<> action(result);
        return execute(invocation, manager, action);
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
    int use_cache = 1;
    if (!PyArg_ParseTuple(arguments, "Os|p", &argv, &cwd, &use_cache)) return nullptr;
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
    bool preamble_hit = false, preamble_built = false;
    Py_BEGIN_ALLOW_THREADS
    {
        // Use the canonical preprocessor so its callbacks also cover probes,
        // unsupported directives and diagnostics. No parsing or code generation.
        // FileManager is request-scoped: stale negative lookups cannot survive.
        FileSystemOptions options;
        options.WorkingDir = cwd;
        llvm::IntrusiveRefCntPtr<FileManager> manager(new FileManager(options, inputs));
        Scan action(*inputs, result, use_cache);
        tooling::ToolInvocation invocation(command, &action, manager.get(),
                                           std::make_shared<PCHContainerOperations>());
        invocation.setDiagnosticConsumer(&diagnostics);
        invocation.run();
        preamble_hit = action.hit;
        preamble_built = action.built;
        ok = !diagnostics.unsafe && !result.dependencies.empty();
        // Probes and guarded aliases can resolve without opening the file.
        // Preserve those names and bytes so the worker can repeat every lookup.
        for (const auto &path : result.dependencies)
            if (!inputs->contents.count(inputs->path(path)) &&
                !inputs->openFileForRead(path)) ok = false;
        for (const auto &path : result.dependencies) {
            auto it = inputs->contents.find(inputs->path(path));
            // The directives scanner recognizes module declarations without
            // language context. In C, "module" can simply be a typedef name.
            if (action.module_syntax && it != inputs->contents.end() && it->second->hasModules())
                result.unsupported = true;
        }
        ok = inputs->validate() && ok && !result.unsupported;
    }
    Py_END_ALLOW_THREADS
    previous_inputs = ok ? inputs->contents : Images{};
    if (PyErr_CheckSignals() < 0) return nullptr;
    if (inputs->changed)
        return PyErr_Format(input_changed_error, "Input changed during Clang dependency scan: %s",
                            inputs->changed_path.c_str());
    if (!ok) return PyErr_Format(PyExc_RuntimeError, "Clang dependency scan: %s",
        result.unsupported ? "unsupported preprocessor input" :
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
    return Py_BuildValue("{s:N,s:N,s:N,s:O,s:O,s:n}", "files", files, "lookups", paths,
        "dependencies", deps, "preamble_hit", preamble_hit ? Py_True : Py_False,
        "preamble_built", preamble_built ? Py_True : Py_False,
        "preamble_bytes", static_cast<Py_ssize_t>(preamble_cache.compiled.size()));
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
    dcc_clang_compile_init();
    PyObject *result = PyModule_Create(&module);
    if (result) {
        input_changed_error = PyErr_NewException("clang_scan.InputChangedError", PyExc_RuntimeError, nullptr);
        if (!input_changed_error || PyModule_AddObject(result, "InputChangedError", input_changed_error) < 0) {
            Py_XDECREF(input_changed_error);
            Py_DECREF(result);
            return nullptr;
        }
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
