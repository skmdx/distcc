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
#include <sys/syscall.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <string>
#include <set>
#include <algorithm>
#include <sstream>
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

    Profile()
    {
        const char *path = std::getenv("DISTCC_RUST_NATIVE");
        if (!path)
            fail("missing profile directory");
        directory = path;
        const char *mode = std::getenv("DISTCC_RUST_NATIVE_CAPTURE");
        capture = mode && std::strcmp(mode, "1") == 0;
    }

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

const Profile &profile()
{
    static const Profile value;
    return value;
}

// rustc's own native query selects this slot's LLVM and runs after startup.
void capture_host_features(const char *path)
{
    auto get = original<char *(*)()>("LLVMGetHostCPUFeatures");
    auto dispose = original<void (*)(char *)>("LLVMDisposeMessage");
    auto mask = rust::affinity();
    if (mask.empty()) fail("cannot determine CPU affinity");
    auto size = mask.size() * sizeof(mask[0]);
    auto allowed = reinterpret_cast<cpu_set_t *>(mask.data());
    rust::CpuMask one(mask.size());
    auto pin = reinterpret_cast<cpu_set_t *>(one.data());
    const char *mode = std::getenv("DISTCC_RUST_HOST_UNION");
    bool first = true, intersection = !mode || std::strcmp(mode, "1") != 0;
    std::set<std::string> result;
    for (size_t cpu = 0; cpu < size * 8; ++cpu) {
        if (!CPU_ISSET_S(cpu, size, allowed))
            continue;
        CPU_ZERO_S(size, pin);
        CPU_SET_S(cpu, size, pin);
        if (sched_setaffinity(0, size, pin))
            fail("cannot pin host feature probe");
        char *raw = get();
        if (!raw || !*raw)
            fail("LLVM host feature list unavailable");
        std::istringstream stream(raw);
        dispose(raw);
        std::set<std::string> current;
        for (std::string token; std::getline(stream, token, ',');)
            if (token.size() > 1 && token.front() == '+')
                current.insert(token.substr(1));
        if (first || !intersection)
            result.insert(current.begin(), current.end());
        else {
            std::set<std::string> common;
            std::set_intersection(result.begin(), result.end(), current.begin(), current.end(),
                                  std::inserter(common, common.end()));
            result.swap(common);
        }
        first = false;
    }
    if (first || sched_setaffinity(0, size, allowed))
        fail("cannot restore host feature probe affinity");
    std::ofstream file(path);
    if (result.empty()) file << '-';
    for (auto it = result.begin(); it != result.end(); ++it)
        file << (it == result.begin() ? "" : ",") << *it;
    file.close();
    if (!file) fail("cannot write host features");
}
}

namespace llvm::sys {
StringRef getHostCPUName()
{
    if (!std::getenv("DISTCC_RUST_NATIVE"))
        return original<StringRef (*)()>("_ZN4llvm3sys14getHostCPUNameEv")();
    static const std::string cpu = [] {
        const auto &p = profile();
        if (p.capture) {
            auto value = original<StringRef (*)()>("_ZN4llvm3sys14getHostCPUNameEv")().str();
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
    if (const char *path = std::getenv("DISTCC_RUST_HOST_PROFILE")) {
        static const bool captured = (capture_host_features(path), true);
        (void)captured;
    }
    if (!std::getenv("DISTCC_RUST_NATIVE"))
        return original<char *(*)()>("LLVMGetHostCPUFeatures")();
    static const std::string features = [] {
        const auto &p = profile();
        if (p.capture) {
            char *raw = original<char *(*)()>("LLVMGetHostCPUFeatures")();
            if (!raw)
                fail("LLVMGetHostCPUFeatures returned null");
            std::string value(raw);
            original<void (*)(char *)>("LLVMDisposeMessage")(raw);
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
