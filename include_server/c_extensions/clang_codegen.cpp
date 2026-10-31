/* Optimize the whole translation unit, then use idle local CPU leases for
 * independent machine-code generation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "clang_codegen.h"
#include "../../src/broker_protocol.h"
#include "../../src/workload.h"
#include "../../src/lto_partition.h"
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/BackendUtil.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <llvm/IR/Module.h>
#include <llvm/ObjCopy/ConfigManager.h>
#include <llvm/ObjCopy/ObjCopy.h>
#include <llvm/Object/Binary.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/BuryPointer.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/TargetParser/Triple.h>
#include <lld/Common/Driver.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <new>
#include <time.h>

LLD_HAS_DRIVER(elf)

namespace {
struct Measurements {
    std::atomic<uint64_t> codegen_ns{0}, instructions{0}, overhead_ns{0}, parts{0};
};
Measurements *measured;
uint64_t clock_ns(clockid_t clock = CLOCK_MONOTONIC) {
    timespec now{};
    clock_gettime(clock, &now);
    return uint64_t(now.tv_sec)*1000000000+now.tv_nsec;
}

// Observe Clang's actual action boundaries without replacing its AST or
// preprocessor callbacks. HandleTranslationUnit also finalizes deferred IR;
// its time must not be reported as LLVM optimization alone.
class TimedIRAction : public clang::EmitLLVMOnlyAction {
    struct Sample {
        uint64_t wall, cpu;
        static Sample now() {
            rusage children{};
            getrusage(RUSAGE_CHILDREN, &children);
            uint64_t child_ns = uint64_t(children.ru_utime.tv_sec + children.ru_stime.tv_sec)*1000000000
                + uint64_t(children.ru_utime.tv_usec + children.ru_stime.tv_usec)*1000;
            return {clock_ns(), clock_ns(CLOCK_PROCESS_CPUTIME_ID) + child_ns};
        }
    };
    class Boundary : public clang::ASTConsumer {
        TimedIRAction &action;
        bool after;
    public:
        Boundary(TimedIRAction &action, bool after) : action(action), after(after) {}
        void HandleTranslationUnit(clang::ASTContext &) override {
            if (!after) {
                if (auto *module = action.getCodeGenerator()->GetModule())
                    for (const auto &function : *module)
                        action.unfinalized_instructions += function.getInstructionCount();
            }
            action.samples[after ? 2 : 1] = Sample::now();
        }
    };
    bool enabled = getenv("DISTCC_VERBOSE");
    bool ready = false;
    Sample samples[4]{};
    std::string source;
    uint64_t unfinalized_instructions = 0, optimized_instructions = 0;
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
            clang::CompilerInstance &compiler, llvm::StringRef input) override {
        auto backend = EmitLLVMOnlyAction::CreateASTConsumer(compiler, input);
        if (!enabled || !backend) return backend;
        source = input.str();
        std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
        consumers.push_back(std::make_unique<Boundary>(*this, false));
        consumers.push_back(std::move(backend));
        consumers.push_back(std::make_unique<Boundary>(*this, true));
        return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
    }
public:
    TimedIRAction() { if (enabled) samples[0] = Sample::now(); }
    void irReady(uint64_t instructions) {
        if (!enabled) return;
        optimized_instructions = instructions;
        samples[3] = Sample::now();
        ready = true;
    }
    ~TimedIRAction() override {
        // IR input bypasses AST callbacks, and frontend failures have no
        // complete phase sequence. Neither is a zero-duration observation.
        if (!ready || !samples[1].wall || !samples[2].wall) return;
        auto end = Sample::now();
        llvm::json::Array phases;
        const char *names[] = {"frontend", "finalize_optimize", "ir_cleanup", "codegen"};
        for (unsigned i = 0; i != 4; ++i) {
            auto next = i == 3 ? end : samples[i+1];
            phases.push_back(llvm::json::Object{{"name", names[i]},
                {"wall_ns", next.wall-samples[i].wall}, {"cpu_ns", next.cpu-samples[i].cpu}});
        }
        llvm::json::Object result{{"pid", getpid()}, {"source", source},
            {"start_ns", samples[0].wall}, {"end_ns", end.wall},
            {"unfinalized_instructions", unfinalized_instructions},
            {"optimized_instructions", optimized_instructions}, {"phases", std::move(phases)}};
        std::string line;
        llvm::raw_string_ostream stream(line);
        stream << "distcc compiler phases: " << llvm::json::Value(std::move(result)) << '\n';
        llvm::errs() << line;
    }
};
// This is an optional extra CPU, never a queued request. The enclosing
// compilation already owns one lane and can always finish without another.
int try_lane() {
    const char *path = getenv("DISTCC_BROKER");
    sockaddr_un address{};
    if (!path || strlen(path) >= sizeof(address.sun_path)) return -1;
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct { request header; dcc_work_hint hint; } message{};
    message.header = {BROKER_VERSION, TRY_LOCAL};
    reply response{};
    if (!connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) &&
        send(fd, &message, sizeof(message), MSG_NOSIGNAL) == sizeof(message) &&
        recv(fd, &response, sizeof(response), MSG_TRUNC) == sizeof(response) &&
        !response.status && response.slots == 1) return fd;
    close(fd);
    return -1;
}

bool emit(clang::CompilerInstance &compiler, llvm::Module &module,
          llvm::StringRef output, int descriptor = -1) {
    std::unique_ptr<llvm::raw_pwrite_stream> stream;
    if (descriptor >= 0)
        stream = std::make_unique<llvm::raw_fd_ostream>(descriptor, false);
    else
        stream = compiler.createOutputFile(output, true, true, true);
    if (!stream) return false;
    // IPO and all requested LLVM optimization passes have already run on the
    // complete module. Each part still receives the original target/codegen options.
    auto options = compiler.getCodeGenOpts();
    options.DisableLLVMPasses = true;
    clang::emitBackendOutput(compiler, options, compiler.getTarget().getDataLayoutString(),
        &module, clang::Backend_EmitObj, compiler.getFileManager().getVirtualFileSystemPtr(),
        std::move(stream));
    // ExecuteAction restores LLVM's previous diagnostic handler. Machine-code
    // errors emitted afterwards belong to that handler, not only Clang's.
    bool ok = !compiler.getDiagnostics().hasErrorOccurred() &&
              !module.getContext().getDiagHandlerPtr()->HasErrors;
    compiler.clearOutputFiles(!ok);
    return ok;
}

std::string fd_path(int fd) { return "/proc/self/fd/" + std::to_string(fd); }

}

void dcc_clang_codegen_init() {
    void *memory = mmap(nullptr, sizeof(Measurements), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory != MAP_FAILED) measured = new(memory) Measurements;
}

std::optional<bool> dcc_clang_codegen(clang::CompilerInstance &compiler,
    const std::function<unsigned(unsigned)> &reserve) {
    const char *enabled = getenv("DISTCC_CLANG_CODEGEN");
    auto &frontend = compiler.getFrontendOpts();
    const auto &codegen = compiler.getCodeGenOpts();
    // These actions produce other artifacts, or have wrappers around the
    // frontend action; their original Clang path owns those contracts.
    if (!enabled || strcmp(enabled, "1") || (!reserve && !getenv("DISTCC_BROKER")) ||
        frontend.ProgramAction != clang::frontend::EmitObj || frontend.ShowHelp || frontend.ShowVersion ||
        frontend.OutputFile.empty() || frontend.OutputFile == "-" ||
        frontend.FixAndRecompile || frontend.EmitSymbolGraph || !frontend.ASTMergeFiles.empty() ||
        codegen.PrepareForLTO || codegen.PrepareForThinLTO || !codegen.SplitDwarfFile.empty() ||
        !llvm::Triple(compiler.getTargetOpts().Triple).isOSBinFormatELF()) return std::nullopt;

    compiler.LoadRequestedPlugins();
    if (!frontend.LLVMArgs.empty()) {
        llvm::SmallVector<const char *> args{"clang (LLVM option parsing)"};
        for (const auto &arg : frontend.LLVMArgs) args.push_back(arg.c_str());
        args.push_back(nullptr);
        llvm::cl::ParseCommandLineOptions(args.size()-1, args.data(), "", nullptr,
                                          &compiler.getVirtualFileSystem());
    }
    TimedIRAction action;
    std::unique_ptr<llvm::Module> module;
    auto dispose = llvm::scope_exit([&] {
        if (frontend.DisableFree) {
            if (!module) module = action.takeModule();
            llvm::BuryPointer(std::move(module));
            llvm::BuryPointer(action.takeLLVMContext());
        }
    });
    if (!compiler.ExecuteAction(action)) return false;
    module = action.takeModule();
    if (!module) return false;
    uint64_t total = 0, largest = 0;
    unsigned functions = 0;
    for (const auto &function : *module) {
        uint64_t size = function.getInstructionCount();
        total += size;
        largest = std::max(largest, size);
        functions += !function.isDeclarationForLinker();
    }
    action.irReady(total);
    unsigned useful = largest ? std::min<uint64_t>(functions, total/largest) : 1;
    std::vector<int> lanes;
    unsigned extra = 0;
    auto release = llvm::scope_exit([&] {
        for (int fd : lanes) close(fd);
        if (reserve && extra) reserve(0);
    });
    double serial = 0, overhead = 0;
    bool overhead_known = false;
    if (measured) {
        auto instructions = measured->instructions.load();
        auto parts = measured->parts.load();
        if (instructions) serial = double(measured->codegen_ns.load())*total/instructions;
        if (parts) {
            overhead = double(measured->overhead_ns.load())/parts;
            overhead_known = true;
        }
    }
    // Learn both serial codegen work and split/merge overhead from completed
    // jobs in this compiler environment, rather than a source-size threshold.
    unsigned selected = dcc_codegen_parallelism(serial, overhead, useful, overhead_known);
    if (reserve) {
        if (selected > 1) extra = reserve(selected-1);
    } else {
        while (lanes.size()+1 < selected) {
            int fd = try_lane();
            if (fd < 0) break;
            lanes.push_back(fd);
        }
        extra = lanes.size();
    }
    if (getenv("DISTCC_VERBOSE"))
        llvm::errs() << "distcc codegen plan: serial_ns=" << serial << " overhead_ns=" << overhead
                     << " useful=" << useful << " selected=" << selected
                     << " available=" << extra+1 << '\n';
    if (!extra) {
        auto start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        bool ok = emit(compiler, *module, frontend.OutputFile);
        if (ok && measured) {
            measured->codegen_ns.fetch_add(clock_ns(CLOCK_PROCESS_CPUTIME_ID)-start);
            measured->instructions.fetch_add(total);
        }
        return ok;
    }

    auto prepare_start = clock_ns();
    std::vector<int> parts;
    auto cleanup = llvm::scope_exit([&] {
        for (int fd : parts) close(fd);
    });
    std::vector<pid_t> children;
    uint64_t prepared = 0;
    auto names = dcc_split_module(*module, extra+1, [&](llvm::Module &part, bool last) -> llvm::Error {
        unsigned i = parts.size();
        int fd = memfd_create("distcc-codegen", MFD_CLOEXEC);
        if (fd < 0) return llvm::errorCodeToError(std::error_code(errno, std::generic_category()));
        parts.push_back(fd);
        if (last) {
            prepared = clock_ns()-prepare_start;
            return emit(compiler, part, {}, fd) ? llvm::Error::success() :
                llvm::createStringError("cannot emit LLVM partition");
        }
        // Fork while the splitter's module is alive. The child consumes its
        // COW snapshot as the parent prepares the next partition; no bitcode
        // serialization, temporary input file or second LLVM parser is needed.
        llvm::outs().flush(); llvm::errs().flush();
        pid_t parent = getpid();
        pid_t child = fork();
        if (child < 0) return llvm::errorCodeToError(std::error_code(errno, std::generic_category()));
        if (!child) {
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(1);
            for (unsigned j = 0; j < lanes.size(); ++j) if (j != i) close(lanes[j]);
            for (int other : parts) if (other != fd) close(other);
            bool success = emit(compiler, part, {}, fd);
            llvm::outs().flush(); llvm::errs().flush();
            _exit(success ? 0 : 1);
        }
        children.push_back(child);
        return llvm::Error::success();
    });
    bool ok = bool(names);
    if (!names) llvm::logAllUnhandledErrors(names.takeError(), llvm::errs());
    for (pid_t child : children) {
        int status = 0;
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        ok &= result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (!ok) return false;
    ok = false;
    auto remove_failed_output = llvm::scope_exit([&] {
        if (!ok) llvm::sys::fs::remove(frontend.OutputFile);
    });
    auto merge_start = clock_ns();
    // Internal linking owns the same CPU budget as the parts it merges. LLD's
    // hardware-thread default would otherwise oversubscribe unleased CPUs.
    std::vector<std::string> link{DCC_LLD_PATH, "--threads=" + std::to_string(extra+1),
                                 "-r", "-o", frontend.OutputFile};
    for (int fd : parts) link.push_back(fd_path(fd));
    // LLD is already resident for local driver jobs. This compiler child owns
    // one merge, so its linker state cannot leak into another invocation.
    llvm::SmallVector<const char *> link_args;
    for (const auto &argument : link) link_args.push_back(argument.c_str());
    if (lld::lldMain(link_args, llvm::outs(), llvm::errs(),
                     {{lld::Gnu, &lld::elf::link}}).retCode) return false;
    // LLD owns output-file creation. Read its merged object before opening
    // Clang's temporary replacement, then restore local symbols with the
    // resident library. No symbol-list file or external objcopy is needed.
    auto binary = llvm::object::createBinary(frontend.OutputFile);
    if (!binary) { llvm::logAllUnhandledErrors(binary.takeError(), llvm::errs()); return false; }
    llvm::objcopy::ConfigManager config;
    config.Common.OutputFilename = frontend.OutputFile;
    for (const auto &name : *names) {
        auto matcher = llvm::objcopy::NameOrPattern::create(name, llvm::objcopy::MatchStyle::Literal,
            [](llvm::Error error) { return error; });
        if (auto error = config.Common.SymbolsToLocalize.addMatcher(std::move(matcher))) {
            llvm::logAllUnhandledErrors(std::move(error), llvm::errs()); return false;
        }
    }
    auto output = compiler.createOutputFile(frontend.OutputFile, true, true, true);
    if (!output) return false;
    auto error = llvm::objcopy::executeObjcopyOnBinary(config, *binary->getBinary(), *output);
    output->flush();
    ok = !error;
    if (error) llvm::logAllUnhandledErrors(std::move(error), llvm::errs());
    output.reset();
    ok &= !compiler.getDiagnostics().hasErrorOccurred();
    compiler.clearOutputFiles(!ok);
    if (!ok) return false;
    if (measured) {
        measured->overhead_ns.fetch_add(prepared+clock_ns()-merge_start);
        measured->parts.fetch_add(parts.size());
    }
    if (getenv("DISTCC_VERBOSE"))
        llvm::errs() << "distcc codegen: " << parts.size() << " parts, " << total
                     << " instructions, " << codegen.MainFileName << '\n';
    return true;
}
