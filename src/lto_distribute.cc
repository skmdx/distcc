/* LLVM's DTLTO and offload distributor interface. The coordinator owns the
 * ready queue and snapshots. Threads borrow those snapshots through completion. */
#include "lto_distribute.hh"
#include <array>
#include <climits>
#include <atomic>
#include <optional>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <poll.h>
#include <set>
#include <sys/eventfd.h>
extern "C" {
#include <sys/pidfd.h>
#include "exitcode.h"
#include "file_cache.h"
#include "hosts.h"
#include "resource.h"
#include "workload.h"
}

namespace dtlto {
static std::atomic<int> interrupted{0};
static_assert(decltype(interrupted)::is_always_lock_free);
static void wake_lanes() {
    uint64_t one = 1;
    ssize_t n;
    do { n = write(dcc_client_cancel_fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
}
static void interrupt(int sig) {
    int saved = errno;
    interrupted.store(sig, std::memory_order_relaxed);
    wake_lanes();
    errno = saved;
}
void check_cancelled() {
    if (int sig = interrupted.load(std::memory_order_relaxed)) throw Failure("cancelled", 128 + sig);
}
static bool cancelling() {
    struct pollfd p{dcc_client_cancel_fd, POLLIN, 0};
    return poll(&p, 1, 0) > 0;
}

using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
static Json own(json_object *value) { return Json(value, json_object_put); }

static Json parse(const string &s) {
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tok(json_tokener_new(), json_tokener_free);
    if (!tok) throw std::bad_alloc();
    json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT);
    require(s.size() < INT_MAX, "oversize JSON");
    auto obj = own(json_tokener_parse_ex(tok.get(), s.c_str(), int(s.size() + 1)));
    require(obj && json_tokener_get_error(tok.get()) == json_tokener_success &&
            s.find('\0') == string::npos, "invalid JSON");
    return obj;
}
static json_object *field(json_object *obj, const char *key) {
    json_object *v = nullptr;
    require(json_object_object_get_ex(obj, key, &v), key);
    return v;
}
static string word(json_object *obj) {
    require(json_object_is_type(obj, json_type_string), "expected string in compiler arguments or paths");
    string s(json_object_get_string(obj), json_object_get_string_len(obj));
    require(s.find('\0') == string::npos, "NUL in compiler arguments or paths");
    return s;
}
static size_t count(json_object *array) {
    require(json_object_is_type(array, json_type_array), "expected array");
    return json_object_array_length(array);
}
static Words words(json_object *obj) {
    Words out;
    size_t n = count(obj);
    for (size_t i = 0; i < n; ++i) out.push_back(word(json_object_array_get_idx(obj, i)));
    return out;
}
static json_object *strings(const Words &items) {
    auto array = own(json_object_new_array());
    for (const auto &s : items) json_object_array_add(array.get(), json_object_new_string(s.c_str()));
    return array.release();
}
static uint64_t number(json_object *obj, const char *key) {
    auto v = field(obj, key);
    require(json_object_is_type(v, json_type_int) && json_object_get_int64(v) >= 0, key);
    return json_object_get_uint64(v);
}

Request parse_request(const string &data) {
    auto doc = parse(data);
    Request request;
    auto common = field(doc.get(), "common");
    request.args = words(field(common, "args"));
    request.inputs = words(field(common, "inputs"));
    json_object *v;
    if (json_object_object_get_ex(common, "linker_output", &v)) request.linker_output = word(v);
    if (json_object_object_get_ex(common, "outputs", &v)) request.outputs = words(v);
    auto list = field(doc.get(), "jobs");
    for (size_t i = 0, n = count(list); i < n; ++i) {
        auto obj = json_object_array_get_idx(list, i);
        Backend job{words(field(obj, "args")), words(field(obj, "inputs")), words(field(obj, "outputs")), {}};
        const auto &driver = request.args.empty() ? job.args : request.args;
        require(!driver.empty() && !driver.front().empty(), "missing compiler");
        require((!request.inputs.empty() || !job.inputs.empty()) && !job.outputs.empty(), "missing backend input or output");
        request.jobs.push_back(std::move(job));
    }
    return request;
}

static string absolute_name(const string &p) {
    return fs::absolute(p).lexically_normal().string();
}
static struct stat state(int fd) {
    struct stat st;
    check(!fstat(fd, &st), "stat input");
    require(S_ISREG(st.st_mode), "expected regular file");
    return st;
}
static auto stamp(const struct stat &s) {
    return std::make_tuple(s.st_dev, s.st_ino, s.st_size, s.st_mtim.tv_sec, s.st_mtim.tv_nsec,
                           s.st_ctim.tv_sec, s.st_ctim.tv_nsec);
}
struct Snapshot {
    string path;
    uint64_t size;
    std::once_flag ready;
    std::array<unsigned char, DCC_FILE_HASH_SIZE> hash;
    Snapshot(string p, uint64_t bytes) : path(std::move(p)), size(bytes) {}
    Snapshot(string p, int input, const struct stat &before) : Snapshot(std::move(p), before.st_size) {
        Fd output(open(path.c_str(), O_WRONLY | O_EXCL | O_CREAT | O_CLOEXEC, 0400));
        check(!dcc_copy_snapshot(input, output.fd, size), "snapshot " + path);
        require(stamp(before) == stamp(state(input)), "input changed during snapshot");
    }
    const auto &digest() {
        std::call_once(ready, [&] {
            auto fd = regular(path);
            check(!dcc_file_digest(fd.fd, size, hash.data()), "fingerprint " + path);
        });
        return hash;
    }
};

struct Group {
    fs::path dir;
    Words parts, locals;
    string destination;
    size_t remaining;
};
struct Task : Backend {
    struct Input { string name; Snapshot &data; };
    std::vector<Input> files;
    std::optional<size_t> partition;
    uint64_t bytes = 0, instructions = 0, partitions = 1;
    string index;
    const Snapshot *index_file = nullptr;
    std::shared_ptr<Group> group;
};

// LLD supplies these four per-module arguments after the common driver
// options. Any other shape keeps its original driver invocation.
struct BackendTemplate {
    static constexpr const char *marker = "__distcc_lto_arg_";
    string input, index = string(marker) + "index.bc", output = string(marker) + "out.o";
    Words cc1;
    size_t source_arg, index_arg, output_arg;
    std::optional<size_t> basename_arg;

    bool matches(const Task &t) const {
        if (t.inputs.empty() || t.outputs.size() != 1 || t.args.size() != (t.partition ? 6 : 4)) return false;
        const auto &source = t.inputs.front();
        return !source.empty() && source[0] != '-' && source[0] != '@' && t.args[0] == source &&
               t.args[1] == "-fthinlto-index=" + t.index && t.args[2] == "-o" &&
               !t.args[3].empty() && t.args[3] != "-" && absolute_name(t.args[3]) == t.outputs[0] &&
               (t.partition || fs::path(source).extension() == fs::path(input).extension());
    }

    Words driver(const Words &common, const Task &t) {
        if (common.size() > 1 && common[1] == "-cc1") return {};
        if (std::any_of(common.begin(), common.end(), [](const string &s) {
            return s.find(marker) != string::npos;
        })) return {};
        input = string(marker) + "dir/" + marker + "in" + fs::path(t.inputs.front()).extension().string();
        if (!matches(t)) return {};
        Words args = common;
        args.insert(args.end(), {input, "-fthinlto-index=" + index, "-o", output});
        return args;
    }

    void resolve(Words args, const string &compiler) {
        if (args.empty() || args.front() != "-cc1") return;
        args.insert(args.begin(), compiler);
        const string basename = fs::path(input).filename();
        const string index_flag = "-fthinlto-index=" + index;
        unsigned sources = 0, indexes = 0, outputs = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == input && i >= 2 && args[i-2] == "-x" && args[i-1] == "ir") {
                source_arg = i; ++sources;
            }
            else if (args[i] == index_flag) { index_arg = i; ++indexes; }
            else if (args[i] == output && args[i-1] == "-o") { output_arg = i; ++outputs; }
            else if (args[i] == basename && args[i-1] == "-main-file-name" && !basename_arg) basename_arg = i;
            else if (args[i].find(marker) != string::npos) return; // e.g. a derived side output
        }
        if (sources == 1 && indexes == 1 && outputs == 1) cc1 = std::move(args);
    }

    Words bind(const Task &t) const {
        if (cc1.empty() || !matches(t)) return {};
        Words args = cc1;
        args[source_arg] = t.args[0];
        args[index_arg] = t.args[1];
        args[output_arg] = t.args[3];
        if (basename_arg) args[*basename_arg] = fs::path(t.args[0]).filename();
        if (t.partition) args.insert(args.end(), t.args.end()-2, t.args.end());
        return args;
    }
};

// The ready queue is touched only by the coordinator. A worker returns a
// continuation which can enqueue partitions or a newly unblocked merge.
using Completion = std::function<void()>;
using Operation = std::function<Completion(int &)>;
class Scheduler {
    struct Entry {
        double weight;
        size_t serial;
        bool backend;
        Operation execute;
        bool operator<(const Entry &b) const {
            return weight == b.weight ? serial > b.serial : weight < b.weight;
        }
    };
    size_t slots, serial = 0, active = 0;
    std::priority_queue<Entry> ready;
    std::mutex lock;
    std::condition_variable changed;
    struct Pending { Operation op; std::optional<Fd> writer; };
    std::queue<Pending> pending;
    std::queue<Completion> done;
    std::vector<std::thread> threads;
    std::exception_ptr error;
    bool stopping = false;
    void worker() {
        std::unique_lock<std::mutex> guard(lock);
        for (;;) {
            changed.wait(guard, [&]{ return stopping || !pending.empty(); });
            if (stopping) return;
            auto task = std::move(pending.front());
            pending.pop();
            guard.unlock();
            try {
                check_cancelled();
                int unused = -1;
                auto complete = task.op(task.writer ? task.writer->fd : unused);
                if (complete) {
                    std::lock_guard<std::mutex> hold(lock);
                    done.push(std::move(complete));
                }
            } catch (...) {
                std::lock_guard<std::mutex> hold(lock);
                if (!error) { error = std::current_exception(); wake_lanes(); }
            }
            guard.lock();
            --active;
            changed.notify_all();
        }
    }
public:
    explicit Scheduler(size_t n) : slots(n) {}
    ~Scheduler() {
        { std::lock_guard<std::mutex> guard(lock); stopping = true; }
        changed.notify_all();
        for (auto &t : threads) t.join();
        // A sandboxed linker can submit another description after a failed batch.
        if (error && !interrupted.load(std::memory_order_relaxed)) {
            uint64_t count;
            if (read(dcc_client_cancel_fd, &count, sizeof(count)) < 0 && errno != EAGAIN)
                perror("distributor cancellation");
        }
    }
    void enqueue(double weight, bool backend, Operation op) {
        ready.push({weight, serial++, backend, std::move(op)});
    }
    void run() {
        std::unique_lock<std::mutex> guard(lock);
        while (!ready.empty() || active || !done.empty()) {
            check_cancelled();
            if (error) std::rethrow_exception(error);
            while (!done.empty()) {
                auto complete = std::move(done.front());
                done.pop();
                complete();
            }
            if (ready.empty() && !active) break;
            if (ready.empty() || active == slots) { changed.wait(guard); continue; }
            auto entry = ready.top();
            ready.pop();
            std::optional<Fd> reader, writer;
            if (entry.backend) {
                int pipe[2];
                check(!pipe2(pipe, O_CLOEXEC), "submission pipe");
                reader.emplace(pipe[0]);
                writer.emplace(pipe[1]);
            }
            if (threads.size() == active) threads.emplace_back([this]{ worker(); });
            pending.push({std::move(entry.execute), std::move(writer)});
            ++active;
            changed.notify_all();
            if (entry.backend) {
                guard.unlock();
                char queued;
                ssize_t n;
                do { n = read(reader->fd, &queued, 1); } while (n < 0 && errno == EINTR);
                check(n >= 0, "submission notification");
                require(!n || queued == 'Q', "invalid submission notification");
                guard.lock();
            }
        }
        if (error) std::rethrow_exception(error);
    }
};

static void local_tool(const string &compiler, char operation, const fs::path &description,
                       json_object *spec, int output = -1) {
    Fd file(open(description.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    rust::put(file.fd, json_object_to_json_string_ext(spec, JSON_C_TO_STRING_PLAIN));
    Words args{compiler, description};
    int status = dcc_clang_local_tool(rust::pointers(args).data(), operation, output, nullptr);
    if (status) throw Failure("local LLVM tool failed", status);
}

static int retry_backend(const Words &args) {
    Fd out(dcc_job_memfd("distcc-backend-stdout")), err(dcc_job_memfd("distcc-backend-stderr"));
    pid_t child = rust::spawn(args, {}, out.fd, err.fd, nullptr, true);
    try {
        // Establish the group even if cancellation beats the child's first instruction.
        check(setpgid(child, child) == 0 || errno == EACCES, "group local backend");
        Fd watcher(pidfd_open(child, 0));
        struct pollfd p{watcher.fd, POLLIN, 0};
        int ready;
        do { ready = dcc_poll_cancelable(&p, -1); } while (ready < 0 && errno == EINTR);
        check(ready > 0, "wait for local backend");
    } catch (...) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        rust::collect(child);
        check_cancelled();
        throw;
    }
    int ret = rust::collect(child);
    auto diagnostics = rust::contents(err.fd), output = rust::contents(out.fd);
    flockfile(stderr);
    if (dcc_writex(STDERR_FILENO, diagnostics.data(), diagnostics.size()) ||
        dcc_writex(STDOUT_FILENO, output.data(), output.size())) ret = EXIT_IO_ERROR;
    funlockfile(stderr);
    return ret;
}

static void publish(const fs::path &source, const fs::path &destination) {
    auto input = regular(source);
    check(!fchmod(input.fd, 0600), "set merged object mode");
    if (!rename(source.c_str(), destination.c_str())) return;
    check(errno == EXDEV, "publish merged object");
    string tmp = (destination.parent_path() / ".distcc-XXXXXX").string();
    Fd output(mkostemp(tmp.data(), O_CLOEXEC));
    try {
        check(!dcc_copy_snapshot(input.fd, output.fd, state(input.fd).st_size), "stage merged object");
        check(!rename(tmp.c_str(), destination.c_str()), "publish merged object");
    } catch (...) { unlink(tmp.c_str()); throw; }
}

struct Distribution {
    const Request &request;
    dcc_job_kind kind;
    LinkFiles *link;
    const string cwd = fs::current_path();
    const char *helper;
    const bool verbose = rust::env("DISTCC_VERBOSE") == "1";
    rust::Work work{fs::temp_directory_path(), "distcc-lto-XXXXXX"};
    std::map<decltype(stamp({})), Snapshot> snapshots;
    std::map<string, Snapshot *> by_name;
    BackendTemplate backend_template;
    uint64_t total_bytes = 0, total_instructions = 0;
    size_t sequence = 0;
    Scheduler pool;

    Distribution(const Request &r, dcc_job_kind k, LinkFiles *l, size_t slots)
        : request(r), kind(k), link(l),
          helper(k == DCC_JOB_THINLTO ? "distcc-thinlto" : "distcc-offload-link"), pool(slots) {}

    void register_inputs(Task &t) {
        Words names = t.inputs;
        names.insert(names.end(), request.inputs.begin(), request.inputs.end());
        std::set<string> seen;
        for (auto &name : names) {
            name = absolute_name(name);
            if (!seen.insert(name).second) continue;
            auto [it, inserted] = by_name.try_emplace(name, nullptr);
            if (inserted) {
                bool generated = t.partition && t.files.empty();
                auto file = generated ? regular(name) : link ? link->input(name) :
                    Fd(open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
                auto before = state(file.fd);
                auto generation = stamp(before);
                auto frozen = generated ? snapshots.try_emplace(generation, name, before.st_size) :
                    snapshots.try_emplace(generation, work.dir / ("input-" + std::to_string(snapshots.size())),
                                          file.fd, before);
                it->second = &frozen.first->second;
            }
            t.files.push_back({name, *it->second});
            if (kind != DCC_JOB_THINLTO || t.files.size() == 1) t.bytes += it->second->size;
        }
        for (auto &out : t.outputs) out = absolute_name(out);
        if (t.destinations.empty()) t.destinations = t.outputs;
    }

    Words command(const Task &t) const {
        auto bound = backend_template.bind(t);
        if (!bound.empty()) return bound;
        Words args = request.args;
        args.insert(args.end(), t.args.begin(), t.args.end());
        return args;
    }

    void backend(const Task &t, int &queued) {
        auto args = command(t);
        if (verbose) fprintf(stderr, "%s: submitting %s instructions=%llu bytes=%llu command=%s\n", helper,
                              t.files.front().name.c_str(), (unsigned long long)t.instructions,
                              (unsigned long long)t.bytes, args.size() > 1 && args[1] == "-cc1" ? "cc1" : "driver");
        Words files, destinations;
        std::vector<dcc_job_input> inputs;
        dcc_work_hint hint{};
        hint.kind = kind == DCC_JOB_THINLTO ? DCC_WORK_THINLTO : DCC_WORK_OFFLOAD_LINK;
        uint64_t key = XXH3_64bits(cwd.data(), cwd.size());
        auto hash = [&](const string &s) { key = XXH3_64bits_withSeed(s.c_str(), s.size()+1, key); };
        std::map<string, string> paths;
        for (auto &[name, data] : t.files) {
            paths[name] = "input:" + std::to_string(files.size());
            files.push_back(data.path);
            dcc_job_input in{};
            in.fd = -1;
            in.path = const_cast<char *>(name.c_str());
            in.kind = DCC_JOB_FILE;
            in.plain_size = data.size;
            if (t.partition && inputs.empty()) {
                // The shared parent index below identifies the module. Its
                // partition ordinal is independent of the private work path.
                hash(std::to_string(*t.partition));
                in.cached = DCC_UNCACHED;
            } else {
                in.cached = DCC_CACHE_LOOKUP;
                auto &digest = data.digest();
                memcpy(in.digest, digest.data(), digest.size());
                key = XXH3_64bits_withSeed(in.digest, sizeof(in.digest), key);
            }
            key = XXH3_64bits_withSeed(&in.plain_size, sizeof(in.plain_size), key);
            inputs.push_back(in);
        }
        for (size_t i = 0; i < t.outputs.size(); ++i) {
            paths[t.outputs[i]] = "output:" + std::to_string(i);
            destinations.push_back(std::to_string(i));
            destinations.push_back(t.destinations[i]);
        }
        // Use the driver vocabulary for scheduling, including when cc1 contains
        // a derived -main-file-name for a private partition path.
        auto hash_arg = [&](const string &s) {
            auto eq = s.find('=');
            auto found = paths.find(absolute_name(s.empty() ? "." : s));
            if (found != paths.end()) hash(found->second);
            else if (eq != string::npos && eq+1 < s.size() && (found = paths.find(absolute_name(s.substr(eq+1)))) != paths.end())
                hash(s.substr(0, eq+1) + found->second);
            else hash(s);
        };
        for (const auto &s : request.args) hash_arg(s);
        for (const auto &s : t.args) hash_arg(s);
        auto argv = rust::pointers(args), outputs = rust::pointers(t.outputs);
        auto targets = rust::pointers(destinations), snapshots = rust::pointers(files);
        dcc_job job{};
        job.kind = kind;
        job.cwd = const_cast<char *>(cwd.c_str());
        job.count = inputs.size();
        job.inputs = inputs.data();
        job.argv = argv.data();
        job.llvm_outputs = outputs.data();
        job.destinations = targets.data();
        hint.bytes = dcc_resource_input_bytes(&job);
        hint.key = key ? key : 1;
        hint.memory_class = dcc_resource_class(&job);
        hint.work_units = std::min<uint64_t>(UINT32_MAX, kind == DCC_JOB_THINLTO ? t.instructions : hint.bytes);
        dcc_job_result result;
        dcc_job_result_init(&result);
        int ret = dcc_transfer_prepared(&job, snapshots.data(), nullptr, &hint, queued, &result);
        queued = -1;
        if (!ret && !cancelling()) ret = dcc_finish_file_job(&result, t.outputs.size());
        dcc_job_result_free(&result);
        check_cancelled();
        if (ret && !link && !cancelling() && rust::env("DISTCC_FALLBACK") != "0" && rust::env("DISTCC_SKIP_LOCAL_RETRY") != "1") {
            fprintf(stderr, "%s: retrying backend locally\n", helper);
            ret = retry_backend(args);
        }
        if (ret) throw Failure("backend failed", ret);
    }

    std::vector<Task> prepare(const fs::path &dir, const Task &t) {
        fs::create_directory(dir);
        auto spec = own(json_object_new_object());
        auto paths = json_object_new_object();
        json_object_object_add(spec.get(), "files", paths);
        for (auto &[name, data] : t.files) json_object_object_add(paths, name.c_str(), json_object_new_string(data.path.c_str()));
        json_object_object_add(spec.get(), "cwd", json_object_new_string(cwd.c_str()));
        // The compiler has no /proc in its chroot. Expose only the frozen,
        // read-only index there. The sandbox drops capabilities and denies chmod.
        fs::create_hard_link(t.index_file->path, dir/"index.thinlto.bc");
        json_object_object_add(spec.get(), "index", json_object_new_string(absolute_name(t.index).c_str()));
        json_object_object_add(spec.get(), "partitions", json_object_new_uint64(t.partitions));
        json_object_object_add(spec.get(), "args", strings(command(t)));
        local_tool(request.args[0], 'A', dir / "request.json", spec.get());
        auto manifest = parse(rust::contents(regular(dir/"partitions.json").fd));
        auto parts = field(manifest.get(), "parts");
        size_t n = count(parts);
        require(n && n <= t.partitions, "unexpected LLVM partition count");
        auto locals = words(field(manifest.get(), "locals"));
        require(n > 1 || locals.empty(), "unsplit module has promoted symbols");
        if (verbose) fprintf(stderr, "%s: partitioned %s into %zu\n", helper, t.inputs[0].c_str(), n);
        std::shared_ptr<Group> group;
        if (n > 1) {
            group = std::make_shared<Group>(Group{dir/"merge", {}, std::move(locals), t.destinations[0], n});
            fs::create_directory(group->dir);
        }
        std::vector<Task> children;
        for (size_t i = 0; i < n; ++i) {
            auto part = json_object_array_get_idx(parts, i);
            string name = "partitions.json.part-" + std::to_string(i) + ".bc";
            require(word(field(part, "path")) == name, "unexpected LLVM partition path");
            string input = (dir/name).string(), output = t.outputs[0];
            Task child;
            child.partition = i;
            child.index = t.index;
            if (group) {
                group->parts.push_back("part-" + std::to_string(i) + ".o");
                output = (group->dir/group->parts.back()).string();
            }
            child.args = t.args;
            for (auto &arg : child.args)
                if (!arg.empty() && absolute_name(arg) == t.files[0].name) arg = input;
            auto out = std::find(child.args.begin(), child.args.end(), "-o");
            require(out != child.args.end() && ++out != child.args.end(), "missing partition output");
            *out = output;
            child.args.insert(child.args.end(), {"-mllvm", "-thinlto-assume-merged"});
            child.inputs = {input, t.index};
            child.outputs = {output};
            child.destinations = group ? child.outputs : t.destinations;
            child.instructions = number(part, "instructions");
            child.group = group;
            children.push_back(std::move(child));
        }
        return children;
    }

    void enqueue(Task t) {
        double priority = std::max(total_bytes ? double(t.bytes)/total_bytes : 0.,
                                   total_instructions ? double(t.instructions)/total_instructions : 0.);
        bool split = t.partitions > 1;
        auto dir = work.dir / std::to_string(sequence++);
        pool.enqueue(priority, !split, [this, split, dir, t = std::move(t)](int &fd) -> Completion {
            if (split) {
                return [this, children = prepare(dir, t)]() mutable {
                    for (auto &child : children) { register_inputs(child); enqueue(std::move(child)); }
                };
            }
            backend(t, fd);
            return [this, group = t.group] {
                if (!group || --group->remaining) return;
                pool.enqueue(INFINITY, false, [this, group](int &) -> Completion {
                    auto spec = own(json_object_new_object());
                    json_object_object_add(spec.get(), "parts", strings(group->parts));
                    json_object_object_add(spec.get(), "locals", strings(group->locals));
                    local_tool(request.args[0], 'M', group->dir / "request.json", spec.get());
                    publish(group->dir/"merged.o", group->destination);
                    return {};
                });
            };
        });
    }

    void run(size_t slots, double parallelism) {
        std::vector<Task> tasks;
        std::vector<size_t> planned;
        std::optional<size_t> template_row;
        auto plan = own(json_object_new_array());
        for (const auto &job : request.jobs) {
            Task &t = tasks.emplace_back();
            static_cast<Backend &>(t) = job;
            register_inputs(t);
            require(t.files.size() <= DCC_JOB_INPUT_MAX, "too many backend inputs");
            total_bytes += t.bytes;
            if (kind != DCC_JOB_THINLTO || request.args.empty() || t.inputs.empty()) continue;
            for (const auto &arg : t.args) if (arg.rfind("-fthinlto-index=", 0) == 0) {
                t.index = arg.substr(strlen("-fthinlto-index="));
                auto row = json_object_new_object();
                json_object_array_add(plan.get(), row);
                planned.push_back(tasks.size()-1);
                json_object_object_add(row, "source", json_object_new_string(t.inputs[0].c_str()));
                t.index_file = by_name.at(absolute_name(t.index));
                json_object_object_add(row, "index", json_object_new_string(t.index_file->path.c_str()));
                if (!template_row) {
                    auto driver = backend_template.driver(request.args, t);
                    if (!driver.empty()) {
                        template_row = planned.size()-1;
                        json_object_object_add(row, "driver", strings(driver));
                    }
                }
                break;
            }
        }
        if (!planned.empty()) {
            Fd fd(dcc_job_memfd("distcc-index-weights"));
            bool supported = true;
            try { local_tool(request.args[0], 'N', work.dir/"plan.json", plan.get(), fd.fd); }
            catch (const Failure &e) { if (e.status != EXIT_LOCAL_COMPILE) throw; supported = false; }
            if (supported) {
                auto weights = parse(rust::contents(fd.fd));
                require(count(weights.get()) == planned.size(), "incomplete LLVM work estimates");
                if (template_row)
                    backend_template.resolve(words(field(json_object_array_get_idx(weights.get(), *template_row), "cc1")),
                                             request.args[0]);
                for (size_t i = 0; i < planned.size(); ++i) {
                    auto &t = tasks[planned[i]];
                    t.instructions = number(json_object_array_get_idx(weights.get(), i), "instructions");
                    total_instructions += t.instructions;
                }
                for (size_t i = 0; total_instructions && i < planned.size(); ++i) {
                    auto &t = tasks[planned[i]];
                    if (t.outputs.size() != 1) continue;
                    auto row = json_object_array_get_idx(weights.get(), i);
                    auto largest = number(row, "largest_function");
                    t.partitions = std::max<uint64_t>(1, std::min<uint64_t>({slots, number(row, "functions"),
                        largest ? t.instructions / largest : 1, t.instructions / 64,
                        uint64_t(std::ceil(t.instructions * parallelism / total_instructions))}));
                }
            }
        }
        for (auto &t : tasks) enqueue(std::move(t));
        pool.run();
    }
};

void distribute(Request request, dcc_job_kind kind, LinkFiles *link) {
    if (request.jobs.empty()) return;
    char *json = nullptr;
    int ret = dcc_lto_capacity(&json);
    if (ret) throw Failure("unable to obtain broker capacity", ret);
    std::unique_ptr<char, decltype(&free)> storage(json, free);
    auto cap = parse(json);
    auto slots = std::max<uint64_t>(1, number(cap.get(), "jobs"));
    double rate = json_object_get_double(field(cap.get(), "min_rate"));
    double parallelism = rate ? json_object_get_double(field(cap.get(), "work_rate")) / rate : slots;
    // All threads see the same immutable host configuration for this link.
    if (dcc_read_localslots_configuration()) throw Failure("invalid host configuration", EXIT_BAD_HOSTSPEC);
    Distribution(request, kind, link, slots).run(slots, parallelism);
}
} // namespace dtlto

extern "C" int dcc_lto_distribute(int argc, char **argv) {
    using namespace dtlto;
    const string name = fs::path(argv[0]).filename();
    try {
        if (string(argv[0]) == "/__distcc_rust/dtlto-distributor") {
            require(argc == 2, "expected linker description");
            return rust_shim(argv[1]);
        }
        // Finish or discard in-flight results before releasing their snapshots.
        Fd cancellation(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        dcc_client_cancel_fd = cancellation.fd;
        struct sigaction action{};
        action.sa_handler = interrupt;
        sigemptyset(&action.sa_mask);
        for (int sig : {SIGTERM, SIGHUP, SIGINT}) check(!sigaction(sig, &action, nullptr), "set cancellation handler");
        if (name == "distcc-thinlto" && argc >= 8 && string(argv[1]) == "--rust-link")
            return rust_link(argv + 2);
        require(argc == 2, "expected LLVM-JOB.json");
        distribute(parse_request(rust::read(argv[1])), name == "distcc-thinlto" ? DCC_JOB_THINLTO : DCC_JOB_OFFLOAD_LINK);
        return 0;
    } catch (const Failure &e) {
        fprintf(stderr, "%s: %s (status %d)\n", name.c_str(), e.what(), e.status);
        return e.status;
    } catch (const std::exception &e) {
        fprintf(stderr, "%s: %s\n", name.c_str(), e.what());
        return 1;
    }
}
