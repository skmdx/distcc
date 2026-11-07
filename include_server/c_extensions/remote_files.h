/* Compiler-owned remote lookups. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../../src/tcp_job.h"
#include "../../src/file_cache.h"
#include "../../src/frontend_fs.h"
#include "../../src/remote_files.h"
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

using dccfs::number;
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
    std::unique_ptr<llvm::MemoryBuffer> storage;
    std::string prefix;
    size_t offset = 0;
public:
    Directory(llvm::StringRef parent, std::unique_ptr<llvm::MemoryBuffer> storage)
        : storage(std::move(storage)) {
        prefix.reserve(parent.size()+1);
        prefix.append(parent.data(), parent.size()); prefix += '/';
    }
    std::error_code increment() override {
        auto bytes = storage->getBuffer();
        if (offset == bytes.size()) { CurrentEntry = {}; return {}; }
        if (bytes.size()-offset < 8) {
            CurrentEntry = {};
            return std::make_error_code(std::errc::protocol_error);
        }
        unsigned length = number(bytes.data()+offset, 4), mode = number(bytes.data()+offset+4, 4);
        offset += 8;
        if (length > bytes.size()-offset) {
            CurrentEntry = {};
            return std::make_error_code(std::errc::protocol_error);
        }
        CurrentEntry = llvm::vfs::directory_entry((llvm::Twine(prefix)+bytes.substr(offset, length)).str(), file_type(mode));
        offset += length;
        return {};
    }
};
class RemoteFiles : public llvm::vfs::FileSystem {
    // The key owns the path and the wire metadata owns its attributes.
    // Materialize LLVM Status only for entries the compiler actually asks for.
    struct Entry {
        std::error_code error;
        std::array<char, DCC_FS_METADATA> version{};
        std::shared_ptr<const llvm::MemoryBuffer> contents;
        bool queried = false, listed = false;
        explicit Entry(std::error_code error) : error(error) {}
        explicit Entry(const char *bytes) {
            memcpy(version.data(), bytes, version.size());
        }
        unsigned mode() const { return number(version.data()+32, 4); }
        uint64_t size() const { return number(version.data()+24, 8); }
        llvm::ErrorOr<llvm::vfs::Status> status(llvm::StringRef name) const {
            if (error) return error;
            return metadata(name, version.data());
        }
        void update(const char *bytes) {
            if (!error && !memcmp(version.data(), bytes, version.size())) return;
            error.clear();
            memcpy(version.data(), bytes, version.size());
            contents.reset(); queried = listed = false;
        }
    };
    dccfs::Connection connection;
    std::string cwd;
    llvm::StringMap<Entry> entries;
    Entry &remember(llvm::StringRef name, const char *bytes) {
        auto [found, inserted] = entries.try_emplace(name, bytes);
        if (!inserted) found->second.update(bytes);
        return found->second;
    }
    std::error_code remember(llvm::StringRef name, std::error_code error) {
        entries.insert_or_assign(name, Entry(error));
        return error;
    }
    llvm::SmallString<256> absolute(const llvm::Twine &path) const {
        llvm::SmallString<256> name;
        path.toVector(name);
        if (!llvm::sys::path::is_absolute(name)) {
            name.insert(name.begin(), '/');
            name.insert(name.begin(), cwd.begin(), cwd.end());
        }
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
                if (found->second.error) return found->second.error;
                if (end < name.size() && !S_ISDIR(found->second.mode()))
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
        dccfs::Buffer bytes;
        if (auto error = connection.request(op, {path.data(), path.size()}, bytes)) return error;
        return std::unique_ptr<llvm::MemoryBuffer>(new SnapshotBuffer(
            {bytes.data, bytes.size}, path.str(), std::move(bytes.owner)));
    }
public:
    RemoteFiles(int fd, std::string cwd, const dcc_file_cache_api &cache,
                std::string identity, std::string cache_root)
        : connection(fd, cache, std::move(identity), cache_root), cwd(std::move(cwd)) {}
    bool share_stream() { return connection.share_stream(); }
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
        if (found != entries.end() && (found->second.error ||
            !S_ISDIR(found->second.mode()) || found->second.queried)) return found->second.status(name);
        if (found == entries.end()) {
            if (auto error = cachedError(name)) return error;
        }
        auto response = request(DCC_FS_STAT, name);
        if (!response) return remember(name, response.getError());
        auto answer = (*response)->getBuffer();
        if (answer.size() < DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
        auto &record = remember(name, answer.data());
        if (S_ISDIR(record.mode()) && answer.size() >= DCC_FS_METADATA+4) {
            unsigned count = number(answer.data() + DCC_FS_METADATA, 4);
            size_t offset = DCC_FS_METADATA+4;
            llvm::SmallString<256> entry(name);
            entry += '/';
            size_t prefix = entry.size();
            for (unsigned i = 0; i < count; ++i) {
                if (answer.size() - offset < 8) return std::make_error_code(std::errc::protocol_error);
                unsigned length = number(answer.data() + offset, 4), error = number(answer.data() + offset + 4, 4);
                offset += 8;
                if (length > answer.size() - offset) return std::make_error_code(std::errc::protocol_error);
                entry.resize(prefix);
                entry += answer.substr(offset, length);
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
        return record.status(name);
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &path) override {
        auto name = absolute(path);
        std::array<char, DCC_FS_METADATA> version{};
        auto found = entries.find(name);
        if (found != entries.end()) {
            auto &record = found->second;
            if (record.error) return record.error;
            if (record.contents)
                return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(metadata(name, record.version.data()), record.contents));
            if (S_ISREG(record.mode())) {
                version = record.version;
            }
        } else if (auto error = cachedError(name)) return error;
        dccfs::Snapshot snapshot;
        if (auto error = connection.read({name.data(), name.size()},
                found != entries.end() && S_ISREG(found->second.mode()) ? version.data() : nullptr, snapshot))
            return remember(name, error);
        auto &record = remember(name, snapshot.version.data());
        record.contents = std::make_shared<SnapshotBuffer>(
            llvm::StringRef(snapshot.bytes.data, snapshot.bytes.size), name.str().str(), std::move(snapshot.bytes.owner));
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(metadata(name, record.version.data()), record.contents));
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &path, std::error_code &error) override {
        auto name = absolute(path);
        auto response = request(DCC_FS_LIST, name);
        if (!response) { error = response.getError(); return {}; }
        auto iterator = std::make_shared<Directory>(name, std::move(*response));
        error = iterator->increment();
        if (error) return {};
        return llvm::vfs::directory_iterator(std::move(iterator));
    }
    std::error_code setCurrentWorkingDirectory(const llvm::Twine &path) override {
        auto info = status(path);
        if (!info) return info.getError();
        if (!info->isDirectory()) return std::make_error_code(std::errc::not_a_directory);
        cwd = absolute(path).str().str();
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
