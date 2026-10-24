/* Preserve the client's LLVM CPU discovery when rustc runs on another host.
 * The client records the actual compiler's native answers; the worker replays
 * them. Rust remains responsible for target defaults, cfg and user overrides.
 * No LLVM library is linked: symbols resolve in the running compiler.
 */
#define LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING 1
#include <llvm/ADT/StringRef.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <string>

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
}

namespace llvm::sys {
StringRef getHostCPUName()
{
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
