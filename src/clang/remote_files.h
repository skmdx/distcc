/* Compiler-owned remote lookups. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../job.h"
extern "C" {
#include "../file_cache.h"
}
#include "../frontend_fs.h"
#include "../remote_files.h"
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
#include <map>
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
        std::unique_ptr<dccfs::DirectoryListing> directory;
        mutable std::string content_hash;
        bool queried = false, have_real = false;
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
            contents.reset(); content_hash.clear(); resolved.clear();
            directory.reset();
            queried = have_real = false;
        }
    };
    dccfs::Connection connection;
    std::string cwd;
    llvm::StringMap<Entry> entries;
    std::optional<dccfs::SourceHints> hints;
    dccfs::SourceHints::Directories reads;
    std::map<std::string, bool> include_roots; // true for a system search entry outside cwd
    std::string system_key;
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
            if (entry.contents) {
                if (entry.content_hash.empty()) entry.content_hash = digest(entry.contents->getBuffer());
                result["contents"] = entry.content_hash;
            }
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
    void remember(llvm::StringRef name, std::error_code error) {
        auto old = entries.find(name);
        if (old != entries.end() && old->second.observed && old->second.error != error) stable = false;
        if (old == entries.end()) entries.try_emplace(name, error);
        else old->second.error = error;
    }
    llvm::SmallString<256> absolute(const llvm::Twine &path) const {
        llvm::SmallString<256> name;
        path.toVector(name);
        if (!llvm::sys::path::is_absolute(name)) {
            name.insert(name.begin(), '/');
            name.insert(name.begin(), cwd.begin(), cwd.end());
        }
        // Directory replies join parent and leaf with one separator. Keep
        // trailing slashes and symlink/.. traversal significant.
        name.erase(std::unique(name.begin(), name.end(),
                               [](char a, char b) { return a == '/' && b == '/'; }), name.end());
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
                    parent != entries.end() && parent->second.directory &&
                    parent->second.directory->excludes({leaf.data(), leaf.size()}))
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
    std::error_code prefetch(const std::vector<Query> &queries) {
        return connection.batch(DCC_FS_BATCH, queries, [this](const Query &query, std::error_code ec, const dccfs::Buffer &data) {
            if (query.operation == DCC_FS_REALPATH) {
                auto found = entries.find(query.path);
                if (!ec && found != entries.end()) {
                    found->second.resolved.assign(data.data, data.size);
                    found->second.have_real = true;
                }
            } else if (!ec) {
                if (data.size != DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
                remember(query.path, data.data).queried = true;
            } else {
                remember(query.path, ec);
            }
            return std::error_code{};
        });
    }
    void retain(llvm::StringRef name, dccfs::Snapshot snapshot) {
        auto &record = remember(name, snapshot.version.data());
        record.content_hash.clear();
        record.contents = std::make_shared<SnapshotBuffer>(
            llvm::StringRef(snapshot.bytes.data, snapshot.bytes.size), name.str(), std::move(snapshot.bytes.owner));
    }
    // absolute() collapses repeated separators. In shared records a double
    // slash separates the owning include root from the absolute directory.
    // Remember the most specific root: a broad SDK root must not activate
    // an optional -isystem subtree learned through a narrower entry.
    std::string sharedDirectory(const std::string &dir) const {
        for (llvm::StringRef path(dir); !path.empty(); path = llvm::sys::path::parent_path(path)) {
            if (path == cwd) return dir;
            auto root = include_roots.find(path.str());
            if (root != include_roots.end())
                return root->second ? path.rtrim('/').str() + '/' + dir : dir;
            if (path == "/" || llvm::sys::path::filename(path) == "..") break;
        }
        return dir;
    }
    static std::pair<llvm::StringRef, llvm::StringRef> splitDirectory(llvm::StringRef key) {
        auto split = key.find("//");
        if (split == key.npos) return {{}, key};
        return {split ? key.take_front(split) : "/", key.drop_front(split+1)};
    }
    std::error_code prepare(const clang::CompilerInvocation &invocation) {
        dccfs::SourceHints::Directories directories;
        const auto &search = invocation.getHeaderSearchOpts();
        auto cwd_prefix = llvm::StringRef(cwd).rtrim('/').str() + '/';
        bool has_system = false;
        for (const auto &entry : search.UserEntries) {
            auto path = absolute(!entry.IgnoreSysRoot && llvm::sys::path::is_absolute(entry.Path)
                                     ? search.Sysroot + entry.Path : entry.Path).str().str();
            directories.try_emplace(path);
            auto root = llvm::StringRef(path).rtrim('/');
            if (root.empty()) root = "/";
            bool system = entry.Group >= clang::frontend::System && root != cwd && !root.starts_with(cwd_prefix);
            include_roots[root.str()] |= system;
            has_system |= system;
        }
        const auto &inputs = invocation.getFrontendOpts().Inputs;
        if (!inputs.empty() && inputs[0].isFile()) {
            auto source = absolute(inputs[0].getFile()).str().str();
            auto lang = std::to_string(static_cast<unsigned>(inputs[0].getKind().getLanguage()));
            hints.emplace(connection, "clang-source-v2:" + source, "clang-scope-v3:" + lang + ':' + cwd);
            if (has_system) system_key = "clang-system-v1:" + lang;
            dccfs::SourceHints::Directories predicted;
            if (int error = hints->load(predicted)) return {error, std::generic_category()};
            auto bootstrap = [&](const auto &record) {
                for (const auto &[key, names] : record) {
                    auto [root, dir] = splitDirectory(key);
                    if (!root.empty()) {
                        auto active = include_roots.find(root.str());
                        if (active == include_roots.end() || !active->second) continue;
                    }
                    directories[dir.str()].insert(names.begin(), names.end());
                }
            };
            bootstrap(predicted);
            if (!hints->known_source() && !system_key.empty()) {
                predicted.clear();
                if (int error = hints->decode(connection.source_hints(system_key), predicted))
                    return {error, std::generic_category()};
                bootstrap(predicted);
            }
            directories.try_emplace(llvm::sys::path::parent_path(source).str());
        }
        std::vector<dccfs::DirectoryQuery> queries;
        for (const auto &[path, names] : directories) {
            auto &q = queries.emplace_back();
            q.path = path;
            q.previous = connection.directory_names(path);
            if (int error = q.encode(names, directories, true)) return {error, std::generic_category()};
        }
        std::vector<Query> follow;
        auto error = connection.batch(DCC_FS_DIRECTORIES, queries, [&](const auto &q, std::error_code error,
                                                                       const dccfs::Buffer &reply) {
            if (!error) return accept(q, reply, follow);
            // A search entry can itself be a file. ENOTDIR for path/ does
            // not establish the status of path.
            if (error == std::errc::not_a_directory) follow.push_back({DCC_FS_STATUS, q.path});
            // A speculative access denial is not a compiler observation.
            else if (error != std::errc::permission_denied && error != std::errc::file_too_large)
                remember(q.path, error);
            return std::error_code{};
        });
        return error ? error : prefetch(follow);
    }
    void finish() {
        connection.finish_writes();
        if (!hints) return;
        hints->save_source(connection, reads);
        dccfs::SourceHints::Directories scope, system;
        for (auto &[dir, names] : reads) {
            auto key = sharedDirectory(dir);
            if (key != dir) system.emplace(key, names);
            scope.emplace(std::move(key), std::move(names));
        }
        auto prune = [&](const std::string &key, auto &names) {
            auto dir = splitDirectory(key).second;
            return dccfs::prune_missing(dir.str(), names, [&](const std::string &path) -> dccfs::HintStatus {
                auto found = entries.find(path);
                if (found == entries.end()) return {};
                auto &entry = found->second;
                return {entry.mode(), entry.error.value(), entry.directory.get()};
            });
        };
        hints->merge_scope(connection, scope, prune);
        if (!system_key.empty()) hints->merge(connection, system_key, 2048, system, prune);
    }
    int claimResult(llvm::StringRef action) { return connection.claim_compilation({action.data(), action.size()}); }
    dccfs::Buffer result(llvm::StringRef action) { return connection.compilation({action.data(), action.size()}); }
    void saveResult(llvm::StringRef action, llvm::StringRef value) {
        connection.save_compilation({action.data(), action.size()}, {value.data(), value.size()});
    }
    void beginObservations() {
        for (auto &entry : entries) entry.second.observed = 0;
        observations.clear(); reads.clear(); stable = recording = true;
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
        if (prefetch(queries)) return false;
        for (const auto &value : expected) {
            const auto *item = value.getAsObject();
            auto path = item->getString("path");
            auto ops = item->getInteger("ops");
            if (*ops & 1) (void)status(*path);
            if (*ops & 2) (void)openFileForRead(*path);
            if (*ops & 4) {
                llvm::SmallString<256> resolved;
                if (getRealPath(*path, resolved)) return false;
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
        Entry *entry;
        if (auto e = lookup(name, entry)) return e;
        if (entry && (!S_ISDIR(entry->mode()) || entry->queried)) return entry->status(name);
        dccfs::DirectoryQuery query;
        query.path = name.str();
        query.previous = connection.directory_names(query.path);
        if (auto e = fetch(query, {}, entry != nullptr)) return e;
        return entries.find(name)->second.status(name);
    }
    std::error_code lookup(llvm::StringRef name, Entry *&entry) {
        auto found = entries.find(name);
        if (found == entries.end()) {
            auto parent = entries.find(llvm::sys::path::parent_path(name));
            auto leaf = llvm::sys::path::filename(name);
            if (parent != entries.end() && parent->second.directory &&
                parent->second.directory->listing == dccfs::DirectoryListing::Partial &&
                dccfs::valid_name({leaf.data(), leaf.size()})) {
                // An unpredicted lookup upgrades a partial footprint once.
                dccfs::DirectoryQuery query;
                query.path = parent->getKey().str();
                if (auto e = fetch(query, {leaf.str()}, true)) return e;
                found = entries.find(name);
            }
        }
        entry = found == entries.end() ? nullptr : &found->second;
        return entry ? entry->error : cachedError(name);
    }
    std::error_code fetch(dccfs::DirectoryQuery &query, const std::set<std::string> &children, bool directory) {
        if (int e = query.encode(children, {}, directory)) return {e, std::generic_category()};
        dccfs::Buffer response;
        if (auto e = connection.request(query.operation, query.bytes, response)) {
            if (directory && e == std::errc::not_a_directory)
                return prefetch({{DCC_FS_STATUS, query.path}});
            remember(query.path, e);
            return e;
        }
        std::vector<Query> follow;
        if (auto error = accept(query, response, follow)) return error;
        return prefetch(follow);
    }
    std::error_code accept(const dccfs::DirectoryQuery &query, const dccfs::Buffer &reply,
                           std::vector<Query> &follow) {
        if (reply.size < DCC_FS_METADATA) return std::make_error_code(std::errc::protocol_error);
        // Versioned LSTAT works with every peer. Clang observes stat
        // semantics, so resolve just the links in one metadata-only batch.
        if (S_ISLNK(number(reply.data+32, 4))) {
            follow.push_back({DCC_FS_STATUS, query.path});
            return {};
        }
        auto &entry = remember(query.path, reply.data);
        entry.queried = true;
        entry.directory.reset();
        if (reply.size == DCC_FS_METADATA) return {};
        auto listing = std::make_unique<dccfs::DirectoryListing>();
        std::string prefix = query.path;
        if (prefix.back() != '/') prefix += '/';
        int e = listing->decode(connection, query, reply, [&](std::string_view leaf, unsigned error, const char *version) {
            auto path = prefix + std::string(leaf);
            if (error) remember(path, {static_cast<int>(error), std::generic_category()});
            else if (S_ISLNK(number(version+32, 4))) follow.push_back({DCC_FS_STATUS, std::move(path)});
            else remember(path, version);
        });
        if (e) return {e, std::generic_category()};
        entry.directory = std::move(listing);
        return {};
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &path) override {
        auto name = absolute(path);
        auto result = openImpl(name);
        if (result) reads[llvm::sys::path::parent_path(name).str()].insert(llvm::sys::path::filename(name).str());
        // A failed file open does not invalidate path metadata: an include
        // candidate may be a directory containing a later requested header.
        // Do not certify a failed read using a successful stat observation.
        if (!result) {
            auto error = result.getError();
            if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
                remember(name, error);
            else { stable = false; return result; }
        }
        observe(name, 2, result ? std::error_code{} : result.getError());
        return result;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openImpl(llvm::StringRef name) {
        Entry *entry;
        if (auto error = lookup(name, entry)) return error;
        if (entry && entry->contents)
            return std::unique_ptr<llvm::vfs::File>(new SnapshotFile(metadata(name, entry->version.data()), entry->contents));
        dccfs::Snapshot snapshot;
        if (auto error = connection.read({name.data(), name.size()},
                entry && S_ISREG(entry->mode()) ? entry->version.data() : nullptr, snapshot)) return error;
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
