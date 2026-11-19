/* Isolated LLVM backend for a single Rust codegen unit. */
#include "rust/cgu.h"
#include "exitcode.h"
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/CodeGen/LibcallLoweringInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Endian.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>

using namespace llvm;
namespace {
codegen::RegisterCodeGenFlags codegen_flags;
void execute() {
    auto buffer = MemoryBuffer::getFile("/input", false, false);
    if (!buffer)
        throw std::system_error(buffer.getError());
    auto [header, bc] = (*buffer)->getBuffer().split('\n');
    if (bc.empty() || header.size() > 1024 * 1024)
        throw std::runtime_error("invalid CGU request");
    auto parsed = json::parse(header);
    if (!parsed)
        throw std::runtime_error(toString(parsed.takeError()));
    auto *spec = parsed->getAsObject();
    if (!spec || cgu::string(*spec, "llvm") != cgu::abi)
        throw std::runtime_error("incompatible LLVM backend");
    std::vector<std::string> flag_strings;
    auto *flags = spec->getArray("flags");
    if (!flags)
        throw std::runtime_error("missing CGU flags");
    for (const auto &flag : *flags) {
        auto s = flag.getAsString();
        if (!s)
            throw std::runtime_error("unsupported LLVM flag");
        flag_strings.push_back(s->str());
    }
    std::vector<const char *> argv{"distcc-rust-cgu"};
    for (auto &arg : flag_strings)
        argv.push_back(arg.c_str());
    if (!cl::ParseCommandLineOptions(argv.size(), argv.data(), "", &errs()))
        throw std::runtime_error("LLVM options");
    Triple triple(cgu::string(*spec, "triple"));
    std::string error;
    const auto *target = TargetRegistry::lookupTarget(triple, error);
    if (!target || cgu::string(*spec, "backend") != target->getName())
        throw std::runtime_error("CGU target unavailable");
    auto opts = codegen::InitTargetOptionsFromCodeGenFlags(triple);
    PipelineTuningOptions pto;
    cgu::configure(*spec, opts, pto);
    auto tm = std::unique_ptr<TargetMachine>(
        target->createTargetMachine(triple, cgu::string(*spec, "cpu"), cgu::string(*spec, "features"), opts,
                                    static_cast<Reloc::Model>(cgu::integer(*spec, "reloc")),
                                    static_cast<CodeModel::Model>(cgu::integer(*spec, "model")),
                                    static_cast<CodeGenOptLevel>(cgu::integer(*spec, "cgopt"))));
    if (!tm)
        throw std::runtime_error("cannot create target machine");
    LLVMContext context;
    auto module = parseBitcodeFile(MemoryBufferRef(bc, cgu::string(*spec, "id")), context);
    if (!module)
        throw std::runtime_error(toString(module.takeError()));
    auto &m = **module;
    if (m.getTargetTriple() != triple || m.getDataLayout() != tm->createDataLayout())
        throw std::runtime_error("incompatible module layout");
    std::error_code ec;
    raw_fd_ostream out("/output", ec);
    if (ec)
        throw std::system_error(ec);
    auto kind = cgu::string(*spec, "kind");
    TargetLibraryInfoImpl tli(triple);
    if (cgu::integer(*spec, "no-builtins"))
        tli.disableAllFunctions();
    SmallVector<char, 0> optimized;
    if (kind != "object") {
        cgu::run_pipeline(m, *tm, pto, tli, *spec);
        if (cgu::integer(*spec, "return-ir")) {
            raw_svector_ostream bitcode(optimized);
            WriteBitcodeToFile(m, bitcode, true);
        }
    }
    char header_bytes[8];
    SmallVector<char, 0> object;
    raw_svector_ostream native(object);
    {
        legacy::PassManager pm;
        pm.add(createTargetTransformInfoWrapperPass(tm->getTargetIRAnalysis()));
        pm.add(new TargetLibraryInfoWrapperPass(tli));
#if LLVM_VERSION_MAJOR >= 23
        pm.add(new RuntimeLibraryInfoWrapper(triple, opts.ExceptionModel, opts.FloatABIType, opts.EABIVersion,
                                             opts.MCOptions.ABIName, opts.VecLib));
#endif
        if (tm->addPassesToEmitFile(pm, native, nullptr, CodeGenFileType::ObjectFile))
            throw std::runtime_error("target cannot emit objects");
        pm.run(m);
    }
    support::endian::write64le(header_bytes, object.size());
    out.write(header_bytes, sizeof(header_bytes));
    out.write(object.data(), object.size());
    out.write(optimized.data(), optimized.size());
    out.close();
    if (out.has_error())
        throw std::system_error(out.error());
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (!cgu::matching_llvm())
            throw std::runtime_error("installed LLVM differs from CGU build");

        InitializeAllTargetInfos();
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmPrinters();
        InitializeAllAsmParsers();
        if (argc == 2 && StringRef(argv[1]) == "--capabilities") {
            outs() << cgu::abi;
            for (auto &t : TargetRegistry::targets())
                if (t.hasTargetMachine() && t.hasMCAsmBackend())
                    outs() << ' ' << t.getName();
            outs() << '\n';
        } else if (argc == 4 && StringRef(argv[1]) == "--target-name") {
            std::string reason;
            const auto *target = TargetRegistry::lookupTarget(Triple(Triple::normalize(argv[3])), reason);
            if (StringRef(argv[2]) != cgu::abi || !target || !target->hasTargetMachine() ||
                !target->hasMCAsmBackend())
                return EXIT_LOCAL_COMPILE;
            outs() << target->getName();
        } else if (argc == 2 && StringRef(argv[1]) == "--execute")
            execute();
        else
            return 1;
        return 0;
    } catch (const std::exception &e) {
        errs() << "distcc-rust-cgu: " << e.what() << '\n';
        return 1;
    }
}
