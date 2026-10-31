/* Execute local driver jobs with resident libraries and isolated compiler state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/ToolChain.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <lld/Common/Driver.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <functional>
#include "clang_codegen.h"
#include "clang_compiler.h"
#include "compiler_command.h"

LLD_HAS_DRIVER(elf)

extern "C" int dcc_clang_frontend(char **, char ***, char ***);

namespace {
const dcc_argv_file_api *command_api;
int isolated(const std::function<int()> &run) {
    llvm::outs().flush(); llvm::errs().flush();
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        int status = run();
        llvm::outs().flush(); llvm::errs().flush();
        _exit(status);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

class ResidentCommand : public clang::driver::Command {
    bool single;
public:
    ResidentCommand(const Command &command, bool single) : Command(command), single(single) {}
    int Execute(llvm::ArrayRef<std::optional<llvm::StringRef>> redirects,
                std::string *error, bool *failed) const override {
        if (!redirects.empty())
            return Command::Execute(redirects, error, failed);
        auto link = [&] {
            llvm::SmallVector<const char *> command{getExecutable()};
            command.append(getArguments());
            return lld::lldMain(command, llvm::outs(), llvm::errs(),
                                {{lld::Gnu, &lld::elf::link}}).retCode;
        };
        return single ? link() : isolated(link);
    }
};

int run(llvm::ArrayRef<const char *> args) {
    int status = 1;
    {
        clang::DiagnosticOptions options;
        auto diagnostics = clang::CompilerInstance::createDiagnostics(*llvm::vfs::getRealFileSystem(), options);
        clang::driver::Driver driver(args[0], llvm::sys::getDefaultTargetTriple(), *diagnostics);
        driver.setTargetAndMode(clang::driver::ToolChain::getTargetAndModeFromProgramName(args[0]));
        bool single = false;
        auto cc1 = [&](llvm::SmallVectorImpl<const char *> &command) {
            // Assembly and other specialized cc1 tools retain native execution.
            if (command.size() < 2 || llvm::StringRef(command[1]) != "-cc1") {
                llvm::SmallVector<llvm::StringRef> strings(command.begin(), command.end());
                return llvm::sys::ExecuteAndWait(command[0], strings);
            }
            auto compile = [&] {
                auto invocation = std::make_shared<clang::CompilerInvocation>();
                DccChildCompiler instance(new clang::CompilerInstance(invocation));
                instance->createDiagnostics();
                if (!clang::CompilerInvocation::CreateFromArgs(*invocation,
                    llvm::ArrayRef(command).drop_front(2), instance->getDiagnostics(), args[0])) return 1;
                if (auto result = dcc_clang_codegen(*instance)) return *result ? 0 : 1;
                return clang::ExecuteCompilerInvocation(instance.get()) ? 0 : 1;
            };
            // The enclosing driver is already a per-invocation child. A single
            // phase cannot leak compiler state into another phase or job.
            return single ? compile() : isolated(compile);
        };
        driver.CC1Main = cc1;
        std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(args));
        if (compilation && !compilation->containsError()) {
            std::vector<std::unique_ptr<clang::driver::Command>> originals;
            // This Compilation owns a mutable list. LLVM 22 exposes it only
            // through a const accessor; preserve all other job subclasses.
            auto &jobs = const_cast<clang::driver::JobList::list_type &>(compilation->getJobs().getJobs());
            single = jobs.size() == 1;
            for (auto &job : jobs) {
                if (llvm::sys::fs::equivalent(job->getExecutable(), DCC_LLD_PATH)) {
                    auto replacement = std::make_unique<ResidentCommand>(*job, single);
                    originals.push_back(std::move(job));
                    job = std::move(replacement);
                }
            }
            llvm::SmallVector<std::pair<int, const clang::driver::Command *>> failed;
            status = driver.ExecuteCompilation(*compilation, failed);
            if (!status && !failed.empty()) status = failed.front().first;
        }
        llvm::outs().flush(); llvm::errs().flush();
    }
    return status;
}
int prepare(int fd, char **args) {
    char **command = nullptr, **roots = nullptr;
    auto cleanup = llvm::scope_exit([&] {
        for (auto values : {command, roots}) {
            if (values) for (char **p = values; *p; ++p) free(*p);
            free(values);
        }
    });
    if (dcc_clang_frontend(args, &command, &roots)) return 1;
    // The complete input is now owned in memory. Reuse its file for the reply.
    if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) return 1;
    return command_api->write(fd, "ARGC", "ARGV", command) ||
           command_api->write(fd, "ARGC", "ARGV", roots);
}
PyObject *execute(PyObject *, PyObject *input) {
    int fd, preparing;
    if (!PyArg_ParseTuple(input, "ip", &fd, &preparing)) return nullptr;
    CompilerCommand command(*command_api);
    if (!command.read(fd, 2)) return nullptr;
    char **directory = command[0], **argv = command[1];
    if (!directory[0] || !directory[1] || directory[2] || !argv[0])
        return PyErr_Format(PyExc_ValueError, "invalid local compiler command");
    if (chdir(directory[0])) return PyErr_SetFromErrno(PyExc_OSError);
    argv[0] = directory[1];
    llvm::SmallVector<const char *> args;
    if (!preparing) for (char **p = argv; *p; ++p) args.push_back(*p);
    int status;
    Py_BEGIN_ALLOW_THREADS
    status = preparing ? prepare(fd, argv) : run(args);
    Py_END_ALLOW_THREADS
    if (PyErr_CheckSignals() < 0) return nullptr;
    return PyLong_FromLong(status);
}
PyMethodDef methods[] = {
    {"execute", execute, METH_VARARGS, "Consume a command file for execution or frontend preparation."},
    {nullptr, nullptr, 0, nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "clang_native", nullptr, -1, methods, nullptr, nullptr, nullptr, nullptr};
}
PyMODINIT_FUNC PyInit_clang_native() {
    command_api = CompilerCommand::load();
    if (!command_api) return nullptr;
    dcc_clang_codegen_init();
    llvm::InitializeAllTargetInfos(); llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs(); llvm::InitializeAllAsmParsers(); llvm::InitializeAllAsmPrinters();
    return PyModule_Create(&module);
}
