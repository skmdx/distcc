/* Plan the Rust dependency closure using rustc itself, and lend original
 * file descriptors to the pooled transport until the result is verified. */
#include "rust/api.h"
#include "rust/common.hh"
#include "rust/probe.h"
#include "rust/response.hh"
#include "rust/runtime.hh"
#include "rust/sandbox.hh"

#include <array>
#include <chrono>
#include <elf.h>
#include <functional>
#include <iconv.h>
#include <memory>
#include <optional>
#include <sched.h>
#include <set>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>

extern "C" {
#include "broker.h"
#include "exitcode.h"
#include "file_cache.h"
#include "lock.h"
#include "plan_memo.h"
#include "resource.h"
#include "where.h"
#include "workload.h"
}
using namespace rust;
namespace {
using States = std::map<fs::path, struct stat>;
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
bool unavailable_input(int error) {
    return error == ENOENT || error == ENOTDIR || error == ELOOP || error == EACCES;
}
template <class Read> auto read_user_input(Read read) {
    try {
        return read();
    } catch (const std::system_error &error) {
        if (unavailable_input(error.code().value())) throw Local(error.what());
        throw;
    }
}
/* rustc's -Zshell-argfiles uses shlex tokenization, with neither shell
 * expansions nor execution. Plain response files retain one argument per line. */
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
            auto args = quoted ? response_words(data, ResponseSyntax::RustShell)
                               : split(replace(data, "\r\n", "\n"), '\n');
            verify(path, entry.first->second, fd.fd);
            expand(args);
            active.erase(canonical);
        }
    }
};
uint64_t hash(const string &s, uint64_t seed = 0) {
    return XXH3_64bits_withSeed(s.data(), s.size(), seed);
}
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
fs::path absolute_name(const string &s) {
    return fs::absolute(s).lexically_normal();
}
struct Option {
    string key;
    std::optional<string> value;
    void append(Words &v) const {
        if (!key.empty())
            v.push_back(key);
        if (value)
            v.push_back(*value);
    }
};
inline std::vector<Option> options(const Words &args) {
    static const std::set<string> values = {"--cap-lints",
                                            "--cfg",
                                            "--check-cfg",
                                            "--crate-name",
                                            "--crate-type",
                                            "--edition",
                                            "--error-format",
                                            "--extern",
                                            "--json",
                                            "--remap-path-prefix",
                                            "--target",
                                            "--sysroot",
                                            "--emit",
                                            "--out-dir",
                                            "-o",
                                            "-C",
                                            "-L",
                                            "-l",
                                            "-A",
                                            "-W",
                                            "-D",
                                            "-F",
                                            "--allow",
                                            "--warn",
                                            "--deny",
                                            "--forbid",
                                            "--force-warn",
                                            "--diagnostic-width",
                                            "--color",
                                            "--codegen",
                                            "-Z"};
    std::vector<Option> parsed;
    for (size_t i = 0; i < args.size(); ++i) {
        const string &word = args[i];
        if (word == "--") {
            parsed.push_back({word, {}});
            for (++i; i < args.size(); ++i)
                parsed.push_back({"", args[i]});
            break;
        }
        auto eq = word.find('=');
        string key = word.substr(0, eq);
        if (values.count(key)) {
            if (eq == string::npos && i + 1 == args.size())
                throw Local("missing option value");
            parsed.push_back(
                {key == "--codegen" ? "-C" : key, eq == string::npos ? args[++i] : word.substr(eq + 1)});
        } else if (word.size() > 2 && values.count(word.substr(0, 2))) {
            parsed.push_back({word.substr(0, 2), word.substr(2)});
        } else if (!word.empty() && word[0] == '-')
            parsed.push_back({word, {}});
        else
            parsed.push_back({"", word});
    }
    return parsed;
}
bool thin_link_argument(const string &arg) {
    auto options = split(arg, ',');
    return std::any_of(options.begin(), options.end(), [](const string &s) {
        return s == "-flto=thin" || s == "-plugin-opt=thinlto";
    });
}
struct Invocation {
    Words base, actions, types;
    fs::path source, outdir = fs::current_path(), scope;
    string single_output, target, sysroot, name, opt = "0", cpu, linker;
    std::set<fs::path> inputs, search, native_search, dirs;
    std::map<fs::path, fs::path> observed;
    std::set<string> libraries;
    bool trace_macro = false, needs_host = false, static_archive = false, pipeline = false;
    bool multi_cgu = true, incremental = false, linker_plugin_lto = false, explicit_linker = false;
    Words compiler_flags;
    string metadata_name;
    std::set<string> link_arguments;
    std::set<string> dep_names;
    std::map<string, string> outputs;
    Env environment;
    bool distributed_link() const {
        return !linker.empty() && !host_artifact() &&
            (linker_plugin_lto || std::any_of(link_arguments.begin(), link_arguments.end(), thin_link_argument));
    }
    bool host_artifact() const {
        return name == "build_script_build" ||
            (!types.empty() && std::all_of(types.begin(), types.end(),
                [](const string &type) { return type == "proc-macro"; }));
    }
    explicit Invocation(const Words &args) {
        auto opts = options(args);
        incremental = std::any_of(opts.begin(), opts.end(), [](const Option &o) {
            return o.key == "-C" && o.value->rfind("incremental=", 0) == 0;
        });
        bool notify = false, native_link = false, emits_link = false, emits_metadata = false;
        string driver = "cc";
        for (auto &o : opts) {
            if (o.key == "-C" && o.value->rfind("incremental=", 0) == 0)
                continue;
            if (incremental && o.key == "-C" && o.value->rfind("codegen-units=", 0) == 0) {
                auto count = o.value->substr(sizeof("codegen-units=") - 1);
                char *end;
                auto units = strtoul(count.c_str(), &end, 10);
                if (!count.empty() && !*end && units > 16)
                    o.value = "codegen-units=16";
            }
            if (incremental)
                o.append(compiler_flags);
            const auto &k = o.key;
            string v = o.value.value_or("");
            if (k == "-" || (k.empty() && v == "-"))
                throw Local("standard input requires local compilation");
            if (k == "--explain" || k == "--print" || k.compare(0, 8, "--print=") == 0)
                throw Local("local rustc query or test");
            if (k == "--")
                continue;
            if (k == "--test")
                native_link = true;
            if (k == "--json")
                for (const auto &notice : split(v, ','))
                    notify |= notice == "artifacts";
            if (k == "--emit") {
                for (const auto &item : split(v, ',')) {
                    const string output = item.substr(0, item.find('='));
                    emits_link |= output == "link";
                    emits_metadata |= output == "metadata";
                    actions.push_back(item);
                }
                continue;
            }
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
                base.push_back(!v.empty() && v.front() == '-' ? "./" + v : v);
                continue;
            }
            if (k == "--crate-name")
                name = v;
            if (k == "--crate-type") {
                for (auto &type : split(v, ',')) {
                    types.push_back(type);
                    needs_host |= type == "proc-macro";
                    native_link |= type == "bin" || type == "cdylib" || type == "dylib" || type == "proc-macro";
                    static_archive |= type == "staticlib";
                }
                continue;
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
                if (key == "linker") {
                    driver = val;
                    explicit_linker = true;
                }
                if (key == "linker-plugin-lto")
                    linker_plugin_lto = val != "no" && val != "off" && val != "false";
                if (key == "codegen-units")
                    multi_cgu = val != "1";
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
        if (actions.empty()) {
            actions.push_back("link");
            emits_link = true;
        }
        if (std::all_of(actions.begin(), actions.end(),
                        [](const string &a) { return a.substr(0, a.find('=')) == "dep-info"; }))
            throw Local("dependency query");
        pipeline = notify && emits_metadata && emits_link;
        if (emits_link && (native_link || types.empty()))
            linker = driver;
        if (source.empty())
            throw Local("compiler query");
        if (name.empty())
            name = source.stem();
        if (!single_output.empty())
            outdir = absolute_name(single_output).parent_path();
    }
    fs::path work_parent() const {
        auto tmp = env("TMPDIR");
        if (!tmp.empty())
            return tmp;
        if (env("SANDBOX_ON") != "1")
            return "/tmp";
        auto phase_tmp = env("T");
        return phase_tmp.empty() ? outdir : fs::path(phase_tmp);
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
        Env envs = {{"RUSTC_BOOTSTRAP", "1"},
                    {"DISTCC_RUST_LOADS_FD", std::to_string(loads.fd)}};
        Words cmd = compiler_command(compiler);
        cmd.insert(cmd.end(), base.begin(), base.end());
        for (auto &type : types)
            cmd.emplace_back("--crate-type=" + type);
        if (sysroot.empty())
            cmd.insert(cmd.end(), {"--sysroot", root});
        auto staging = work.dir / "probe";
        fs::create_directory(staging);
        auto depfile = staging / "probe.d";
        cmd.insert(cmd.end(), {"-Zbinary-dep-depinfo", "--emit=dep-info=" + depfile.string()});
        {
            ProbeSlot slot;
            envs["DISTCC_RUST_SYSROOT"] = root;
            if (run_probe(cmd, envs, staging, trace_macro, quiet.fd, {loads.fd}))
                throw Local(trace_macro ? "isolated dependency expansion failed" : "dependency expansion failed");
        }
        for (const auto &dir : native_search) {
            if (runtime_path(dir, root))
                continue;
            for (const auto &entry : fs::directory_iterator(dir))
                if (entry.path().extension() == ".a" && entry.is_regular_file())
                    inputs.insert(entry.path());
            for (const auto &lib : libraries)
                for (const auto &name : {lib, "lib" + lib + ".so"})
                    if (fs::is_regular_file(dir / name))
                        inputs.insert(dir / name);
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
        needs_host |= audit(depfile, loads.fd, root, search, inputs, dirs, observed);
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
    Inputs(const std::set<fs::path> &paths, const std::map<fs::path, fs::path> &observed, timespec start) {
        if (paths.size() > DCC_JOB_INPUT_MAX)
            throw Local("too many Rust inputs");
        Digests digests;
        for (const auto &p : paths) {
            if (below(p, DCC_RUST_ROOT) || below(p, "/dev") || below(p, "/proc") || below(p, "/sys") ||
                std::any_of(std::begin(runtime), std::end(runtime), [&](auto &r) { return below(p, r); }))
                throw Local("input overlaps worker runtime");
            int opened = open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (opened < 0 && unavailable_input(errno))
                throw Local("unavailable Rust input " + p.string());
            held.emplace_back(opened);
            int fd = held.back().fd;
            // A parent symlink may have changed since the journal was
            // resolved. Check the opened descriptor before hashing its data.
            auto observation = observed.find(p);
            if (observation != observed.end() &&
                fs::read_symlink("/proc/self/fd/" + std::to_string(fd)) != observation->second)
                throw Local("input changed during dependency discovery: " + p.string());
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
string profile(const fs::path &compiler, const Work &work, const States &tool,
               CpuProfile mode = CpuProfile::Target) {
    auto library = native_library();
    auto watched = tool;
    watched.emplace(library, metadata(library));
    auto prefix = mode == CpuProfile::HostUnion ? "host-union:" : "native:";
    auto context = prefix + profile_context(compiler) + '\0' + library.string() + '\0' +
                   read("/proc/sys/kernel/random/boot_id") + affinity_key();
    return memo(context, watched, [&] {
        return capture_cpu_profile(compiler, work, mode);
    });
}
string relocated(string data, const Invocation &inv) {
    for (auto &item : inv.outputs)
        data = replace(std::move(data), string(DCC_RUST_OUT) + "/" + item.first, item.second);
    return replace(std::move(data), DCC_RUST_OUT, inv.outdir.string());
}
// rustc computed its runpaths relative to the worker's output directory.
string relocate_runpath(string arg, const fs::path &destination, const fs::path &compiler_root) {
    size_t start = 0;
    while ((start = arg.find("$ORIGIN/", start)) != string::npos) {
        auto end = arg.find_first_of(",:", start);
        auto base = start + sizeof("$ORIGIN/") - 1;
        auto path = (fs::path(DCC_RUST_OUT) / arg.substr(base, end - base)).lexically_normal();
        if (below(path, DCC_RUST_COMPILER))
            path = compiler_root / path.lexically_relative(DCC_RUST_COMPILER);
        auto relative = path.lexically_relative(destination).string();
        arg.replace(base, end - base, relative);
        start = base + relative.size();
    }
    return arg;
}
bool wrapper(const fs::path &binary) {
    return binary.filename() == "distcc" || binary.filename() == "ccache";
}
fs::path link_driver(const fs::path &name) {
    if (name.has_parent_path() && !wrapper(executable(name)))
        return fs::absolute(name);
    // The sandbox needs the driver itself. Keep its invocation name for
    // multicall drivers such as clang++ and ld.lld.
    for (const auto &dir : split(env("PATH"), ':')) {
        auto candidate = fs::absolute(fs::path(dir) / name.filename());
        auto binary = find_program(candidate);
        if (!binary.empty() && !wrapper(binary))
            return candidate;
    }
    throw std::runtime_error("link driver unavailable: " + name.string());
}
string link_path(const fs::path &tools, const fs::path &name) {
    string path = tools;
    for (const auto &dir : split(env("PATH"), ':')) {
        auto alias = [&](const fs::path &tool) {
            return wrapper(find_program(fs::absolute(fs::path(dir) / tool)));
        };
        auto directory = fs::path(dir);
        if (!wrapper(directory) && !wrapper(directory.parent_path()) &&
            !alias("cc") && !alias("gcc") && !alias("clang") && !alias(name.filename())) {
            if (!path.empty()) path += ':';
            path += dir;
        }
    }
    return path;
}
fs::path thinlto_compiler(const fs::path &driver, const Sandbox &box) {
    auto binary = fs::canonical(driver);
    auto name = binary.filename().string();
    if (name == "clang" || (name.rfind("clang-", 0) == 0 && name.size() > 6 &&
                            name.find_first_not_of("0123456789.", 6) == string::npos))
        return binary;
    // An environment-dependent wrapper can select a different LLVM slot
    // than the client's default clang. Ask through the same confined PATH.
    Words query = box.args;
    query.insert(query.end(), {"--remount-ro", "/", "--", driver, "--version"});
    Fd version(dcc_job_memfd("rust-link-driver")), quiet(dcc_job_memfd("rust-link-driver-diagnostics"));
    const std::vector<int> inherited{box.filter.fd};
    auto status = run(query, {}, version.fd, quiet.fd, &inherited);
    check(lseek(box.filter.fd, 0, SEEK_SET) == 0, "rewind linker seccomp filter");
    auto banner = contents(version.fd);
    if (!status && banner.substr(0, banner.find('\n')).find("clang version ") != string::npos)
        for (const auto &line : split(banner, '\n'))
            if (line.rfind("InstalledDir: /", 0) == 0)
                return find_program((fs::path(line.substr(sizeof("InstalledDir: ") - 1)) / "clang").string());
    return {};
}
int finish_links(Result &result, const Invocation &inv, const fs::path &work, const fs::path &compiler,
                 const fs::path &sysroot, const string &host) {
    std::map<string, dcc_named_output *> outputs;
    Words plans;
    for (unsigned i = 0; i < result.named_count; ++i) {
        auto &f = result.named[i];
        string name = f.name;
        if (name.compare(0, sizeof(DCC_RUST_LINK_PREFIX) - 1, DCC_RUST_LINK_PREFIX)) {
            outputs.emplace(name, &f);
            continue;
        }
        if (fs::path(f.destination).parent_path() != work)
            throw std::runtime_error("invalid linker artifact");
        check(!rename(f.temporary, f.destination), f.destination);
        free(f.temporary);
        f.temporary = nullptr;
        if (fs::path(name).extension() == ".plan")
            plans.push_back(f.destination);
        else if (fs::path(name).extension() != ".input")
            throw std::runtime_error("invalid linker artifact type");
    }
    for (auto &plan : plans) {
        auto args = split(read(plan), '\0');
        if (args.size() < 2 || !outputs.count(args.front()))
            throw std::runtime_error("invalid linker output");
        auto &f = *outputs.at(args.front());
        Work final(fs::path(f.destination).parent_path());
        auto linked = final.dir / args.front();
        auto driver = link_driver(inv.linker);
        Sandbox box(Root::HostWithOutputs);
        auto &cmd = box.args;
        fs::path tools = sysroot / "lib/rustlib" / host / "bin";
        cmd.insert(cmd.end(), {"--unsetenv", "DISTCC_LOG", "--setenv", "PATH", link_path(tools, inv.linker)});
        // Preserve declared inputs and toolchains residing on the shadowed temporary mount.
        auto mounts = inv.search;
        mounts.insert(inv.inputs.begin(), inv.inputs.end());
        mounts.insert(driver);
        mounts.insert(sysroot);
        for (const auto &path : mounts)
            if (below(path, "/tmp"))
                cmd.insert(cmd.end(), {"--ro-bind", path, path});
        auto cwd = fs::current_path();
        if (below(cwd, "/tmp"))
            cmd.insert(cmd.end(), {"--dir", cwd});
        cmd.insert(cmd.end(), {"--chdir", cwd});
        box.mount(work, work);
        box.mount(final.dir, final.dir, true);
        unsigned destinations = 0;
        Words command;
        bool thin = inv.linker_plugin_lto, distributed = false;
        for (auto it = args.begin() + 1; it != args.end(); ++it) {
            auto compiler_root = compiler.parent_path().parent_path();
            string arg = *it;
            thin |= thin_link_argument(arg);
            for (const auto &option : split(arg, ','))
                distributed |= option.rfind("--thinlto-distributor=", 0) == 0;
            if (!inv.link_arguments.count(arg) &&
                (arg.find("-rpath") != string::npos || (it[-1] == "-rpath" || it[-1] == "--rpath")))
                arg = relocate_runpath(std::move(arg), fs::canonical(fs::path(f.destination).parent_path()),
                                       compiler_root);
            arg = replace(std::move(arg), DCC_RUST_COMPILER, compiler_root);
            arg = replace(std::move(arg), string(DCC_RUST_OUT) + "/", work.string() + "/");
            if (arg == "-o") {
                if (++it == args.end() || *it != (fs::path(DCC_RUST_FINAL) / f.name).string())
                    throw std::runtime_error("invalid link destination");
                ++destinations;
                command.insert(command.end(), {"-o", linked});
                continue;
            }
            if (arg == "--output" || arg.rfind("--output=", 0) == 0 || (!arg.empty() && arg.front() == '@'))
                throw std::runtime_error("invalid captured linker output or nested response");
            command.push_back(std::move(arg));
        }
        if (destinations != 1)
            throw std::runtime_error("missing linker destination");
        // Only the local driver's identity selects the remote compiler.
        fs::path binary;
        if (thin && !inv.host_artifact() && !distributed && !env("DISTCC_BROKER").empty())
            binary = thinlto_compiler(driver, box);
        Words launcher;
        std::optional<Work> temporary;
        if (!binary.empty()) {
            temporary.emplace(work.parent_path());
            auto helper = fs::read_symlink("/proc/self/exe").parent_path() / "distcc-thinlto";
            const string shim = DCC_RUST_ROOT "/dtlto-distributor";
            box.mount(helper, shim);
            box.mount(temporary->dir, temporary->dir, true);
            cmd.insert(cmd.end(), {"--setenv", "TMPDIR", temporary->dir});
            command.emplace_back("-Wl,--thinlto-distributor=" + shim +
                                 ",--thinlto-remote-compiler=" + binary.string());
            auto declared = work / "dtlto-inputs";
            Fd list(open(declared.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
            for (const auto &path : inv.inputs)
                put(list.fd, path.string() + '\0');
            launcher = {helper, "--rust-link", work, linked, temporary->dir, binary, std::to_string(box.filter.fd)};
        }
        box.command({driver});
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
        cmd.push_back("@" + response_path.string());
        const std::vector<int> inherited{box.filter.fd};
        launcher.insert(launcher.end(), cmd.begin(), cmd.end());
        int rc = run(launcher, {}, 1, 2, &inherited);
        if (rc)
            return rc;
        Fd output(open(linked.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        auto st = descriptor_state(output.fd);
        check(!rename(linked.c_str(), f.temporary), f.temporary);
        std::swap(f.fd, output.fd);
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
std::optional<int> try_local_frontend(const Invocation &inv, char **original, const States &responses,
                                      const dcc_work_hint &hint) {
    Lease lease{dcc_broker_try_local(&hint)};
    if (lease.id < 0)
        return {};
    for (auto &[p, st] : responses)
        verify(p, st);
    Words cmd;
    for (auto p = original; *p; ++p)
        cmd.emplace_back(*p);
    Env environment;
    if (!inv.linker.empty()) {
        // The lease covers rustc and its linker. Resolve the same native driver
        // as finish_links so a Portage masquerade cannot start a second scheduler.
        if (inv.explicit_linker) {
            auto driver = link_driver(inv.linker);
            // An opaque linker script can invoke distcc by absolute path.
            // Preserve its behavior on the ordinary captured-link path.
            Fd file(open(driver.c_str(), O_RDONLY | O_CLOEXEC));
            char magic[SELFMAG];
            if (::read(file.fd, magic, sizeof(magic)) != sizeof(magic) || memcmp(magic, ELFMAG, SELFMAG))
                return {};
            cmd.emplace_back("-Clinker=" + driver.string());
        }
        environment["PATH"] = link_path({}, inv.linker);
    }
    auto start = std::chrono::steady_clock::now();
    int status = run(cmd, environment, 1, 2);
    struct rusage usage{};
    getrusage(RUSAGE_CHILDREN, &usage);
    uint64_t cpu = (usage.ru_stime.tv_sec + usage.ru_utime.tv_sec) * 1000000000ULL +
                   (usage.ru_stime.tv_usec + usage.ru_utime.tv_usec) * 1000ULL;
    dcc_broker_local_done(lease.id,
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                          cpu, status);
    lease.id = -1;
    return status;
}
struct CguRoute {
    fs::path shim;
    int cgu_peers = 0;
    int native_peers = 0;
};
// A rustc-private LLVM and a system LLVM can report the same version but
// cannot exchange C++ objects inside rustc. Trace the loader without running
// constructors before selecting the slot's in-process hook.
fs::path linked_llvm(const Words &command) {
    Fd output(dcc_job_memfd("rust-llvm-library"));
    if (run(command, {{"LD_TRACE_LOADED_OBJECTS", "1"}}, output.fd, 2))
        throw std::runtime_error("LLVM loader query failed: " + command.front());
    for (const auto &line : split(contents(output.fd), '\n')) {
        auto start = line.find(" => /"), end = line.rfind(" (");
        if (line.compare(0, 8, "\tlibLLVM") == 0 && start != string::npos && end != string::npos)
            return line.substr(start + 4, end - start - 4);
    }
    return {};
}
CguRoute cgu_route(const Invocation &inv, const string &version, const fs::path &sysroot,
                   dcc_work_hint hint, bool metadata_only) {
    CguRoute route;
    auto llvm = llvm_abi(field(version, "LLVM version: "));
    auto host = field(version, "host: ");
    auto triple = inv.target.empty() ? host : inv.target;
    auto compiler = sysroot / "bin/rustc";
    for (auto &helper : metadata_only ? std::vector<fs::path>{} : backend_helpers(sysroot, llvm)) {
        auto shim = helper.parent_path() / "distcc-rust-cgu.so";
        if (!fs::is_regular_file(shim))
            continue;
        auto key = "cgu-target:" + helper.string() + '\0' + profile_context(compiler) + '\0' + llvm + '\0' + triple;
        auto target = memo(key, {{helper, metadata(helper)}, {compiler, metadata(compiler)}}, [&] {
            Fd output(dcc_job_memfd("rust-target"));
            int rc = run({helper, "--target-name", llvm, triple}, {}, output.fd, 2);
            if (rc == EXIT_LOCAL_COMPILE)
                return string{};
            if (rc)
                throw std::runtime_error("LLVM target query failed: " + helper.string());
            auto frontend = linked_llvm(compiler_command(compiler));
            auto backend = linked_llvm({helper});
            if (frontend.empty() || backend.empty() || !fs::equivalent(frontend, backend))
                return string{};
            return contents(output.fd);
        });
        if (target.empty())
            continue;
        string requirement = llvm + " " + target;
        if (requirement.size() >= sizeof(hint.cgu_backend))
            throw Local("CGU target record too long");
        strcpy(hint.cgu_backend, requirement.c_str());
        route.shim = shim;
        break;
    }
    hint.kind = DCC_WORK_RUST_CGU;
    hint.memory_class = 1 + DCC_JOB_RUST_CGU * DCC_MEMORY_BUCKETS + dcc_resource_bucket(hint.bytes);
    route.cgu_peers = dcc_broker_cgu_capacity(&hint, &route.native_peers);
    check(route.cgu_peers >= 0, "Rust capacity request");
    return route;
}
int cgu_frontend(const CguRoute &route, const Invocation &inv, const fs::path &exe, char **original,
                 const States &responses) {
    for (auto &[p, st] : responses)
        verify(p, st);
    auto local = compiler_command(exe);
    std::optional<Work> argfile;
    if (!inv.incremental) {
        for (auto p = original + 1; *p; ++p)
            local.emplace_back(*p);
    } else if (responses.empty()) {
        local.insert(local.end(), inv.compiler_flags.begin(), inv.compiler_flags.end());
    } else {
        argfile.emplace(inv.work_parent());
        unsigned sequence = 0;
        string lines;
        auto flush = [&] {
            if (lines.empty())
                return;
            auto path = argfile->dir / ("args-" + std::to_string(++sequence));
            Fd fd(open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
            put(fd.fd, lines);
            local.push_back("@" + path.string());
            lines.clear();
        };
        for (auto &arg : inv.compiler_flags) {
            if (arg.find_first_of("\r\n") == string::npos)
                lines += arg + "\n";
            else {
                flush();
                local.push_back(arg);
            }
        }
        flush();
    }
    bool machine_outputs = std::all_of(inv.actions.begin(), inv.actions.end(), [](const string &action) {
        auto emit = action.substr(0, action.find('='));
        return emit == "metadata" || emit == "link" || emit == "dep-info" || emit == "obj";
    });
    Env vars = {{"DISTCC_RUST_CGU_OBJECT_ONLY", machine_outputs ? "1" : "0"},
                {"DISTCC_RUST_CGU_PLUGIN_LTO", inv.linker_plugin_lto ? "1" : "0"},
                {"LD_PRELOAD", route.shim.string() + " " + env("LD_PRELOAD")},
                {"DISTCC_RUST_CGU_CLIENT", fs::canonical("/proc/self/exe")},
                {"DISTCC_RUST_CGU_PREVIOUS", env("LD_PRELOAD")}};
    pid_t pid;
    {
        ProbeSlot frontend;
        int pipefd[2];
        check(!pipe2(pipefd, O_CLOEXEC), "CGU frontend completion pipe");
        Fd reader(pipefd[0]);
        {
            Fd writer(pipefd[1]);
            check(!fcntl(writer.fd, F_SETFD, 0), "inherit frontend completion pipe");
            vars["DISTCC_RUST_CGU_STARTED"] = std::to_string(writer.fd);
            pid = spawn(local, vars, 1, 2);
        }
        // EOF releases the frontend slot when LLVM starts, or on early
        // compiler exit. Cargo can now schedule a metadata-dependent crate.
        char unused;
        while (read(reader.fd, &unused, 1) < 0 && errno == EINTR) {
        }
    }
    return collect(pid);
}
bool small_crate(const fs::path &source, uintmax_t bytes) {
    constexpr uintmax_t limit = 64 * 1024;
    if (bytes > limit)
        return false;
    // Cargo normally puts a crate's modules beside its entry point. Stop as
    // soon as their combined size justifies scheduling individual CGUs.
    auto entry = fs::absolute(source);
    auto root = entry.parent_path();
    std::error_code ec;
    bool package = fs::exists(root / "Cargo.toml", ec);
    if (ec) return false;
    bool package_main = package && (entry.stem() == "lib" || entry.stem() == "main");
    if (package && !package_main) {
        root /= entry.stem();
        if (!fs::is_directory(root, ec))
            return !ec || ec == std::errc::no_such_file_or_directory;
    }
    fs::recursive_directory_iterator it(root, ec), end;
    while (!ec && it != end) {
        const auto &path = it->path();
        auto name = path.filename().string();
        if (name.front() == '.' || name == "target" ||
            (package_main && it.depth() == 0 && (name == "tests" || name == "benches" || name == "examples")))
            it.disable_recursion_pending();
        if (path.extension() == ".rs" && path != entry && it->is_regular_file(ec)) {
            auto size = it->file_size(ec);
            if (ec || size > limit - bytes)
                return false;
            bytes += size;
        }
        if (!ec)
            it.increment(ec);
    }
    return !ec;
}
int distributed(char **original, const Words &args, const States &responses) {
    const string compiler = *original;
    if (env("DISTCC_BROKER").empty())
        throw Local("Rust distribution requires DISTCC_BROKER");
    auto inv = read_user_input([&] { return Invocation(args); });
    auto exe = executable(compiler);
    struct stat source;
    if (stat(inv.source.c_str(), &source)) {
        if (unavailable_input(errno)) throw Local("source unavailable");
        check(false, inv.source);
    }
    dcc_work_hint hint{};
    hint.kind = DCC_WORK_RUST;
    hint.key = hash(inv.source.string());
    for (auto &s : args)
        hint.key = hash(s, hint.key);
    if (!hint.key)
        hint.key = 1;
    hint.scope_key = hash(inv.scope.empty() ? inv.outdir : inv.scope);
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
    bool small = small_crate(inv.source, hint.bytes);
    States tool = {{exe, metadata(exe)}};
    string info = memo("compiler:" + profile_context(exe), tool,
                       [&] { return query(exe, "-vV") + '\0' + query(exe, "--print=sysroot"); });
    auto separator = info.find('\0');
    if (separator == string::npos)
        throw std::runtime_error("invalid Rust compiler memo");
    string version = info.substr(0, separator);
    string id = identity(version), cpu, features;
    auto root = info.substr(separator + 1);
    hint.rust_compiler = compiler_key(version);
    auto route = cgu_route(inv, version, root, hint, metadata_only);
    bool mixed = route.cgu_peers > route.native_peers;
    if (!route.native_peers && !route.cgu_peers)
        throw Local("worker has no matching Rust toolchain");
    if (!route.native_peers || (!small && inv.multi_cgu && (inv.linker_plugin_lto ? mixed : route.cgu_peers >= 2)))
        return cgu_frontend(route, inv, exe, original, responses);
    if (small && !inv.incremental && !inv.distributed_link())
        if (auto status = try_local_frontend(inv, original, responses, hint))
            return *status;
    auto refused = [&](const Local &reason) {
        if (!route.cgu_peers)
            throw reason;
        if (env("DISTCC_VERBOSE") == "1")
            fprintf(stderr, "distcc-rust: local: %s\n", reason.what());
        return cgu_frontend(route, inv, exe, original, responses);
    };
    Work work(inv.work_parent());
    job.rust.compiler = id.data();
    string driver = fs::path(inv.linker).filename();
    job.rust.linker = driver.data();
    string crate_types;
    for (auto &type : inv.types) {
        if (!crate_types.empty())
            crate_types += ',';
        crate_types += type;
    }
    job.rust.crate_types = crate_types.data();
    job.rust.pipeline = inv.pipeline;
    auto cmd = inv.command(field(version, "host: "), root);
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
    try {
        inv.discover(exe, root, work);
        if (inv.needs_host) {
            auto required = profile(exe, work, tool, CpuProfile::HostUnion);
            if (required.empty() || required.size() >= sizeof(hint.rust_host_features))
                throw Local("cannot represent required Rust host instruction set");
            strcpy(hint.rust_host_features, required.c_str());
        }
    } catch (const Local &reason) {
        return refused(reason);
    }
    string host = field(version, "host: ");
    if (inv.needs_host && env("DISTCC_VERBOSE") == "1")
        fprintf(stderr, "distcc-rust: proc-macro host ABI %s\n", host.c_str());
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
    Inputs inputs(inv.inputs, inv.observed, start);
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
                    throw std::runtime_error("invalid metadata notification");
                json_object_object_add(obj.get(), "artifact", json_object_new_string(f->destination));
                string notice =
                    string(json_object_to_json_string_ext(obj.get(), JSON_C_TO_STRING_PLAIN)) + '\n';
                mode_t mask = umask(0);
                umask(mask);
                check(!fchmod(f->fd, f->mode & ~mask), "set metadata mode");
                int written = f->fd;
                f->fd = -1;
                check(!close(written), "close metadata before publication");
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
    try {
        verify_job();
    } catch (const Local &e) {
        throw std::runtime_error(e.what());
    }
    if (job.metadata_published && transport)
        return EXIT_DISTCC_FAILED;
    if (transport == EXIT_LOCAL_COMPILE)
        return refused(Local("no worker with the required Rust host ABI"));
    if (transport)
        throw std::runtime_error("Rust transport failed (" + std::to_string(transport) + ")");
    int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 128 + WTERMSIG(result.status);
    if (status == EXIT_LOCAL_COMPILE)
        return job.metadata_published ? EXIT_DISTCC_FAILED
                                      : refused(Local("worker has no matching Rust toolchain"));
    try {
        if (!status && !inv.linker.empty())
            status = finish_links(result, inv, linkdir, exe, inv.sysroot.empty() ? root : inv.sysroot,
                                  field(version, "host: "));
        if (!status)
            publish(result, inv);
    } catch (const std::exception &e) {
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
extern "C" int dcc_rust_submit_cgu(const char *version, const char *target, const char *phase) {
    try {
        struct stat st = descriptor_state(STDIN_FILENO);
        if (!S_ISREG(st.st_mode) || !st.st_size)
            return EXIT_BAD_ARGUMENTS;
        dcc_job_input in{};
        in.path = const_cast<char *>("/cgu");
        in.fd = STDIN_FILENO;
        in.wire_size = in.plain_size = st.st_size;
        if (dcc_file_digest(in.fd, st.st_size, in.digest))
            return EXIT_IO_ERROR;
        dcc_job job{};
        job.kind = DCC_JOB_RUST_CGU;
        job.cwd = const_cast<char *>("/");
        char *args[] = {const_cast<char *>("distcc-rust-cgu"), const_cast<char *>(version), nullptr};
        job.argv = args;
        job.inputs = &in;
        job.count = 1;
        dcc_work_hint hint{};
        hint.kind = DCC_WORK_RUST_CGU;
        hint.bytes = st.st_size;
        hint.work_units = std::min<uint64_t>(st.st_size, UINT32_MAX);
        hint.key = hash(string(reinterpret_cast<char *>(in.digest), sizeof(in.digest))) | 1;
        hint.memory_class = dcc_resource_class(&job);
        string required = string(version) + " " + target;
        if (required.size() >= sizeof(hint.cgu_backend))
            return EXIT_BAD_ARGUMENTS;
        strcpy(hint.cgu_backend, required.c_str());
        Result result;
        int rc = dcc_transfer_prepared(&job, nullptr, nullptr, &hint, -1, &result);
        if (rc)
            return rc;
        if (result.fd[DCC_JOB_STDERR] >= 0)
            put(STDERR_FILENO, contents(result.fd[DCC_JOB_STDERR]));
        if (result.status) {
            if (WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_LOCAL_COMPILE)
                return EXIT_LOCAL_COMPILE;
            return EXIT_DISTCC_FAILED;
        }
        if (env("DISTCC_VERBOSE") == "1")
            fprintf(stderr, "distcc-rust-cgu: finished %s\n", phase);
        off_t offset = 0;
        while (uint64_t(offset) < result.size[DCC_JOB_OBJECT]) {
            ssize_t n = sendfile(STDOUT_FILENO, result.fd[DCC_JOB_OBJECT], &offset, 1U << 30);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return EXIT_IO_ERROR;
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "distcc-rust-cgu: %s\n", e.what());
        return EXIT_DISTCC_FAILED;
    }
}
extern "C" int dcc_rust_client(char **argv) {
    Words args;
    for (char **p = argv + 1; *p; ++p)
        args.emplace_back(*p);
    try {
        ResponseFiles response;
        read_user_input([&] { response.expand(args); });
        return distributed(argv, response.flat, response.files);
    } catch (const Local &e) {
        if (env("DISTCC_VERBOSE") == "1")
            fprintf(stderr, "distcc-rust: local: %s\n", e.what());
    } catch (const std::exception &e) {
        fprintf(stderr, "distcc-rust: %s\n", e.what());
        return EXIT_DISTCC_FAILED;
    }
    execvp(*argv, argv);
    return EXIT_COMPILER_MISSING;
}
