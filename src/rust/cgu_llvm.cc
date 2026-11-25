/* LLVM Rust CGU boundaries. rustc retains its local pass managers, ThinLTO
 * summary writer, object streams and archive writer throughout the job. */
#include "rust/cgu.h"
#include "rust/cgu_hooks.h"
#include "rust/cgu_transport.h"
#include "fd_io.h"
#include "exitcode.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/Transforms/IPO/ThinLTOBitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/xxhash.h>
#include <atomic>
#include <mutex>
#include <map>
#include <llvm/Support/Endian.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace llvm;
namespace {
[[noreturn]] void fatal(const Twine &message);
struct File {
    int fd = -1;
    File() = default;
    explicit File(const char *name) : fd(memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING)) {}
    ~File() {
        if (fd >= 0)
            close(fd);
    }
};
struct Builder {
    PassBuilder *pb = nullptr;
    TargetMachine *tm = nullptr;
    PipelineTuningOptions tuning;
    bool custom = false;
};
thread_local Builder builder;
struct Emission {
    legacy::PassManagerBase *pm = nullptr;
    TargetMachine *tm = nullptr;
    raw_fd_ostream *object = nullptr;
};
thread_local Emission emission;
thread_local bool no_builtins;
std::vector<std::string> compiler_flags;
bool object_only;
bool plugin_lto;
const char *transport;
std::atomic<int> frontend_pipe{-1};
void backend_started() {
    if (frontend_pipe.load(std::memory_order_relaxed) < 0)
        return;
    int fd = frontend_pipe.exchange(-1);
    if (fd >= 0)
        close(fd);
}
void configure_environment(const char *client) {
    if (const char *started = getenv("DISTCC_RUST_CGU_STARTED")) {
        char *end;
        long fd = strtol(started, &end, 10);
        if (*started && !*end && fd >= 3 && fd <= INT_MAX && !fcntl(fd, F_SETFD, FD_CLOEXEC))
            frontend_pipe.store(fd);
        unsetenv("DISTCC_RUST_CGU_STARTED");
    }
    transport = strdup(client);
    object_only =
        getenv("DISTCC_RUST_CGU_OBJECT_ONLY") && !strcmp(getenv("DISTCC_RUST_CGU_OBJECT_ONLY"), "1");
    unsetenv("DISTCC_RUST_CGU_OBJECT_ONLY");
    plugin_lto = getenv("DISTCC_RUST_CGU_PLUGIN_LTO") && !strcmp(getenv("DISTCC_RUST_CGU_PLUGIN_LTO"), "1");
    unsetenv("DISTCC_RUST_CGU_PLUGIN_LTO");
    const char *previous = getenv("DISTCC_RUST_CGU_PREVIOUS");
    if (previous && *previous)
        setenv("LD_PRELOAD", previous, 1);
    else
        unsetenv("LD_PRELOAD");
    unsetenv("DISTCC_RUST_CGU_PREVIOUS");
    unsetenv("DISTCC_RUST_CGU_CLIENT");
}
bool supported(const TargetMachine &tm) {
    return tm.Options.MCOptions.SplitDwarfFile.empty() && !tm.getPGOOption() &&
           tm.getCodeModel() == CodeModel::Small;
}
[[noreturn]] void fatal(const Twine &message) {
    errs() << "distcc-rust-cgu: " << message << '\n';
    _exit(EXIT_DISTCC_FAILED);
}
struct Reply {
    std::unique_ptr<MemoryBuffer> buffer;
    StringRef object, bitcode;
    Reply(std::unique_ptr<MemoryBuffer> b, bool ir_only) : buffer(std::move(b)) {
        auto data = buffer->getBuffer();
        if (data.size() < 8)
            fatal("truncated CGU reply");
        auto length = support::endian::read64le(data.data());
        data = data.drop_front(8);
        if (length > data.size() || (ir_only ? length != 0 || data.empty() : length == 0))
            fatal("invalid object size in CGU reply");
        object = data.take_front(length);
        bitcode = data.drop_front(length);
    }
};
// Smaller modules cost less to emit in-process than to serialize and schedule.
constexpr unsigned min_remote_instructions = 128;
bool eligible(Module &m) {
    return transport && StringRef(m.getModuleIdentifier()).contains("cgu.") &&
           m.getInstructionCount() >= min_remote_instructions;
}
struct SocketPair {
    int fd[2];
    SocketPair() {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fd))
            fatal(strerror(errno));
    }
    void sent() { close(fd[1]); fd[1] = -1; }
    ~SocketPair() { close(fd[0]); if (fd[1] >= 0) close(fd[1]); }
};

class Dispatcher {
    SocketPair control;
    pid_t pid;
public:
    Dispatcher() {
        const char *args[] = {transport, "--rust-cgu-worker", nullptr};
        posix_spawn_file_actions_t actions;
        int rc = posix_spawn_file_actions_init(&actions);
        if (rc) fatal(strerror(rc));
        rc = posix_spawn_file_actions_adddup2(&actions, control.fd[1], STDIN_FILENO);
        if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
        if (!rc) rc = posix_spawn(&pid, transport, &actions, nullptr, const_cast<char **>(args), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc) fatal(strerror(rc));
        control.sent();
    }
    ~Dispatcher() {
        shutdown(control.fd[0], SHUT_WR);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    unsigned submit(int in, int &out, StringRef target, StringRef phase, StringRef module) const {
        dcc_cgu_request header{};
        auto field = [](auto &slot, StringRef value) {
            if (value.empty() || value.size() >= sizeof(slot)) fatal("invalid CGU request field");
            memcpy(slot, value.data(), value.size());
        };
        field(header.version, cgu::abi);
        field(header.target, target);
        field(header.phase, phase);
        header.identity = xxh3_64bits(module);
        SocketPair reply;
        const int fds[] = {in, reply.fd[1]};
        if (dcc_send_fds(control.fd[0], &header, sizeof(header), fds, 2)) fatal(strerror(errno));
        reply.sent();
        unsigned char status;
        unsigned count = 1;
        if (dcc_recv_fds(reply.fd[0], &status, 1, &out, &count) != 1)
            fatal("CGU dispatcher disconnected");
        if (count != (status ? 0U : 1U)) fatal("invalid CGU result descriptor count");
        return status;
    }
};

std::optional<Reply> dispatch(Module &m, cgu::Object spec) {
    const bool ir_only = spec.get("ir-only") != nullptr;
    File in("rust-cgu-input"), out;
    if (in.fd < 0)
        fatal(strerror(errno));
    auto kind = cgu::string(spec, "kind").str();
    auto target = cgu::string(spec, "backend").str();
    spec["id"] = m.getModuleIdentifier();
    if (!spec.get("no-builtins"))
        spec["no-builtins"] = int(no_builtins);
    json::Array flags;
    for (auto &flag : compiler_flags)
        flags.push_back(flag);
    spec["flags"] = std::move(flags);
    {
        raw_fd_ostream stream(in.fd, false);
        stream << json::Value(std::move(spec)) << '\n';
        WriteBitcodeToFile(m, stream, true);
        stream.flush();
        if (stream.has_error())
            fatal(stream.error().message());
    }
    if (fcntl(in.fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
        fatal(strerror(errno));
    static Dispatcher dispatcher;
    unsigned status = dispatcher.submit(in.fd, out.fd, target, kind, m.getModuleIdentifier());
    if (status == EXIT_LOCAL_COMPILE)
        return std::nullopt;
    if (status)
        fatal(Twine("backend failed for ") + m.getModuleIdentifier());
    struct stat st;
    if (fstat(out.fd, &st))
        fatal(strerror(errno));
    auto bytes = MemoryBuffer::getOpenFile(out.fd, m.getModuleIdentifier(), st.st_size, false);
    if (!bytes)
        fatal(bytes.getError().message());
    if (getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
        errs() << "distcc-rust-cgu: completed " + kind + " " + m.getModuleIdentifier() + "\n";
    return Reply(std::move(*bytes), ir_only);
}
// rustc can hand a module to a different codegen thread. Its identity owns
// the pending work until its object stream is opened.
struct Pending {
    cgu::Object spec;
    std::optional<Reply> reply;
    TargetMachine *tm;
    void run_locally(Module &m) {
        PipelineTuningOptions tuning;
        auto opts = tm->Options;
        cgu::configure(spec, opts, tuning);
        TargetLibraryInfoImpl tli(m.getTargetTriple());
        if (cgu::integer(spec, "no-builtins"))
            tli.disableAllFunctions();
        cgu::run_pipeline(m, *tm, tuning, tli, spec);
    }
    bool materialize(Module &m, ModuleAnalysisManager &am) {
        spec["return-ir"] = 1;
        reply = dispatch(m, spec);
        if (!reply)
            return false;
        auto ir = parseBitcodeFile(MemoryBufferRef(reply->bitcode, m.getModuleIdentifier()), m.getContext());
        if (!ir)
            fatal(toString(ir.takeError()));
        am.invalidate(m, PreservedAnalyses::none());
        m = std::move(**ir);
        return true;
    }
};
std::mutex pending_lock;
std::map<Module *, Pending> modules;
auto take(Module &m) {
    std::lock_guard<std::mutex> guard(pending_lock);
    return modules.extract(&m);
}
void remember(Module &m, Pending work) {
    std::lock_guard<std::mutex> guard(pending_lock);
    if (!modules.try_emplace(&m, std::move(work)).second)
        fatal("duplicate module submission");
}
struct RemotePass : PassInfoMixin<RemotePass> {
    static bool isRequired() { return true; }
    ModulePassManager local;
    TargetMachine *tm;
    bool frontend = false;
    cgu::Object spec;
    RemotePass(ModulePassManager &&p, TargetMachine *machine, cgu::Object s)
        : local(std::move(p)), tm(machine), spec(std::move(s)) {}
    PreservedAnalyses run(Module &m, ModuleAnalysisManager &am) {
        if (frontend || (!spec.get("tail") && !eligible(m)))
            return local.run(m, am);
        // These final pipelines consume an already-resolved module. Rust has
        // retained its pre-link bitcode separately and may still attach it as
        // module assembly immediately before native emission.
        bool late = spec.get("tail") != nullptr;
        spec["no-builtins"] = int(no_builtins);
        if (late)
            spec["initial-asm"] = int64_t(cgu::assembly_size(m));
        spec["return-ir"] = int(!late);
        if (plugin_lto)
            spec["ir-only"] = 1;
        Pending work{std::move(spec), {}, tm};
        if (!late && !work.materialize(m, am))
            return local.run(m, am);
        if (!plugin_lto)
            remember(m, std::move(work));
        return late ? PreservedAnalyses::all() : PreservedAnalyses::none();
    }
};
// LLVM flattens nested module managers as they are composed. At the
// outer rustc boundary the passes after our marker are the actual compiler
// continuation. Preserve their order on the worker, including named LLVM
// transformations added by rustc. A pass owning an untransportable stream or
// a private callback keeps the original manager on the frontend.
struct ModulePasses : ModulePassManager {
    static auto &list(ModulePassManager &pm) { return pm.*(&ModulePasses::Passes); }
};
void capture_tail(ModulePassManager &pm, Module &m) {
    if (!eligible(m) || !builder.pb)
        return;
    auto &passes = ModulePasses::list(pm);
    auto marker = llvm::find_if(passes, [](const auto &p) { return p->name() == RemotePass::name(); });
    if (marker == passes.end())
        return;
    using Model = detail::PassModel<Module, RemotePass, ModuleAnalysisManager>;
    auto &remote = static_cast<Model &>(**marker).Pass;
    if (builder.custom || (!plugin_lto && llvm::any_of(make_range(std::next(marker), passes.end()), [](const auto &p) {
            return p->name() == ThinLTOBitcodeWriterPass::name() || p->name() == BitcodeWriterPass::name();
        }))) {
        remote.frontend = true; // rustc needs this pre-link module for its own ThinLTO session.
        return;
    }
    // The linker plugin consumes bitcode. Return optimized IR at the marker
    // and leave rustc's writer and any subsequent passes on the frontend.
    if (plugin_lto || !object_only || !m.getTargetTriple().isOSBinFormatELF())
        return;
    auto *pic = builder.pb->getPassInstrumentationCallbacks();
    bool has_names = true;
    std::string tail;
    raw_string_ostream out(tail);
    for (auto it = std::next(marker); it != passes.end(); ++it) {
        if (it != std::next(marker))
            out << ',';
        (*it)->printPipeline(out, [&](StringRef type) {
            auto name = pic->getPassNameForClassName(type);
            if (name.empty())
                has_names = false;
            return name;
        });
    }
    if (!has_names)
        return;
    ModulePassManager portable;
    if (!tail.empty()) {
        if (auto err = builder.pb->parsePassPipeline(portable, tail)) {
            logAllUnhandledErrors(std::move(err), errs(), "distcc-rust-cgu: frontend continuation: ");
            return;
        }
    }
    remote.spec["tail"] = std::move(tail);
    passes.erase(std::next(marker), passes.end());
}

ModulePassManager pipeline(PassBuilder *pb, function_ref<ModulePassManager()> build, const char *kind,
                           OptimizationLevel level, ThinOrFullLTOPhase phase) {
    auto passes = build();
    if (pb != builder.pb || !builder.tm || !supported(*builder.tm) ||
        (StringRef(kind) == "thin-prelink" && !plugin_lto))
        return passes;
    auto spec = cgu::describe(*builder.tm, builder.tuning);
    spec["kind"] = StringRef(kind);
    spec["level"] = int(cgu::encode(level));
    spec["phase"] = int(phase);
    ModulePassManager extensions;
    pb->invokePipelineStartEPCallbacks(extensions, level);
    pb->invokeOptimizerLastEPCallbacks(extensions, level, phase);
    if (!extensions.isEmpty())
        return passes;
    ModulePassManager result;
    result.addPass(RemotePass(std::move(passes), builder.tm, std::move(spec)));
    return result;
}
void module(ModulePassManager &pm, Module &m, ModuleAnalysisManager &am) {
    // Compiler-side bitcode writers can observe the IR before opening an object stream.
    auto node = take(m);
    if (!node.empty()) {
        auto &work = node.mapped();
        if (work.reply || work.materialize(m, am))
            remember(m, std::move(work));
        else
            work.run_locally(m);
    } else {
        capture_tail(pm, m);
    }
}
void start_builder(PassBuilder *pb, TargetMachine *tm, PipelineTuningOptions tuning) {
    backend_started();
    builder = {pb, tm, tuning};
    no_builtins = false;
}
void start_emission(legacy::PassManagerBase *pm, TargetMachine *tm) {
    backend_started();
    emission = {pm, tm, nullptr};
    no_builtins = false;
}
bool emit(legacy::PassManager *pm, Module &m) {
    auto pending = emission;
    emission = {};
    if (pm == pending.pm && pending.object) {
        auto write_object = [&](const Reply &result) {
            *pending.object << result.object;
            if (getenv("DISTCC_VERBOSE") && !strcmp(getenv("DISTCC_VERBOSE"), "1"))
                errs() << "distcc-rust-cgu: emitted " + m.getModuleIdentifier() + "\n";
            return true;
        };
        auto node = take(m);
        Pending *work = node.empty() ? nullptr : &node.mapped();
        if (work && work->reply) {
            SmallVector<char, 0> final_ir;
            raw_svector_ostream stream(final_ir);
            WriteBitcodeToFile(m, stream, true);
            if (StringRef(final_ir.data(), final_ir.size()) == work->reply->bitcode)
                return write_object(*work->reply);
            // The compiler added target sections after materializing LLVM IR.
            // Its emitter owns those final edits. Never resubmit the module.
        } else if (work) {
            if (auto reply = dispatch(m, work->spec))
                return write_object(*reply);
            work->run_locally(m);
        } else if (eligible(m) && supported(*pending.tm)) {
            auto spec = cgu::describe(*pending.tm, PipelineTuningOptions());
            spec["kind"] = "object";
            if (auto reply = dispatch(m, std::move(spec)))
                return write_object(*reply);
        }
    }
    return false;
}
const cgu::Hooks hooks{
    module, [](PassBuilder *pb) { if (pb == builder.pb) builder.custom = true; },
    [](Module &m) { take(m); }, start_builder, pipeline,
    [](int n, const char *const *args) { compiler_flags.assign(args + 1, args + n); },
    [] { no_builtins = true; }, start_emission,
    [](raw_fd_ostream *os) { if (emission.pm) emission.object = os; }, emit
};
} // namespace

const cgu::Hooks *cgu::dcc_cgu_initialize(const char *client) {
    if (!matching_llvm())
        return nullptr;
    if (client)
        configure_environment(client);
    return &hooks;
}
