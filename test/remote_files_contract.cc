/* Exercise VFS snapshot ownership and the actual local request stream. */
#include "../include_server/c_extensions/remote_files.h"
#include <thread>
#include <csignal>
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char *message) {
    if (!value) { fprintf(stderr, "%s\n", message); exit(1); }
}
static void integer(std::string &bytes, uint64_t value, unsigned size) {
    while (size) bytes += char(value >> (--size * 8));
}
static std::string stamp(unsigned mode, unsigned size, unsigned revision) {
    std::string bytes;
    integer(bytes, 1, 8); integer(bytes, revision, 8); integer(bytes, revision, 8);
    integer(bytes, size, 8); integer(bytes, mode, 4);
    integer(bytes, 0, 4); integer(bytes, 0, 4); integer(bytes, revision, 8);
    return bytes;
}
static void child(std::string &bytes, const std::string &name, const std::string &info) {
    integer(bytes, name.size(), 4); integer(bytes, 0, 4); bytes += name; bytes += info;
}
static void read_bytes(int fd, char *bytes, size_t size) {
    while (size) {
        ssize_t count = read(fd, bytes, std::min(size_t(1024), size));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "truncated query"); bytes += count; size -= count;
    }
}
static unsigned cache_reads, cache_writes;
static dcc_record_cache *cache_open(const char *) { return reinterpret_cast<dcc_record_cache *>(&cache_reads); }
static void cache_close(dcc_record_cache *) {}
static void *cached(dcc_record_cache *, const void *key, size_t size, const void **data, size_t *length) {
    ++cache_reads;
    if (!llvm::StringRef(static_cast<const char *>(key), size).ends_with("/tree/b")) return nullptr;
    *length = 5;
    char *storage = strdup("headcache");
    *data = storage+4;
    return storage;
}
static void store(dcc_record_cache *, const void *key, size_t size, const void *bytes, size_t length) {
    ++cache_writes;
    require(llvm::StringRef(static_cast<const char *>(key), size).ends_with("/tree/c"), "cached changed version");
    require(llvm::StringRef(static_cast<const char *>(bytes), length) == "third", "wrong cache bytes");
}
static volatile sig_atomic_t interrupted;
static void interrupt(int) { interrupted = 1; }

int main() {
    auto directory = stamp(S_IFDIR | 0755, 0, 1);
    auto regular = stamp(S_IFREG | 0644, 5, 1);
    auto listing = directory;
    integer(listing, 4, 4);
    child(listing, "a", regular); child(listing, "b", regular); child(listing, "c", regular);
    child(listing, "child", directory);
    std::string types;
    integer(types, 1, 4); integer(types, S_IFREG, 4); types += 'a';
    integer(types, 5, 4); integer(types, S_IFDIR, 4); types += "child";
    auto empty_directory = directory; integer(empty_directory, 0, 4);
    std::string long_path = '/' + std::string(65536, 'x');
    struct Reply { unsigned operation; std::string path, bytes; unsigned error = 0, extra = 0; };
    std::vector<Reply> replies{
        {DCC_FS_STAT, "/tree", listing},
        {DCC_FS_READ, "/tree/a", stamp(S_IFREG | 0644, 5, 2) + "fresh"},
        {DCC_FS_READ, "/tree/c", regular + "third"},
        {DCC_FS_STAT, "/tree/child", empty_directory},
        {DCC_FS_LIST, "/tree", types},
        {DCC_FS_REALPATH, long_path, "/tree/a"},
        {DCC_FS_READ, "/direct", stamp(S_IFREG | 0644, 0, 1)},
        {DCC_FS_STAT, "/denied", {}, EACCES},
        {DCC_FS_REALPATH, "/broken", "bad", 0, 10},
    };
    int sockets[2];
    require(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "socketpair");
    int small = 1024;
    require(!setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), "send buffer");
    struct sigaction action{};
    action.sa_handler = interrupt;
    require(!sigaction(SIGUSR1, &action, nullptr), "signal handler");
    auto caller = pthread_self();
    std::thread peer([&] {
        for (const auto &reply : replies) {
            char header[12]; read_bytes(sockets[1], header, sizeof(header));
            require(number(header, 4) == DCC_TCP_FS_QUERY && number(header+8, 4) == reply.operation,
                    "query operation");
            require(number(header+4, 4) == reply.path.size()+4, "query length");
            if (reply.path == long_path) pthread_kill(caller, SIGUSR1);
            std::string path(reply.path.size(), '\0');
            read_bytes(sockets[1], path.data(), path.size());
            require(path == reply.path, "partial query repeated or lost bytes");
            std::string bytes;
            integer(bytes, reply.error, 4); integer(bytes, reply.bytes.size()+reply.extra, 4);
            bytes += reply.bytes;
            for (size_t offset = 0; offset < bytes.size();) {
                ssize_t count = send(sockets[1], bytes.data()+offset,
                                     std::min(size_t(3), bytes.size()-offset), MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR) continue;
                require(count > 0, "reply write"); offset += count;
            }
        }
        close(sockets[1]);
    });
    std::unique_ptr<llvm::MemoryBuffer> retained, retained_cache;
    {
        dcc_file_cache_api cache{cache_open, cache_close, cached, store};
        RemoteFiles files(sockets[0], "/", cache, "client", "/unused-cache");
        auto tree = files.status("/tree");
        require(tree && tree->isDirectory(), "directory snapshot");
        auto a = files.openFileForRead("/tree/a"); require(bool(a), "changed-version read");
        auto view = (*a)->getBuffer("a", -1, true, false); require(bool(view), "first view");
        retained = std::move(*view);
        require(retained->getBuffer() == "fresh" && !retained->getBufferEnd()[0], "payload/terminator");
        auto again = files.openFileForRead("/tree/a"); require(bool(again), "cached open");
        auto second = (*again)->getBuffer("again", -1, true, false);
        require(second && (*second)->getBufferStart() == retained->getBufferStart(), "copied cached payload");
        require(files.status("/tree/a")->getUniqueID() == llvm::sys::fs::UniqueID(1, 2), "stale metadata");
        for (auto name : {"/tree/b", "/tree/c"}) {
            auto file = files.openFileForRead(name); require(bool(file), "cache/read file");
            auto bytes = (*file)->getBuffer(name, -1, true, false);
            require(bytes && (*bytes)->getBuffer() == (name[6] == 'b' ? "cache" : "third"), "cache/read bytes");
            if (name[6] == 'b') retained_cache = std::move(*bytes);
        }
        require(files.status("/tree/new/file").getError() == std::errc::no_such_file_or_directory,
                "missing child snapshot");
        require(files.status("/tree/a/file").getError() == std::errc::not_a_directory, "file parent");
        require(bool(files.status("/tree/child")), "batched directory was treated as already queried");
        require(files.status("/tree/child/missing").getError() == std::errc::no_such_file_or_directory,
                "empty directory snapshot");
        std::error_code error;
        auto it = files.dir_begin("/tree", error);
        require(!error && it->path() == "/tree/a" && it->type() == llvm::sys::fs::file_type::regular_file,
                "directory entry");
        it.increment(error);
        require(!error && it->path() == "/tree/child", "second directory entry");
        it.increment(error); require(!error && it == llvm::vfs::directory_iterator(), "directory end");
        llvm::SmallString<64> resolved;
        require(!files.getRealPath(long_path, resolved) && resolved == "/tree/a" && interrupted,
                "interrupted scatter/gather query");
        auto empty = files.openFileForRead("/direct"); require(bool(empty), "direct read");
        auto zero = (*empty)->getBuffer("empty", -1, true, false);
        require(zero && (*zero)->getBufferSize() == 0 && !(*zero)->getBufferStart()[0], "empty buffer");
        require(files.status("/denied").getError() == std::errc::permission_denied, "denied status");
        require(files.openFileForRead("/denied").getError() == std::errc::permission_denied, "lost cached error");
        require(files.getRealPath("/broken", resolved) == std::errc::io_error, "published truncated reply");
        require(cache_reads == 3 && cache_writes == 1, "cache admission count");
    }
    peer.join();
    require(retained->getBuffer() == "fresh", "buffer did not outlive filesystem");
    require(retained_cache->getBuffer() == "cache", "cache view did not outlive filesystem");
}
