/* Only the Rust linker owns names in its writable mounts. Pin each directory
 * through publication so renaming an entry cannot redirect a client write. */
#include "lto_distribute.hh"
#include <charconv>
#include <poll.h>
#include <set>
#include <sys/socket.h>

namespace dtlto {
Fd regular(const fs::path &path, int parent) {
    Fd fd(openat(parent, path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat st;
    check(fstat(fd.fd, &st) == 0, "stat link input");
    require(S_ISREG(st.st_mode), "expected regular ThinLTO file");
    return fd;
}

int LinkFiles::directory(const fs::path &root, const fs::path &path) {
    auto cached = directories.find(path);
    if (cached != directories.end()) return cached->second.fd;
    int parent = root == path ? AT_FDCWD : directory(root, path.parent_path());
    Fd dir(openat(parent, root == path ? path.c_str() : path.filename().c_str(),
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    return directories.emplace(path, std::move(dir)).first->second.fd;
}

int LinkFiles::parent(const fs::path &path, bool writable) {
    require(path.is_absolute() && !path.filename().empty() &&
            std::find(path.begin(), path.end(), "..") == path.end(), "invalid ThinLTO path");
    for (const fs::path &root : {linked.parent_path(), temporary, work}) {
        if (writable && root == work) continue;
        auto relative = path.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != ".." && relative != ".")
            return directory(root, path.parent_path());
    }
    throw std::runtime_error("ThinLTO path outside link directories");
}

LinkFiles::LinkFiles(const char *w, const char *l, const char *t)
    : work(w), linked(l), temporary(t) {
    for (const fs::path &root : {linked.parent_path(), temporary, work}) directory(root, root);
    auto list = regular("dtlto-inputs", directory(work, work));
    for (const auto &name : rust::split(rust::contents(list.fd), '\0'))
        if (!name.empty()) declared.emplace(name, fs::canonical(name));
}

Fd LinkFiles::input(const string &name) {
    auto found = declared.find(name);
    if (found != declared.end()) return regular(found->second);
    return regular(fs::path(name).filename(), parent(name, false));
}

Request LinkFiles::request(const string &description, const string &compiler) {
    auto file = regular(fs::path(description).filename(), parent(description, true));
    string data;
    char buffer[16384];
    for (;;) {
        ssize_t n = read(file.fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        check(n >= 0, "read ThinLTO description");
        if (!n) break;
        require(data.size() + n <= 16 * 1024 * 1024, "ThinLTO description too large");
        data.append(buffer, n);
    }
    Request result = parse_request(data);
    require(result.linker_output == linked.string() && result.outputs.empty(), "unexpected ThinLTO link output");
    require(result.args.size() >= 2 && result.args[0] == compiler && result.args[1] == "-c" &&
            std::none_of(result.args.begin(), result.args.end(), [](const string &s) { return !s.empty() && s.front() == '@'; }),
            "unexpected ThinLTO compiler arguments");
    std::set<string> outputs;
    for (auto &job : result.jobs) {
        require(job.inputs.size() >= 2 && job.outputs.size() == 1, "unexpected ThinLTO backend job");
        const string &out = job.outputs.front();
        fs::path path(out);
        require(path.lexically_normal().string() == out && path != linked &&
                outputs.insert(out).second &&
                job.args == Words{job.inputs[0], "-fthinlto-index=" + job.inputs[1], "-o", out},
                "unexpected ThinLTO backend job");
        int dir = parent(path, true);
        job.destinations = {(fs::path("/proc/self/fd") / std::to_string(dir) / path.filename()).string()};
    }
    return result;
}

static int descriptor(const char *text) {
    int fd = -1;
    require(text && *text, "missing linker descriptor");
    const char *end = text + strlen(text);
    auto parsed = std::from_chars(text, end, fd);
    require(parsed.ec == std::errc() && parsed.ptr == end && fd >= 0, "invalid linker descriptor");
    return fd;
}

int rust_shim(const char *description) {
    int fd = descriptor(getenv("DISTCC_RUST_DTLTO_FD"));
    size_t size = strlen(description);
    require(size && size <= 4096, "invalid ThinLTO description name");
    // The linker sandbox permits read/write on its inherited socket.
    check(write(fd, description, size) == (ssize_t)size, "send link request");
    unsigned char status;
    check(read(fd, &status, 1) == 1, "receive link status");
    return status;
}

int rust_link(char **args) {
    LinkFiles files(args[0], args[1], args[2]);
    int pair[2];
    check(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair), "link socket");
    Fd parent(pair[0]);
    pid_t child;
    {
        Fd socket(pair[1]);
        const std::vector<int> inherited{descriptor(args[4]), socket.fd};
        Words command;
        for (char **p = args + 5; *p; ++p) command.emplace_back(*p);
        child = rust::spawn(command, {{"DISTCC_RUST_DTLTO_FD", std::to_string(socket.fd)}}, 1, 2, &inherited);
    }
    int failure = 0;
    try {
        char name[4096];
        for (;;) {
            check_cancelled();
            struct pollfd p = {parent.fd, POLLIN, 0};
            int ready = dcc_poll_cancelable(&p, -1);
            if (ready < 0 && errno == EINTR) continue;
            check_cancelled();
            check(ready > 0, "wait for linker request");
            ssize_t n = recv(parent.fd, name, sizeof(name), MSG_TRUNC);
            if (n < 0 && errno == EINTR) continue;
            check(n >= 0, "receive link request");
            if (!n) break;
            require(size_t(n) <= sizeof(name) && !memchr(name, 0, n), "invalid ThinLTO description name");
            unsigned char status = 0;
            try { distribute(files.request(string(name, n), args[3]), DCC_JOB_THINLTO, &files); }
            catch (const Failure &e) { status = e.status; }
            if (!failure) failure = status;
            check(send(parent.fd, &status, 1, MSG_NOSIGNAL) == 1, "send link status");
        }
    } catch (...) {
        kill(child, SIGKILL);
        rust::collect(child);
        throw;
    }
    int status = rust::collect(child);
    return failure ? failure : status;
}
} // namespace dtlto
