/* Compiler-independent remote filesystem transport and immutable snapshots.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "tcp_job.h"
#include "file_cache.h"
#include "frontend_fs.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <pthread.h>
#include <unistd.h>
#include <array>
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
    std::string directory_key(std::string_view path) const {
        std::string key("directory-uses\0", 15);
        key += identity; key += '\0'; key += path;
        return key;
    }
public:
    Connection(int fd, const dcc_file_cache_api &cache, std::string identity,
               const std::string &cache_root)
        : fd(fd), cache(cache), records(cache.open(cache_root.c_str())), identity(std::move(identity)) {}
    Connection(const Connection &) = delete;
    ~Connection() {
        cache.close(records);
        if (shared) { pthread_mutex_destroy(shared); munmap(shared, sizeof(*shared)); }
        close(fd);
    }
    bool share_stream() {
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
    Buffer directory_hints(std::string_view path) { return cached(directory_key(path)); }
    void save_directory_hints(std::string_view path, std::string_view names) {
        auto key = directory_key(path);
        // Concurrent publications may lose a prefetch hint, never a file or
        // its metadata. The next job always enumerates and stats afresh.
        cache.write(records, key.data(), key.size(), names.data(), names.size());
    }
    std::error_code read(std::string_view path, const char *version, Snapshot &out) {
        std::string key;
        if (version) {
            key.reserve(identity.size()+1+DCC_FS_METADATA+path.size());
            key.append(identity); key += '\0'; key.append(version, DCC_FS_METADATA); key.append(path);
            auto bytes = cached(key);
            if (bytes.owner && bytes.size == number(version+24, 8)) {
                memcpy(out.version.data(), version, DCC_FS_METADATA);
                out.bytes = std::move(bytes);
                return {};
            }
        }
        Buffer response;
        if (auto error = request(DCC_FS_READ, path, response)) return error;
        if (response.size < DCC_FS_METADATA ||
            number(response.data+24, 8) != response.size-DCC_FS_METADATA)
            return std::make_error_code(std::errc::protocol_error);
        memcpy(out.version.data(), response.data, DCC_FS_METADATA);
        out.bytes = {response.owner, response.data+DCC_FS_METADATA, response.size-DCC_FS_METADATA};
        if (version && !memcmp(version, out.version.data(), DCC_FS_METADATA))
            cache.write(records, key.data(), key.size(), out.bytes.data, out.bytes.size);
        return {};
    }
};
}
