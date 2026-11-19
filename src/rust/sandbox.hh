#pragma once
#include "rust/common.hh"
namespace rust {
enum class Root { Host, HostWithOutputs, Inputs, Backend };
struct Sandbox {
    Fd filter;
    Words args;
    explicit Sandbox(Root policy, const fs::path &tree = {});
    void mount(const fs::path &from, const fs::path &to, bool writable = false);
    void command(const Words &argv);
};
} // namespace rust
