/* Compiler-independent remote filesystem transport and immutable snapshots.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "tcp_job.h"
#include "file_cache.h"
#include "frontend_fs.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <pthread.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <climits>
#include <map>
#include <set>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace dccfs {
inline uint64_t number(const char *p, unsigned bytes) {
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n = (n << 8) | (unsigned char)p[i];
    return n;
}
struct Buffer {
    std::shared_ptr<const void> owner;
    const char *data = nullptr;
    size_t size = 0;
};
struct Snapshot {
    std::array<char, DCC_FS_METADATA> version{};
    Buffer bytes;
};
class Connection {
    int fd;
    pthread_mutex_t *shared = nullptr;
    const dcc_file_cache_api &cache;
    dcc_record_cache *records;
    std::string identity;
    struct Writer {
        const dcc_file_cache_api &api;
        dcc_record_cache *records;
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::pair<std::string, Buffer>> queue;
        size_t queued = 0;
        bool stopping = false;
        std::thread thread;
        Writer(const dcc_file_cache_api &api, const std::string &root)
            : api(api), records(api.open(root.c_str())), thread([this] { run(); }) {}
        ~Writer() {
            { std::lock_guard lock(mutex); stopping = true; }
            ready.notify_one();
            thread.join();
            api.close(records);
        }
        void put(std::string key, Buffer bytes) {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return queued < 16 * 1024 * 1024; });
            queued += bytes.size;
            queue.emplace_back(std::move(key), std::move(bytes));
            ready.notify_all();
        }
        void run() {
            std::unique_lock lock(mutex);
            for (;;) {
                ready.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return;
                auto [key, bytes] = std::move(queue.front());
                queue.pop_front();
                lock.unlock();
                api.write(records, key.data(), key.size(), bytes.data, bytes.size);
                lock.lock();
                queued -= bytes.size;
                ready.notify_all();
            }
        }
    };
    std::unique_ptr<Writer> writer;
    std::string write_root;
    static bool receive(int fd, void *data, size_t size) {
        auto p = static_cast<char *>(data);
        while (size) {
            ssize_t n = recv(fd, p, size, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n; size -= n;
        }
        return true;
    }
    static bool send(int fd, unsigned operation, std::string_view path) {
        uint32_t header[] = {htonl(DCC_TCP_FS_QUERY), htonl(path.size()+4), htonl(operation)};
        iovec parts[] = {{header, sizeof(header)}, {const_cast<char *>(path.data()), path.size()}};
        msghdr message{};
        message.msg_iov = parts; message.msg_iovlen = path.empty() ? 1 : 2;
        while (message.msg_iovlen) {
            ssize_t written = sendmsg(fd, &message, MSG_NOSIGNAL);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            size_t consumed = written;
            while (message.msg_iovlen && consumed >= message.msg_iov->iov_len) {
                consumed -= message.msg_iov->iov_len;
                ++message.msg_iov; --message.msg_iovlen;
            }
            if (message.msg_iovlen) {
                message.msg_iov->iov_base = static_cast<char *>(message.msg_iov->iov_base)+consumed;
                message.msg_iov->iov_len -= consumed;
            }
        }
        return true;
    }
    Buffer cached(const std::string &key) {
        size_t extent = 0, length = 0;
        const void *bytes = nullptr;
        int snapshot = cache.lookup(records, key.data(), key.size(), &extent);
        if (snapshot < 0) return {};
        // LLVM also consumes these records. A page-aligned EOF has no readable
        // trailing NUL, so that uncommon record needs an owned buffer.
        void *mapped = extent && extent % sysconf(_SC_PAGESIZE)
            ? mmap(nullptr, extent, PROT_READ, MAP_PRIVATE, snapshot, 0) : MAP_FAILED;
        std::shared_ptr<const void> owner;
        const void *record;
        if (mapped != MAP_FAILED) {
            close(snapshot);
            owner = {mapped, [extent](const void *p) { munmap(const_cast<void *>(p), extent); }};
            record = mapped;
        } else {
            auto data = std::shared_ptr<char[]>(new (std::nothrow) char[extent+1]);
            size_t used = 0;
            while (data && used < extent) {
                ssize_t n = pread(snapshot, data.get()+used, extent-used, used);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                used += n;
            }
            close(snapshot);
            if (!data || used != extent) return {};
            data[extent] = 0;
            record = data.get(); owner = std::move(data);
        }
        if (!cache.contents(record, extent, key.data(), key.size(), &bytes, &length)) return {};
        return {std::move(owner), static_cast<const char *>(bytes), length};
    }
    std::string cache_key(std::string_view kind, std::string_view path) const {
        std::string key(kind);
        key += '\0'; key += identity; key += '\0';
        key += path;
        return key;
    }
public:
    Connection(int fd, const dcc_file_cache_api &cache, std::string identity,
               const std::string &cache_root)
        : fd(fd), cache(cache), records(cache.open(cache_root.c_str())), identity(std::move(identity)) {}
    Connection(const Connection &) = delete;
    ~Connection() {
        finish_writes();
        cache.close(records);
        if (shared) { pthread_mutex_destroy(shared); munmap(shared, sizeof(*shared)); }
        close(fd);
    }
    void async_writes(std::string root) { write_root = std::move(root); }
    void finish_writes() { writer.reset(); }
    bool share_stream() {
        finish_writes();
        if (shared) return true;
        void *memory = mmap(nullptr, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return false;
        auto mutex = static_cast<pthread_mutex_t *>(memory);
        pthread_mutexattr_t attributes;
        int error = pthread_mutexattr_init(&attributes);
        if (!error) {
            error = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
            if (!error) error = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
            if (!error) error = pthread_mutex_init(mutex, &attributes);
            pthread_mutexattr_destroy(&attributes);
        }
        if (error) { munmap(memory, sizeof(*mutex)); return false; }
        shared = mutex;
        return true;
    }
    std::error_code request(unsigned op, std::string_view path, Buffer &out) {
        // A forked backend may share the stream; ownership spans the entire
        // exchange so a reader cannot consume another child's reply.
        if (shared) {
            int error = pthread_mutex_lock(shared);
            if (error) {
                if (error == EOWNERDEAD) {
                    shutdown(fd, SHUT_RDWR);
                    pthread_mutex_unlock(shared);
                }
                return {error, std::generic_category()};
            }
        }
        struct Unlock {
            pthread_mutex_t *mutex;
            ~Unlock() { if (mutex) pthread_mutex_unlock(mutex); }
        } unlock{shared};
        uint32_t header[2];
        if (!send(fd, op, path) || !receive(fd, header, sizeof(header)))
            return std::make_error_code(std::errc::io_error);
        unsigned error = ntohl(header[0]), size = ntohl(header[1]);
        if (size > DCC_TCP_BYTES) return std::make_error_code(std::errc::message_size);
        auto data = std::shared_ptr<char[]>(new (std::nothrow) char[size_t(size)+1]);
        if (!data) return std::make_error_code(std::errc::not_enough_memory);
        if (!receive(fd, data.get(), size)) return std::make_error_code(std::errc::io_error);
        data[size] = 0;
        if (error) return {static_cast<int>(error), std::generic_category()};
        out = {data, data.get(), size};
        return {};
    }
    struct Query {
        unsigned operation;
        std::string path;
        const std::string &payload() const { return path; }
    };
    template<typename Q, typename Apply>
    std::error_code batch(unsigned op, const std::vector<Q> &queries, Apply apply) {
        size_t first = 0;
        while (first < queries.size()) {
            std::string packet;
            size_t end = first;
            while (end < queries.size()) {
                const auto &q = queries[end];
                size_t size = 8 + q.payload().size();
                if (size + 4 > DCC_FS_QUERY_MAX) return std::make_error_code(std::errc::filename_too_long);
                if (packet.size() + size + 4 > DCC_FS_QUERY_MAX) break;
                uint32_t header[] = {htonl(size - 4), htonl(q.operation)};
                packet.append(reinterpret_cast<const char *>(header), sizeof(header));
                packet += q.payload();
                ++end;
            }
            Buffer reply;
            if (auto ec = request(op, packet, reply)) return ec;
            std::string_view data(reply.data, reply.size);
            while (first < end) {
                if (data.size() < 8) return std::make_error_code(std::errc::protocol_error);
                unsigned status = number(data.data(), 4);
                if (status > INT_MAX) return std::make_error_code(std::errc::protocol_error);
                std::error_code ec(status, std::generic_category());
                size_t length = number(data.data()+4, 4);
                data.remove_prefix(8);
                if (length > data.size()) return std::make_error_code(std::errc::protocol_error);
                if (auto error = apply(queries[first++], ec, Buffer{reply.owner, data.data(), length})) return error;
                data.remove_prefix(length);
            }
            if (!data.empty()) return std::make_error_code(std::errc::protocol_error);
        }
        return {};
    }
    Buffer directory_names(std::string_view path) { return cached(cache_key("directory-names", path)); }
    Buffer compilation(std::string_view action) { return cached(cache_key("clang-result", action)); }
    int claim_compilation(std::string_view action) {
        auto key = cache_key("clang-result", action);
        int claim = cache.claim(records, key.data(), key.size(), 0);
        if (claim >= 0 || errno != EAGAIN) return claim;
        auto execution = [this](unsigned active) {
            uint32_t value = htonl(active);
            Buffer answer;
            auto error = request(DCC_FS_EXECUTION,
                {reinterpret_cast<const char *>(&value), sizeof(value)}, answer);
            if (!error && (answer.size != 4 || number(answer.data, 4) != 1))
                error = std::make_error_code(std::errc::protocol_error);
            if (error) throw std::system_error(error, "compiler execution lane");
        };
        execution(0);
        claim = cache.claim(records, key.data(), key.size(), 1);
        int error = errno;
        try { execution(1); }
        catch (...) { if (claim >= 0) close(claim); throw; }
        errno = error;
        return claim;
    }
    void save_compilation(std::string_view action, std::string_view result) {
        auto key = cache_key("clang-result", action);
        cache.write(records, key.data(), key.size(), result.data(), result.size());
    }
    Buffer source_hints(std::string_view source) {
        return cached(cache_key("gcc-source-hints", source));
    }
    void save_directory_names(std::string_view path, std::string_view record) {
        auto key = cache_key("directory-names", path);
        // One atomic record couples names to their observed directory version.
        cache.write(records, key.data(), key.size(), record.data(), record.size());
    }
    void save_source_hints(std::string_view source, std::string_view hints) {
        auto key = cache_key("gcc-source-hints", source);
        cache.write(records, key.data(), key.size(), hints.data(), hints.size());
    }
    int claim_source_hints(std::string_view scope) {
        auto key = cache_key("gcc-source-hints", scope);
        return cache.claim(records, key.data(), key.size(), 1);
    }
    std::string file_key(std::string_view path, const char *version) const {
        std::string key(identity);
        key += '\0'; key.append(version, DCC_FS_METADATA); key.append(path);
        return key;
    }
    bool cached_file(std::string_view path, const char *version, Snapshot &out) {
        auto bytes = cached(file_key(path, version));
        if (!bytes.owner || bytes.size != number(version+24, 8)) return false;
        memcpy(out.version.data(), version, DCC_FS_METADATA);
        out.bytes = std::move(bytes);
        return true;
    }
    std::error_code snapshot(std::string_view path, const char *version, const Buffer &response, Snapshot &out) {
        if (response.size < DCC_FS_METADATA ||
            number(response.data+24, 8) != response.size-DCC_FS_METADATA)
            return std::make_error_code(std::errc::protocol_error);
        memcpy(out.version.data(), response.data, DCC_FS_METADATA);
        out.bytes = {response.owner, response.data+DCC_FS_METADATA, response.size-DCC_FS_METADATA};
        if ((!version && !write_root.empty()) || (version && !memcmp(version, response.data, DCC_FS_METADATA))) {
            auto key = file_key(path, response.data);
            if (!write_root.empty() && !shared) {
                if (!writer) writer = std::make_unique<Writer>(cache, write_root);
                writer->put(std::move(key), out.bytes);
            } else cache.write(records, key.data(), key.size(), out.bytes.data, out.bytes.size);
        }
        return {};
    }
    std::error_code read(std::string_view path, const char *version, Snapshot &out) {
        if (version && cached_file(path, version, out)) return {};
        Buffer response;
        if (auto error = request(DCC_FS_READ, path, response)) return error;
        return snapshot(path, version, response, out);
    }
};

inline bool valid_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
        name.find_first_of("/\0", 0, 2) == std::string_view::npos;
}

// The source owns its lookup footprint. Only a cold source consults the scope.
class SourceHints {
public:
    using Directories = std::map<std::string, std::set<std::string>>;
private:
    std::string source, scope;
    Buffer previous;
    bool source_hit;
    static std::string encode(const Directories &directories) {
        std::string record;
        for (const auto &[directory, names] : directories) {
            record += directory; record += '\0';
            for (const auto &name : names) { record += name; record += '\0'; }
            record += '\0';
        }
        return record;
    }
    static bool equals(const Buffer &snapshot, std::string_view record) {
        return snapshot.owner && record == std::string_view(snapshot.data, snapshot.size);
    }
public:
    SourceHints(Connection &c, std::string source, std::string scope)
        : source(std::move(source)), scope(std::move(scope)), previous(c.source_hints(this->source)),
          source_hit(bool(previous.owner)) {
        if (!source_hit) previous = c.source_hints(this->scope);
    }
    bool known_source() const { return source_hit; }
    bool learned() const { return bool(previous.owner); }
    int load(Directories &directories) const { return decode(previous, directories); }
    static int decode(const Buffer &snapshot, Directories &directories) {
        if (!snapshot.owner) return 0;
        std::string_view bytes(snapshot.data, snapshot.size);
        while (!bytes.empty()) {
            auto end = bytes.find('\0');
            if (end == std::string_view::npos || bytes.front() != '/') return EPROTO;
            auto [entry, inserted] = directories.try_emplace(std::string(bytes.substr(0, end)));
            if (!inserted) return EPROTO;
            bytes.remove_prefix(end+1);
            for (;;) {
                end = bytes.find('\0');
                if (end == std::string_view::npos) return EPROTO;
                auto name = bytes.substr(0, end);
                bytes.remove_prefix(end+1);
                if (name.empty()) break;
                if (!valid_name(name) || !entry->second.emplace(name).second) return EPROTO;
            }
        }
        return 0;
    }
    void save_source(Connection &c, const Directories &actual) const {
        auto record = encode(actual);
        if (!source_hit || !equals(previous, record)) c.save_source_hints(source, record);
    }
    // Count both directory queries and child stats, including empty GCC footprints.
    // Take this job's observations first, then retain still-live prior hints.
    template<class Prune>
    static void merge(Connection &c, const std::string &key, size_t limit,
                      const Directories &actual, Prune prune) {
        struct Claim {
            int fd;
            ~Claim() { if (fd >= 0) close(fd); }
        } claim{c.claim_source_hints(key)};
        if (claim.fd < 0) return; // The optional cache cannot publish this update.
        auto latest = c.source_hints(key);
        if (actual.empty() && !latest.owner) return;
        Directories prior, merged;
        if (decode(latest, prior)) return;
        auto add = [&](const Directories &from) {
            for (const auto &[dir, names] : from) {
                if (!limit) break;
                auto [entry, inserted] = merged.try_emplace(dir);
                limit -= inserted;
                for (const auto &name : names) {
                    if (!limit) break;
                    limit -= entry->second.insert(name).second;
                }
            }
        };
        add(actual);
        for (auto it = prior.begin(); it != prior.end(); ) {
            if (prune(it->first, it->second)) it = prior.erase(it);
            else ++it;
        }
        add(prior);
        auto record = encode(merged);
        if (!equals(latest, record)) c.save_source_hints(key, record);
    }
    template<class Prune>
    void merge_scope(Connection &c, const Directories &actual, Prune prune) const {
        merge(c, scope, 4096, actual, prune);
    }
};

struct DirectoryQuery {
    unsigned operation = DCC_FS_LSTAT;
    std::string path, bytes;
    Buffer previous;
    const std::string &payload() const { return bytes; }

    template<class Scheduled = std::set<std::string>>
    int encode(const std::set<std::string> &hints, const Scheduled &scheduled = {}, bool directory = false) {
        if (previous.owner && previous.size < DCC_FS_METADATA) return EPROTO;
        bytes = path;
        if (directory && bytes.back() != '/') bytes += '/';
        bytes += '\0';
        if (previous.owner) bytes.append(previous.data, DCC_FS_METADATA);
        else bytes.append(DCC_FS_METADATA, '\0');
        if (bytes.size() > DCC_FS_QUERY_MAX-12) return ENAMETOOLONG;
        const std::string prefix = path == "/" ? path : path + '/';
        for (const auto &name : hints) {
            if (scheduled.count(prefix + name)) continue;
            if (name.size()+1 > DCC_FS_QUERY_MAX-12-bytes.size()) break;
            bytes += name; bytes += '\0';
        }
        return 0;
    }
};

struct DirectoryListing {
    enum Listing { Deferred, Unavailable, Partial, Ready } listing = Deferred;
    std::vector<std::string_view> names;
    // Owns the name views for a directory, or the file contents for a GCC inode.
    Buffer bytes;
    bool excludes(std::string_view name) const {
        return listing == Ready && !std::binary_search(names.begin(), names.end(), name);
    }
    template<class Child>
    int decode(Connection &connection, const DirectoryQuery &query, const Buffer &response, Child child) {
        if (response.size < DCC_FS_METADATA+8 || !S_ISDIR(number(response.data+32, 4))) return EPROTO;
        const auto &previous = query.previous;
        auto flags = number(response.data+DCC_FS_METADATA, 4);
        if (flags != 0 && flags != DCC_FS_DIRECTORY_CACHED &&
            flags != DCC_FS_DIRECTORY_STABLE && flags != DCC_FS_DIRECTORY_PARTIAL) return EPROTO;
        bool reused = flags == DCC_FS_DIRECTORY_CACHED;
        bool partial = flags == DCC_FS_DIRECTORY_PARTIAL;
        names.clear();
        bytes = reused ? previous : response;
        if (reused) {
            if (!previous.owner || memcmp(response.data, previous.data, DCC_FS_METADATA)) return EPROTO;
            std::string_view saved(previous.data+DCC_FS_METADATA, previous.size-DCC_FS_METADATA);
            while (!saved.empty()) {
                auto end = saved.find('\0');
                if (end == std::string_view::npos || !valid_name(saved.substr(0, end))) return EPROTO;
                names.push_back(saved.substr(0, end));
                saved.remove_prefix(end+1);
            }
        }
        std::string saved;
        if (flags == DCC_FS_DIRECTORY_STABLE) saved.assign(response.data, DCC_FS_METADATA);
        auto count = number(response.data+DCC_FS_METADATA+4, 4);
        std::string_view rows(response.data+DCC_FS_METADATA+8, response.size-DCC_FS_METADATA-8);
        for (unsigned i = 0; i < count; ++i) {
            if (rows.size() < 8) return EPROTO;
            size_t length = number(rows.data(), 4);
            unsigned error = number(rows.data()+4, 4);
            rows.remove_prefix(8);
            if (length > rows.size()) return EPROTO;
            auto name = rows.substr(0, length);
            if (!valid_name(name)) return EPROTO;
            rows.remove_prefix(length);
            if (error != DCC_FS_NO_METADATA) {
                if (error > INT_MAX || (!error && rows.size() < DCC_FS_METADATA)) return EPROTO;
                child(name, error, rows.data());
                if (!error) rows.remove_prefix(DCC_FS_METADATA);
            }
            if (!reused && !partial) names.push_back(name);
            if (flags == DCC_FS_DIRECTORY_STABLE) { saved += name; saved += '\0'; }
        }
        if (!rows.empty()) return EPROTO;
        std::sort(names.begin(), names.end());
        listing = partial ? Partial : Ready;
        if (flags == DCC_FS_DIRECTORY_STABLE)
            connection.save_directory_names(query.path, saved);
        return 0;
    }
};

struct HintStatus {
    unsigned mode = 0;
    int error = 0;
    const DirectoryListing *directory = nullptr;
    bool absent() const { return error == ENOENT || error == ENOTDIR; }
};

template<class Lookup>
bool prune_missing(const std::string &dir, std::set<std::string> &names, Lookup lookup) {
    auto parent = lookup(dir);
    if (parent.error) return parent.absent();
    if (parent.mode && !S_ISDIR(parent.mode)) return true;
    if (names.empty()) return false;
    auto prefix = dir == "/" ? dir : dir + '/';
    for (auto it = names.begin(); it != names.end(); ) {
        if ((parent.directory && parent.directory->excludes(*it)) || lookup(prefix + *it).absent())
            it = names.erase(it);
        else ++it;
    }
    return names.empty();
}
}
