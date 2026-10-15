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
#include <sys/stat.h>
#include <map>
#include <set>

namespace {
using namespace clang;

struct Stamp {
    struct stat value{};
    bool exists;
    explicit Stamp(const std::string &path) : exists(::stat(path.c_str(), &value) == 0) {}
    bool operator==(const Stamp &other) const {
        return exists == other.exists && (!exists ||
            (value.st_dev == other.value.st_dev && value.st_ino == other.value.st_ino &&
             value.st_mode == other.value.st_mode && value.st_size == other.value.st_size &&
             value.st_mtim.tv_sec == other.value.st_mtim.tv_sec &&
             value.st_mtim.tv_nsec == other.value.st_mtim.tv_nsec &&
             value.st_ctim.tv_sec == other.value.st_ctim.tv_sec &&
             value.st_ctim.tv_nsec == other.value.st_ctim.tv_nsec));
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
    std::set<std::string> probes;
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
    Inputs &inputs;
    Result &result;
    Callbacks(SourceManager &sources, Inputs &inputs, Result &result)
        : sources(sources), inputs(inputs), result(result) {}
    void FileChanged(SourceLocation loc, FileChangeReason reason,
                     SrcMgr::CharacteristicKind, FileID) override {
        if (reason != EnterFile) return;
        if (auto file = sources.getFileEntryRefForID(sources.getFileID(loc)))
            result.dependencies.insert(file->getName().str());
    }
    void HasInclude(SourceLocation, llvm::StringRef name, bool, OptionalFileEntryRef file,
                    SrcMgr::CharacteristicKind) override {
        if (name.starts_with("/")) result.unsupported = true;
        if (file) inputs.probes.insert(file->getName().str());
    }
    void InclusionDirective(SourceLocation loc, const Token &, llvm::StringRef name,
                            bool, CharSourceRange, OptionalFileEntryRef,
                            llvm::StringRef, llvm::StringRef, const Module *, bool imported,
                            SrcMgr::CharacteristicKind) override {
        // The existing GCC-compatible transfer rewrites command-line paths,
        // but cannot rewrite absolute include operands inside source files.
        if ((name.starts_with("/") && sources.getFileEntryRefForID(sources.getFileID(loc))) || imported)
            result.unsupported = true;
    }
    void EmbedDirective(SourceLocation, llvm::StringRef, bool, OptionalFileEntryRef,
                        const LexEmbedParametersResult &) override { result.unsupported = true; }
};

struct Action : PreprocessOnlyAction {
    Inputs &inputs;
    Result &result;
    Action(Inputs &inputs, Result &result) : inputs(inputs), result(result) {}
    bool BeginSourceFileAction(CompilerInstance &instance) override {
        instance.getPreprocessor().addPPCallbacks(
            std::make_unique<Callbacks>(instance.getSourceManager(), inputs, result));
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
        tooling::ToolInvocation invocation(command, std::make_unique<Action>(*inputs, result), manager.get());
        invocation.setDiagnosticConsumer(&diagnostics);
        invocation.run();
        ok = !diagnostics.unsafe && !result.dependencies.empty();
        // Existence probes can succeed without opening a header. Transfer those
        // files too, so __has_include has the same answer on the worker.
        for (const auto &path : inputs->probes)
            if (!inputs->openFileForRead(path)) ok = false;
        result.dependencies.insert(inputs->probes.begin(), inputs->probes.end());
        for (const auto &path : result.dependencies) {
            auto it = inputs->contents.find(inputs->path(path));
            if (it != inputs->contents.end() && scanInputForCXX20ModulesUsage(it->second))
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
        PyObject *value = entry.second.exists ? Py_BuildValue("(KKKKLL)",
            (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
            (unsigned long long)st.st_mode, (unsigned long long)st.st_size,
            (long long)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec,
            (long long)st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec) : Py_NewRef(Py_None);
        if (!key || !value || PyDict_SetItem(paths, key, value) < 0) success = false;
        Py_XDECREF(key); Py_XDECREF(value);
    }
    for (const auto &entry : result.dependencies) append(deps, entry);
    if (!success) { Py_DECREF(files); Py_DECREF(paths); Py_DECREF(deps); return nullptr; }
    return Py_BuildValue("{s:N,s:N,s:N}", "files", files, "lookups", paths, "dependencies", deps);
}

struct SourceTokens {
    std::vector<Token> tokens, comments;
    SourceTokens(const char *text, Py_ssize_t size) {
        LangOptions options;
        options.CPlusPlus = true;
        options.LineComment = true;
        Lexer lexer(SourceLocation::getFromRawEncoding(1), options, text, text, text + size);
        lexer.SetCommentRetentionState(true);
        Token token;
        do {
            lexer.LexFromRawLexer(token);
            (token.is(tok::comment) ? comments : tokens).push_back(token);
        } while (!token.is(tok::eof));
    }
    std::string expression(const char *text, unsigned start, unsigned end) const {
        std::string value(text + start, end - start);
        for (const auto &comment : comments) {
            unsigned offset = comment.getLocation().getRawEncoding() - 1;
            if (offset >= start && offset + comment.getLength() <= end)
                value.replace(offset - start, comment.getLength(), comment.getLength(), ' ');
        }
        return value;
    }
};

PyObject *conditional_expressions(PyObject *, PyObject *arguments) {
    const char *text;
    Py_ssize_t size;
    if (!PyArg_ParseTuple(arguments, "y#", &text, &size)) return nullptr;
    SourceTokens source(text, size);
    const auto &tokens = source.tokens;
    PyObject *result = PyList_New(0);
    if (!result) return nullptr;
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
        if (!tokens[i].is(tok::hash) || !tokens[i].isAtStartOfLine() ||
            !tokens[i + 1].is(tok::raw_identifier)) continue;
        auto directive = tokens[i + 1].getRawIdentifier();
        if (directive != "if" && directive != "elif") continue;
        size_t end = i + 2;
        while (end < tokens.size() && !tokens[end].isAtStartOfLine() &&
               !tokens[end].is(tok::eof)) ++end;
        unsigned start = tokens[i + 1].getLocation().getRawEncoding() - 1 + tokens[i + 1].getLength();
        unsigned stop = end < tokens.size() ? tokens[end].getLocation().getRawEncoding() - 1 : size;
        auto value = source.expression(text, start, stop);
        PyObject *item = PyUnicode_DecodeLatin1(value.data(), value.size(), nullptr);
        if (!item || PyList_Append(result, item) < 0) {
            Py_XDECREF(item); Py_DECREF(result); return nullptr;
        }
        Py_DECREF(item);
        i = end - 1;
    }
    return result;
}

PyObject *include_probes(PyObject *, PyObject *arguments) {
    const char *text;
    Py_ssize_t size;
    if (!PyArg_ParseTuple(arguments, "y#", &text, &size)) return nullptr;
    SourceTokens source(text, size);
    const auto &tokens = source.tokens;
    PyObject *result = PyList_New(0);
    if (!result) return nullptr;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (!tokens[i].is(tok::raw_identifier)) continue;
        auto name = tokens[i].getRawIdentifier();
        if (name != "__has_include" && name != "__has_include_next") continue;
        size_t line = i;
        while (line && !tokens[line].isAtStartOfLine()) --line;
        bool macro = line + 2 <= i && tokens[line].is(tok::hash) &&
                     tokens[line + 1].is(tok::raw_identifier) &&
                     tokens[line + 1].getRawIdentifier() == "define";
        // Callers must expand conditions before extracting probes. This
        // lexer cannot substitute arguments inside wrapper definitions.
        if (macro && i != line + 2) {
            Py_DECREF(result);
            return PyErr_Format(PyExc_ValueError, "indirect header existence probe requires local preprocessing");
        }
        if (!tokens[i + 1].is(tok::l_paren)) {
            auto identifier = [&](size_t position, llvm::StringRef word) {
                return tokens[position].is(tok::raw_identifier) &&
                       tokens[position].getRawIdentifier() == word;
            };
            bool feature_test = (i && (identifier(i - 1, "defined") ||
                                      identifier(i - 1, "ifdef") || identifier(i - 1, "ifndef"))) ||
                                (i >= 2 && tokens[i - 1].is(tok::l_paren) && identifier(i - 2, "defined"));
            if (!macro && !feature_test) {
                Py_DECREF(result);
                return PyErr_Format(PyExc_ValueError, "indirect header existence probe requires local preprocessing");
            }
            continue;
        }
        // A function-like compatibility definition declares a parameter. Its
        // replacement body is still scanned for real existence probes.
        bool definition = i && tokens[i - 1].is(tok::raw_identifier) &&
                          tokens[i - 1].getRawIdentifier() == "define";
        size_t j = i + 2;
        unsigned depth = 1;
        for (; j < tokens.size(); ++j) {
            if (tokens[j].is(tok::l_paren)) ++depth;
            else if (tokens[j].is(tok::r_paren) && !--depth) break;
        }
        if (j == tokens.size()) {
            Py_DECREF(result);
            return PyErr_Format(PyExc_ValueError, "unterminated header existence probe");
        }
        if (!definition) {
            unsigned start = tokens[i + 1].getLocation().getRawEncoding();
            unsigned end = tokens[j].getLocation().getRawEncoding() - 1;
            auto expression = source.expression(text, start, end);
            PyObject *operand = PyUnicode_DecodeLatin1(expression.data(), expression.size(), nullptr);
            PyObject *item = operand ? Py_BuildValue("(iN)", name == "__has_include_next", operand) : nullptr;
            if (!item || PyList_Append(result, item) < 0) {
                Py_XDECREF(item); Py_DECREF(result); return nullptr;
            }
            Py_DECREF(item);
        }
        i = j;
    }
    return result;
}

PyMethodDef methods[] = {
    {"conditional_expressions", conditional_expressions, METH_VARARGS, "Lex conditional expressions from all branches."},
    {"scan", scan, METH_VARARGS, "Scan using libclang-cpp; return immutable inputs and lookup paths."},
    {"include_probes", include_probes, METH_VARARGS, "Lex existence probes independently of conditional branches."},
    {nullptr, nullptr, 0, nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "clang_scan", nullptr, -1, methods, nullptr, nullptr, nullptr, nullptr};
}

PyMODINIT_FUNC PyInit_clang_scan() {
    PyObject *result = PyModule_Create(&module);
    if (result) PyModule_AddStringConstant(result, "version", CLANG_VERSION_STRING);
    return result;
}
