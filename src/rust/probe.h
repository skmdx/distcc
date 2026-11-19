#pragma once
#include "rust/common.hh"
#include <set>
namespace rust {
bool runtime_path(const fs::path &path, const fs::path &sysroot);
void probe(const Words &cmd, const Env &envs, const fs::path &outputs, bool isolate, int diagnostics);
bool audit(const fs::path &depfile, int loads, const fs::path &sysroot, const std::set<fs::path> &search,
           std::set<fs::path> &inputs, std::set<fs::path> &dirs);
} // namespace rust
