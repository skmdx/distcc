#pragma once
#include "rust_common.h"
namespace rust {
void probe(const Words &cmd, const Env &envs, const fs::path &work, const fs::path &outputs, bool trace,
           int out, int err);
bool audit(const fs::path &work, const fs::path &depfile, int loads, const fs::path &sysroot,
           const std::set<fs::path> &search, std::set<fs::path> &inputs, std::set<fs::path> &dirs);
} // namespace rust
