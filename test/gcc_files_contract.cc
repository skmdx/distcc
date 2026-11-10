/* Exercise directory batching through the real GCC filesystem service.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/mount.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
static pid_t mounted_child;
static bool reader_alive_at_unmount;
static int checked_unmount(const char *path, int flags) {
    if (mounted_child && !kill(mounted_child, 0)) reader_alive_at_unmount = true;
    return syscall(SYS_umount2, path, flags);
}
#define umount2 checked_unmount
#include "src/gcc_files.cc"
#undef umount2
#include <thread>

// These framing tests inspect published hints but do not retain snapshots.
// Native compiler integration tests exercise the shared cache and invalidation.
static std::vector<std::string> published_hints;
extern "C" const dcc_file_cache_api dcc_file_cache_records = {
    [](const char *) -> dcc_record_cache * { return nullptr; },
    [](dcc_record_cache *) {},
    [](dcc_record_cache *, const void *, size_t, size_t *) { return -1; },
    [](const void *, size_t, const void *, size_t, const void **, size_t *) { return 0; },
    [](dcc_record_cache *, const void *key, size_t key_size, const void *data, size_t size) {
        if (key_size >= sizeof("gcc-source-hints") &&
            !memcmp(key, "gcc-source-hints", sizeof("gcc-source-hints")))
            published_hints.emplace_back(static_cast<const char *>(data), size);
    }
};

static void require(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}
static void read_bytes(int fd, void *bytes, size_t size)
{
    auto p = static_cast<char *>(bytes);
    while (size) {
        ssize_t count = read(fd, p, size);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "request ended early");
        p += count; size -= count;
    }
}
static void integer(std::string &bytes, unsigned value)
{
    uint32_t wire = htonl(value);
    bytes.append(reinterpret_cast<const char *>(&wire), sizeof(wire));
}
static void read_query(int fd)
{
    uint32_t header[3];
    read_bytes(fd, header, sizeof(header));
    unsigned size = ntohl(header[1]);
    require(ntohl(header[0]) == DCC_TCP_FS_QUERY && ntohl(header[2]) == DCC_FS_DIRECTORIES &&
            size > 4 && size <= DCC_FS_QUERY_MAX, "batch request envelope");
    std::string payload(size-4, '\0');
    read_bytes(fd, payload.data(), payload.size());
    std::vector<std::string> paths;
    for (size_t offset = 0; offset < payload.size();) {
        require(payload.size()-offset >= 4, "query length field");
        unsigned length = number(payload.data()+offset, 4);
        offset += 4;
        require(length > 4+DCC_FS_METADATA && length <= payload.size()-offset &&
                number(payload.data()+offset, 4) == DCC_FS_LSTAT, "nested query");
        auto end = payload.find('\0', offset+4);
        require(end != std::string::npos && end < offset+length, "query path");
        paths.emplace_back(payload.data()+offset+4, end-offset-4);
        offset += length;
    }
    require(paths == std::vector<std::string>{"/", "/tree", "/tree/include"},
            "native search path ancestors");
}
static char directory[] = "/tree/include", terminator[] = "";
static char *directories[] = {directory, terminator};

static void replies(const std::string &mode)
{
    int pair[2];
    require(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "socketpair");
    std::thread peer([&] {
        read_query(pair[1]);
        if (mode != "disconnect") {
            std::string payload;
            for (unsigned i = 0; i < (mode == "missing" ? 2U : 3U); ++i) {
                std::string stamp(DCC_FS_METADATA, '\0');
                uint32_t permissions = htonl(S_IFDIR | 0755);
                memcpy(stamp.data()+32, &permissions, sizeof(permissions));
                unsigned error = 0, extra = 0;
                if (i == 2) {
                    if (mode == "denied") { error = EACCES; stamp.clear(); }
                    if (mode == "bad-error") { error = UINT_MAX; stamp.clear(); }
                    if (mode == "short") stamp.pop_back();
                    if (mode == "length") extra = 1;
                    if (mode == "flags") { integer(stamp, 8); integer(stamp, 0); }
                }
                integer(payload, error); integer(payload, stamp.size()+extra);
                payload += stamp;
            }
            if (mode == "trailing") payload += 'x';
            std::string response;
            integer(response, 0); integer(response, payload.size()); response += payload;
            for (size_t offset = 0; offset < response.size();) {
                ssize_t count = send(pair[1], response.data()+offset,
                                     std::min(size_t(3), response.size()-offset), MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR) continue;
                require(count > 0, "fragmented reply"); offset += count;
            }
        }
        close(pair[1]);
    });
    Files files(pair[0], "/sandbox", "fixture", "/cache", "/tree", "part.c");
    int result = files.prefetch("/tree", directories);
    peer.join();
    if (mode == "valid" || mode == "denied") {
        require(!result, "valid batch rejected");
        Entry *entry;
        require(files.status("/tree/include", entry) == (mode == "denied" ? EACCES : 0),
                "cached per-path status");
    } else require(result == (mode == "disconnect" ? EIO : EPROTO), "bad reply accepted");
}

static void negative_hint_queries()
{
    Files files(-1, "/sandbox", "fixture", "/cache", "/tree", "part.c");
    Query query;
    require(!files.prepare({"/tree"}, query), "prepare empty directory");
    std::string reply(DCC_FS_METADATA, '\0');
    uint32_t mode = htonl(S_IFDIR | 0755);
    memcpy(reply.data()+32, &mode, sizeof(mode));
    integer(reply, DCC_FS_DIRECTORY_STABLE); integer(reply, 0);
    require(!files.accept(query, {{}, reply.data(), reply.size()}), "empty directory reply");
    Entry *entry;
    for (unsigned i = 0; i < 3; ++i)
        require(files.status("/tree/missing", entry) == ENOENT, "cached negative lookup");
    Query next;
    require(!files.prepare({"/tree"}, next), "prepare subsequent query");
    require(next.bytes.size() == strlen("/tree")+1+DCC_FS_METADATA,
            "negative lookup became a metadata hint");

    // Previously learned hints may outlive a removed file. A failed fresh
    // lookup must drop them instead of being repeated in subsequent jobs.
    require(!files.status("/tree", entry), "parent status");
    entry->hints.insert("missing");
    reply.resize(DCC_FS_METADATA);
    integer(reply, DCC_FS_DIRECTORY_STABLE); integer(reply, 1);
    integer(reply, 7); integer(reply, ENOENT); reply += "missing";
    require(!files.accept(query, {{}, reply.data(), reply.size()}), "disappeared child reply");
    require(files.status("/tree/missing", entry) == ENOENT, "fresh negative lookup");
}

static void negative_hints()
{
    published_hints.clear();
    negative_hint_queries();
    std::string searched("/tree\0\0", 7);
    require(published_hints == std::vector<std::string>{searched, searched},
            "searched directory was forgotten or absent child metadata was retained");
}

static void cached_links()
{
    for (const char *target : {"relative/../header", "/absolute/header"}) {
        int pair[2];
        require(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "link socketpair");
        std::thread peer([&] {
            uint32_t header[3];
            read_bytes(pair[1], header, sizeof(header));
            require(ntohl(header[0]) == DCC_TCP_FS_QUERY &&
                    ntohl(header[2]) == DCC_FS_READLINK, "link query");
            std::string path(ntohl(header[1])-4, '\0');
            read_bytes(pair[1], path.data(), path.size());
            require(path == "/tree/link", "link spelling changed");
            std::string response;
            integer(response, 0); integer(response, strlen(target)); response += target;
            require(send(pair[1], response.data(), response.size(), MSG_NOSIGNAL) ==
                    static_cast<ssize_t>(response.size()), "link reply");
            close(pair[1]); // Any repeated request fails deterministically.
        });
        Files files(pair[0], "/sandbox", "fixture", "/cache", "/tree", "part.c");
        char output[128];
        require(files.link("/tree/link", output, 1) == -ENAMETOOLONG, "short link buffer");
        for (int i = 0; i < 3; ++i) {
            require(!files.link("/tree/link", output, sizeof(output)), "link snapshot was not reused");
            require(std::string(output) == (target[0] == '/' ? "/sandbox" : "") + std::string(target),
                    "link target rewritten incorrectly");
        }
        peer.join();
    }
}

static void disconnected_mount(const char *root)
{
    int pair[2];
    require(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "mount socketpair");
    pid_t peer = fork();
    require(peer >= 0, "peer fork");
    if (!peer) {
        close(pair[0]); read_query(pair[1]); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    std::string mountpoint = std::string(root)+"/files";
    void *mount = dcc_gcc_files_start(pair[0], mountpoint.c_str(), "fixture", root,
                                     "/tree", "part.c", directories);
    require(mount, "FUSE mount");
    close(pair[0]);
    // This lookup may already be waiting in the kernel when prefetch fails.
    struct stat info;
    require(lstat((mountpoint+"/probe").c_str(), &info) < 0, "failed service answered lookup");
    uint64_t cpu = 0;
    unsigned long long peak = 0;
    require(dcc_gcc_files_stop(mount, &cpu, &peak) != 0, "failed prefetch reported success");
    int status;
    require(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && !WEXITSTATUS(status), "peer exit");
    require(!rmdir(mountpoint.c_str()), "mount cleanup");
}

static void clean_mount(const char *root, bool lookup)
{
    int pair[2];
    require(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "clean mount socketpair");
    pid_t peer = fork();
    require(peer >= 0, "clean mount peer fork");
    if (!peer) {
        close(pair[0]); read_query(pair[1]);
        std::string payload;
        for (unsigned i = 0; i < 3; ++i) {
            std::string stamp(DCC_FS_METADATA, '\0');
            uint32_t permissions = htonl(S_IFDIR | 0755);
            memcpy(stamp.data()+32, &permissions, sizeof(permissions));
            integer(payload, 0); integer(payload, stamp.size()); payload += stamp;
        }
        std::string response;
        integer(response, 0); integer(response, payload.size()); response += payload;
        require(send(pair[1], response.data(), response.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(response.size()), "clean mount reply");
        close(pair[1]); _exit(0);
    }
    close(pair[1]);
    // The worker's own signal policy must survive starting the service, while
    // the service needs an interruptible event loop for ordered shutdown.
    auto previous_handler = signal(SIGTERM, SIG_IGN);
    sigset_t stop_signal, previous_mask, current_mask;
    sigemptyset(&stop_signal); sigaddset(&stop_signal, SIGTERM);
    sigprocmask(SIG_BLOCK, &stop_signal, &previous_mask);
    std::string mountpoint = std::string(root)+"/clean-files";
    void *mount = dcc_gcc_files_start(pair[0], mountpoint.c_str(), "fixture", root,
                                     "/tree", "part.c", directories);
    require(mount, "clean FUSE mount");
    require(signal(SIGTERM, previous_handler) == SIG_IGN, "worker signal policy changed");
    sigprocmask(SIG_SETMASK, nullptr, &current_mask);
    require(sigismember(&current_mask, SIGTERM), "worker signal mask changed");
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    close(pair[0]);
    struct stat info;
    if (lookup) require(!lstat((mountpoint+"/tree").c_str(), &info), "live FUSE lookup");
    mounted_child = static_cast<Mount *>(mount)->child;
    uint64_t cpu = 0;
    unsigned long long peak = 0;
    int result = dcc_gcc_files_stop(mount, &cpu, &peak);
    mounted_child = 0;
    require(!result, "clean shutdown reported failure");
    // Detaching while the kernel read is in flight can return ECONNABORTED,
    // even for a regular unmount. The reader must have exited first.
    require(!reader_alive_at_unmount, "unmount raced the live FUSE reader");
    int status;
    require(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && !WEXITSTATUS(status), "clean peer exit");
    require(!rmdir(mountpoint.c_str()), "clean mount cleanup");
}

int main(int argc, char **argv)
{
    cached_links();
    negative_hints();
    for (const char *mode : {"valid", "denied", "short", "length", "flags", "missing",
                             "trailing", "bad-error", "disconnect"}) replies(mode);
    require(argc == 2, "temporary root argument");
    clean_mount(argv[1], true);
    clean_mount(argv[1], false);
    disconnected_mount(argv[1]);
}
