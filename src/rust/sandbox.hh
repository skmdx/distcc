#pragma once
#include "rust/common.hh"
namespace rust {
enum class CpuProfile { Target, HostUnion, HostIntersection };
string capture_cpu_profile(const fs::path &compiler, const Work &work, CpuProfile mode);
enum class Root { Host, HostWithOutputs, Inputs, LLVM, ResidentLLVM };
struct Sandbox {
    Fd filter;
    Words args;
    explicit Sandbox(Root policy, const fs::path &tree = {});
    void mount(const fs::path &from, const fs::path &to, bool writable = false);
    void command(const Words &argv);
};
} // namespace rust
