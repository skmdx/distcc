/* Inspect the input graph represented by an ELF linker invocation. */
#include "link_inputs.h"
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Object/Archive.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace llvm;
using namespace llvm::opt;
enum {
    OPT_INVALID,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "ElfOptions.inc"
#undef OPTION
};
#define OPTTABLE_STR_TABLE_CODE
#include "ElfOptions.inc"
#undef OPTTABLE_STR_TABLE_CODE
#define OPTTABLE_PREFIXES_TABLE_CODE
#include "ElfOptions.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE
constexpr opt::OptTable::Info options[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "ElfOptions.inc"
#undef OPTION
};
struct Table : opt::GenericOptTable {
    Table() : GenericOptTable(OptionStrTable, OptionPrefixesTable, options) {}
};

bool native(StringRef data) { return data.starts_with(StringRef("\177ELF", 4)); }

// Zero means all inputs are native. A positive size describes one or two
// small standalone modules. Any other graph requires the normal distributor.
struct Inputs {
    unsigned modules = 0;
    int bytes = 0;

    bool inspect(StringRef path, bool member = false) {
        int fd = open(path.str().c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return false;
        auto closeFile = scope_exit([&] { close(fd); });
        struct stat st;
        char header[8];
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) return false;
        ssize_t n = pread(fd, header, sizeof(header), 0);
        if (n < 4) return false;
        StringRef data(header, n);
        if (native(data)) return true;
        if (member) return false;
        if (identify_magic(data) == file_magic::bitcode) {
            if (++modules > 2 || st.st_size > 64 * 1024 - bytes) return false;
            bytes += st.st_size;
            return true;
        }
        if (identify_magic(data) != file_magic::archive) return false;
        auto buffer = MemoryBuffer::getOpenFile(fd, path, st.st_size, false);
        if (!buffer) return false;
        auto archive = object::Archive::create((*buffer)->getMemBufferRef());
        if (!archive) { consumeError(archive.takeError()); return false; }
        Error error = Error::success();
        bool allNative = true;
        for (const auto &child : (*archive)->children(error)) {
            if ((*archive)->isThin()) {
                auto name = child.getFullName();
                if (!name) { consumeError(name.takeError()); allNative = false; }
                else allNative = inspect(*name, true);
            } else {
                auto content = child.getBuffer();
                if (!content) { consumeError(content.takeError()); allNative = false; }
                else allNative = native(*content);
            }
            if (!allNative) break;
        }
        if (error) { consumeError(std::move(error)); return false; }
        return allNative;
    }

    bool library(StringRef name, const std::vector<std::string> &dirs, bool onlyStatic) {
        std::vector<std::string> names;
        if (name.consume_front(":")) names.push_back(name.str());
        else {
            if (!onlyStatic) names.push_back(("lib" + name + ".so").str());
            names.push_back(("lib" + name + ".a").str());
        }
        for (const auto &dir : dirs)
            for (const auto &file : names) {
                SmallString<256> path(dir);
                sys::path::append(path, file);
                struct stat st;
                if (!stat(path.c_str(), &st)) return inspect(path);
                if (errno != ENOENT && errno != ENOTDIR) return false;
            }
        return false;
    }
};
}

int dcc_small_link_bytes(char **command) {
    using namespace llvm;
    if (!command[0]) return -1;
    StringRef linker = sys::path::filename(command[0]);
    if (linker != "ld.lld" && !linker.starts_with("ld.lld-")) return -1;
    SmallVector<const char *> raw;
    for (char **p = command+1; *p; ++p) {
        if (**p == '@') return -1;
        raw.push_back(*p);
    }
    unsigned index, missing;
    static const Table table;
    auto args = table.ParseArgs(raw, index, missing);
    if (missing) return -1;
    const auto root = args.getLastArgValue(OPT_sysroot);
    std::vector<std::string> dirs;
    for (auto *arg : args.filtered(OPT_library_path)) {
        StringRef path = arg->getValue();
        if (path.consume_front("=") || path.consume_front("$SYSROOT"))
            dirs.push_back((root + path).str());
        else dirs.push_back(path.str());
    }
    Inputs inputs;
    SmallVector<unsigned> modes;
    bool onlyStatic = false;
    for (auto *arg : args) {
        switch (arg->getOption().getID()) {
        case OPT_UNKNOWN: case OPT_script: case OPT_default_script: case OPT_chroot:
        case OPT_remap_inputs: case OPT_remap_inputs_file: case OPT_format:
        case OPT_start_lib: case OPT_just_symbols:
            return -1;
        case OPT_push_state: modes.push_back(onlyStatic); break;
        case OPT_pop_state:
            if (modes.empty()) return -1;
            onlyStatic = modes.pop_back_val(); break;
        case OPT_Bstatic: onlyStatic = true; break;
        case OPT_Bdynamic: onlyStatic = false; break;
        case OPT_library:
            if (!inputs.library(arg->getValue(), dirs, onlyStatic)) return -1;
            break;
        case OPT_INPUT:
            if (!inputs.inspect(arg->getValue())) return -1;
            break;
        }
    }
    return inputs.bytes;
}
