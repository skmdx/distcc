#pragma once
#include "rust/common.hh"
#include <set>
namespace rust {
bool runtime_path(const fs::path &path, const fs::path &sysroot);
int run_probe(const Words &cmd, Env envs, const fs::path &outputs, bool isolate, int out, int err,
              std::vector<int> inherited = {});
bool audit(const fs::path &depfile, int loads, const fs::path &sysroot, const std::set<fs::path> &search,
           std::set<fs::path> &inputs, std::set<fs::path> &dirs, std::map<fs::path, fs::path> &observed,
           std::set<string> &env_keys);
} // namespace rust
