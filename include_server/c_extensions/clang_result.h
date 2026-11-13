/* Validated reuse of frontend IR, within one resident compiler.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "remote_files.h"
#include <clang/Frontend/Utils.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>

namespace dcc_result {
// Initialized when the library is loaded in the resident parent, before its
// job forks. A different library owner never consumes this owner's IR.
inline const auto instance = [] {
    std::pair<std::array<uint8_t, 16>, bool> value{};
    value.second = getentropy(value.first.data(), value.first.size()) == 0;
    return value;
}();
// Use Clang's dependency collector AND serializer. In particular, targets,
// quoting, -MP, system headers and ordering are not a second Makefile parser.
class Dependencies final : public clang::DependencyFileGenerator {
    class Macros final : public clang::PPCallbacks {
        bool &reusable;
    public:
        explicit Macros(bool &value) : reusable(value) {}
        void MacroExpands(const clang::Token &token, const clang::MacroDefinition &definition,
                          clang::SourceRange, const clang::MacroArgs *) override {
            if (definition.getMacroInfo() && definition.getMacroInfo()->isBuiltinMacro()) {
                auto name = token.getIdentifierInfo()->getName();
                if (name == "__DATE__" || name == "__TIME__" || name == "__TIMESTAMP__") reusable = false;
            }
        }
        void moduleImport(clang::SourceLocation, clang::ModuleIdPath, const clang::Module *) override {
            reusable = false;
        }
    };
public:
    bool reusable = true;
    explicit Dependencies(const clang::DependencyOutputOptions &options)
        : clang::DependencyFileGenerator(options) {}
    void attachToPreprocessor(clang::Preprocessor &pp) override {
        clang::DependencyFileGenerator::attachToPreprocessor(pp);
        pp.addPPCallbacks(std::make_unique<Macros>(reusable));
    }
    void maybeAddDependency(llvm::StringRef name, bool from_module, bool system, bool module_file,
#if LLVM_VERSION_MAJOR >= 23
                            bool direct_import,
#endif
                            bool missing) override {
        if (from_module || module_file) reusable = false;
        clang::DependencyFileGenerator::maybeAddDependency(name, from_module, system, module_file,
#if LLVM_VERSION_MAJOR >= 23
                                                           direct_import,
#endif
                                                           missing);
    }
    void finishedMainFile(clang::DiagnosticsEngine &) override {}
    bool write(const llvm::json::Array &names, llvm::StringRef path) {
        for (const auto &name : names) {
            auto text = name.getAsString();
            if (!text) return false;
            addDependency(*text);
        }
        std::error_code error;
        llvm::raw_fd_ostream stream(path, error);
        if (error) return false;
        outputDependencyFile(stream); stream.close();
        return !stream.has_error();
    }
};

class Diagnostics final : public clang::DiagnosticConsumer {
public:
    std::string bytes;
private:
    std::unique_ptr<clang::DiagnosticConsumer> original;
    llvm::raw_string_ostream stream;
    clang::TextDiagnosticPrinter printer;
public:
    Diagnostics(std::unique_ptr<clang::DiagnosticConsumer> original,
                clang::DiagnosticOptions &options)
        : original(std::move(original)), stream(bytes), printer(stream, options) {
        NumWarnings = this->original->getNumWarnings();
    }
    void BeginSourceFile(const clang::LangOptions &lang, const clang::Preprocessor *pp) override {
        original->BeginSourceFile(lang, pp); printer.BeginSourceFile(lang, pp);
    }
    void EndSourceFile() override { original->EndSourceFile(); printer.EndSourceFile(); }
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic &info) override {
        clang::DiagnosticConsumer::HandleDiagnostic(level, info);
        original->HandleDiagnostic(level, info); printer.HandleDiagnostic(level, info);
    }
};

inline bool eligible(const clang::CompilerInvocation &invocation) {
    const auto &f = invocation.getFrontendOpts();
    const auto &c = invocation.getCodeGenOpts();
    const auto &p = invocation.getPreprocessorOpts();
    const auto &d = invocation.getDependencyOutputOpts();
    const auto &g = invocation.getDiagnosticOpts();
    const auto &l = invocation.getLangOpts();
    // Share only the frontend of an ordinary C/C++ object action. Other actions,
    // unobserved frontend inputs and additional output streams retain their
    // existing compiler path. The backend owns every final object write.
    return f.ProgramAction == clang::frontend::EmitObj && f.Inputs.size() == 1 &&
        f.Inputs[0].isFile() && (f.Inputs[0].getKind().getLanguage() == clang::Language::C ||
                               f.Inputs[0].getKind().getLanguage() == clang::Language::CXX) &&
        !f.ShowHelp && !f.ShowVersion && !f.ShowStats && !f.FixAndRecompile &&
        !f.EmitSymbolGraph && f.CodeCompletionAt.FileName.empty() &&
        f.Plugins.empty() && f.AddPluginActions.empty() && f.ASTMergeFiles.empty() &&
        f.ModuleFiles.empty() && f.ModuleMapFiles.empty() && f.LLVMArgs.empty() &&
        f.OverrideRecordLayoutsFile.empty() && f.StatsFile.empty() && f.TimeTracePath.empty() &&
        !(l.Modules && l.ImplicitModules) && !l.AsmPreprocessor && l.Sanitize.empty() &&
        invocation.getHeaderSearchOpts().VFSOverlayFiles.empty() &&
        p.ImplicitPCHInclude.empty() && p.ChainedIncludes.empty() &&
        p.RemappedFiles.empty() && p.RemappedFileBuffers.empty() &&
        !c.PrepareForLTO && !c.PrepareForThinLTO && !c.CoverageMapping &&
        !c.TimePasses && !c.TimeTrace && c.getEmbedBitcode() == clang::CodeGenOptions::Embed_Off &&
        c.RecordCommandLine.empty() && c.DwarfDebugFlags.empty() &&
        c.CoverageDataFile.empty() && c.CoverageNotesFile.empty() && c.DebugPass.empty() &&
        c.SplitDwarfFile.empty() && c.SplitDwarfOutput.empty() && c.ObjectFilenameForDebug.empty() &&
        c.LinkBitcodeFiles.empty() && c.PassPlugins.empty() && c.SampleProfileFile.empty() &&
        c.ProfileInstrumentUsePath.empty() && c.ProfileRemappingFile.empty() &&
        c.MemoryProfileUsePath.empty() && c.ThinLTOIndexFile.empty() && c.ThinLinkBitcodeFile.empty() &&
        c.SaveTempsFilePrefix.empty() && c.OptRecordFile.empty() &&
#if LLVM_VERSION_MAJOR >= 23
        c.StackUsageFile.empty() &&
#else
        c.StackUsageOutput.empty() &&
#endif
        c.CudaGpuBinaryFileName.empty() && c.OffloadObjects.empty() &&
        c.SanitizeCoverageAllowlistFiles.empty() && c.SanitizeCoverageIgnorelistFiles.empty() &&
        c.SanitizeMetadataIgnorelistFiles.empty() && c.AsSecureLogFile.empty() &&
        c.MSSecureHotPatchFunctionsFile.empty() &&
        !g.VerifyDiagnostics && g.DiagnosticLogFile.empty() && g.DiagnosticSerializationFile.empty() &&
        g.getFormat() == clang::DiagnosticOptions::Clang &&
        !d.ShowHeaderIncludes && d.ShowIncludesDest == clang::ShowIncludesDestination::None &&
        d.HeaderIncludeOutputFile.empty() && d.DOTOutputFile.empty() && d.ModuleDependencyOutputDir.empty();
}

class Result {
    RemoteFiles &files;
    clang::CompilerInstance &compiler;
    std::string key;
    int lock = -1;
    std::shared_ptr<Dependencies> dependencies;
    Diagnostics *diagnostics = nullptr;
public:
    Result(RemoteFiles &files, clang::CompilerInstance &compiler, llvm::StringRef cwd)
        : files(files), compiler(compiler) {
        const char *enabled = getenv("DISTCC_CLANG_CODEGEN");
        if (!enabled || strcmp(enabled, "1")) return;
        if (!eligible(compiler.getInvocation())) {
            if (getenv("DISTCC_VERBOSE")) {
                const auto &i = compiler.getInvocation();
                llvm::errs() << "distcc compiler result: bypass invocation modules=" << i.getLangOpts().Modules
                    << " llvm-args=" << i.getFrontendOpts().LLVMArgs.size()
                    << " trace=" << !i.getFrontendOpts().TimeTracePath.empty()
                    << " debug-object=" << !i.getCodeGenOpts().ObjectFilenameForDebug.empty() << '\n';
            }
            return;
        }
        auto canonical = compiler.getInvocation();
        canonical.getFrontendOpts().OutputFile = "object";
        auto &dep = canonical.getDependencyOutputOpts();
        if (!dep.OutputFile.empty()) { dep.OutputFile = "dependencies"; dep.Targets = {"target"}; }
        // The inherited library nonce identifies this resident compiler, its
        // loaded libraries and environment. Different worker execs never share
        // an action, even when an on-disk compiler is replaced in place.
        if (!instance.second) return;
        key = "clang-ir-v1:" LLVM_VERSION_STRING ":" + llvm::toHex(instance.first);
        key.append(cwd.data(), cwd.size()); key += '\0';
        canonical.generateCC1CommandLine([&](const llvm::Twine &arg) { key += arg.str(); key += '\0'; });
        lock = files.claimResult(key);
        if (lock < 0) { llvm::errs() << "distcc: result claim: " << strerror(errno) << '\n'; key.clear(); }
    }
    ~Result() { if (lock >= 0) close(lock); }
    void release() { if (lock >= 0) { close(lock); lock = -1; } }
    // A miss (including changed inputs) runs the real frontend. The backend
    // always runs, so native assembler inputs need no additional certificate.
    std::unique_ptr<llvm::Module> replay(llvm::LLVMContext &context) {
        if (key.empty()) return {};
        auto cached = files.result(key);
        if (!cached.data || cached.size < 8) return {};
        uint64_t size = dccfs::number(cached.data, 8);
        if (size > cached.size-8) return {};
        auto parsed = llvm::json::parse({cached.data+8, size});
        if (!parsed) { llvm::consumeError(parsed.takeError()); return {}; }
        auto *record = parsed->getAsObject();
        if (!record) return {};
        auto *inputs = record->getArray("inputs"); auto *names = record->getArray("dependencies");
        auto messages = record->getString("diagnostics");
        auto warnings = record->getInteger("warnings");
        if (!inputs || !names || !messages || !warnings || *warnings < 0 || *warnings > UINT_MAX) return {};
        const bool measured = getenv("DISTCC_VERBOSE");
        auto now = [](clockid_t clock) {
            timespec value{};
            clock_gettime(clock, &value);
            return uint64_t(value.tv_sec)*1000000000 + value.tv_nsec;
        };
        const auto wall = measured ? now(CLOCK_MONOTONIC) : 0;
        const auto cpu = measured ? now(CLOCK_PROCESS_CPUTIME_ID) : 0;
        const bool valid = files.validate(*inputs);
        if (measured)
            llvm::errs() << "distcc compiler result: validation inputs=" << inputs->size()
                << " valid=" << valid << " cpu_ns=" << now(CLOCK_PROCESS_CPUTIME_ID)-cpu
                << " wall_ns=" << now(CLOCK_MONOTONIC)-wall << '\n';
        if (!valid) return {};
        llvm::MemoryBufferRef bitcode({cached.data+8+size, cached.size-8-size}, "distcc frontend IR");
        auto module = llvm::parseBitcodeFile(bitcode, context);
        if (!module) { llvm::consumeError(module.takeError()); return {}; }
        const auto &dep = compiler.getDependencyOutputOpts();
        Dependencies output(dep);
        if (!dep.OutputFile.empty() && !output.write(*names, dep.OutputFile)) {
            llvm::errs() << "distcc: cannot publish reused compiler artifacts\n";
            publication_failed = true;
            return {};
        }
        llvm::errs() << *messages;
        struct Counts final : clang::DiagnosticConsumer {
            explicit Counts(unsigned count) { NumWarnings = count; }
        };
        auto original = compiler.getDiagnostics().takeClient();
        compiler.getDiagnostics().setClient(new Counts(*warnings));
        compiler.getDiagnostics().setNumWarnings(*warnings);
        compiler.printDiagnosticStats();
        compiler.getDiagnostics().setClient(original.release());
        if (getenv("DISTCC_VERBOSE")) llvm::errs() << "distcc compiler result: hit inputs=" << inputs->size() << '\n';
        release();
        return std::move(*module);
    }
    bool publication_failed = false;
    void start() {
        if (key.empty()) return;
        files.beginObservations();
        dependencies = std::make_shared<Dependencies>(compiler.getDependencyOutputOpts());
        compiler.addDependencyCollector(dependencies);
        auto &engine = compiler.getDiagnostics();
        diagnostics = new Diagnostics(engine.takeClient(), compiler.getDiagnosticOpts());
        engine.setClient(diagnostics);
    }
    void save(const llvm::Module &module) {
        auto unlock = llvm::scope_exit([&] { release(); });
        if (!dependencies) return;
        if (!dependencies->reusable) {
            if (getenv("DISTCC_VERBOSE")) llvm::errs() << "distcc compiler result: bypass volatile/module input\n";
            return;
        }
        auto inputs = files.certificate();
        if (!inputs || inputs->empty() || !llvm::json::isUTF8(diagnostics->bytes)) {
            if (getenv("DISTCC_VERBOSE")) llvm::errs() << "distcc compiler result: bypass observations\n";
            return;
        }
        const auto input_count = inputs->size();
        llvm::json::Array names;
        for (const auto &name : dependencies->getDependencies()) {
            if (!llvm::json::isUTF8(name)) return;
            names.push_back(name);
        }
        std::string metadata;
        llvm::raw_string_ostream stream(metadata);
        stream << llvm::json::Value(llvm::json::Object{{"inputs", std::move(*inputs)},
            {"dependencies", std::move(names)}, {"diagnostics", diagnostics->bytes},
            {"warnings", diagnostics->getNumWarnings()}});
        llvm::SmallVector<char> encoded;
        llvm::raw_svector_ostream bitcode(encoded);
        llvm::WriteBitcodeToFile(module, bitcode, true);
        llvm::StringRef bytes(encoded.data(), encoded.size());
        if (metadata.size()+bytes.size()+8+key.size() > DCC_FILE_CACHE_MAX-64) return;
        std::string record(8, '\0');
        for (unsigned n = 0; n < 8; ++n) record[7-n] = uint64_t(metadata.size()) >> (8*n);
        record += metadata; record.append(bytes.data(), bytes.size());
        files.saveResult(key, record);
        if (getenv("DISTCC_VERBOSE")) llvm::errs() << "distcc compiler result: stored inputs=" << input_count << '\n';
    }
};
}
