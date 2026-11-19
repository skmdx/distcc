/* Plan the Rust dependency closure using rustc itself, and lend original
 * file descriptors to the pooled transport until the result is verified. */
#include "rust_common.h"
#include "rust_job.h"
#include "rust_probe.h"
#include "rust_link_args.h"
#include <regex>
#include <chrono>
#include <array>
#include <functional>
#include <iconv.h>
#include <memory>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sched.h>
extern "C" {
#include "broker.h"
#include "workload.h"
#include "resource.h"
#include "where.h"
#include "lock.h"
#include "exitcode.h"
#include "file_cache.h"
#include "plan_memo.h"
}
using namespace rust;
namespace {
using States = std::map<fs::path, struct stat>;
struct Work {
    fs::path dir;
    explicit Work(fs::path parent = {}) {
        auto base = env("TMPDIR");
        if (parent.empty())
            parent = base.empty() ? "/tmp" : base;
        string tmp = (parent / "distcc-rust-XXXXXX").string();
        check(mkdtemp(tmp.data()) != nullptr, "Rust working directory");
        dir = fs::absolute(tmp);
    }
    ~Work() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
struct Lease {
    int id = -1;
    ~Lease() {
        if (id >= 0)
            dcc_broker_release(id);
    }
};
struct ProbeSlot {
    int id = -1;
    ProbeSlot() {
        if (dcc_lock_local_cpp(&id))
            throw std::runtime_error("Rust dependency probe lease unavailable");
    }
    ~ProbeSlot() { dcc_unlock(id); }
};
struct Result : dcc_job_result {
    Result() { dcc_job_result_init(this); }
    ~Result() { dcc_job_result_free(this); }
};
struct stat descriptor_state(int fd) {
    struct stat s;
    check(!fstat(fd, &s), "input generation");
    if (!S_ISREG(s.st_mode))
        throw Local("input is not a regular file");
    return s;
}
void verify(const fs::path &path, const struct stat &expected, int fd = -1) {
    struct stat st;
    if (stat(path.c_str(), &st) || !dcc_plan_stat_equal(DCC_PLAN_STAT, &expected, &st) ||
        (fd >= 0 && (fstat(fd, &st) || !dcc_plan_stat_equal(DCC_PLAN_STAT, &expected, &st))))
        throw Local("input changed: " + path.string());
}
void validate_response_text(string &text) {
    iconv_t convert = iconv_open("UTF-16LE", "UTF-8");
    if (convert == (iconv_t)-1)
        check(false, "UTF-8 decoder");
    char *in = text.data(), buffer[4096];
    size_t left = text.size();
    while (left) {
        char *out = buffer;
        size_t size = sizeof(buffer);
        if (iconv(convert, &in, &left, &out, &size) == (size_t)-1 && errno != E2BIG)
            break;
    }
    iconv_close(convert);
    if (left || text.find('\0') != string::npos)
        throw Local("invalid response file text");
}
/* rustc's -Zshell-argfiles uses shlex tokenization, with neither shell
 * expansions nor execution. Plain response files retain one argument per line. */
Words shell_words(const string &text) {
    Words args;
    string word;
    char quote = 0;
    bool started = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!quote && (c == ' ' || c == '\t' || c == '\n')) {
            if (started) {
                args.push_back(word);
                word.clear();
                started = false;
            }
        } else if (!quote && c == '#' && !started) {
            i = text.find('\n', i);
            if (i == string::npos)
                break;
        } else if (c == '\\' && quote != '\'') {
            if (++i == text.size())
                throw Local("unterminated response file escape");
            c = text[i];
            if (c == '\n')
                continue;
            if (quote && c != '$' && c != '`' && c != '"' && c != '\\')
                word += '\\';
            word += c;
            started = true;
        } else if (quote && c == quote)
            quote = 0;
        else if (!quote && (c == '\'' || c == '"')) {
            quote = c;
            started = true;
        } else {
            word += c;
            started = true;
        }
    }
    if (quote)
        throw Local("unterminated response file quote");
    if (started)
        args.push_back(word);
    return args;
}
struct ResponseFiles {
    Words flat;
    States files;
    std::set<fs::path> active;
    bool shell = false, unstable = false;
    void expand(const Words &words) {
        for (const auto &word : words) {
            if (word.empty() || word[0] != '@') {
                if ((unstable && word == "shell-argfiles") || word == "-Zshell-argfiles")
                    shell = true;
                unstable = !unstable && word == "-Z";
                flat.push_back(word);
                continue;
            }
            bool quoted = shell && word.compare(0, 7, "@shell:") == 0;
            fs::path path = fs::absolute(word.substr(quoted ? 7 : 1));
            fs::path canonical = fs::canonical(path);
            if (!active.insert(canonical).second || active.size() > 128)
                throw Local("recursive response file");
            Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
            auto st = descriptor_state(fd.fd);
            auto entry = files.emplace(path, st);
            string data = contents(fd.fd);
            validate_response_text(data);
            auto args = quoted ? shell_words(data) : split(replace(data, "\r\n", "\n"), '\n');
            verify(path, entry.first->second, fd.fd);
            expand(args);
            active.erase(canonical);
        }
    }
};
uint64_t hash(const string &s, uint64_t seed = 0) { return XXH3_64bits_withSeed(s.data(), s.size(), seed); }
string memo(string context, const States &dependencies, const std::function<string()> &compute) {
    auto h = XXH3_128bits(context.data(), context.size());
    string key = "rust-plan:" + std::to_string(h.high64) + ":" + std::to_string(h.low64);
    Fd output(dcc_job_memfd("rust-memo"));
    char *value = nullptr;
    size_t length = 0;
    unsigned count = 0;
    int verified = 0;
    Lease owner{dcc_broker_memo_get(key.c_str(), &value, &length, &verified)};
    while (value) {
        std::unique_ptr<char, decltype(&free)> saved(value, free);
        if (dcc_plan_memo_decode(value, length, output.fd, &count, verified))
            return contents(output.fd);
        value = nullptr;
        if (owner.id >= 0)
            dcc_broker_release(owner.id);
        owner.id = dcc_broker_memo_refresh(key.c_str(), saved.get(), length, &value, &length, &verified);
    }
    string computed = compute();
    if (owner.id >= 0) {
        Fd journal(dcc_job_memfd("rust-stamps"));
        put(journal.fd, string("\1\1\0\0", 4));
        for (auto &[path, st] : dependencies) {
            dcc_plan_dependency record{};
            record.operation = DCC_PLAN_STAT;
            record.metadata = st;
            record.length = path.native().size() + 1;
            put(journal.fd, string(reinterpret_cast<char *>(&record), sizeof(record)));
            put(journal.fd, path.string() + '\0');
        }
        check(!ftruncate(output.fd, 0) && lseek(output.fd, 0, SEEK_SET) == 0, "reset compiler memo");
        put(output.fd, computed);
        value = dcc_plan_memo_encode(journal.fd, output.fd, &count, &length);
        if (value) {
            dcc_broker_memo_put(owner.id, value, length);
            owner.id = -1;
            free(value);
        }
    }
    return computed;
}
fs::path absolute_name(const string &s) { return fs::absolute(s).lexically_normal(); }
struct Invocation {
    Words base, actions, types;
    fs::path source, outdir = fs::current_path(), scope;
    string single_output, target, sysroot, name, opt = "0", cpu, linker;
    std::set<fs::path> inputs, search, native_search, dirs;
    std::set<string> libraries;
    bool trace_macro = false, needs_host = false, static_archive = false, pipeline = false;
    string metadata_name;
    std::set<string> link_arguments;
    std::set<string> dep_names;
    std::map<string, string> outputs;
    Env environment;
    explicit Invocation(const Words &args) {
        auto opts = options(args);
        actions = emits(opts);
        pipeline = metadata_notifications(opts);
        linker = link_driver(opts);
        if (std::all_of(actions.begin(), actions.end(),
                        [](auto &s) { return s.substr(0, s.find('=')) == "dep-info"; }))
            throw Local("dependency query");
        for (const auto &o : opts) {
            const auto &k = o.key;
            string v = o.value.value_or("");
            if (k == "-" || (k.empty() && v == "-"))
                throw Local("standard input requires local compilation");
            if (k == "--explain" || k == "--print" || k.compare(0, 8, "--print=") == 0)
                throw Local("local rustc query or test");
            if (k == "--emit")
                continue;
            if (k == "--out-dir") {
                outdir = absolute_name(v);
                continue;
            }
            if (k == "-o") {
                single_output = v;
                continue;
            }
            if (k.empty()) {
                if (v == "-" || !source.empty())
                    throw Local("expected one source file");
                source = v;
            }
            if (k == "--crate-name")
                name = v;
            if (k == "--crate-type") {
                types.push_back(v);
                for (auto &type : split(v, ','))
                    static_archive |= type == "staticlib";
            }
            if (k == "--sysroot")
                sysroot = v;
            if (k == "--target") {
                target = v;
                if (fs::path(v).extension() == ".json")
                    inputs.insert(absolute_name(v));
            }
            auto eq = v.find('=');
            string key = v.substr(0, eq), val = eq == string::npos ? "" : v.substr(eq + 1);
            if (k == "-C") {
                if (key == "link-arg" || key == "link-args") {
                    Words expanded;
                    std::set<fs::path> active;
                    expand_link_args(key == "link-arg" ? Words{val} : split(val, ' '), expanded, active,
                                     &inputs);
                    link_arguments.insert(expanded.begin(), expanded.end());
                    for (const auto &arg : expanded)
                        if (!arg.empty() && arg.front() != '-' && fs::is_regular_file(arg))
                            inputs.insert(absolute_name(arg));
                }
                if (key == "incremental")
                    continue;
                if (key == "profile-use")
                    inputs.insert(absolute_name(val));
                if (key == "target-cpu")
                    cpu = val;
                if (key == "opt-level")
                    opt = val;
            }
            if (k == "-O")
                opt = "2";
            if (k == "--extern" && eq != string::npos) {
                trace_macro |= fs::path(val).extension() == ".so";
                inputs.insert(absolute_name(val));
            }
            if (k == "-l") {
                auto lib = eq == string::npos ? v : val;
                libraries.insert(lib.substr(0, lib.find(':')));
            }
            if (k == "-L") {
                auto dir = absolute_name(eq == string::npos ? v : val);
                if (key == "dependency" && scope.empty())
                    scope = dir;
                if (fs::is_directory(dir)) {
                    search.insert(fs::canonical(dir));
                    trace_macro |= key != "native" && key != "framework";
                    if (key != "crate" && key != "dependency")
                        native_search.insert(dir);
                }
            }
            o.append(base);
        }
        if (source.empty())
            throw Local("compiler query");
        if (name.empty())
            name = source.stem();
        if (!single_output.empty())
            outdir = absolute_name(single_output).parent_path();
        if (scope.empty())
            scope = outdir;
    }
    string destination(const string &spelling) {
        if (spelling == "-")
            throw Local("non-file output");
        auto path = absolute_name(spelling);
        if (below(path, "/dev") || !fs::is_directory(path.parent_path()))
            throw Local("non-file output");
        string filename = path.filename();
        if (!outputs.emplace(filename, path).second && outputs.at(filename) != path)
            throw Local("colliding output filenames");
        return string(DCC_RUST_OUT) + "/" + filename;
    }
    Words command(const string &host, const string &root) {
        Words cmd = base;
        if (sysroot.empty())
            cmd.insert(cmd.end(), {"--sysroot", root});
        if (target.empty())
            cmd.insert(cmd.end(), {"--target", host});
        string mapped;
        for (auto &action : actions) {
            auto eq = action.find('=');
            if (!mapped.empty())
                mapped += ',';
            mapped += action.substr(0, eq);
            if (eq != string::npos && action.substr(0, eq) == "dep-info")
                dep_names.insert(fs::path(action.substr(eq + 1)).filename());
            if (eq != string::npos && action.substr(0, eq) == "metadata")
                metadata_name = fs::path(action.substr(eq + 1)).filename();
            if (eq != string::npos)
                mapped += "=" + destination(action.substr(eq + 1));
        }
        cmd.insert(cmd.end(), {"--emit", mapped});
        if (single_output.empty())
            cmd.insert(cmd.end(), {"--out-dir", DCC_RUST_OUT});
        else
            cmd.insert(cmd.end(), {"-o", destination(single_output)});
        return cmd;
    }
    void discover(const string &compiler, const string &root, const Work &work) {
        Fd loads(dcc_job_memfd("rust-libraries")), quiet(dcc_job_memfd("rust-probe-diagnostics"));
        check(!fcntl(loads.fd, F_SETFD, 0), "trace descriptor");
        Env envs = {{"RUSTC_BOOTSTRAP", "1"},
                    {"DISTCC_RUST_LOADS_FD", std::to_string(loads.fd)},
                    {"LD_PRELOAD", native_library().string() + " " + env("LD_PRELOAD")}};
        Words cmd = compiler_command(compiler);
        cmd.insert(cmd.end(), base.begin(), base.end());
        if (sysroot.empty())
            cmd.insert(cmd.end(), {"--sysroot", root});
        auto staging = work.dir / "probe";
        fs::create_directory(staging);
        auto depfile = staging / "probe.d";
        cmd.insert(cmd.end(), {"-Zbinary-dep-depinfo", "--emit=dep-info=" + depfile.string()});
        Fd expanded(dcc_job_memfd("rust-expanded"));
        if (!native_search.empty())
            cmd.push_back("-Zunpretty=expanded");
        {
            ProbeSlot slot;
            probe(cmd, envs, work.dir, staging, trace_macro, expanded.fd, quiet.fd);
        }
        if (!native_search.empty()) {
            string src = contents(expanded.fd);
            const std::regex attribute(R"re(#\s*\[\s*(?:unsafe\s*\(\s*)?link\s*\(([^\]]*)\)\s*\])re");
            const std::regex libname(R"re(\bname\s*=\s*"([A-Za-z0-9_.+-]+)")re");
            for (std::sregex_iterator it(src.begin(), src.end(), attribute), end; it != end; ++it) {
                std::smatch m;
                string body = (*it)[1];
                if (!std::regex_search(body, m, libname))
                    throw Local("unresolved native library attribute");
                libraries.insert(m[1]);
            }
            for (auto &dir : native_search)
                for (auto &lib : libraries)
                    for (auto &name : {lib, "lib" + lib + ".a"}) {
                        auto path = dir / name;
                        if (fs::is_regular_file(path))
                            inputs.insert(path);
                    }
        }
        string data = read(depfile);
        auto first = data.find("\n\n");
        if (first == string::npos)
            throw Local("missing dependency records");
        bool any = false;
        for (auto &line : split(data.substr(first + 2), '\n')) {
            if (line.empty())
                continue;
            if (line.compare(0, 10, "# env-dep:") == 0) {
                string key = line.substr(10, line.find('=', 10) - 10);
                if (getenv(key.c_str()))
                    environment[key] = env(key.c_str());
                continue;
            }
            if (line.back() != ':')
                throw Local("invalid dependency record");
            auto spelling = fs::absolute(replace(line.substr(0, line.size() - 1), "\\ ", " "));
            auto path = spelling.lexically_normal();
            if (spelling != path) {
                if (fs::canonical(spelling) != fs::canonical(path))
                    throw Local("dependency traversal crosses a symlink");
                for (auto parent = spelling.parent_path(); parent != parent.root_path();
                     parent = parent.parent_path())
                    if (parent.filename() == "..")
                        dirs.insert(parent.parent_path().lexically_normal());
            }
            inputs.insert(path);
            if (path.extension() == ".rmeta" && (!linker.empty() || static_archive)) {
                for (auto ext : {".rlib", ".so"}) {
                    auto binary = path;
                    binary.replace_extension(ext);
                    if (fs::is_regular_file(binary))
                        inputs.insert(binary);
                }
            }
            any = true;
        }
        if (!any)
            throw Local("empty dependency records");
        needs_host = audit(work.dir, depfile, loads.fd, root, search, inputs, dirs);
        if (needs_host) {
            for (auto key : {"CARGO_MANIFEST_DIR", "CARGO_PKG_NAME", "OUT_DIR"})
                if (getenv(key))
                    environment[key] = env(key);
        }
        if (getenv("RUSTC_BOOTSTRAP"))
            environment["RUSTC_BOOTSTRAP"] = env("RUSTC_BOOTSTRAP");
    }
};

// Warm hits read exactly one shared cache line group. Writers only lock a
// colliding slot while publishing it. POSIX record locks die with the client,
// so a later writer can repair an interrupted (odd-generation) publication.
class Digests {
    using Key = std::array<uint64_t, 7>;
    struct Entry {
        uint64_t sequence, key[7], digest;
    };
    static constexpr size_t slots = 16384, bytes = slots * sizeof(Entry);
    Fd backing;
    Entry *table;
    static int acquire() {
        int fd = -1;
        Lease owner{dcc_broker_worker_get(DCC_RUST_DIGEST_TABLE, &fd)};
        if (owner.id >= 0) {
            Fd file(dcc_job_memfd("distcc-rust-input-digests"));
            check(!ftruncate(file.fd, bytes) &&
                      !fcntl(file.fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL),
                  "bound digest table");
            dcc_broker_worker_put(owner.id, file.fd);
            owner.id = -1;
            fd = file.fd;
            file.fd = -1;
        }
        return fd;
    }

  public:
    Digests() : backing(acquire()) {
        check(descriptor_state(backing.fd).st_size == bytes, "digest table size");
        table = static_cast<Entry *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, backing.fd, 0));
        check(table != MAP_FAILED, "map digest table");
    }
    ~Digests() { munmap(table, bytes); }
    void get(const fs::path &path, int fd, const struct stat &st, unsigned char *out) {
        Key key = {uint64_t(st.st_dev),         uint64_t(st.st_ino),          uint64_t(st.st_size),
                   uint64_t(st.st_mtim.tv_sec), uint64_t(st.st_mtim.tv_nsec), uint64_t(st.st_ctim.tv_sec),
                   uint64_t(st.st_ctim.tv_nsec)};
        size_t i = XXH3_64bits(key.data(), sizeof(key)) % slots;
        Entry &e = table[i];
        auto seq = __atomic_load_n(&e.sequence, __ATOMIC_SEQ_CST);
        if (seq && !(seq & 1)) {
            Key observed;
            for (size_t j = 0; j < key.size(); ++j)
                observed[j] = __atomic_load_n(e.key + j, __ATOMIC_SEQ_CST);
            uint64_t value = __atomic_load_n(&e.digest, __ATOMIC_SEQ_CST);
            if (observed == key && seq == __atomic_load_n(&e.sequence, __ATOMIC_SEQ_CST)) {
                memcpy(out, &value, sizeof(value));
                return;
            }
        }
        check(!dcc_file_digest(fd, st.st_size, out), "hash Rust input");
        verify(path, st, fd);
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = i;
        lock.l_len = 1;
        if (fcntl(backing.fd, F_SETLK, &lock)) {
            if (errno == EACCES || errno == EAGAIN)
                return;
            check(false, "lock digest entry");
        }
        seq = __atomic_load_n(&e.sequence, __ATOMIC_SEQ_CST) | 1;
        __atomic_store_n(&e.sequence, seq, __ATOMIC_SEQ_CST);
        for (size_t j = 0; j < key.size(); ++j)
            __atomic_store_n(e.key + j, key[j], __ATOMIC_SEQ_CST);
        uint64_t digest;
        memcpy(&digest, out, sizeof(digest));
        __atomic_store_n(&e.digest, digest, __ATOMIC_SEQ_CST);
        __atomic_store_n(&e.sequence, seq + 1, __ATOMIC_SEQ_CST);
        lock.l_type = F_UNLCK;
        check(!fcntl(backing.fd, F_SETLK, &lock), "unlock digest entry");
    }
};

class Inputs {
    States states;
    std::vector<Fd> held;

  public:
    std::vector<dcc_job_input> files;
    Inputs(const std::set<fs::path> &paths, timespec start) {
        if (paths.size() > DCC_JOB_INPUT_MAX)
            throw Local("too many Rust inputs");
        Digests digests;
        for (const auto &p : paths) {
            if (below(p, DCC_RUST_ROOT) || below(p, "/dev") || below(p, "/proc") || below(p, "/sys") ||
                std::any_of(runtime.begin(), runtime.end(), [&](auto &r) { return below(p, r); }))
                throw Local("input overlaps worker runtime");
            int opened = open(p.c_str(), O_RDONLY | O_CLOEXEC);
            if (opened < 0 && (errno == ENOENT || errno == ENOTDIR || errno == ELOOP || errno == EACCES))
                throw Local("unavailable Rust input " + p.string());
            held.emplace_back(opened);
            int fd = held.back().fd;
            auto st = descriptor_state(fd);
            auto after = [&](timespec t) {
                return t.tv_sec > start.tv_sec || (t.tv_sec == start.tv_sec && t.tv_nsec > start.tv_nsec);
            };
            if (after(st.st_ctim) || after(st.st_mtim))
                throw Local("input changed during dependency discovery");
            auto entry = states.emplace(p, st);
            dcc_job_input in{};
            in.path = const_cast<char *>(entry.first->first.c_str());
            in.fd = fd;
            in.plain_size = in.wire_size = st.st_size;
            verify(p, st, fd);
            digests.get(p, fd, st, in.digest);
            in.cached = st.st_size <= DCC_FILE_CACHE_MAX ? DCC_CACHE_LOOKUP : DCC_UNCACHED;
            files.push_back(in);
        }
    }
    void verify_all() const {
        for (auto &f : files)
            verify(f.path, states.at(f.path), f.fd);
    }
};

string profile_context(const string &compiler) {
    return compiler + '\0' + env("LD_LIBRARY_PATH") + '\0' + env("LD_PRELOAD");
}
string profile(const fs::path &compiler, const Work &work, const States &tool) {
    auto library = native_library();
    auto watched = tool;
    watched.emplace(library, metadata(library));
    cpu_set_t mask;
    CPU_ZERO(&mask);
    check(!sched_getaffinity(0, sizeof(mask), &mask), "native CPU affinity");
    auto context = "native:" + profile_context(compiler) + '\0' + library.string() + '\0' +
                   read("/proc/sys/kernel/random/boot_id") +
                   string(reinterpret_cast<char *>(&mask), sizeof(mask));
    return memo(context, watched, [&] {
        Env envs = {{"LD_PRELOAD", library.string() + " " + env("LD_PRELOAD")},
                    {"DISTCC_RUST_NATIVE", work.dir},
                    {"DISTCC_RUST_NATIVE_CAPTURE", "1"}};
        Fd quiet(dcc_job_memfd("rust-native-diagnostics"));
        auto cmd = compiler_command(compiler);
        cmd.insert(cmd.end(), {"-Ctarget-cpu=native", "--print=cfg"});
        if (run(cmd, envs, quiet.fd, quiet.fd))
            throw Local("native CPU discovery failed");
        return read(work.dir / "cpu") + '\0' + read(work.dir / "features");
    });
}
string relocated(string data, const Invocation &inv) {
    for (auto &item : inv.outputs)
        data = replace(std::move(data), string(DCC_RUST_OUT) + "/" + item.first, item.second);
    return replace(std::move(data), DCC_RUST_OUT, inv.outdir.string());
}
int finish_links(Result &result, const Invocation &inv, const fs::path &work, const fs::path &compiler) {
    std::map<string, dcc_named_output *> outputs;
    std::set<fs::path> generated;
    Words plans;
    for (unsigned i = 0; i < result.named_count; ++i) {
        auto &f = result.named[i];
        string name = f.name;
        if (name.compare(0, sizeof(DCC_RUST_LINK_PREFIX) - 1, DCC_RUST_LINK_PREFIX)) {
            outputs.emplace(name, &f);
            continue;
        }
        if (fs::path(f.destination).parent_path() != work)
            throw Local("invalid linker artifact");
        check(!rename(f.temporary, f.destination), f.destination);
        free(f.temporary);
        f.temporary = nullptr;
        if (fs::path(name).extension() == ".plan")
            plans.push_back(f.destination);
        else if (fs::path(name).extension() == ".input")
            generated.insert(fs::path(DCC_RUST_OUT) / name);
        else
            throw Local("invalid linker artifact type");
    }
    for (auto &plan : plans) {
        auto args = split(read(plan), '\0');
        if (args.size() < 2 || !outputs.count(args.front()))
            throw Local("invalid linker output");
        auto &f = *outputs.at(args.front());
        Work final(fs::path(f.destination).parent_path());
        auto linked = final.dir / args.front();
        auto driver = executable(inv.linker);
        Words cmd = {"/usr/bin/bwrap", "--unshare-all", "--die-with-parent",
                     "--ro-bind",      "/usr",          "/usr",
                     "--dev",          "/dev",          "--proc",
                     "/proc",          "--tmpfs",       "/tmp",
                     "--clearenv",     "--setenv",      "PATH",
                     "/usr/bin:/bin"};
        for (auto p : {"/bin", "/lib", "/lib64", "/etc/ld.so.cache"})
            if (fs::exists(p))
                cmd.insert(cmd.end(), {"--ro-bind", p, p});
        std::set<fs::path> needed = inv.search;
        needed.insert(driver);
        needed.insert(inv.inputs.begin(), inv.inputs.end());
        for (auto &path : needed) {
            if (!below(path, "/usr") && !below(path, "/lib") && !below(path, "/lib64"))
                cmd.insert(cmd.end(), {"--ro-bind", path, path});
        }
        auto cwd = fs::current_path();
        cmd.insert(cmd.end(), {"--dir", cwd, "--chdir", cwd, "--ro-bind", work, DCC_RUST_OUT, "--bind",
                               final.dir, DCC_RUST_FINAL, "--", driver});
        unsigned destinations = 0;
        Words command;
        fs::path sysroot = inv.sysroot.empty() ? compiler.parent_path().parent_path() : fs::path(inv.sysroot);
        auto path_arg = [&](const fs::path &p, bool directory) {
            if (!p.is_absolute() || p.lexically_normal() != p)
                return false;
            if (directory)
                return inv.search.count(p) != 0 || below(p, sysroot / "lib/rustlib");
            return generated.count(p) != 0 || inv.inputs.count(p) != 0;
        };
        const std::set<string> switches = {"-m64",
                                           "-m32",
                                           "-shared",
                                           "-pie",
                                           "-no-pie",
                                           "-static",
                                           "-static-pie",
                                           "-nodefaultlibs",
                                           "-nostdlib",
                                           "-nostartfiles",
                                           "-rdynamic",
                                           "-s",
                                           "-pthread",
                                           "-Wl,--as-needed",
                                           "-Wl,--no-as-needed",
                                           "-Wl,-Bstatic",
                                           "-Wl,-Bdynamic",
                                           "-Wl,--eh-frame-hdr",
                                           "-Wl,--gc-sections",
                                           "-Wl,--no-gc-sections",
                                           "-Wl,--no-undefined-version",
                                           "-Wl,--export-dynamic",
                                           "-Wl,-z,relro,-z,now",
                                           "-Wl,-z,relro",
                                           "-Wl,-z,now",
                                           "-Wl,-z,noexecstack",
                                           "-Wl,--whole-archive",
                                           "-Wl,--no-whole-archive",
                                           "-Wl,--start-group",
                                           "-Wl,--end-group",
                                           "-Wl,--push-state",
                                           "-Wl,--pop-state",
                                           "-fuse-ld=lld"};
        for (auto it = args.begin() + 1; it != args.end(); ++it) {
            string arg = replace(*it, DCC_RUST_COMPILER, compiler.parent_path().parent_path());
            if (arg == "-o") {
                if (++it == args.end() || *it != (fs::path(DCC_RUST_FINAL) / f.name).string())
                    throw Local("invalid link destination");
                ++destinations;
                command.insert(command.end(), {"-o", *it});
                continue;
            }
            if (arg == "-L") {
                if (++it == args.end())
                    throw Local("incomplete linker search directory");
                string path = replace(*it, DCC_RUST_COMPILER, compiler.parent_path().parent_path());
                if (!path_arg(path, true))
                    throw Local("unexpected linker search directory: " + path);
                command.insert(command.end(), {"-L", path});
                continue;
            }
            bool ok = path_arg(arg, false) || switches.count(arg) || inv.link_arguments.count(arg);
            if (arg.compare(0, 2, "-l") == 0 && arg.size() > 2)
                ok |= arg.find_first_not_of(
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_+.-:", 2) ==
                      string::npos;
            for (string prefix : {"-Wl,--version-script=", "-Wl,--dynamic-list="})
                if (arg.compare(0, prefix.size(), prefix) == 0)
                    ok |= generated.count(arg.substr(prefix.size())) != 0;
            if (!ok)
                throw Local("unsupported client linker argument: " + arg);
            command.push_back(std::move(arg));
        }
        if (destinations != 1)
            throw Local("missing linker destination");
        string response;
        for (const auto &arg : command) {
            response += '"';
            for (char c : arg) {
                if (c == '"' || c == '\\')
                    response += '\\';
                response += c;
            }
            response += "\"\n";
        }
        auto response_path = work / "link.rsp";
        Fd response_fd(open(response_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
        put(response_fd.fd, response);
        cmd.push_back(string("@") + DCC_RUST_OUT + "/link.rsp");
        const std::vector<int> inherited;
        int rc = run(cmd, {}, 1, 2, &inherited);
        if (rc)
            return rc;
        struct stat st = metadata(linked);
        check(S_ISREG(st.st_mode) && !rename(linked.c_str(), f.temporary), f.temporary);
        close(f.fd);
        f.fd = open(f.temporary, O_RDWR | O_CLOEXEC);
        check(f.fd >= 0, f.temporary);
        f.mode = st.st_mode & 0777;
    }
    return 0;
}
void publish(Result &result, const Invocation &inv) {
    mode_t mask = umask(0);
    umask(mask);
    for (unsigned i = 0; i < result.named_count; ++i) {
        auto &f = result.named[i];
        if (!f.temporary)
            continue;
        struct stat existing;
        if (!fstatat(AT_FDCWD, f.destination, &existing, AT_SYMLINK_NOFOLLOW)) {
            if (S_ISDIR(existing.st_mode))
                throw std::runtime_error(string("output is a directory: ") + f.destination);
        } else
            check(errno == ENOENT, f.destination);
        if (fs::path(f.name).extension() == ".d" || inv.dep_names.count(f.name)) {
            auto text = relocated(contents(f.fd), inv);
            check(!ftruncate(f.fd, 0) && lseek(f.fd, 0, SEEK_SET) == 0, "rewrite dependency file");
            put(f.fd, text);
        }
        check(!fchmod(f.fd, f.mode & ~mask), "set Rust output mode");
        int fd = f.fd;
        f.fd = -1;
        check(!close(fd), f.temporary);
    }
    for (unsigned i = 0; i < result.named_count; ++i) {
        auto &f = result.named[i];
        if (!f.temporary)
            continue;
        check(!renameat(AT_FDCWD, f.temporary, AT_FDCWD, f.destination), f.destination);
        free(f.temporary);
        f.temporary = nullptr;
    }
}
int distributed(char **original, const Words &args, const States &responses) {
    const string compiler = *original;
    if (env("DISTCC_BROKER").empty())
        throw Local("Rust distribution requires DISTCC_BROKER");
    Invocation inv(args);
    auto exe = executable(compiler);
    struct stat source;
    if (stat(inv.source.c_str(), &source))
        throw Local("source unavailable");
    dcc_work_hint hint{};
    hint.kind = DCC_WORK_RUST;
    hint.key = hash(inv.source.string());
    for (auto &s : args)
        hint.key = hash(s, hint.key);
    if (!hint.key)
        hint.key = 1;
    hint.scope_key = hash(inv.scope);
    hint.family = hash(inv.opt, hash(inv.name));
    for (auto &s : inv.types)
        hint.family = hash(s, hint.family);
    bool metadata_only = std::all_of(inv.actions.begin(), inv.actions.end(), [](const string &s) {
        auto action = s.substr(0, s.find('='));
        return action == "dep-info" || action == "metadata";
    });
    dcc_job job{};
    job.kind = DCC_JOB_RUST;
    job.rust.phase = metadata_only    ? DCC_RUST_METADATA
                     : inv.opt == "0" ? DCC_RUST_CODEGEN
                                      : DCC_RUST_OPTIMIZED;
    string cwd = fs::current_path();
    job.cwd = cwd.data();
    dcc_job_input primary{};
    primary.path = const_cast<char *>(inv.source.c_str());
    primary.plain_size = source.st_size;
    job.inputs = &primary;
    job.count = 1;
    hint.work_units = dcc_rust_source_bytes(&job);
    hint.bytes = std::max<uint64_t>(1, source.st_size);
    hint.memory_class = dcc_resource_class(&job);
    if (hint.bytes <= 65536) {
        Lease lease{dcc_broker_try_local(&hint)};
        if (lease.id >= 0) {
            for (auto &[p, st] : responses)
                verify(p, st);
            Words cmd;
            for (auto p = original; *p; ++p)
                cmd.emplace_back(*p);
            auto start = std::chrono::steady_clock::now();
            int status = run(cmd, {}, 1, 2);
            struct rusage usage{};
            getrusage(RUSAGE_CHILDREN, &usage);
            uint64_t cpu = (usage.ru_stime.tv_sec + usage.ru_utime.tv_sec) * 1000000000ULL +
                           (usage.ru_stime.tv_usec + usage.ru_utime.tv_usec) * 1000ULL;
            dcc_broker_local_done(
                lease.id, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                cpu, status);
            lease.id = -1;
            return status;
        }
    }
    Work work;
    States tool = {{exe, metadata(exe)}};
    string info = memo("compiler:" + profile_context(exe), tool,
                       [&] { return query(exe, "-vV") + '\0' + query(exe, "--print=sysroot"); });
    auto separator = info.find('\0');
    if (separator == string::npos)
        throw std::runtime_error("invalid Rust compiler memo");
    string version = info.substr(0, separator);
    string id = identity(version), cpu, features;
    job.rust.compiler = id.data();
    auto cmd = inv.command(field(version, "host: "), info.substr(separator + 1));
    if (inv.cpu == "native") {
        auto value = profile(exe, work, tool);
        auto sep = value.find('\0');
        if (sep == string::npos)
            throw std::runtime_error("invalid native CPU memo");
        cpu = value.substr(0, sep);
        features = value.substr(sep + 1);
    }
    timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    inv.discover(exe, info.substr(separator + 1), work);
    string host = inv.needs_host ? field(version, "host: ") : "";
    if (host.size() >= sizeof(hint.rust_host))
        throw Local("Rust host triple too long");
    if (!host.empty() && env("DISTCC_VERBOSE") == "1")
        fprintf(stderr, "distcc-rust: proc-macro host ABI %s\n", host.c_str());
    strcpy(hint.rust_host, host.c_str());
    job.rust.host = host.data();
    job.rust.cpu = cpu.data();
    job.rust.features = features.data();
    Words directories(inv.dirs.begin(), inv.dirs.end()), environment;
    for (auto &[k, v] : inv.environment)
        environment.push_back(k + "=" + v);
    auto dirs = pointers(directories), envs = pointers(environment), compiler_args = pointers(cmd);
    job.rust.directories = dirs.data();
    job.rust.environment = envs.data();
    job.argv = compiler_args.data();
    job.outdir = inv.outdir.c_str();
    auto linkdir = work.dir / "link";
    if (!inv.linker.empty()) {
        fs::create_directory(linkdir);
        job.linkdir = linkdir.c_str();
    }
    Words pairs;
    for (auto &[k, v] : inv.outputs)
        pairs.insert(pairs.end(), {k, v});
    auto destinations = pointers(pairs);
    job.destinations = destinations.data();
    Inputs inputs(inv.inputs, start);
    job.count = inputs.files.size();
    job.inputs = inputs.files.data();
    hint.bytes = std::max<uint64_t>(1, dcc_resource_input_bytes(&job));
    hint.memory_class = dcc_resource_class(&job);
    uint64_t units = dcc_rust_source_bytes(&job);
    hint.work_units = std::min<uint64_t>(UINT32_MAX, units << job.rust.phase);
    for (auto &f : inputs.files)
        hint.key = XXH3_64bits_withSeed(f.digest, sizeof(f.digest), hint.key);
    if (!hint.key)
        hint.key = 1;
    auto verify_job = [&] {
        inputs.verify_all();
        for (auto &[p, st] : responses)
            verify(p, st);
        verify(exe, tool.at(exe));
    };
    verify_job();
    Result result;
    string forwarded;
    struct Early {
        const Invocation &inv;
        decltype(verify_job) &verify;
        string &line;
        dcc_job &job;
    } early{inv, verify_job, forwarded, job};
    job.metadata_context = &early;
    if (inv.pipeline)
        job.publish_metadata = [](void *context, dcc_named_output *f, const char *line) {
            auto &e = *static_cast<Early *>(context);
            try {
                e.verify();
                Json obj(json_tokener_parse(line), json_object_put);
                if (json_field(obj.get(), "$message_type") != "artifact" ||
                    json_field(obj.get(), "emit") != "metadata" ||
                    json_field(obj.get(), "artifact") != (fs::path(DCC_RUST_OUT) / f->name).string() ||
                    (e.inv.metadata_name.empty() ? fs::path(f->name).extension() != ".rmeta"
                                                 : e.inv.metadata_name != f->name))
                    throw Local("invalid metadata notification");
                json_object_object_add(obj.get(), "artifact", json_object_new_string(f->destination));
                string notice =
                    string(json_object_to_json_string_ext(obj.get(), JSON_C_TO_STRING_PLAIN)) + '\n';
                mode_t mask = umask(0);
                umask(mask);
                check(!fchmod(f->fd, f->mode & ~mask), "set metadata mode");
                check(!renameat(AT_FDCWD, f->temporary, AT_FDCWD, f->destination), f->destination);
                e.job.metadata_published = 1;
                free(f->temporary);
                f->temporary = nullptr;
                e.line = line;
                put(2, notice);
                return 0;
            } catch (const std::exception &error) {
                fprintf(stderr, "distcc-rust: metadata: %s\n", error.what());
                return -1;
            }
        };
    int transport = dcc_transfer_prepared(&job, nullptr, nullptr, &hint, -1, &result);
    if (job.metadata_published) {
        try {
            verify_job();
        } catch (const std::exception &e) {
            fprintf(stderr, "distcc-rust: %s\n", e.what());
            return EXIT_DISTCC_FAILED;
        }
        if (transport)
            return EXIT_DISTCC_FAILED;
    } else
        verify_job();
    if (transport == EXIT_LOCAL_COMPILE)
        throw Local("no worker with the required Rust host ABI");
    if (transport)
        throw std::runtime_error("Rust transport failed (" + std::to_string(transport) + ")");
    int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
    if (status == EXIT_LOCAL_COMPILE)
        throw Local("worker has no matching Rust toolchain");
    if (status && !job.metadata_published && env("DISTCC_FALLBACK") != "0" &&
        env("DISTCC_SKIP_LOCAL_RETRY") != "1")
        throw Local("worker compilation failed; retrying locally");
    try {
        if (!status && !inv.linker.empty())
            status = finish_links(result, inv, linkdir, exe);
        if (!status)
            publish(result, inv);
    } catch (const std::exception &e) {
        if (!job.metadata_published)
            throw;
        fprintf(stderr, "distcc-rust: %s\n", e.what());
        return EXIT_DISTCC_FAILED;
    }
    auto diagnostics = contents(result.fd[DCC_JOB_STDERR]);
    if (!forwarded.empty()) {
        auto pos = diagnostics.find(forwarded);
        if (pos != string::npos)
            diagnostics.erase(pos, forwarded.size());
    }
    put(2, relocated(std::move(diagnostics), inv));
    put(1, relocated(contents(result.fd[DCC_JOB_STDOUT]), inv));
    return status;
}
} // namespace
extern "C" int dcc_rust_client(char **argv) {
    Words args;
    for (char **p = argv + 1; *p; ++p)
        args.emplace_back(*p);
    auto expand_args = [&]() {
        ResponseFiles response;
        try {
            response.expand(args);
        } catch (const std::system_error &e) {
            throw Local(e.what());
        }
        return response;
    };
    try {
        auto response = expand_args();
        return distributed(argv, response.flat, response.files);
    } catch (const Local &e) {
        if (env("DISTCC_VERBOSE") == "1")
            fprintf(stderr, "distcc-rust: local: %s\n", e.what());
    } catch (const fs::filesystem_error &e) {
        if (env("DISTCC_VERBOSE") == "1")
            fprintf(stderr, "distcc-rust: local: %s\n", e.what());
    } catch (const std::exception &e) {
        fprintf(stderr, "distcc-rust: %s\n", e.what());
        if (env("DISTCC_FALLBACK") == "0")
            return EXIT_DISTCC_FAILED;
    }
    execvp(*argv, argv);
    return EXIT_COMPILER_MISSING;
}
