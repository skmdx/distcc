/* Let the native GCC frontend read client files through the existing RPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#define FUSE_USE_VERSION 31
#include <fuse3/fuse_lowlevel.h>
#include <limits.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <map>
#include <filesystem>
#include <set>
#include <vector>
#include <algorithm>
#include <limits>
extern "C" {
#include "file_cache.h"
}
#include "remote_files.h"
#include "gcc_files.h"

namespace {
using dccfs::number;
struct Entry {
    // The map owns the spelling; entries and keys live until this job ends.
    const std::string *path = nullptr;
    enum Listing { Deferred, Unavailable, Partial, Ready } listing = Deferred;
    int error = 0;
    std::array<char, DCC_FS_METADATA> version{};
    bool queried = false, searched = false;
    std::set<std::string> hints, observed;
    std::vector<std::string_view> names;
    // File contents or the immutable reply/cache record owning directory names.
    dccfs::Buffer bytes;
    // A job owns one filesystem snapshot. Repeated FUSE readlink callbacks
    // must not round-trip to the client and reauthorize the same hop.
    dccfs::Buffer link_target;
    unsigned mode() const { return number(version.data()+32, 4); }
};
struct Query {
    unsigned operation = DCC_FS_LSTAT;
    std::string path, bytes;
    dccfs::Buffer previous;
};
class Files {
    dccfs::Connection connection;
    std::string source, scope;
    dccfs::Buffer previous_hints;
    std::map<std::string, std::set<std::string>> predictions;
    int hints_error = 0;
    bool learned = false;
    std::map<std::string, Entry> entries;
    std::map<std::pair<uint64_t, uint64_t>, ino_t> inodes;
    static bool valid_name(std::string_view name) {
        return !name.empty() && name != "." && name != ".." &&
            name.find_first_of("/\0", 0, 2) == std::string_view::npos;
    }
    int load_hints(const dccfs::Buffer &snapshot) {
        if (!snapshot.owner) return 0;
        std::string_view bytes(snapshot.data, snapshot.size);
        while (!bytes.empty()) {
            auto end = bytes.find('\0');
            if (!end || end == std::string_view::npos || bytes.front() != '/') return EPROTO;
            auto [entry, inserted] = predictions.try_emplace(std::string(bytes.substr(0, end)));
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
public:
    std::string root;
    Files(int fd, std::string root, const char *identity, const char *cache_root,
          const char *cwd, const char *input)
        : connection(fd, dcc_file_cache_records, identity, cache_root),
          source((std::filesystem::path(cwd)/input).lexically_normal().string()),
          scope(std::filesystem::path(source).parent_path().string()), root(std::move(root)) {
        previous_hints = connection.source_hints(source);
        // A known source retains its own footprint. A new source can bootstrap
        // from a nearby job without overwriting either source's future hints.
        auto hints = previous_hints.owner ? previous_hints : connection.source_hints(scope);
        learned = bool(hints.owner);
        hints_error = load_hints(hints);
    }
    ~Files() {
        if (hints_error) return;
        std::string snapshot;
        snapshot.reserve(previous_hints.size);
        for (const auto &[path, entry] : entries) {
            if (!entry.searched) continue;
            // Speculative queries do not teach the next job to repeat them.
            // Retain only children the compiler actually requested this time.
            // An empty footprint still records a searched directory: its fresh
            // names can answer absent-header lookups without individual RPCs.
            snapshot += path; snapshot += '\0';
            for (const auto &name : entry.observed) { snapshot += name; snapshot += '\0'; }
            snapshot += '\0';
        }
        if (!previous_hints.owner || snapshot.size() != previous_hints.size ||
            memcmp(snapshot.data(), previous_hints.data, snapshot.size()))
            connection.save_source_hints(source, snapshot);
        // One atomic record replaces hundreds of per-directory cache accesses.
        // Concurrent jobs may change only the next cold source's prediction.
        connection.save_source_hints(scope, snapshot);
    }
    ino_t inode(uint64_t device, uint64_t number) {
        // One FUSE mount represents several client devices. Preserve hardlink
        // identity without aliasing equal inode numbers on distinct devices.
        return inodes.try_emplace({device, number}, inodes.size()+1).first->second;
    }
    int prepare(const std::set<std::string> &scheduled, Query &query, bool speculative = false) {
        const auto &path = *scheduled.begin();
        auto &entry = entries[path];
        if (hints_error) return entry.error = hints_error;
        query.path = path;
        auto predicted = predictions.find(path);
        if (speculative && learned && predicted == predictions.end()) {
            query.operation = DCC_FS_ATTRIBUTES;
            query.bytes = path;
            return query.bytes.size() <= DCC_FS_QUERY_MAX-12 ? 0 : (entry.error = ENAMETOOLONG);
        }
        auto &previous = query.previous = connection.directory_names(path);
        std::array<char, DCC_FS_METADATA> listing_version{};
        if (previous.owner) {
            if (previous.size < DCC_FS_METADATA) return entry.error = EPROTO;
            // A partial reply suffices while the compiler uses known names.
            // Once it requests an unknown child, refresh the complete index:
            // sending its old version again would only yield another partial
            // reply and force an individual RPC for each new include candidate.
            if (entry.listing != Entry::Partial)
                memcpy(listing_version.data(), previous.data, DCC_FS_METADATA);
        }
        if (predicted != predictions.end()) {
            entry.hints = std::move(predicted->second);
            predictions.erase(predicted);
        }
        query.bytes = path; query.bytes += '\0';
        query.bytes.append(listing_version.data(), DCC_FS_METADATA);
        // Reserve the outer operation and the batch entry's length/operation.
        if (query.bytes.size() > DCC_FS_QUERY_MAX-12) return entry.error = ENAMETOOLONG;
        std::string child = path == "/" ? path : path+'/';
        size_t prefix = child.size();
        for (const auto &name : entry.hints) {
            child.resize(prefix); child += name;
            // This child's own query will refresh its identity and permissions.
            // Do not ask the parent to stat it again, even across batch splits.
            if (scheduled.count(child)) continue;
            if (name.size()+1 > DCC_FS_QUERY_MAX-12-query.bytes.size()) break;
            query.bytes += name; query.bytes += '\0';
        }
        return 0;
    }
    int accept(const Query &query, const dccfs::Buffer &response) {
        const auto &path = query.path;
        auto &entry = entries[path];
        const auto &previous = query.previous;
        entry.queried = true;
        entry.error = 0;
        entry.listing = query.operation == DCC_FS_ATTRIBUTES ? Entry::Deferred : Entry::Unavailable;
        if (response.size < DCC_FS_METADATA) return entry.error = EPROTO;
        memcpy(entry.version.data(), response.data, DCC_FS_METADATA);
        if (response.size == DCC_FS_METADATA) return 0;
        if (!S_ISDIR(entry.mode()) || response.size < DCC_FS_METADATA+8) return entry.error = EPROTO;
        unsigned flags = number(response.data+DCC_FS_METADATA, 4);
        if (flags & ~(DCC_FS_DIRECTORY_CACHED | DCC_FS_DIRECTORY_STABLE | DCC_FS_DIRECTORY_PARTIAL))
            return entry.error = EPROTO;
        bool reused = flags & DCC_FS_DIRECTORY_CACHED;
        bool partial = flags & DCC_FS_DIRECTORY_PARTIAL;
        if (partial && flags != DCC_FS_DIRECTORY_PARTIAL) return entry.error = EPROTO;
        entry.names.clear();
        entry.bytes = reused ? previous : response;
        if (reused) {
            if (!previous.owner || memcmp(entry.version.data(), previous.data, DCC_FS_METADATA))
                return entry.error = EPROTO;
            std::string_view names(previous.data+DCC_FS_METADATA, previous.size-DCC_FS_METADATA);
            while (!names.empty()) {
                auto end = names.find('\0');
                if (end == std::string_view::npos || !valid_name(names.substr(0, end)))
                    return entry.error = EPROTO;
                entry.names.push_back(names.substr(0, end));
                names.remove_prefix(end+1);
            }
        }
        size_t offset = DCC_FS_METADATA+8;
        unsigned count = number(response.data+DCC_FS_METADATA+4, 4);
        std::string names;
        for (unsigned i = 0; i < count; ++i) {
            if (response.size-offset < 8) return entry.error = EPROTO;
            unsigned length = number(response.data+offset, 4), error = number(response.data+offset+4, 4);
            offset += 8;
            if (!length || length > response.size-offset) return entry.error = EPROTO;
            std::string_view name(response.data+offset, length);
            if (!valid_name(name))
                return entry.error = EPROTO;
            offset += length;
            if (error != DCC_FS_NO_METADATA) {
                auto &child = entries[(path == "/" ? path : path+'/')+std::string(name)];
                child.error = error;
                if (!error) {
                    if (response.size-offset < DCC_FS_METADATA) return entry.error = EPROTO;
                    memcpy(child.version.data(), response.data+offset, DCC_FS_METADATA);
                    offset += DCC_FS_METADATA;
                }
                child.queried = true;
            }
            if (!reused && !partial) entry.names.push_back(name);
            if (!reused && (flags & DCC_FS_DIRECTORY_STABLE)) { names += name; names += '\0'; }
        }
        if (offset != response.size) return entry.error = EPROTO;
        // Keep names as borrowed views, not an Entry and full path for every
        // unrelated sibling. Only compiler lookups and fresh attributes need
        // child state; the same index still answers negative lookups locally.
        std::sort(entry.names.begin(), entry.names.end());
        entry.listing = partial ? Entry::Partial : Entry::Ready;
        if (!reused && (flags & DCC_FS_DIRECTORY_STABLE)) {
            connection.save_directory_names(path, entry.version, names);
        }
        return 0;
    }
    int fetch(std::set<std::string> paths, bool speculative = false) {
        std::vector<Query> pending;
        std::string payload;
        auto flush = [&]() -> int {
            dccfs::Buffer response;
            if (auto error = connection.request(DCC_FS_DIRECTORIES, payload, response))
                return error.value();
            size_t offset = 0;
            for (const auto &query : pending) {
                if (response.size-offset < 8) return EPROTO;
                unsigned error = number(response.data+offset, 4), length = number(response.data+offset+4, 4);
                offset += 8;
                if (length > response.size-offset || error > INT_MAX) return EPROTO;
                if (error) {
                    auto &entry = entries[query.path];
                    entry.queried = true; entry.error = error;
                } else if (int invalid = accept(query, {response.owner, response.data+offset, length}))
                    return invalid;
                offset += length;
            }
            if (offset != response.size) return EPROTO;
            // Every reply in this batch must be applied before scheduling
            // children: some are already queried by another batch entry.
            // Only freshly observed directories from this source's hints
            // expand the next batch, never every name in a directory.
            for (const auto &query : pending) {
                const auto &entry = entries[query.path];
                if (entry.error || (entry.listing != Entry::Ready && entry.listing != Entry::Partial)) continue;
                for (const auto &name : entry.hints) {
                    auto path = query.path == "/" ? query.path+name : query.path+'/'+name;
                    auto child = entries.find(path);
                    if (child != entries.end() && child->second.listing == Entry::Deferred &&
                        predictions.count(path) &&
                        !child->second.error && S_ISDIR(child->second.mode()))
                        paths.insert(std::move(path));
                }
            }
            pending.clear(); payload.clear();
            return 0;
        };
        while (!paths.empty() || !pending.empty()) {
            if (paths.empty()) {
                if (int error = flush()) return error;
                continue;
            }
            Query query;
            if (int error = prepare(paths, query, speculative)) return error;
            if (payload.size()+8+query.bytes.size() > DCC_FS_QUERY_MAX-4)
                if (int error = flush()) return error;
            paths.erase(query.path);
            uint32_t header[] = {htonl(query.bytes.size()+4), htonl(query.operation)};
            payload.append(reinterpret_cast<const char *>(header), sizeof(header));
            payload += query.bytes;
            pending.push_back(std::move(query));
        }
        return 0;
    }
    int prefetch(const char *cwd, char *const *directories) {
        std::set<std::string> paths, roots;
        auto add = [&](const char *name) {
            auto path = (std::filesystem::path(cwd)/name).lexically_normal();
            if (path != path.root_path() && !path.has_filename()) path = path.parent_path();
            roots.insert(path.string());
            for (;;) {
                paths.insert(path.string());
                if (path == path.root_path()) break;
                path = path.parent_path();
            }
        };
        // Native GCC supplied these paths; no command-line option parsing is
        // duplicated here. Lexical spellings are only speculative cache keys:
        // the compiler still resolves symlinks and parent traversal through FUSE.
        add(cwd);
        for (auto p = directories; *p && **p; ++p) add(*p);
        // The complete prior footprint is already loaded. Submit known
        // descendants now rather than rediscovering one tree level per reply.
        // Targets outside the current roots still wait for client-authorized
        // symlink traversal; an early denial must not poison their job entry.
        for (const auto &[path, hints] : predictions) {
            for (const auto &root : roots) {
                if (path == root || (path.compare(0, root.size(), root) == 0 &&
                                    (root == "/" || path[root.size()] == '/'))) {
                    paths.insert(path);
                    break;
                }
            }
        }
        return fetch(std::move(paths), true);
    }
    int status(const std::string &path, Entry *&result) {
        auto parent = entries.end();
        size_t separator = path.find_last_of('/');
        if (path != "/") {
            parent = entries.find(separator == 0 ? "/" : path.substr(0, separator));
            auto found = entries.find(path);
            // Fetch names only when a real lookup needs them. Known children
            // in a partial reply already have fresh attributes; the first
            // unknown child upgrades that reply to a complete name index.
            if ((found == entries.end() || !found->second.queried) && parent != entries.end() &&
                !parent->second.error && S_ISDIR(parent->second.mode()) &&
                (parent->second.listing == Entry::Deferred || parent->second.listing == Entry::Partial)) {
                parent->second.hints.insert(path.substr(separator+1));
                if (int error = fetch({parent->first})) return error;
            }
        }
        auto found = entries.find(path);
        if (parent != entries.end() && parent->second.listing == Entry::Ready &&
            (found == entries.end() || !found->second.queried)) {
            parent->second.searched = true;
            std::string_view name(path.data()+separator+1, path.size()-separator-1);
            if (!std::binary_search(parent->second.names.begin(), parent->second.names.end(), name)) {
                // The immutable name index already owns this negative answer.
                // Do not allocate a second full-path Entry for every failed
                // include candidate. The kernel also retains the reply.
                result = nullptr;
                return ENOENT;
            }
        }
        auto item = entries.try_emplace(path).first;
        auto &entry = item->second;
        entry.path = &item->first;
        result = &entry;
        if (path != "/") {
            if (parent != entries.end() &&
                (parent->second.listing == Entry::Ready || parent->second.listing == Entry::Partial)) {
                parent->second.searched = true;
                // A listed parent already proves absence. Repeated failed
                // lookups must not turn that into a fresh stat on every job.
                if (!entry.queried || entry.error != ENOENT)
                    parent->second.observed.insert(path.substr(separator+1));
            }
        }
        if (entry.queried) return entry.error;
        if (int error = fetch({path})) {
            entry.queried = true;
            entry.error = error;
        }
        return entry.error;
    }
    int list(const char *path, std::vector<std::string> &names) {
        dccfs::Buffer reply;
        if (auto error = connection.request(DCC_FS_LIST, path, reply)) return error.value();
        size_t offset = 0;
        while (offset < reply.size) {
            if (reply.size-offset < 8) return EPROTO;
            unsigned length = number(reply.data+offset, 4);
            offset += 8; // LIST's type is not needed by FUSE readdir.
            if (length > reply.size-offset || !valid_name({reply.data+offset, length})) return EPROTO;
            names.emplace_back(reply.data+offset, length);
            offset += length;
        }
        return 0;
    }
    int open(const char *path, Entry *&entry) {
        if (int error = status(path, entry)) return error;
        if (!S_ISREG(entry->mode())) return EINVAL;
        if (!entry->bytes.owner) {
            dccfs::Snapshot snapshot;
            if (auto error = connection.read(path, entry->version.data(), snapshot)) return error.value();
            entry->version = snapshot.version;
            entry->bytes = std::move(snapshot.bytes);
        }
        return 0;
    }
    int link(const char *path, char *buffer, size_t size) {
        auto &target = entries[path].link_target;
        if (!target.owner)
            if (auto error = connection.request(DCC_FS_READLINK, path, target)) return -error.value();
        size_t prefix = target.size && target.data[0] == '/' ? root.size() : 0;
        if (size <= prefix || target.size >= size - prefix) return -ENAMETOOLONG;
        if (prefix) memcpy(buffer, root.data(), prefix);
        memcpy(buffer + prefix, target.data, target.size);
        buffer[prefix + target.size] = 0;
        return 0;
    }
};
struct Mount {
    std::unique_ptr<Files> files;
    struct fuse_session *fs = nullptr;
    std::string mountpoint;
    pid_t child = -1;
};
Files &files(fuse_req_t request) {
    return *static_cast<Mount *>(fuse_req_userdata(request))->files;
}
Entry &node(Files &fs, fuse_ino_t inode) {
    if (inode != FUSE_ROOT_ID) return *reinterpret_cast<Entry *>(inode);
    Entry *entry;
    fs.status("/", entry);
    return *entry;
}
void initialize(void *, struct fuse_conn_info *connection) {
    connection->want |= connection->capable & FUSE_CAP_CACHE_SYMLINKS;
}
void attributes(Files &fs, const Entry &entry, struct stat &st) {
    st = {};
    const char *p = entry.version.data();
    st.st_ino = fs.inode(number(p, 8), number(p+8, 8));
    st.st_mode = number(p+32, 4); st.st_size = number(p+24, 8);
    st.st_nlink = S_ISDIR(st.st_mode) ? 2 : 1;
    // Client RPC enforces original permissions. The private namespace maps
    // this job's sole compiler user/group to zero; no credential syscall is
    // needed for each lookup. Preserve the existing ownership presentation.
    st.st_uid = 0; st.st_gid = 0;
    int64_t mtime = number(p+16, 8), ctime = number(p+44, 8);
    st.st_mtim = {mtime/1000000000, mtime%1000000000};
    st.st_ctim = {ctime/1000000000, ctime%1000000000};
}
void lookup(fuse_req_t request, fuse_ino_t parent, const char *name) {
    auto &fs = files(request);
    auto &directory = node(fs, parent);
    if (directory.error) { fuse_reply_err(request, directory.error); return; }
    std::string path = *directory.path;
    if (path != "/") path += '/';
    path += name;
    Entry *entry;
    int error = fs.status(path, entry);
    if (error && error != ENOENT) { fuse_reply_err(request, error); return; }
    struct fuse_entry_param reply{};
    // Positive and negative names have the same job lifetime as Files.
    reply.entry_timeout = std::numeric_limits<double>::max();
    reply.attr_timeout = 1; // Preserve libfuse's previous attribute lifetime.
    if (!error) {
        reply.ino = reinterpret_cast<fuse_ino_t>(entry);
        attributes(fs, *entry, reply.attr);
    }
    fuse_reply_entry(request, &reply);
}
void forget(fuse_req_t request, fuse_ino_t, uint64_t) {
    // The compiler may forget a dentry and ask again. Files owns the snapshot
    // until job completion, so no second inode reference table is necessary.
    fuse_reply_none(request);
}
void get_attributes(fuse_req_t request, fuse_ino_t inode, struct fuse_file_info *) {
    auto &fs = files(request);
    auto &entry = node(fs, inode);
    if (entry.error) { fuse_reply_err(request, entry.error); return; }
    struct stat st;
    attributes(fs, entry, st);
    fuse_reply_attr(request, &st, 1);
}
void open_file(fuse_req_t request, fuse_ino_t inode, struct fuse_file_info *info) {
    if ((info->flags & O_ACCMODE) != O_RDONLY) { fuse_reply_err(request, EROFS); return; }
    auto &fs = files(request);
    Entry *entry = &node(fs, inode);
    if (int error = fs.open(entry->path->c_str(), entry)) { fuse_reply_err(request, error); return; }
    info->keep_cache = 1;
    fuse_reply_open(request, info);
}
void read_file(fuse_req_t request, fuse_ino_t inode, size_t size, off_t offset,
               struct fuse_file_info *) {
    auto &bytes = node(files(request), inode).bytes;
    if (offset < 0) { fuse_reply_err(request, EINVAL); return; }
    if (static_cast<uint64_t>(offset) >= bytes.size) { fuse_reply_buf(request, nullptr, 0); return; }
    size = std::min(size, bytes.size-static_cast<size_t>(offset));
    // The immutable snapshot already owns the reply body. Low-level FUSE
    // writes it directly, without a second allocation and memcpy per read.
    fuse_reply_buf(request, bytes.data+offset, size);
}
void read_link(fuse_req_t request, fuse_ino_t inode) {
    auto &fs = files(request);
    char target[PATH_MAX];
    if (int error = fs.link(node(fs, inode).path->c_str(), target, sizeof(target))) {
        fuse_reply_err(request, -error); return;
    }
    fuse_reply_readlink(request, target);
}
void open_directory(fuse_req_t request, fuse_ino_t inode, struct fuse_file_info *info) {
    auto &fs = files(request);
    auto names = std::make_unique<std::vector<std::string>>();
    if (int error = fs.list(node(fs, inode).path->c_str(), *names)) {
        fuse_reply_err(request, error); return;
    }
    info->fh = reinterpret_cast<uintptr_t>(names.release());
    fuse_reply_open(request, info);
}
void read_directory(fuse_req_t request, fuse_ino_t, size_t size, off_t offset,
                    struct fuse_file_info *info) {
    if (offset < 0) { fuse_reply_err(request, EINVAL); return; }
    auto &names = *reinterpret_cast<std::vector<std::string> *>(info->fh);
    std::vector<char> buffer(size);
    size_t used = 0;
    struct stat st{};
    for (size_t i = offset; i < names.size(); ++i) {
        size_t length = fuse_add_direntry(request, buffer.data()+used, size-used,
                                          names[i].c_str(), &st, i+1);
        if (length > size-used) break;
        used += length;
    }
    fuse_reply_buf(request, buffer.data(), used);
}
void close_directory(fuse_req_t request, fuse_ino_t, struct fuse_file_info *info) {
    delete reinterpret_cast<std::vector<std::string> *>(info->fh);
    fuse_reply_err(request, 0);
}
bool mapping(const char *path, const std::string &value) {
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    int error = errno;
    close(fd); errno = error;
    return ok;
}
}
extern "C" int dcc_gcc_files_stdin(int channel, const char *job_root) {
    int connection_fd = dup(channel);
    if (connection_fd < 0) return -1;
    dccfs::Connection connection(connection_fd, dcc_file_cache_records, "", job_root);
    dccfs::Buffer input;
    auto error = connection.request(DCC_FS_STDIN, "/", input);
    if (error) { errno = error.value(); return -1; }
    if (input.size < DCC_FS_METADATA ||
        dccfs::number(input.data+24, 8) != input.size-DCC_FS_METADATA) {
        errno = EPROTO; return -1;
    }
    int fd = memfd_create("distcc-stdin", MFD_CLOEXEC);
    if (fd < 0) return -1;
    size_t offset = DCC_FS_METADATA;
    while (offset < input.size) {
        ssize_t written = write(fd, input.data+offset, input.size-offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { int error = errno; close(fd); errno = error; return -1; }
        offset += written;
    }
    return fd;
}

extern "C" void *dcc_gcc_files_start(int channel, const char *mountpoint,
                                      const char *identity, const char *job_root,
                                      const char *cwd, const char *input, char *const *directories) {
    uid_t uid = getuid(); gid_t gid = getgid();
    if (mkdir(mountpoint, 0700) || unshare(CLONE_NEWUSER | CLONE_NEWNS) ||
        !mapping("/proc/self/setgroups", "deny") ||
        !mapping("/proc/self/uid_map", "0 "+std::to_string(uid)+" 1\n") ||
        !mapping("/proc/self/gid_map", "0 "+std::to_string(gid)+" 1\n") ||
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)) return nullptr;
    auto state = std::make_unique<Mount>();
    state->mountpoint = mountpoint;
    struct fuse_lowlevel_ops operations{};
    operations.init = initialize;
    operations.lookup = lookup;
    operations.forget = forget;
    operations.getattr = get_attributes;
    operations.open = open_file;
    operations.read = read_file;
    operations.readlink = read_link;
    operations.opendir = open_directory;
    operations.readdir = read_directory;
    operations.releasedir = close_directory;
    char name[] = "distcc-files", options[] = "-oro";
    char *argv[] = {name, options};
    struct fuse_args args = FUSE_ARGS_INIT(2, argv);
    state->fs = fuse_session_new(&args, &operations, sizeof(operations), state.get());
    fuse_opt_free_args(&args);
    if (!state->fs) { errno = EINVAL; return nullptr; }
    if (fuse_session_mount(state->fs, mountpoint)) { fuse_session_destroy(state->fs); return nullptr; }
    // Shutdown may arrive before the child installs libfuse's handler. Keep it
    // pending across fork, without changing the worker's own signal policy.
    sigset_t stop_signal, previous_mask;
    sigemptyset(&stop_signal); sigaddset(&stop_signal, SIGTERM);
    sigprocmask(SIG_BLOCK, &stop_signal, &previous_mask);
    state->child = fork();
    if (state->child) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    if (state->child < 0) {
        fuse_session_unmount(state->fs); fuse_session_destroy(state->fs); return nullptr;
    }
    if (!state->child) {
        auto session = state->fs;
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, nullptr) || fuse_set_signal_handlers(session)) {
            perror("distcc GCC filesystem signal handlers"); _exit(1);
        }
        sigdelset(&previous_mask, SIGTERM);
        sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
        // This service must not keep the worker's cancellation/result sockets
        // alive. It owns only the FUSE device and its filesystem RPC stream.
        int device = fuse_session_fd(state->fs);
        int first = std::min(device, channel), last = std::max(device, channel);
        if (first > 3) close_range(3, first-1, 0);
        if (last > first+1) close_range(first+1, last-1, 0);
        close_range(last+1, ~0U, 0);
        // TMPDIR is already the job root here. Like the Clang frontend, keep
        // immutable snapshots in its parent so job cleanup cannot remove them.
        auto cache_root = std::filesystem::path(job_root).parent_path().string();
        state->files = std::make_unique<Files>(channel, mountpoint, identity, cache_root.c_str(), cwd, input);
        int result = state->files->prefetch(cwd, directories);
        if (result) {
            fprintf(stderr, "distcc GCC directory prefetch: %s\n", strerror(result));
            fuse_session_unmount(state->fs);
        } else {
            result = fuse_session_loop(state->fs);
            if (result == SIGTERM) result = 0;
        }
        state->files.reset();
        fuse_remove_signal_handlers(session);
        fuse_session_destroy(state->fs);
        _exit(result ? 1 : 0);
    }
    // Destroying the parent's handle closes its device fd without unmounting.
    // The service is the sole owner: its death must abort pending kernel reads.
    fuse_session_destroy(state->fs);
    state->fs = nullptr;
    return state.release();
}
extern "C" int dcc_gcc_files_stop(void *opaque, uint64_t *cpu_ns, unsigned long long *peak_bytes) {
    std::unique_ptr<Mount> state(static_cast<Mount *>(opaque));
    int error = 0;
    // Stop the reader before detaching. Linux can return ECONNABORTED when a
    // regular unmount races its transition from pending to in-flight reads.
    if (kill(state->child, SIGTERM) && errno != ESRCH) {
        error = errno;
        fprintf(stderr, "distcc GCC filesystem stop: %s\n", strerror(error));
    }
    int status;
    struct rusage usage{};
    pid_t result;
    do { result = wait4(state->child, &status, 0, &usage); } while (result < 0 && errno == EINTR);
    if (umount2(state->mountpoint.c_str(), MNT_DETACH) && errno != EINVAL && errno != ENOENT) {
        error = errno;
        fprintf(stderr, "distcc GCC filesystem unmount: %s\n", strerror(error));
    }
    if (result >= 0) {
        *cpu_ns += (uint64_t)(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000000 +
                   (uint64_t)(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1000;
        *peak_bytes += (unsigned long long)usage.ru_maxrss*1024;
    }
    return !error && result >= 0 && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
