/* rustc runs this linker in the worker namespace. Only generated files are
 * returned; the client owns the linker executable and its output locations. */
#include "rust/common.hh"
#include "rust/api.h"
#include "rust/response.hh"
using namespace rust;
extern "C" int dcc_rust_capture(char **argv) {
    try {
        Words args, flat;
        for (; *argv; ++argv)
            args.emplace_back(*argv);
        std::set<fs::path> active;
        expand_link_args(args, flat, active);
        auto dest = std::find(flat.begin(), flat.end(), "-o");
        if (dest == flat.end() || ++dest == flat.end())
            throw Local("linker output missing");
        fs::path output = *dest;
        if (output.parent_path() != DCC_RUST_OUT)
            throw Local("unexpected rustc linker output");
        string base = DCC_RUST_LINK_PREFIX + std::to_string(getpid()) + "-";
        std::map<fs::path, string> copies;
        string plan = output.filename().string() + '\0';
        for (auto it = flat.begin(); it != flat.end(); ++it) {
            auto &arg = *it;
            if (arg == "-L" && std::next(it) != flat.end()) {
                const auto &dir = *std::next(it);
                if ((below(dir, DCC_RUST_OUT) || below(dir, DCC_RUST_SCRATCH)) && fs::is_directory(dir) &&
                    fs::is_empty(dir)) {
                    ++it;
                    continue;
                }
            }
            if (&arg == &*dest) {
                plan += (fs::path(DCC_RUST_FINAL) / output.filename()).string() + '\0';
                continue;
            }
            // Driver arguments also carry ld paths in -Wl,... and --option=path.
            size_t start = 0;
            while (start < arg.size()) {
                auto end = arg.find_first_of(",=", start);
                string part = arg.substr(start, end == string::npos ? end : end - start);
                if (below(part, DCC_RUST_SCRATCH) || below(part, DCC_RUST_OUT)) {
                    if (!fs::is_regular_file(part))
                        throw Local("unresolved generated linker input: " + part);
                    auto it = copies.find(part);
                    if (it == copies.end()) {
                        fs::path saved =
                            fs::path(DCC_RUST_OUT) / (base + std::to_string(copies.size()) + ".input");
                        fs::copy_file(part, saved);
                        it = copies.emplace(part, saved.string()).first;
                    }
                    plan += it->second;
                } else
                    plan += part;
                if (end == string::npos)
                    break;
                plan += arg[end];
                start = end + 1;
            }
            plan += '\0';
        }
        Fd record(open((fs::path(DCC_RUST_OUT) / (base + "args.plan")).c_str(),
                       O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
        put(record.fd, plan);
        Fd placeholder(open(output.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC | O_TRUNC, 0755));
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "distcc-rust: capture linker: %s\n", e.what());
        return 1;
    }
}
