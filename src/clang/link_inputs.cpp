/* Inspect the input graph represented by an ELF linker invocation. */
#include "link_inputs.h"
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Object/Archive.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/FileSystem.h>
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

// Keep quoted tokens intact so a quoted filename cannot become punctuation.
struct Script {
    SmallVector<StringRef> tokens;

    bool tokenize(StringRef text) {
        while (!(text = text.ltrim()).empty()) {
            if (text.consume_front("/*")) {
                auto end = text.find("*/");
                if (end == StringRef::npos) return false;
                text = text.drop_front(end + 2);
                continue;
            }
            size_t n = 1;
            if (text.front() == '"') {
                auto end = text.find('"', 1);
                if (end == StringRef::npos) return false;
                n = end + 1;
            } else if (!StringRef("(),").contains(text.front())) {
                n = std::min(text.find_first_of(" \t\r\n\v\f(),\""), text.find("/*"));
                if (n == StringRef::npos) n = text.size();
            }
            StringRef token = text.take_front(n);
            if (token.contains('\0') || token.contains('\\')) return false;
            tokens.push_back(token);
            text = text.drop_front(n);
        }
        return !tokens.empty();
    }
};

// Zero means all inputs are native. A positive size describes one or two
// small standalone modules. Any other graph requires the normal distributor.
struct Inputs {
    StringRef root;
    std::vector<std::string> dirs;
    SmallVector<std::pair<dev_t, ino_t>, 8> scripts;
    bool onlyStatic = false;
    unsigned modules = 0;
    int bytes = 0;

    bool scriptFile(StringRef name, StringRef directory, bool rooted) {
        if (name.consume_front("-l")) return library(name);
        if (name.consume_front("=") || name.consume_front("$SYSROOT") ||
            (rooted && sys::path::is_absolute(name))) {
            if (root.empty()) return inspect(name);
            SmallString<256> path(root);
            sys::path::append(path, name);
            return inspect(path);
        }
        if (sys::path::is_absolute(name)) return inspect(name);
        // ScriptParser::addFile resolves siblings, then the working directory,
        // then the linker's search paths.
        SmallString<256> path(directory);
        sys::path::append(path, name);
        if (sys::fs::exists(path)) return inspect(path);
        if (sys::fs::exists(name)) return inspect(name);
        for (const auto &dir : dirs) {
            path = dir;
            sys::path::append(path, name);
            if (sys::fs::exists(path)) return inspect(path);
        }
        return false;
    }

    bool script(StringRef text, StringRef path) {
        Script script;
        if (!script.tokenize(text)) return false;
        ArrayRef<StringRef> rest(script.tokens);
        auto take = [&](StringRef token) {
            if (rest.empty() || rest.front() != token) return false;
            rest = rest.drop_front();
            return true;
        };
        const auto directory = sys::path::parent_path(path);
        bool rooted = false;
        if (!root.empty())
            for (auto dir = directory; !dir.empty(); dir = sys::path::parent_path(dir))
                if (sys::fs::equivalent(root, dir)) { rooted = true; break; }
        unsigned depth = 0;
        while (!rest.empty()) {
            if (!depth && take("OUTPUT_FORMAT")) {
                if (!take("(")) return false;
                for (unsigned i = 0;; ++i) {
                    if (rest.empty() || rest.front().find_first_of("(),") != StringRef::npos) return false;
                    rest = rest.drop_front();
                    if (take(")")) { if (i != 0 && i != 2) return false; break; }
                    if (i >= 2 || !take(",")) return false;
                }
            } else if ((!depth && (take("GROUP") || take("INPUT"))) || (depth && take("AS_NEEDED"))) {
                if (!take("(")) return false;
                ++depth;
                continue;
            } else if (depth && take(")")) {
                --depth;
            } else {
                if (!depth || rest.front() == "(" || rest.front() == ",") return false;
                StringRef name = rest.front();
                rest = rest.drop_front();
                if (name.consume_front("\"")) name = name.drop_back();
                if (name.empty() || !scriptFile(name, directory, rooted)) return false;
            }
            if (depth && take(",") && (rest.empty() || rest.front() == ")")) return false;
        }
        return depth == 0;
    }

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
            if (!scripts.empty() || ++modules > 2 || st.st_size > 64 * 1024 - bytes) return false;
            bytes += st.st_size;
            return true;
        }
        bool archiveFile = identify_magic(data) == file_magic::archive;
        if (!archiveFile && (st.st_size > 64 * 1024 || scripts.size() >= 8)) return false;
        auto buffer = MemoryBuffer::getOpenFile(fd, path, st.st_size, false);
        if (!buffer) return false;
        if (!archiveFile) {
            auto id = std::make_pair(st.st_dev, st.st_ino);
            if (llvm::is_contained(scripts, id)) return false;
            scripts.push_back(id);
            auto pop = scope_exit([&] { scripts.pop_back(); });
            return script((*buffer)->getBuffer(), path);
        }
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

    bool library(StringRef name) {
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
    Inputs inputs;
    inputs.root = root;
    for (auto *arg : args.filtered(OPT_library_path)) {
        StringRef path = arg->getValue();
        if (path.consume_front("=") || path.consume_front("$SYSROOT"))
            inputs.dirs.push_back((root + path).str());
        else inputs.dirs.push_back(path.str());
    }
    SmallVector<unsigned> modes;
    for (auto *arg : args) {
        switch (arg->getOption().getID()) {
        case OPT_UNKNOWN: case OPT_script: case OPT_default_script: case OPT_chroot:
        case OPT_remap_inputs: case OPT_remap_inputs_file: case OPT_format:
        case OPT_start_lib: case OPT_just_symbols:
            return -1;
        case OPT_push_state: modes.push_back(inputs.onlyStatic); break;
        case OPT_pop_state:
            if (modes.empty()) return -1;
            inputs.onlyStatic = modes.pop_back_val(); break;
        case OPT_Bstatic: inputs.onlyStatic = true; break;
        case OPT_Bdynamic: inputs.onlyStatic = false; break;
        case OPT_library:
            if (!inputs.library(arg->getValue())) return -1;
            break;
        case OPT_INPUT:
            if (!inputs.inspect(arg->getValue())) return -1;
            break;
        }
    }
    return inputs.bytes;
}
