/* Bind the backend's import namespace to the snapshots of this invocation. */
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "../lto_partition.h"
#include "lto_backend.h"
#include <llvm/Support/Path.h>
#include <llvm/Object/IRObjectFile.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/ToolChain.h>
#include <clang/FrontendTool/Utils.h>
#include <llvm/TargetParser/Host.h>
#include "clang_compiler.h"
#include "clang_inputs.h"
#include <dlfcn.h>

using namespace llvm;
namespace {
thread_local const LtoInputs *active;

// For IR, CodeGenAction creates a virtual output file even when the compiler
// instance already has an output stream. Keep LLVM's stdout for diagnostics.
class ObjectStream final : public vfs::OutputFileImpl {
    raw_fd_ostream stream;
public:
    explicit ObjectStream(int fd) : stream(fd, false) {}
    raw_pwrite_stream &getOS() override { return stream; }
    Error keep() override {
        stream.flush();
        auto ec = stream.error();
        stream.clear_error();
        return errorCodeToError(ec);
    }
    Error discard() override { return keep(); }
};

class ObjectOutput final : public vfs::OutputBackend {
    int fd;
    IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override {
        return makeIntrusiveRefCnt<ObjectOutput>(fd);
    }
    Expected<std::unique_ptr<vfs::OutputFileImpl>>
    createFileImpl(StringRef path, std::optional<vfs::OutputConfig>) override {
        if (path != "-") return createStringError("unexpected ThinLTO output: %s", path.str().c_str());
        return std::make_unique<ObjectStream>(fd);
    }
public:
    explicit ObjectOutput(int fd) : fd(fd) {}
};

SmallString<256> clientPath(StringRef cwd, StringRef name) {
    SmallString<256> path(name);
    if (!sys::path::is_absolute(path)) {
        path = cwd;
        sys::path::append(path, name);
    }
    sys::path::remove_dots(path, true);
    return path;
}
}

Expected<int> LtoInputs::lookup(StringRef name) const {
    auto it = files.find(name);
    if (it == files.end()) it = files.find(clientPath(cwd, name));
    if (it == files.end())
        return createStringError("undeclared ThinLTO input: %s", name.str().c_str());
    return it->second;
}

llvm::Error llvm::lto::thinBackend(
    const Config &config, unsigned task, AddStreamFn addStream, Module &module,
    const ModuleSummaryIndex &index,
    const FunctionImporter::ImportMapTy &imports,
    const GVSummaryMapTy &globals,
    MapVector<StringRef, BitcodeModule> *modules, bool codegenOnly,
#if LLVM_VERSION_MAJOR >= 23
    ArrayRef<StringRef> libfuncs,
#endif
    AddStreamFn irStream, const std::vector<uint8_t> &args) {
  Dl_info symbol{};
  if (!dladdr(reinterpret_cast<void *>(&thinBackend), &symbol))
    report_fatal_error("cannot locate the ThinLTO backend symbol");
  auto original = reinterpret_cast<decltype(&thinBackend)>(
      dlsym(RTLD_NEXT, symbol.dli_sname));
  if (!original)
    report_fatal_error("cannot locate the installed ThinLTO backend");

  auto invoke = [&] {
    return original(config, task, addStream, module, index, imports, globals,
                    modules, codegenOnly,
#if LLVM_VERSION_MAJOR >= 23
                    libfuncs,
#endif
                    irStream, args);
  };
  if (!active) return invoke();
  MapVector<StringRef, BitcodeModule> declared;
  SmallVector<std::unique_ptr<MemoryBuffer>> storage;
  for (StringRef id : imports.getSourceModules()) {
    auto path = active->lookup(id);
    if (!path) return path.takeError();
    auto buffer = MemoryBuffer::getOpenFile(*path, id, -1);
    if (!buffer) return errorCodeToError(buffer.getError());
    auto object = object::IRObjectFile::findBitcodeInMemBuffer(
        MemoryBufferRef((*buffer)->getBuffer(), id));
    if (!object) return object.takeError();
    auto bitcode = findThinLTOModule(*object);
    if (!bitcode) return bitcode.takeError();
    declared.insert({id, *bitcode});
    storage.push_back(std::move(*buffer));
  }
  modules = &declared;
  if (!active->partitions) return invoke();
  if (codegenOnly) return createStringError("cannot partition a codegen-only backend");
  const auto &destination = active->manifest;

  // Clang owns a mutable Config for this single invocation. Borrow only its
  // import hook, restoring it before returning. Interposing opt() instead is
  // insufficient: optimized LLVM builds inline it inside thinBackend().
  auto &hook = const_cast<Config &>(config).PostImportModuleHook;
  auto previous = std::move(hook);
  auto restore = scope_exit([&] { hook = std::move(previous); });
  hook = [&](unsigned task, const Module &prepared) {
    if (previous && !previous(task, prepared))
      return false;
    // Clang owns this mutable module. Returning false ends its backend, so
    // partitioning can externalize locals without changing any later pass.
    // Writing and reparsing the entire imported IR here doubles expensive
    // constant-data serialization; emit only the final partition inputs.
    auto result = dcc_partition_module(const_cast<Module &>(prepared), active->partitions,
                                       (Twine(destination) + ".part-").str());
    if (!result) {
      logAllUnhandledErrors(result.takeError(), errs(), "distcc-lto: ");
      report_fatal_error("cannot partition ThinLTO module");
    }
    std::error_code error;
    raw_fd_ostream output(destination, error, sys::fs::OF_None);
    if (error)
      report_fatal_error(Twine("cannot prepare ThinLTO module: ") + error.message());
    output << std::move(*result) << '\n';
    output.close();
    if (output.has_error())
      report_fatal_error("cannot finish ThinLTO module output");
    return false;
  };
  return invoke();
}

std::vector<std::string> dcc_lto_cc1(ArrayRef<const char *> args) {
    clang::DiagnosticOptions opts;
    auto diagnostics = clang::CompilerInstance::createDiagnostics(*vfs::getRealFileSystem(), opts);
    clang::driver::Driver driver(args.front(), sys::getDefaultTargetTriple(), *diagnostics);
    driver.setCheckInputsExist(false);
    driver.setTargetAndMode(clang::driver::ToolChain::getTargetAndModeFromProgramName(args.front()));
    std::unique_ptr<clang::driver::Compilation> plan(driver.BuildCompilation(args));
    if (!plan || plan->containsError() || plan->getJobs().size() != 1) return {};
    const auto &cc1 = plan->getJobs().begin()->getArguments();
    if (cc1.empty() || StringRef(cc1.front()) != "-cc1") return {};
    return {cc1.begin(), cc1.end()};
}

int dcc_lto_compile(const LtoInputs &inputs, ArrayRef<const char *> args, int output) {
    if (args.size() < 2) return 1;
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    DccChildCompiler compiler(new clang::CompilerInstance(invocation));
    auto parse = [&](ArrayRef<const char *> cc1) {
        return dcc_parse_compiler_invocation(*compiler, cc1, args.front());
    };
    if (StringRef(args[1]) == "-cc1") {
        if (!parse(args.drop_front(2))) return 1;
    } else {
        auto words = dcc_lto_cc1(args);
        SmallVector<const char *> cc1;
        for (auto &word : words) cc1.push_back(word.c_str());
        if (cc1.empty() || !parse(ArrayRef(cc1).drop_front())) return 1;
    }
    auto &frontend = invocation->getFrontendOpts();
    if (!inputs.output.empty() &&
        clientPath(inputs.cwd, inputs.output) != clientPath(inputs.cwd, frontend.OutputFile)) {
        errs() << "ThinLTO output does not match the declared output\n";
        return 1;
    }
    if (frontend.ProgramAction != clang::frontend::EmitObj || frontend.Inputs.size() != 1 ||
        !frontend.Inputs.front().isFile() || frontend.Inputs.front().getKind().getLanguage() != clang::Language::LLVM_IR)
        return 1;
    auto &primary = frontend.Inputs.front();
    auto path = inputs.lookup(primary.getFile());
    if (!path) { logAllUnhandledErrors(path.takeError(), errs()); return 1; }
    auto buffer = MemoryBuffer::getOpenFile(*path, primary.getFile(), -1);
    if (!buffer) { errs() << primary.getFile() << ": " << buffer.getError().message() << '\n'; return 1; }
    std::string identifier = primary.getFile().str();
    auto kind = primary.getKind();
    primary = clang::FrontendInputFile(MemoryBufferRef((*buffer)->getBuffer(), identifier), kind);
    auto &codegen = invocation->getCodeGenOpts();
    bool resolved = true;
    auto resolve = [&](std::string &name) {
        auto source = inputs.lookup(name);
        if (!source) { logAllUnhandledErrors(source.takeError(), errs()); resolved = false; return; }
        if (!inputs.partitions) {
            name = "/proc/self/fd/" + std::to_string(*source);
            return;
        }
        // Clang's index/profile readers require a pathname. The client child
        // lives in its private output directory; never expose the host cache.
        auto data = MemoryBuffer::getOpenFile(*source, name, -1, false);
        name = "aux-" + std::to_string(*source);
        std::error_code error;
        raw_fd_ostream aux(name, error);
        if (!data || error) { resolved = false; return; }
        aux << (*data)->getBuffer();
        aux.close();
        if (aux.has_error()) resolved = false;
    };
    if (!codegen.ThinLTOIndexFile.empty()) {
        if (inputs.partitions && clientPath(inputs.cwd, codegen.ThinLTOIndexFile) == inputs.localIndex)
            codegen.ThinLTOIndexFile = "index.thinlto.bc";
        else resolve(codegen.ThinLTOIndexFile);
    }
    dcc_clang_profile_inputs(codegen, resolve);
    if (!resolved) return 1;
    frontend.OutputFile = "-";
    compiler->setOutputManager(makeIntrusiveRefCnt<ObjectOutput>(output));
    active = &inputs;
    auto reset = scope_exit([] { active = nullptr; });
    return clang::ExecuteCompilerInvocation(compiler.get()) ? 0 : 1;
}
