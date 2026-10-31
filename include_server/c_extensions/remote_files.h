/* Compiler-owned remote lookups. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../../src/tcp_job.h"
#include "../../src/file_cache.h"
#include "../../src/frontend_fs.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringMap.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <array>

static uint64_t number(const char *p, unsigned bytes) {
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n = (n << 8) | (unsigned char)p[i];
    return n;
}
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
static bool send_query(int fd, unsigned operation, llvm::StringRef path) {
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
static llvm::sys::fs::file_type file_type(unsigned mode) {
    using Type = llvm::sys::fs::file_type;
    return S_ISREG(mode) ? Type::regular_file : S_ISDIR(mode) ? Type::directory_file :
           S_ISLNK(mode) ? Type::symlink_file : Type::type_unknown;
}
static llvm::vfs::Status metadata(llvm::StringRef path, const char *p) {
    return llvm::vfs::Status(path, {number(p, 8), number(p + 8, 8)},
        llvm::sys::TimePoint<>(std::chrono::nanoseconds((int64_t)number(p + 16, 8))),
        number(p + 36, 4), number(p + 40, 4), number(p + 24, 8),
        file_type(number(p + 32, 4)), (llvm::sys::fs::perms)(number(p + 32, 4) & 07777));
}
// LLVM may retain a buffer after closing its VFS file. Keep the immutable
// backing allocation alive instead of copying the complete header again.
class SnapshotBuffer : public llvm::MemoryBuffer {
    std::shared_ptr<const void> owner;
    std::string identifier;
public:
    SnapshotBuffer(llvm::StringRef bytes, std::string identifier,
                   std::shared_ptr<const void> owner)
        : owner(std::move(owner)), identifier(std::move(identifier)) {
        init(bytes.begin(), bytes.end(), true);
    }
    llvm::StringRef getBufferIdentifier() const override { return identifier; }
    BufferKind getBufferKind() const override { return MemoryBuffer_Malloc; }
};

class SnapshotFile : public llvm::vfs::File {
    llvm::vfs::Status info;
    std::shared_ptr<const llvm::MemoryBuffer> bytes;
public:
    SnapshotFile(llvm::vfs::Status info, std::shared_ptr<const llvm::MemoryBuffer> bytes)
        : info(std::move(info)), bytes(std::move(bytes)) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return info; }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(
            const llvm::Twine &name, int64_t, bool, bool) override {
        return std::unique_ptr<llvm::MemoryBuffer>(
            new SnapshotBuffer(bytes->getBuffer(), name.str(), bytes));
    }
    std::error_code close() override { return {}; }
};
class Directory : public llvm::vfs::detail::DirIterImpl {
    std::vector<llvm::vfs::directory_entry> entries;
    size_t next = 0;
public:
    explicit Directory(std::vector<llvm::vfs::directory_entry> entries)
        : entries(std::move(entries)) { (void)increment(); }
    std::error_code increment() override {
        CurrentEntry = next < entries.size() ? entries[next++] : llvm::vfs::directory_entry();
        return {};
    }
};
class RemoteFiles : public llvm::vfs::FileSystem {
    // One owned path and record replace separate status, version, contents
    // and directory-state tables. StringRef probes need no substring copies.
    struct Entry {
        llvm::ErrorOr<llvm::vfs::Status> info;
        std::array<char, DCC_FS_METADATA> version{};
        std::shared_ptr<const llvm::MemoryBuffer> contents;
        bool queried = false, listed = false;
        explicit Entry(std::error_code error) : info(error) {}
        Entry(llvm::StringRef name, const char *bytes) : info(metadata(name, bytes)) {
            memcpy(version.data(), bytes, version.size());
        }
        void update(llvm::StringRef name, const char *bytes) {
            if (info && !memcmp(version.data(), bytes, version.size())) return;
            info = metadata(name, bytes);
            memcpy(version.data(), bytes, version.size());
            contents.reset(); queried = listed = false;
        }
    };
    int fd;
    pthread_mutex_t *shared = nullptr;
    std::string cwd;
    llvm::StringMap<Entry> entries;
    const dcc_file_cache_api &cache;
    std::string identity, cache_root;
    Entry &remember(llvm::StringRef name, const char *bytes) {
        auto [found, inserted] = entries.try_emplace(name, name, bytes);
        if (!inserted) found->second.update(name, bytes);
        return found->second;
    }
    std::error_code remember(llvm::StringRef name, std::error_code error) {
        entries.insert_or_assign(name, Entry(error));
        return error;
    }
    std::string absolute(const llvm::Twine &path) const {
        auto name = path.str();
        if (!llvm::sys::path::is_absolute(name)) name = cwd + '/' + name;
        return name;
    }
    std::error_code cachedError(llvm::StringRef name) const {
        // Preserve symlink/.. traversal. A missing intermediate component
        // cannot become valid merely by removing the following /.. lexically.
        size_t end = 1;
        while (end < name.size()) {
            end = name.find('/', end);
            if (end == llvm::StringRef::npos) end = name.size();
            auto entry = name.take_front(end);
            auto found = entries.find(entry);
            if (found != entries.end()) {
                if (!found->second.info) return found->second.info.getError();
                if (end < name.size() && !found->second.info->isDirectory())
                    return std::make_error_code(std::errc::not_a_directory);
            } else {
                auto leaf = llvm::sys::path::filename(entry);
                auto parent = entries.find(llvm::sys::path::parent_path(entry));
                if (!leaf.empty() && leaf != "." && leaf != ".." &&
                    parent != entries.end() && parent->second.listed)
                    return std::make_error_code(std::errc::no_such_file_or_directory);
            }
            if (end == name.size()) break;
            ++end;
        }
        return {};
    }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> request(unsigned op, llvm::StringRef path) {
        // Forked backend partitions can still read assembler inputs. One
        // owner covers the complete exchange, so replies cannot cross jobs.
        if (shared) {
            int error = pthread_mutex_lock(shared);
            if (error) {
                if (error == EOWNERDEAD) {
                    shutdown(fd, SHUT_RDWR);
                    pthread_mutex_unlock(shared); // Leave the broken stream unrecoverable.
                }
                return std::error_code(error, std::generic_category());
            }
        }
        auto unlock = llvm::scope_exit([&] { if (shared) pthread_mutex_unlock(shared); });
        uint32_t header[2];
        if (!send_query(fd, op, path) || !receive(fd, header, sizeof(header)))
            return std::make_error_code(std::errc::io_error);
        unsigned error = ntohl(header[0]), size = ntohl(header[1]);
        if (size > 512U * 1024 * 1024) return std::make_error_code(std::errc::message_size);
        // The reply overwrites every byte. LLVM supplies only the trailing
        // NUL, avoiding a zero-fill pass over each incoming header/source.
        auto data = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(size);
        if (!data) return std::make_error_code(std::errc::not_enough_memory);
        if (!receive(fd, data->getBufferStart(), size)) return std::make_error_code(std::errc::io_error);
        if (error) return std::error_code(error, std::generic_category());
        return std::unique_ptr<llvm::MemoryBuffer>(std::move(data));
    }
public:
    RemoteFiles(int fd, std::string cwd, const dcc_file_cache_api &cache,
                std::string identity, std::string cache_root)
        : fd(fd), cwd(std::move(cwd)), cache(cache), identity(std::move(identity)),
          cache_root(std::move(cache_root)) {}
    ~RemoteFiles() override {
        if (shared) { pthread_mutex_destroy(shared); munmap(shared, sizeof(*shared)); }
        ::close(fd);
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
    unsigned codegen_cpus(unsigned requested) {
        if (requested && !share_stream()) return 0;
        uint32_t count = htonl(requested);
        auto answer = request(DCC_FS_CODEGEN, llvm::StringRef(reinterpret_cast<char *>(&count), sizeof(count)));
        if (!answer || (*answer)->getBufferSize() != 4) return 0;
        unsigned granted = number((*answer)->getBufferStart(), 4);
        return granted <= requested ? granted : 0;
    }
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &path) override {
        auto name = absolute(path);
        auto found = entries.find(name);
        if (found != entries.end() && (!found->second.info ||
            !found->second.info->isDirectory() || found->second.queried)) return found->second.info;
        if (found == entries.end()) {
            if (auto error = cachedError(name)) return error;
        }
        auto response = request(DCC_FS_STAT, name);
        if (!response) return remember(name, response.getError());
        auto answer = (*response)->getBuffer();
        if (answer.size() < DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
        auto &record = remember(name, answer.data());
        if (record.info->isDirectory() && answer.size() >= DCC_FS_METADATA+4) {
            unsigned count = number(answer.data() + DCC_FS_METADATA, 4);
            size_t offset = DCC_FS_METADATA+4;
            for (unsigned i = 0; i < count; ++i) {
                if (answer.size() - offset < 8) return std::make_error_code(std::errc::protocol_error);
                unsigned length = number(answer.data() + offset, 4), error = number(answer.data() + offset + 4, 4);
                offset += 8;
                if (length > answer.size() - offset) return std::make_error_code(std::errc::protocol_error);
                llvm::SmallString<256> entry(name);
                entry += '/'; entry += answer.substr(offset, length);
                offset += length;
                if (error) (void)remember(entry, std::error_code(error, std::generic_category()));
                else {
                    if (answer.size() - offset < DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
                    remember(entry, answer.data() + offset);
                    offset += DCC_FS_METADATA;
                }
            }
            if (offset != answer.size()) return std::make_error_code(std::errc::protocol_error);
            record.listed = true;
        }
        record.queried = true;
        return record.info;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &path) override {
        auto name = absolute(path);
        std::string key;
        std::array<char, DCC_FS_METADATA> version{};
        auto found = entries.find(name);
        if (found != entries.end()) {
            auto &record = found->second;
            if (!record.info) return record.info.getError();
            if (record.contents)
                return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*record.info, record.contents));
            if (record.info->isRegularFile()) {
                version = record.version;
                key.reserve(identity.size()+1+version.size()+name.size());
                key.append(identity); key += '\0'; key.append(version.data(), version.size()); key += name;
                size_t length = 0;
                void *bytes = cache.read(key.data(), key.size(), &length, cache_root.c_str());
                if (bytes) {
                    if (length == record.info->getSize()) {
                        auto owner = std::shared_ptr<void>(bytes, free);
                        record.contents = std::make_shared<SnapshotBuffer>(
                            llvm::StringRef(static_cast<char *>(bytes), length), name, std::move(owner));
                        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*record.info, record.contents));
                    }
                    free(bytes);
                }
            }
        } else if (auto error = cachedError(name)) return error;
        auto response = request(DCC_FS_READ, name);
        if (!response) return remember(name, response.getError());
        auto answer = (*response)->getBuffer();
        if (answer.size() < DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
        auto info = metadata(name, answer.data());
        if (info.getSize() != answer.size() - DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
        bool cacheable = !key.empty() && !memcmp(version.data(), answer.data(), version.size());
        auto &record = remember(name, answer.data());
        auto owner = std::shared_ptr<const llvm::MemoryBuffer>(std::move(*response));
        record.contents = std::make_shared<SnapshotBuffer>(answer.drop_front(DCC_FS_METADATA), name, owner);
        if (cacheable)
            cache.write(key.data(), key.size(), record.contents->getBufferStart(),
                        record.contents->getBufferSize(), cache_root.c_str());
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*record.info, record.contents));
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &path, std::error_code &error) override {
        auto name = absolute(path);
        auto response = request(DCC_FS_LIST, name);
        if (!response) { error = response.getError(); return {}; }
        auto answer = (*response)->getBuffer();
        std::vector<llvm::vfs::directory_entry> entries;
        size_t offset = 0;
        while (offset < answer.size()) {
            if (answer.size() - offset < 8) { error = std::make_error_code(std::errc::protocol_error); return {}; }
            unsigned length = number(answer.data() + offset, 4), mode = number(answer.data() + offset + 4, 4);
            offset += 8;
            if (length > answer.size() - offset) { error = std::make_error_code(std::errc::protocol_error); return {}; }
            entries.emplace_back(name + '/' + answer.substr(offset, length).str(), file_type(mode));
            offset += length;
        }
        error.clear();
        return llvm::vfs::directory_iterator(std::make_shared<Directory>(std::move(entries)));
    }
    std::error_code setCurrentWorkingDirectory(const llvm::Twine &path) override {
        auto info = status(path);
        if (!info) return info.getError();
        if (!info->isDirectory()) return std::make_error_code(std::errc::not_a_directory);
        cwd = absolute(path);
        return {};
    }
    llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override { return cwd; }
    std::error_code getRealPath(const llvm::Twine &path, llvm::SmallVectorImpl<char> &output) override {
        auto answer = request(DCC_FS_REALPATH, absolute(path));
        if (!answer) return answer.getError();
        auto bytes = (*answer)->getBuffer();
        output.assign(bytes.begin(), bytes.end());
        return {};
    }
};
