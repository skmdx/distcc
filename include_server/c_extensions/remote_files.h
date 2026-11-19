/* Compiler-owned remote lookups. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../../src/tcp_job.h"
#include "../../src/job.h"
#include "../../src/file_cache.h"
#include "../../src/frontend_fs.h"
#include "../../src/remote_files.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Lex/HeaderSearchOptions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SHA256.h>
#include <llvm/ADT/StringExtras.h>
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
#include <set>

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
        bool queried = false, listed = false, have_real = false;
        unsigned observed = 0;
        std::string resolved;
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
            contents.reset(); resolved.clear();
            queried = listed = have_real = false;
        }
    };
    dccfs::Connection connection;
    std::string cwd;
    llvm::StringMap<Entry> entries;
    std::string scope;
    std::set<std::string> reads;
    bool recording = false, stable = true;
    std::vector<std::string> observations;
    void observe(llvm::StringRef name, unsigned operation, std::error_code error = {}) {
        if (!recording) return;
        if (!llvm::json::isUTF8(name) || observations.size() >= DCC_JOB_INPUT_MAX) {
            stable = false; return;
        }
        auto [found, inserted] = entries.try_emplace(name, error);
        if (!found->second.observed) observations.push_back(name.str());
        found->second.observed |= operation;
    }
    static std::string digest(llvm::StringRef bytes) {
        auto hash = llvm::SHA256::hash(llvm::ArrayRef(
            reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
        return llvm::toHex(hash);
    }
    llvm::json::Object observation(llvm::StringRef name) const {
        const auto &entry = entries.find(name)->second;
        llvm::json::Object result{{"path", name.str()}, {"ops", entry.observed},
                                  {"error", entry.error.value()}};
        if (entry.error) return result;
        auto version = entry.version;
        // A directory's identity/search permission matters, but unrelated
        // output creation does not alter a stat-only observation. Actual
        // negative and positive names are recorded separately.
        if (S_ISDIR(entry.mode())) {
            memset(version.data()+16, 0, 16);
            memset(version.data()+44, 0, 8);
        }
        result["metadata"] = digest({version.data(), version.size()});
        if (entry.observed & 2) {
            if (entry.contents) result["contents"] = digest(entry.contents->getBuffer());
            else result["error"] = EPROTO;
        }
        if (entry.observed & 4) result["resolved"] = entry.resolved;
        return result;
    }
    Entry &remember(llvm::StringRef name, const char *bytes) {
        auto [found, inserted] = entries.try_emplace(name, bytes);
        if (!inserted) {
            if (found->second.observed && (found->second.error ||
                memcmp(found->second.version.data(), bytes, DCC_FS_METADATA))) stable = false;
            found->second.update(bytes);
        }
        return found->second;
    }
    std::error_code remember(llvm::StringRef name, std::error_code error) {
        auto old = entries.find(name);
        if (old != entries.end() && old->second.observed && old->second.error != error) stable = false;
        if (old == entries.end()) entries.try_emplace(name, error);
        else old->second.error = error;
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
        : connection(fd, cache, std::move(identity), cache_root), cwd(std::move(cwd)) { connection.async_writes(cache_root); }
    using Query = dccfs::Connection::Query;
    std::error_code prefetch(unsigned op, const std::vector<Query> &queries) {
        return connection.batch(op, queries, [this](const Query &query, std::error_code ec, std::string_view data) {
            if (query.op == DCC_FS_REALPATH) {
                if (!ec) {
                    auto found = entries.find(query.path);
                    if (found != entries.end()) {
                        found->second.resolved.assign(data);
                        found->second.have_real = true;
                    }
                }
            } else if (!ec) {
                auto result = applyStatus(query.path, {data.data(), data.size()});
                if (!result) return result.getError();
            } else if (ec != std::errc::file_too_large && ec != std::errc::permission_denied) {
                (void)remember(query.path, ec);
            }
            return std::error_code{};
        });
    }
    void retain(llvm::StringRef name, dccfs::Snapshot snapshot) {
        auto &record = remember(name, snapshot.version.data());
        record.contents = std::make_shared<SnapshotBuffer>(
            llvm::StringRef(snapshot.bytes.data, snapshot.bytes.size), name.str(), std::move(snapshot.bytes.owner));
    }
    void prepare(const clang::CompilerInvocation &invocation) {
        std::set<std::string> directories;
        for (const auto &entry : invocation.getHeaderSearchOpts().UserEntries)
            directories.insert(absolute(entry.Path).str().str());
        const auto &inputs = invocation.getFrontendOpts().Inputs;
        if (!inputs.empty() && inputs[0].isFile())
            directories.insert(llvm::sys::path::parent_path(absolute(inputs[0].getFile())).str());
        scope = "clang-scope:" + cwd;
        std::vector<Query> queries;
        for (const auto &path : directories) {
            scope += '\0'; scope += path;
            queries.push_back({DCC_FS_STAT, path});
        }
        (void)prefetch(DCC_FS_DIRECTORIES, queries);
        auto hints = connection.source_hints(scope);
        if (!hints.owner) return;
        llvm::StringRef names(hints.data, hints.size);
        queries.clear();
        while (!names.empty()) {
            auto pair = names.split('\0');
            if (pair.first.starts_with("/"))
                queries.push_back({DCC_FS_STATUS, pair.first.str()});
            names = pair.second;
        }
        if (prefetch(DCC_FS_BATCH, queries)) return;
        std::vector<Query> missing;
        for (const auto &q : queries) {
            auto it = entries.find(q.path);
            if (it == entries.end() || it->second.error || !S_ISREG(it->second.mode())) continue;
            const char *version = it->second.version.data();
            dccfs::Snapshot snapshot;
            if (connection.cached_file(q.path, version, snapshot)) retain(q.path, std::move(snapshot));
            else missing.push_back({DCC_FS_READ, q.path, 8 + DCC_FS_METADATA + number(version+24, 8)});
        }
        (void)connection.batch(DCC_FS_BATCH, missing, [this](const Query &q, std::error_code ec, std::string_view data) {
            if (ec) return std::error_code{}; // Retry speculative failures on actual open.
            // LLVM requires a NUL after every individual source image.
            auto bytes = std::make_shared<std::string>(data);
            dccfs::Snapshot snapshot;
            auto error = connection.snapshot(q.path, entries.find(q.path)->second.version.data(),
                {bytes, bytes->data(), bytes->size()}, snapshot);
            if (!error) retain(q.path, std::move(snapshot));
            return error;
        });
    }
    void finish() {
        connection.finish_writes();
        std::string hints;
        for (const auto &path : reads) {
            if (hints.size() + path.size() + 1 > DCC_FS_QUERY_MAX) break;
            hints += path; hints += '\0';
        }
        if (!hints.empty()) connection.save_source_hints(scope, hints);
    }
    int claimResult(llvm::StringRef action) { return connection.claim_compilation({action.data(), action.size()}); }
    dccfs::Buffer result(llvm::StringRef action) { return connection.compilation({action.data(), action.size()}); }
    void saveResult(llvm::StringRef action, llvm::StringRef value) {
        connection.save_compilation({action.data(), action.size()}, {value.data(), value.size()});
    }
    void beginObservations() {
        for (auto &entry : entries) entry.second.observed = 0;
        observations.clear(); stable = recording = true;
    }
    std::optional<llvm::json::Array> certificate() const {
        if (!stable) return std::nullopt;
        llvm::json::Array result;
        for (const auto &name : observations) result.push_back(observation(name));
        return result;
    }
    bool validate(const llvm::json::Array &expected) {
        if (expected.empty() || expected.size() > DCC_JOB_INPUT_MAX) return false;
        beginObservations();
        std::vector<Query> queries;
        for (const auto &value : expected) {
            auto *item = value.getAsObject();
            if (!item) return false;
            auto path = item->getString("path"); auto ops = item->getInteger("ops");
            if (!path || !ops || *ops <= 0 || (*ops & ~7) || !path->starts_with("/")) return false;
            queries.push_back({DCC_FS_STATUS, path->str()});
            if (*ops & 4) queries.push_back({DCC_FS_REALPATH, path->str()});
        }
        if (prefetch(DCC_FS_BATCH, queries)) return false;
        for (const auto &value : expected) {
            const auto *item = value.getAsObject();
            auto path = item->getString("path");
            auto ops = item->getInteger("ops");
            if (*ops & 1) (void)status(*path);
            if (*ops & 2) (void)openFileForRead(*path);
            if (*ops & 4) {
                llvm::SmallString<256> resolved;
                (void)getRealPath(*path, resolved);
            }
            if (!stable || entries.find(*path) == entries.end() ||
                llvm::json::Value(observation(*path)) != value) return false;
        }
        return true;
    }
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
        auto result = statusImpl(name);
        observe(name, 1, result ? std::error_code{} : result.getError());
        return result;
    }
    llvm::ErrorOr<llvm::vfs::Status> statusImpl(llvm::StringRef name) {
        auto found = entries.find(name);
        if (found != entries.end() && (found->second.error ||
            !S_ISDIR(found->second.mode()) || found->second.queried)) return found->second.status(name);
        if (found == entries.end()) {
            if (auto error = cachedError(name)) return error;
        }
        auto response = request(DCC_FS_STAT, name);
        if (!response) return remember(name, response.getError());
        return applyStatus(name, (*response)->getBuffer());
    }
    llvm::ErrorOr<llvm::vfs::Status> applyStatus(llvm::StringRef name, llvm::StringRef answer) {
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
        auto result = openImpl(name);
        if (result && reads.size() < DCC_JOB_INPUT_MAX) reads.insert(name.str().str());
        // A failed file open does not invalidate path metadata: an include
        // candidate may be a directory containing a later requested header.
        // Do not certify a failed read using a successful stat observation.
        if (!result) {
            auto error = result.getError();
            if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
                (void)remember(name, error);
            else { stable = false; return result; }
        }
        observe(name, 2, result ? std::error_code{} : result.getError());
        return result;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openImpl(llvm::StringRef name) {
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
            return error;
        retain(name, std::move(snapshot));
        auto &record = entries.find(name)->second;
        return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(metadata(name, record.version.data()), record.contents));
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &path, std::error_code &error) override {
        if (recording) stable = false; // Enumeration is not a stat-only dependency.
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
        auto name = absolute(path);
        auto found = entries.find(name);
        if (found != entries.end() && found->second.have_real) {
            auto &resolved = found->second.resolved;
            observe(name, 4);
            output.assign(resolved.begin(), resolved.end());
            return {};
        }
        auto answer = request(DCC_FS_REALPATH, name);
        if (recording && (found == entries.end() || !answer)) stable = false;
        if (!answer) return answer.getError();
        auto bytes = (*answer)->getBuffer();
        if (recording && found != entries.end()) {
            if (!llvm::json::isUTF8(bytes)) stable = false;
            else { found->second.resolved = bytes.str(); observe(name, 4); }
        }
        output.assign(bytes.begin(), bytes.end());
        return {};
    }
};
