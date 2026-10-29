/* Compiler-owned remote lookups. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../../src/tcp_job.h"
#include "../../src/file_cache.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

static uint64_t number(const char *p, unsigned bytes) {
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n = (n << 8) | (unsigned char)p[i];
    return n;
}
static bool transfer(int fd, void *data, size_t size, bool writing) {
    auto p = static_cast<char *>(data);
    while (size) {
        ssize_t n = writing ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
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
class SnapshotFile : public llvm::vfs::File {
    llvm::vfs::Status info;
    std::shared_ptr<std::string> bytes;
public:
    SnapshotFile(llvm::vfs::Status info, std::shared_ptr<std::string> bytes)
        : info(std::move(info)), bytes(std::move(bytes)) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return info; }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(
            const llvm::Twine &name, int64_t, bool, bool) override {
        return llvm::MemoryBuffer::getMemBufferCopy(*bytes, name.str());
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
    int fd;
    std::string cwd;
    std::unordered_map<std::string, llvm::ErrorOr<llvm::vfs::Status>> statuses;
    std::unordered_map<std::string, std::shared_ptr<std::string>> contents;
    std::unordered_map<std::string, std::string> versions;
    const dcc_file_cache_api &cache;
    std::string identity, cache_root;
    std::unordered_set<std::string> queried, listed;
    std::string absolute(const llvm::Twine &path) const {
        auto name = path.str();
        if (!llvm::sys::path::is_absolute(name)) name = cwd + '/' + name;
        return name;
    }
    std::error_code cachedError(const std::string &name) const {
        // Preserve symlink/.. traversal. A missing intermediate component
        // cannot become valid merely by removing the following /.. lexically.
        size_t end = 1;
        while (end < name.size()) {
            end = name.find('/', end);
            if (end == std::string::npos) end = name.size();
            auto entry = name.substr(0, end);
            auto found = statuses.find(entry);
            if (found != statuses.end()) {
                if (!found->second) return found->second.getError();
                if (end < name.size() && !found->second->isDirectory())
                    return std::make_error_code(std::errc::not_a_directory);
            } else {
                auto leaf = llvm::sys::path::filename(entry);
                if (!leaf.empty() && leaf != "." && leaf != ".." &&
                    listed.count(llvm::sys::path::parent_path(entry).str()))
                    return std::make_error_code(std::errc::no_such_file_or_directory);
            }
            if (end == name.size()) break;
            ++end;
        }
        return {};
    }
    llvm::ErrorOr<std::string> request(unsigned op, const std::string &path) {
        uint32_t header[] = {htonl(DCC_TCP_FS_QUERY), htonl(path.size() + 4), htonl(op)};
        std::string data((char *)header, sizeof(header)); data += path;
        if (!transfer(fd, data.data(), data.size(), true) ||
            !transfer(fd, header, 8, false))
            return std::make_error_code(std::errc::io_error);
        unsigned error = ntohl(header[0]), size = ntohl(header[1]);
        if (size > 512U * 1024 * 1024) return std::make_error_code(std::errc::message_size);
        data.resize(size);
        if (!transfer(fd, data.data(), size, false)) return std::make_error_code(std::errc::io_error);
        if (error) return std::error_code(error, std::generic_category());
        return data;
    }
public:
    RemoteFiles(int fd, std::string cwd, const dcc_file_cache_api &cache,
                std::string identity, std::string cache_root)
        : fd(fd), cwd(std::move(cwd)), cache(cache), identity(std::move(identity)),
          cache_root(std::move(cache_root)) {}
    ~RemoteFiles() override { ::close(fd); }
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &path) override {
        auto name = absolute(path);
        auto found = statuses.find(name);
        if (found != statuses.end() && (!found->second || !found->second->isDirectory() || queried.count(name)))
            return found->second;
        if (found == statuses.end()) {
            if (auto error = cachedError(name)) return error;
        }
        auto answer = request(1, name);
        llvm::ErrorOr<llvm::vfs::Status> info = !answer ?
            llvm::ErrorOr<llvm::vfs::Status>(answer.getError()) : answer->size() < 52 ?
            llvm::ErrorOr<llvm::vfs::Status>(std::make_error_code(std::errc::protocol_error)) :
            llvm::ErrorOr<llvm::vfs::Status>(metadata(name, answer->data()));
        if (info) versions.insert_or_assign(name, answer->substr(0, 52));
        if (info && info->isDirectory() && answer->size() >= 56) {
            unsigned count = number(answer->data() + 52, 4);
            size_t offset = 56;
            for (unsigned i = 0; i < count; ++i) {
                if (answer->size() - offset < 8) return std::make_error_code(std::errc::protocol_error);
                unsigned length = number(answer->data() + offset, 4), error = number(answer->data() + offset + 4, 4);
                offset += 8;
                if (length > answer->size() - offset) return std::make_error_code(std::errc::protocol_error);
                auto entry = name + '/' + answer->substr(offset, length);
                offset += length;
                if (error) statuses.insert_or_assign(entry, std::error_code(error, std::generic_category()));
                else {
                    if (answer->size() - offset < 52) return std::make_error_code(std::errc::protocol_error);
                    statuses.insert_or_assign(entry, metadata(entry, answer->data() + offset));
                    versions.insert_or_assign(entry, answer->substr(offset, 52));
                    offset += 52;
                }
            }
            if (offset != answer->size()) return std::make_error_code(std::errc::protocol_error);
            listed.insert(name);
        }
        queried.insert(name);
        statuses.insert_or_assign(name, info);
        return info;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &path) override {
        auto name = absolute(path);
        std::string key, version;
        auto found = statuses.find(name);
        if (found != statuses.end()) {
            if (!found->second) return found->second.getError();
            auto data = contents.find(name);
            if (data != contents.end())
                return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*found->second, data->second));
            auto metadata = versions.find(name);
            if (metadata != versions.end() && found->second->isRegularFile()) {
                version = metadata->second;
                key = identity + '\0' + version + name;
                size_t length = 0;
                void *bytes = cache.read(key.data(), key.size(), &length, cache_root.c_str());
                if (bytes) {
                    auto data = std::make_shared<std::string>(static_cast<char *>(bytes), length);
                    free(bytes);
                    if (length == found->second->getSize()) {
                        contents.emplace(name, data);
                        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(*found->second, std::move(data)));
                    }
                }
            }
        } else if (auto error = cachedError(name)) return error;
        auto answer = request(2, name);
        if (!answer) { statuses.insert_or_assign(name, answer.getError()); return answer.getError(); }
        if (answer->size() < 52) return std::make_error_code(std::errc::protocol_error);
        auto info = metadata(name, answer->data());
        if (info.getSize() != answer->size() - 52) return std::make_error_code(std::errc::protocol_error);
        auto data = std::make_shared<std::string>(answer->substr(52));
        if (!key.empty() && version == answer->substr(0, 52))
            cache.write(key.data(), key.size(), data->data(), data->size(), cache_root.c_str());
        contents.emplace(name, data);
        statuses.insert_or_assign(name, info);
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(info, std::move(data)));
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &path, std::error_code &error) override {
        auto name = absolute(path);
        auto answer = request(4, name);
        if (!answer) { error = answer.getError(); return {}; }
        std::vector<llvm::vfs::directory_entry> entries;
        size_t offset = 0;
        while (offset < answer->size()) {
            if (answer->size() - offset < 8) { error = std::make_error_code(std::errc::protocol_error); return {}; }
            unsigned length = number(answer->data() + offset, 4), mode = number(answer->data() + offset + 4, 4);
            offset += 8;
            if (length > answer->size() - offset) { error = std::make_error_code(std::errc::protocol_error); return {}; }
            entries.emplace_back(name + '/' + answer->substr(offset, length), file_type(mode));
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
        auto answer = request(3, absolute(path));
        if (!answer) return answer.getError();
        output.assign(answer->begin(), answer->end());
        return {};
    }
};
