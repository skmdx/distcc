/* The client planner reads frozen indexes. Execution children own their inputs and outputs. */
#include "lto_backend.h"
#include "compiler_command.h"
#include "../fd_io.h"
#include "../sandbox_child.h"
#include <lld/Common/Driver.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/ModuleSummaryIndex.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/ObjCopy/CommonConfig.h>
#include <llvm/ObjCopy/ELF/ELFConfig.h>
#include <llvm/ObjCopy/ELF/ELFObjcopy.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "../job.h"
#include <unistd.h>

LLD_HAS_DRIVER(elf)
using namespace llvm;
namespace {
bool reserve_descriptors(rlim_t needed) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0) return false;
    auto target = std::min(needed, limit.rlim_max);
    if (limit.rlim_cur >= target) return true;
    limit.rlim_cur = target;
    return setrlimit(RLIMIT_NOFILE, &limit) == 0;
}
}

bool dcc_lto_init(bool client) {
    return (client || reserve_descriptors(DCC_JOB_INPUT_MAX)) && dcc_init_compiler_sandbox(client);
}

namespace {
bool enter_output(StringRef directory) {
    if (chdir(directory.str().c_str())) return false;
    // Open all inputs before giving up host credentials, including when a
    // root Portage install runs beneath a different user's private directory.
    if (chroot(".") && (errno != EPERM || unshare(CLONE_NEWUSER) || chroot("."))) return false;
    return !chdir("/") && dcc_restrict_compiler_child();
}

Error merge(const json::Array &parts, const json::Array &locals, int capture) {
    SmallVector<std::string> command{"ld.lld","-r","-o",locals.empty() ? "merged.o" : "-"};
    for (const auto &part : parts) {
        auto name = part.getAsString();
        if (!name) return createStringError("invalid merge input");
        command.push_back(name->str());
    }
    SmallVector<const char *> args;
    for (auto &word : command) args.push_back(word.c_str());
    if (auto e = dcc_link_elf(args, capture)) return e;
    if (locals.empty()) return Error::success();
    std::vector<std::string> names;
    for (const auto &local : locals) {
        auto name = local.getAsString();
        if (!name) return createStringError("invalid local symbol");
        names.push_back(name->str());
    }
    std::error_code ec;
    raw_fd_ostream output("merged.o", ec);
    if (ec) return errorCodeToError(ec);
    if (Error e = dcc_localize_elf(capture, names, output)) return e;
    output.close();
    return errorCodeToError(output.error());
}

int fail(StringRef message) {
    errs() << "distcc ThinLTO: " << message << '\n';
    return 1;
}
}

Error dcc_link_elf(ArrayRef<const char *> args, int capture) {
    // "-o -" uses LLVM's process stdout. Each caller is a single-job
    // compiler child. Flush both boundaries, including LLD's last partial block.
    int saved = -1;
    if (capture >= 0) {
        outs().flush();
        saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
        if (saved < 0) return errorCodeToError(errnoAsErrorCode());
        if (dup2(capture, STDOUT_FILENO) < 0) {
            auto ec = errnoAsErrorCode();
            close(saved);
            return errorCodeToError(ec);
        }
    }
    int status = lld::lldMain(args, outs(), errs(), {{lld::Gnu, &lld::elf::link}}).retCode;
    if (saved >= 0) {
        outs().flush();
        int restored = dup2(saved, STDOUT_FILENO);
        close(saved);
        if (restored < 0 || outs().has_error()) return createStringError("capturing relocatable link output failed");
    }
    return status ? createStringError("relocatable link failed") : Error::success();
}

Error dcc_localize_elf(int fd, ArrayRef<std::string> names, raw_ostream &output) {
    auto buffer = MemoryBuffer::getOpenFile(fd, "relocatable", -1, false);
    if (!buffer) return errorCodeToError(buffer.getError());
    auto binary = object::createBinary((*buffer)->getMemBufferRef());
    if (!binary) return binary.takeError();
    auto *elf = dyn_cast<object::ELFObjectFileBase>(binary->get());
    if (!elf) return createStringError("expected a relocatable ELF object");
    objcopy::CommonConfig config;
    for (const auto &name : names)
        if (auto e = config.SymbolsToLocalize.addMatcher(objcopy::NameOrPattern::create(
                name, objcopy::MatchStyle::Literal, [](Error e) { return e; }))) return e;
    return objcopy::elf::executeObjcopyOnBinary(config, {}, *elf, output);
}

int dcc_lto_plan(const char *description, int output) {
    auto buffer = MemoryBuffer::getFile(description);
    if (!buffer) return fail(buffer.getError().message());
    auto request = json::parse((*buffer)->getBuffer());
    if (!request) return fail(toString(request.takeError()));
    auto *jobs = request->getAsArray();
    if (!jobs) return fail("expected ThinLTO input descriptions");
    json::Array weights;
    for (auto &value : *jobs) {
        auto *job = value.getAsObject();
        if (!job || !job->getString("source") || !job->getString("index"))
            return fail("missing ThinLTO source or index");
        auto source = *job->getString("source");
        auto index = getModuleSummaryIndexForFile(*job->getString("index"));
        if (!index) return fail(toString(index.takeError()));
        int64_t instructions = 0, functions = 0, largest = 0;
        for (auto &entry : **index) {
            for (auto &summary : entry.second.getSummaryList()) {
                auto *function = dyn_cast<FunctionSummary>(summary.get());
                if (!function || !(**index).isGlobalValueLive(function)) continue;
                bool primary = function->modulePath() == source;
                if (!primary && function->importType() != GlobalValueSummary::Definition) continue;
                instructions += function->instCount();
                largest = std::max<int64_t>(largest, function->instCount());
                if (primary && function->linkage() != GlobalValue::AvailableExternallyLinkage)
                    ++functions;
            }
        }
        json::Object row{{"instructions", instructions}, {"functions", functions},
                         {"largest_function", largest}};
        if (auto *args = job->getArray("driver")) {
            std::vector<std::string> words;
            SmallVector<const char *> command;
            for (auto &arg : *args) {
                auto word = arg.getAsString();
                if (!word) return fail("invalid template argument");
                words.push_back(word->str());
            }
            if (words.empty()) return fail("missing template compiler");
            for (auto &word : words) command.push_back(word.c_str());
            row["cc1"] = json::Array(dcc_lto_cc1(command));
        }
        weights.push_back(std::move(row));
    }
    if (ftruncate(output, 0) || lseek(output, 0, SEEK_SET) < 0) return fail(strerror(errno));
    raw_fd_ostream stream(output, false);
    stream << json::Value(std::move(weights));
    stream.flush();
    auto error = stream.error();
    stream.clear_error();
    return error ? fail(error.message()) : 0;
}

int dcc_lto_local(const char *description, unsigned operation) {
    auto buffer = MemoryBuffer::getFile(description);
    if (!buffer) return fail(buffer.getError().message());
    auto data = json::parse((*buffer)->getBuffer());
    if (!data) return fail(toString(data.takeError()));
    auto *job = data->getAsObject();
    if (!job) return fail("expected job description");
    auto dir = sys::path::parent_path(description);
    close(3); close(STDIN_FILENO);
    if (operation == 'M') {
        auto *parts = job->getArray("parts"), *names = job->getArray("locals");
        if (!parts || !names) return fail("invalid merge job");
        int capture = names->empty() ? -1 : memfd_create("distcc-relocatable", MFD_CLOEXEC);
        if (!names->empty() && capture < 0) return fail(strerror(errno));
        auto release = scope_exit([&] { if (capture >= 0) close(capture); });
        if (!enter_output(dir)) return fail(strerror(errno));
        if (auto error = merge(*parts, *names, capture)) return fail(toString(std::move(error)));
        return 0;
    }
    LtoInputs inputs;
    auto cwd = job->getString("cwd");
    auto index = job->getString("index");
    auto count = job->getInteger("partitions");
    auto *files = job->getObject("files");
    auto *args = job->getArray("args");
    if (!cwd || !index || !count || *count < 1 || *count > UINT_MAX || !args || !files)
        return fail("invalid partition job");
    inputs.cwd = cwd->str(); inputs.partitions = *count; inputs.manifest = "partitions.json";
    inputs.localIndex = index->str();
    if (!reserve_descriptors(files->size() + 64)) return fail(strerror(errno));
    for (auto &item : *files) {
        auto path = item.second.getAsString();
        if (!path) return fail("invalid snapshot");
        int fd = open(path->str().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return fail(strerror(errno));
        inputs.files[item.first] = fd;
    }
    std::vector<std::string> words;
    SmallVector<const char *> command;
    for (const auto &arg : *args) {
        auto word = arg.getAsString();
        if (!word) return fail("invalid compiler argument");
        words.push_back(word->str());
    }
    for (auto &word : words) command.push_back(word.c_str());
    int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (sink < 0 || !enter_output(dir)) return fail(strerror(errno));
    return dcc_lto_compile(inputs, command, sink);
}

int dcc_lto_remote(int fd, unsigned operation) {
    CompilerCommand command;
    if (operation != 'T' || !command.read(fd, 3) || !command[0][0] || !command[0][1] || command[0][2])
        return fail("invalid backend request");
    LtoInputs inputs;
    inputs.cwd = command[0][0];
    inputs.output = command[0][1];
    char **name = command[2];
    while (*name) {
        int descriptors[DCC_IPC_FDS_MAX];
        unsigned received = DCC_IPC_FDS_MAX;
        unsigned char count = 0;
        if (dcc_recv_fds(0, &count, 1, descriptors, &received) != 1 || !count || count != received)
            return fail("invalid input descriptor batch");
        for (unsigned i = 0; i < count; ++i) {
            if (!*name || !inputs.files.try_emplace(*name++, descriptors[i]).second)
                return fail("unexpected input descriptor");
        }
    }
    if (dup2(5, STDIN_FILENO) < 0) return fail(strerror(errno));
    close(5);
    close(fd);
    SmallVector<const char *> args;
    for (char **p = command[1]; *p; ++p) args.push_back(*p);
    if (!dcc_restrict_compiler_child()) return fail("cannot confine compiler");
    int status = dcc_lto_compile(inputs, args, 4);
    close(4);
    return status;
}
