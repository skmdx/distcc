/* Inspect the compiler's own ELF imports before substituting a driver host.
 * Map the files so inspecting PT_TLS in large DSOs only reads their headers. */
#pragma once
#include "rust/common.hh"
#include <elf.h>
#include <link.h>
#include <string_view>
#include <sys/mman.h>

extern "C" const ElfW(Ehdr) __ehdr_start;

namespace rust {
struct UnsupportedElf {};
class DriverElf {
    std::string_view image;

    static std::string_view slice(std::string_view data, uint64_t offset, uint64_t size) {
        if (offset > data.size() || size > data.size() - offset)
            throw UnsupportedElf{};
        return data.substr(offset, size);
    }
    template<class T> static T object(std::string_view data, uint64_t offset = 0) {
        T value;
        memcpy(&value, slice(data, offset, sizeof(T)).data(), sizeof(T));
        return value;
    }
    template<class T, class F> static void entries(std::string_view table, size_t stride, F visit) {
        if (stride != sizeof(T) || table.size() % stride)
            throw UnsupportedElf{};
        while (!table.empty()) {
            visit(object<T>(table));
            table.remove_prefix(stride);
        }
    }
    Elf64_Ehdr header() const {
        auto h = object<Elf64_Ehdr>(image);
        if (memcmp(h.e_ident, ELFMAG, SELFMAG) || h.e_ident[EI_CLASS] != ELFCLASS64 ||
            h.e_ident[EI_CLASS] != __ehdr_start.e_ident[EI_CLASS] ||
            h.e_ident[EI_DATA] != __ehdr_start.e_ident[EI_DATA] || h.e_machine != __ehdr_start.e_machine ||
            (h.e_type != ET_EXEC && h.e_type != ET_DYN))
            throw UnsupportedElf{};
        return h;
    }
    static std::string_view name(std::string_view strings, uint64_t offset) {
        if (offset >= strings.size())
            throw UnsupportedElf{};
        strings.remove_prefix(offset);
        auto end = strings.find('\0');
        if (end == strings.npos)
            throw UnsupportedElf{};
        return strings.substr(0, end);
    }
    static bool driver_main(std::string_view symbol) {
        for (std::string_view suffix : {"17rustc_driver_impl4main", "12rustc_driver4main"})
            if (symbol.substr(0, 2) == "_R" && symbol.size() >= suffix.size() &&
                symbol.substr(symbol.size() - suffix.size()) == suffix)
                return true;
        // Legacy Rust mangling ends in a length-prefixed 16-digit hash.
        constexpr std::string_view legacy = "_ZN12rustc_driver4main17h";
        return symbol.size() == legacy.size() + 17 && symbol.substr(0, legacy.size()) == legacy &&
               symbol.back() == 'E' && symbol.substr(legacy.size(), 16).find_first_not_of("0123456789abcdef") == symbol.npos;
    }
public:
    explicit DriverElf(const fs::path &path) {
        int opened = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (opened < 0 && errno == EACCES)
            throw UnsupportedElf{};
        Fd fd(opened);
        struct stat st;
        check(!fstat(fd.fd, &st), "stat compiler ELF");
        if (!S_ISREG(st.st_mode) || st.st_size < off_t(sizeof(Elf64_Ehdr)) || uint64_t(st.st_size) > SIZE_MAX)
            throw UnsupportedElf{};
        void *p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd.fd, 0);
        check(p != MAP_FAILED, "map compiler ELF");
        image = {static_cast<const char *>(p), size_t(st.st_size)};
    }
    DriverElf(const DriverElf &) = delete;
    ~DriverElf() { munmap(const_cast<char *>(image.data()), image.size()); }

    // Both names must come from this executable, never a proxy's sysroot.
    std::pair<string, string> driver_import() const {
        auto h = header();
        auto sections = slice(image, h.e_shoff, uint64_t(h.e_shnum) * h.e_shentsize);
        Elf64_Phdr dynamic{};
        auto ph = slice(image, h.e_phoff, uint64_t(h.e_phnum) * h.e_phentsize);
        entries<Elf64_Phdr>(ph, h.e_phentsize, [&](auto p) {
            if (p.p_type == PT_DYNAMIC)
                dynamic = p;
        });
        string library, entry;
        entries<Elf64_Shdr>(sections, h.e_shentsize, [&](auto s) {
            if (s.sh_type != SHT_DYNAMIC && s.sh_type != SHT_DYNSYM)
                return;
            auto linked = object<Elf64_Shdr>(sections, uint64_t(s.sh_link) * sizeof(Elf64_Shdr));
            if (linked.sh_type != SHT_STRTAB)
                throw UnsupportedElf{};
            auto strings = slice(image, linked.sh_offset, linked.sh_size);
            auto values = slice(image, s.sh_offset, s.sh_size);
            if (s.sh_type == SHT_DYNAMIC) {
                if (s.sh_offset != dynamic.p_offset || s.sh_size > dynamic.p_filesz)
                    throw UnsupportedElf{};
                bool ended = false;
                entries<Elf64_Dyn>(values, s.sh_entsize, [&](auto d) {
                    ended |= d.d_tag == DT_NULL;
                    if (ended || d.d_tag != DT_NEEDED)
                        return;
                    auto n = name(strings, d.d_un.d_val);
                    if (n.find("librustc_driver-") == 0 && n.size() > 18 && n.substr(n.size() - 3) == ".so") {
                        if (!library.empty())
                            throw UnsupportedElf{};
                        library = n;
                    }
                });
            } else {
                entries<Elf64_Sym>(values, s.sh_entsize, [&](auto sym) {
                    if (sym.st_shndx != SHN_UNDEF)
                        return;
                    auto n = name(strings, sym.st_name);
                    if (driver_main(n)) {
                        if (!entry.empty())
                            throw UnsupportedElf{};
                        entry = n;
                    }
                });
            }
        });
        if (library.empty() || entry.empty())
            throw UnsupportedElf{};
        return {library, entry};
    }
    uint64_t tls_size() const {
        auto h = header();
        auto ph = slice(image, h.e_phoff, uint64_t(h.e_phnum) * h.e_phentsize);
        uint64_t size = 0;
        entries<Elf64_Phdr>(ph, h.e_phentsize, [&](auto p) {
            if (p.p_type == PT_TLS)
                size = p.p_memsz;
        });
        return size;
    }
};
}
