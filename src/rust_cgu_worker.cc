/* Isolated LLVM backend for a single Rust codegen unit. */
#include "rust_cgu.h"
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
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>

using namespace llvm;
namespace {
codegen::RegisterCodeGenFlags codegen_flags;
ModulePassManager pipeline(PassBuilder &pb, StringRef kind, int level, int phase) {
    if (level < 0 || level > 3 || phase < 0 || phase > int(ThinOrFullLTOPhase::FullLTOPostLink))
        throw std::runtime_error("invalid LLVM pipeline");
    auto opt = static_cast<OptimizationLevel>(level);
    auto lto = static_cast<ThinOrFullLTOPhase>(phase);
    if (kind == "O0")
        return pb.buildO0DefaultPipeline(opt, lto);
    if (kind == "per-module")
        return pb.buildPerModuleDefaultPipeline(opt, lto);
    if (kind == "module-opt")
        return pb.buildModuleOptimizationPipeline(opt, lto);
    if (kind == "thin-postlink")
        return pb.buildThinLTODefaultPipeline(opt, nullptr);
    throw std::runtime_error("unknown CGU pipeline");
}
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
    if (!spec || cgu::string(*spec, "llvm") != LLVM_VERSION_STRING)
        throw std::runtime_error("incompatible LLVM backend");
    std::vector<std::string> flag_strings;
    auto *flags = spec->getArray("flags");
    if (!flags)
        throw std::runtime_error("missing CGU flags");
    for (const auto &flag : *flags) {
        auto s = flag.getAsString();
        if (!s || !cgu::supported_flag(*s))
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
    if (kind == "object") {
        legacy::PassManager pm;
        pm.add(createTargetTransformInfoWrapperPass(tm->getTargetIRAnalysis()));
        pm.add(new TargetLibraryInfoWrapperPass(tli));
        pm.add(new RuntimeLibraryInfoWrapper(triple, opts.ExceptionModel, opts.FloatABIType, opts.EABIVersion,
                                             opts.MCOptions.ABIName, opts.VecLib));
        if (tm->addPassesToEmitFile(pm, out, nullptr, CodeGenFileType::ObjectFile))
            throw std::runtime_error("target cannot emit objects");
        pm.run(m);
    } else {
        PassInstrumentationCallbacks pic;
        PassBuilder pb(tm.get(), pto, std::nullopt, &pic);
        LoopAnalysisManager lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager cgam;
        ModuleAnalysisManager mam;
        fam.registerPass([&] { return pb.buildDefaultAAPipeline(); });
        fam.registerPass([&] { return TargetLibraryAnalysis(tli); });
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        auto passes = pipeline(pb, kind, cgu::integer(*spec, "level"), cgu::integer(*spec, "phase"));
        if (cgu::pipeline_name(passes, pic) != cgu::string(*spec, "pipeline"))
            throw std::runtime_error("compiler pipeline has additional passes");
        passes.run(m, mam);
        WriteBitcodeToFile(m, out, true);
    }
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
            outs() << LLVM_VERSION_STRING;
            for (auto &t : TargetRegistry::targets())
                if (t.hasTargetMachine() && t.hasMCAsmBackend())
                    outs() << ' ' << t.getName();
            outs() << '\n';
        } else if (argc == 3 && StringRef(argv[1]) == "--target-name") {
            std::string reason;
            const auto *target = TargetRegistry::lookupTarget(Triple(Triple::normalize(argv[2])), reason);
            if (!target || !target->hasTargetMachine() || !target->hasMCAsmBackend())
                return 1;
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
