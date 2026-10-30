/* Optimize the whole translation unit, then use idle local CPU leases for
 * independent machine-code generation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "clang_codegen.h"
#include "../../src/broker_protocol.h"
#include "../../src/workload.h"
#include "../../src/lto_partition.h"
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/BackendUtil.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/TargetParser/Triple.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <new>
#include <time.h>

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
          llvm::StringRef output) {
    auto stream = compiler.createOutputFile(output, true, true, true);
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

bool tool(llvm::ArrayRef<std::string> arguments) {
    llvm::SmallVector<llvm::StringRef> args(arguments.begin(), arguments.end());
    return llvm::sys::ExecuteAndWait(args[0], args) == 0;
}
}

void dcc_clang_codegen_init() {
    void *memory = mmap(nullptr, sizeof(Measurements), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory != MAP_FAILED) measured = new(memory) Measurements;
}

std::optional<bool> dcc_clang_codegen(clang::CompilerInstance &compiler) {
    const char *enabled = getenv("DISTCC_CLANG_CODEGEN");
    auto &frontend = compiler.getFrontendOpts();
    const auto &codegen = compiler.getCodeGenOpts();
    // These actions produce other artifacts, or have wrappers around the
    // frontend action; their original Clang path owns those contracts.
    if (!enabled || strcmp(enabled, "1") || !getenv("DISTCC_BROKER") ||
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
    clang::EmitLLVMOnlyAction action;
    if (!compiler.ExecuteAction(action)) return false;
    auto module = action.takeModule();
    if (!module) return false;
    uint64_t total = 0, largest = 0;
    unsigned functions = 0;
    for (const auto &function : *module) {
        uint64_t size = function.getInstructionCount();
        total += size;
        largest = std::max(largest, size);
        functions += !function.isDeclarationForLinker();
    }
    unsigned useful = largest ? std::min<uint64_t>(functions, total/largest) : 1;
    std::vector<int> lanes;
    auto release = llvm::scope_exit([&] { for (int fd : lanes) close(fd); });
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
    while (lanes.size()+1 < selected) {
        int fd = try_lane();
        if (fd < 0) break;
        lanes.push_back(fd);
    }
    if (getenv("DISTCC_VERBOSE"))
        llvm::errs() << "distcc codegen plan: serial_ns=" << serial << " overhead_ns=" << overhead
                     << " useful=" << useful << " selected=" << selected
                     << " available=" << lanes.size()+1 << '\n';
    if (lanes.empty()) {
        auto start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        bool ok = emit(compiler, *module, frontend.OutputFile);
        if (ok && measured) {
            measured->codegen_ns.fetch_add(clock_ns(CLOCK_PROCESS_CPUTIME_ID)-start);
            measured->instructions.fetch_add(total);
        }
        return ok;
    }

    auto prepare_start = clock_ns();
    llvm::SmallString<256> directory;
    auto error = llvm::sys::fs::createUniqueDirectory("distcc-codegen", directory);
    if (error) { llvm::errs() << error.message() << '\n'; return false; }
    auto cleanup = llvm::scope_exit([&] {
        if (auto error = llvm::sys::fs::remove_directories(directory))
            llvm::errs() << "distcc codegen cleanup: " << error.message() << '\n';
    });
    std::vector<std::string> outputs;
    std::vector<pid_t> children;
    uint64_t prepared = 0;
    auto names = dcc_split_module(*module, lanes.size()+1, [&](llvm::Module &part, bool last) -> llvm::Error {
        unsigned i = outputs.size();
        outputs.push_back((directory+"/part-"+llvm::Twine(i)+".o").str());
        if (last) {
            prepared = clock_ns()-prepare_start;
            return emit(compiler, part, outputs[i]) ? llvm::Error::success() :
                llvm::createStringError("cannot emit LLVM partition");
        }
        // Fork while the splitter's module is alive. The child consumes its
        // COW snapshot as the parent prepares the next partition; no bitcode
        // serialization, temporary input file or second LLVM parser is needed.
        llvm::outs().flush(); llvm::errs().flush();
        pid_t child = fork();
        if (child < 0) return llvm::errorCodeToError(std::error_code(errno, std::generic_category()));
        if (!child) {
            for (unsigned j = 0; j < lanes.size(); ++j) if (j != i) close(lanes[j]);
            bool success = emit(compiler, part, outputs[i]);
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
    for (int fd : lanes) close(fd);
    lanes.clear();
    if (!ok) return false;
    auto merge_start = clock_ns();
    std::vector<std::string> link{DCC_LLD_PATH, "-r", "-o", frontend.OutputFile};
    link.insert(link.end(), outputs.begin(), outputs.end());
    if (!tool(link)) return false;
    std::string symbols = (directory+"/locals").str();
    llvm::raw_fd_ostream list(symbols, error);
    if (error) return false;
    for (const auto &name : *names) list << name << '\n';
    list.close();
    if (list.has_error()) return false;
    if (!tool({DCC_OBJCOPY_PATH, "--localize-symbols="+symbols, frontend.OutputFile})) return false;
    if (measured) {
        measured->overhead_ns.fetch_add(prepared+clock_ns()-merge_start);
        measured->parts.fetch_add(outputs.size());
    }
    if (getenv("DISTCC_VERBOSE"))
        llvm::errs() << "distcc codegen: " << outputs.size() << " parts, " << total
                     << " instructions, " << codegen.MainFileName << '\n';
    return true;
}
