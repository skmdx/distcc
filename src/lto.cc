/* LLVM owns IR parsing, symbol resolution summaries and module partitioning. */
#include "lto_partition.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static Expected<json::Value> plan(StringRef filename) {
  auto buffer = MemoryBuffer::getFile(filename);
  if (!buffer)
    return errorCodeToError(buffer.getError());
  auto request = json::parse((*buffer)->getBuffer());
  if (!request)
    return request.takeError();
  auto *jobs = request->getAsArray();
  if (!jobs)
    return createStringError("expected ThinLTO input descriptions");
  json::Array result;
  for (auto &value : *jobs) {
    auto *job = value.getAsObject();
    if (!job || !job->getString("source") || !job->getString("index"))
      return createStringError("missing ThinLTO source or index");
    auto index = getModuleSummaryIndexForFile(*job->getString("index"));
    if (!index)
      return index.takeError();
    int64_t instructions = 0, functions = 0;
    for (auto &entry : **index) {
      for (auto &summary : entry.second.getSummaryList()) {
        auto *function = dyn_cast<FunctionSummary>(summary.get());
        if (!function || !(**index).isGlobalValueLive(summary.get()))
          continue;
        if (summary->modulePath() == *job->getString("source")) {
          instructions += function->instCount();
          if (summary->linkage() != GlobalValue::AvailableExternallyLinkage)
            ++functions;
        } else if (summary->importType() == GlobalValueSummary::Definition) {
          instructions += function->instCount();
        }
      }
    }
    result.push_back(json::Object{{"instructions", instructions},
                                  {"functions", functions}});
  }
  return json::Value(std::move(result));
}

int main(int argc, char **argv) {
  auto run = [&]() -> Expected<json::Value> {
    if (argc == 3 && StringRef(argv[1]) == "plan")
      return plan(argv[2]);
    if (argc == 5 && StringRef(argv[1]) == "split") {
      unsigned count;
      if (!StringRef(argv[3]).getAsInteger(10, count) && count) {
        LLVMContext context;
        SMDiagnostic diagnostic;
        auto module = parseIRFile(argv[2], diagnostic, context);
        if (!module) {
          diagnostic.print("distcc-lto", errs());
          return createStringError("cannot read prepared ThinLTO module");
        }
        return dcc_partition_module(*module, count, argv[4]);
      }
    }
    return createStringError("invalid distcc-lto arguments");
  };
  auto result = run();
  if (!result) {
    logAllUnhandledErrors(result.takeError(), errs(), "distcc-lto: ");
    return 1;
  }
  outs() << std::move(*result) << '\n';
  return 0;
}
