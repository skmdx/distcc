/* Native LLD distributor and the Rust linker sandbox boundary. */
#pragma once
#include "rust/common.hh"

namespace dtlto {
using rust::Fd;
using rust::Words;
using rust::string;
namespace fs = std::filesystem;
using rust::check;
inline void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Failure : std::runtime_error {
    int status;
    Failure(const char *message, int status) : std::runtime_error(message), status(status) {}
};
struct Backend {
    Words args, inputs, outputs, destinations;
};
struct Request {
    string linker_output;
    Words args, inputs, outputs;
    std::vector<Backend> jobs;
};

class LinkFiles {
    fs::path work, linked, temporary;
    std::map<fs::path, Fd> directories;
    std::map<string, fs::path> declared;
    int directory(const fs::path &root, const fs::path &path);
    int parent(const fs::path &path, bool writable);
public:
    LinkFiles(const char *work, const char *linked, const char *temporary);
    Fd input(const string &name);
    Request request(const string &path, const string &compiler);
};
void check_cancelled();
Fd regular(const fs::path &path, int parent = AT_FDCWD);
Request parse_request(const string &data);
void distribute(Request request, dcc_job_kind kind, LinkFiles *link = nullptr);
int rust_link(char **argv);
int rust_shim(const char *description);
} // namespace dtlto
