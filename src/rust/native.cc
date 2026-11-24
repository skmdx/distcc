/* Preserve the client's LLVM CPU discovery when rustc runs on another host.
 * The client records the actual compiler's native answers; the worker replays
 * them. Rust remains responsible for target defaults, cfg and user overrides.
 * No LLVM library is linked: symbols resolve in the running compiler.
 */
#define LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING 1
#include <llvm/ADT/StringRef.h>

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <string>
#include <set>
#include <utility>
#include "affinity.hh"

namespace {
[[noreturn]] void fail(const char *message)
{
    std::fprintf(stderr, "distcc-rust: native CPU profile: %s\n", message);
    std::_Exit(100);
}

template<typename T> T original(const char *name)
{
    auto symbol = reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
    if (!symbol)
        fail(name);
    return symbol;
}

struct Profile {
    std::string directory;
    bool capture;

    void write(const char *name, const std::string &value) const
    {
        std::ofstream file(directory + '/' + name);
        file << value;
        file.close();
        if (!file)
            fail("cannot write profile");
    }

    std::string read(const char *name) const
    {
        std::ifstream file(directory + '/' + name);
        if (!file)
            fail("cannot read profile");
        std::string value((std::istreambuf_iterator<char>(file)), {});
        if (file.bad())
            fail("cannot read profile");
        return value;
    }
};

llvm::StringRef host_cpu_name()
{
    return original<llvm::StringRef (*)()>("_ZN4llvm3sys14getHostCPUNameEv")();
}

std::string host_features()
{
    char *raw = original<char *(*)()>("LLVMGetHostCPUFeatures")();
    if (!raw)
        fail("LLVMGetHostCPUFeatures returned null");
    std::string value(raw);
    original<void (*)(char *)>("LLVMDisposeMessage")(raw);
    return value;
}

// Both hooks replay profile 0 in this rustc, even if its thread migrates
// after we restore the allowed mask.
Profile capture_host_profiles(const std::string &directory)
{
    auto mask = rust::affinity();
    if (mask.empty()) fail("cannot determine CPU affinity");
    auto size = mask.size() * sizeof(mask[0]);
    auto allowed = reinterpret_cast<cpu_set_t *>(mask.data());
    rust::CpuMask one(mask.size());
    auto pin = reinterpret_cast<cpu_set_t *>(one.data());
    std::set<std::pair<std::string, std::string>> profiles;
    for (size_t cpu = 0; cpu < size * 8; ++cpu) {
        if (!CPU_ISSET_S(cpu, size, allowed))
            continue;
        CPU_ZERO_S(size, pin);
        CPU_SET_S(cpu, size, pin);
        if (sched_setaffinity(0, size, pin))
            fail("cannot pin host feature probe");
        auto [entry, inserted] = profiles.emplace(host_cpu_name().str(), host_features());
        if (inserted) {
            Profile p{directory + '/' + std::to_string(profiles.size() - 1), false};
            if (mkdir(p.directory.c_str(), 0700))
                fail("cannot create host profile directory");
            p.write("cpu", entry->first);
            p.write("features", entry->second);
        }
    }
    if (profiles.empty() || sched_setaffinity(0, size, allowed))
        fail("cannot restore host feature probe affinity");
    return {directory + "/0", false};
}

const Profile &profile()
{
    static const Profile value = [] {
        std::string path = std::getenv("DISTCC_RUST_NATIVE");
        const char *mode = std::getenv("DISTCC_RUST_NATIVE_CAPTURE");
        if (mode && std::strcmp(mode, "host") == 0)
            return capture_host_profiles(path);
        return Profile{path, mode && std::strcmp(mode, "1") == 0};
    }();
    return value;
}
}

namespace llvm::sys {
StringRef getHostCPUName()
{
    if (!std::getenv("DISTCC_RUST_NATIVE"))
        return host_cpu_name();
    static const std::string cpu = [] {
        const auto &p = profile();
        if (p.capture) {
            auto value = host_cpu_name().str();
            p.write("cpu", value);
            return value;
        }
        auto value = p.read("cpu");
        p.write("cpu-used", "");
        return value;
    }();
    return cpu;
}
}

extern "C" char *LLVMGetHostCPUFeatures()
{
    if (!std::getenv("DISTCC_RUST_NATIVE"))
        return original<char *(*)()>("LLVMGetHostCPUFeatures")();
    static const std::string features = [] {
        const auto &p = profile();
        if (p.capture) {
            auto value = host_features();
            p.write("features", value);
            return value;
        }
        auto value = p.read("features");
        p.write("features-used", "");
        return value;
    }();
    char *result = strdup(features.c_str());
    if (!result)
        fail("cannot allocate native features");
    return result;
}

/* The real dependency pass expands transitive procedural macros. Record the
 * libraries it executes rather than interpreting Rust crate metadata. */
extern "C" void dcc_rust_watch_library(int, const char *);
extern "C" void dcc_rust_library_record(int, const char *, const char *, const char *);
extern "C" void *dlopen(const char *name, int flags)
{
    const char *descriptor = std::getenv("DISTCC_RUST_LOADS_FD");
    if (!descriptor)
        return original<void *(*)(const char *, int)>("dlopen")(name, flags);
    auto mapped = []() {
        std::set<std::string> paths;
        dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *context) {
                if (info->dlpi_name && *info->dlpi_name)
                    static_cast<std::set<std::string> *>(context)->insert(info->dlpi_name);
                return 0;
            },
            &paths);
        return paths;
    };
    auto previous = mapped();
    int fd = std::atoi(descriptor);
    auto load = original<void *(*)(const char *, int)>("dlopen");
    // Reopening a resident DSO by soname runs no constructors. Classify it
    // by its resolved path below: libsandbox opens libc.so.6 during startup.
    void *handle = name && !std::strchr(name, '/') ? load(name, flags | RTLD_NOLOAD) : nullptr;
    if (!handle) {
        dcc_rust_watch_library(fd, name);
        handle = load(name, flags);
    }
    int saved_errno = errno;
    if (handle) {
        struct link_map *map;
        if (dlinfo(handle, RTLD_DI_LINKMAP, &map)) fail("cannot inspect loaded library");
        dcc_rust_watch_library(fd, map->l_name);
        dcc_rust_library_record(fd, "L", name ? name : "", map->l_name);
        for (const auto &dependency : mapped())
            if (dependency != map->l_name && !previous.count(dependency))
                dcc_rust_library_record(fd, "D", "", dependency.c_str());
    }
    errno = saved_errno;
    return handle;
}
