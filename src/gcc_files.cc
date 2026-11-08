/* Let the native GCC frontend read client files through the existing RPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <map>
#include <filesystem>
#include <set>
#include <vector>
#include <algorithm>
extern "C" {
#include "file_cache.h"
}
#include "remote_files.h"
#include "gcc_files.h"

namespace {
using dccfs::number;
struct Entry {
    int error = 0;
    std::array<char, DCC_FS_METADATA> version{};
    std::array<char, DCC_FS_METADATA> listing_version{};
    bool queried = false, listed = false;
    bool cache_changed = false;
    std::vector<std::string> children;
    std::set<std::string> hints;
    dccfs::Buffer bytes;
    unsigned mode() const { return number(version.data()+32, 4); }
};
class Files {
    dccfs::Connection connection;
    std::map<std::string, Entry> entries;
    std::map<std::pair<uint64_t, uint64_t>, ino_t> inodes;
    static bool valid_name(std::string_view name) {
        return !name.empty() && name != "." && name != ".." &&
            name.find_first_of("/\0", 0, 2) == std::string_view::npos;
    }
    static std::string pack_hints(const Entry &entry, size_t limit) {
        std::string result;
        for (const auto &name : entry.hints) {
            if (name.size()+1 > limit-result.size()) break;
            result += name; result += '\0';
        }
        return result;
    }
public:
    std::string root;
    Files(int fd, std::string root, const char *identity, const char *cache_root)
        : connection(fd, dcc_file_cache_records, identity, cache_root), root(std::move(root)) {}
    ~Files() {
        for (const auto &[path, entry] : entries) {
            if (!entry.listed || !entry.cache_changed) continue;
            std::string names;
            if (S_ISDIR(number(entry.listing_version.data()+32, 4)))
                for (const auto &name : entry.children) { names += name; names += '\0'; }
            connection.save_directory_state(path, entry.listing_version, names,
                pack_hints(entry, DCC_FS_QUERY_MAX-5-DCC_FS_METADATA-path.size()));
        }
    }
    ino_t inode(uint64_t device, uint64_t number) {
        // One FUSE mount represents several client devices. Preserve hardlink
        // identity without aliasing equal inode numbers on distinct devices.
        return inodes.try_emplace({device, number}, inodes.size()+1).first->second;
    }
    int status(const std::string &path, Entry *&result) {
        auto [found, inserted] = entries.try_emplace(path);
        auto &entry = found->second;
        result = &entry;
        if (path != "/") {
            auto separator = path.find_last_of('/');
            auto parent = entries.find(separator == 0 ? "/" : path.substr(0, separator));
            if (parent != entries.end() && parent->second.listed) {
                if (inserted) {
                    entry.queried = true;
                    return entry.error = ENOENT;
                }
                if (parent->second.hints.insert(path.substr(separator+1)).second)
                    parent->second.cache_changed = true;
            }
        }
        if (entry.queried) return entry.error;
        auto previous = connection.directory_state(path);
        std::string_view names, hints;
        if (previous.owner) {
            if (previous.size < DCC_FS_METADATA+4) return entry.error = EPROTO;
            size_t length = number(previous.data+DCC_FS_METADATA, 4);
            if (length > previous.size-DCC_FS_METADATA-4) return entry.error = EPROTO;
            memcpy(entry.listing_version.data(), previous.data, DCC_FS_METADATA);
            names = {previous.data+DCC_FS_METADATA+4, length};
            hints = {names.data()+names.size(), previous.size-DCC_FS_METADATA-4-length};
        }
        while (!hints.empty()) {
            auto end = hints.find('\0');
            if (end == std::string_view::npos || !valid_name(hints.substr(0, end)))
                return entry.error = EPROTO;
            entry.hints.emplace(hints.substr(0, end));
            hints.remove_prefix(end+1);
        }
        std::string query = path; query += '\0';
        query.append(entry.listing_version.data(), DCC_FS_METADATA);
        if (query.size() > DCC_FS_QUERY_MAX-4) return entry.error = ENAMETOOLONG;
        query += pack_hints(entry, DCC_FS_QUERY_MAX-4-query.size());
        dccfs::Buffer response;
        auto error = connection.request(DCC_FS_LSTAT, query, response);
        entry.queried = true;
        if (error) return entry.error = error.value();
        if (response.size < DCC_FS_METADATA) return entry.error = EPROTO;
        memcpy(entry.version.data(), response.data, DCC_FS_METADATA);
        if (response.size == DCC_FS_METADATA) return 0;
        if (!S_ISDIR(entry.mode()) || response.size < DCC_FS_METADATA+8) return entry.error = EPROTO;
        unsigned flags = number(response.data+DCC_FS_METADATA, 4);
        if (flags & ~(DCC_FS_DIRECTORY_CACHED | DCC_FS_DIRECTORY_STABLE)) return entry.error = EPROTO;
        bool reused = flags & DCC_FS_DIRECTORY_CACHED;
        if (reused) {
            if (!previous.owner || entry.version != entry.listing_version) return entry.error = EPROTO;
            while (!names.empty()) {
                auto end = names.find('\0');
                if (end == std::string_view::npos || !valid_name(names.substr(0, end)))
                    return entry.error = EPROTO;
                std::string name(names.substr(0, end));
                entries.try_emplace(path == "/" ? path+name : path+'/'+name);
                entry.children.push_back(std::move(name));
                names.remove_prefix(end+1);
            }
        }
        if (!(flags & DCC_FS_DIRECTORY_STABLE)) entry.listing_version = {};
        else entry.listing_version = entry.version;
        entry.cache_changed = !reused || !(flags & DCC_FS_DIRECTORY_STABLE);
        size_t offset = DCC_FS_METADATA+8;
        unsigned count = number(response.data+DCC_FS_METADATA+4, 4);
        std::set<std::string> retained;
        for (unsigned i = 0; i < count; ++i) {
            if (response.size-offset < 8) return entry.error = EPROTO;
            unsigned length = number(response.data+offset, 4), error = number(response.data+offset+4, 4);
            offset += 8;
            if (!length || length > response.size-offset) return entry.error = EPROTO;
            std::string name(response.data+offset, length);
            if (!valid_name(name))
                return entry.error = EPROTO;
            offset += length;
            auto &child = entries[path == "/" ? path+name : path+'/'+name];
            if (error != DCC_FS_NO_METADATA) {
                child.error = error;
                if (!error) {
                    if (response.size-offset < DCC_FS_METADATA) return entry.error = EPROTO;
                    memcpy(child.version.data(), response.data+offset, DCC_FS_METADATA);
                    offset += DCC_FS_METADATA;
                }
                child.queried = error || !S_ISDIR(child.mode());
            }
            if (entry.hints.count(name)) retained.insert(name);
            if (!reused) entry.children.push_back(std::move(name));
        }
        if (offset != response.size) return entry.error = EPROTO;
        if (entry.hints.size() != retained.size()) entry.cache_changed = true;
        entry.hints = std::move(retained);
        entry.listed = true;
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
        dccfs::Buffer target;
        if (auto error = connection.request(DCC_FS_READLINK, path, target)) return -error.value();
        std::string name(target.data, target.size);
        if (!name.empty() && name[0] == '/') name.insert(0, root);
        if (!size || name.size() >= size) return -ENAMETOOLONG;
        memcpy(buffer, name.c_str(), name.size()+1);
        return 0;
    }
};
struct Mount {
    std::unique_ptr<Files> files;
    struct fuse *fs = nullptr;
    pid_t child = -1;
};
Files &files() { return *static_cast<Mount *>(fuse_get_context()->private_data)->files; }
void *initialize(struct fuse_conn_info *, struct fuse_config *config) {
    config->kernel_cache = 1;
    config->use_ino = 1;
    return fuse_get_context()->private_data;
}
int attributes(const char *path, struct stat *st, struct fuse_file_info *) {
    Entry *entry;
    if (int error = files().status(path, entry)) return -error;
    memset(st, 0, sizeof(*st));
    const char *p = entry->version.data();
    st->st_ino = files().inode(number(p, 8), number(p+8, 8));
    st->st_mode = number(p+32, 4); st->st_size = number(p+24, 8);
    st->st_nlink = S_ISDIR(st->st_mode) ? 2 : 1;
    // Client operations enforce the original caller's permissions. Kernel
    // access inside this private namespace belongs to its compiler process.
    st->st_uid = getuid(); st->st_gid = getgid();
    int64_t mtime = number(p+16, 8), ctime = number(p+44, 8);
    st->st_mtim = {mtime/1000000000, mtime%1000000000};
    st->st_ctim = {ctime/1000000000, ctime%1000000000};
    return 0;
}
int open_file(const char *path, struct fuse_file_info *info) {
    if ((info->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    Entry *entry;
    if (int error = files().open(path, entry)) return -error;
    info->fh = reinterpret_cast<uintptr_t>(entry);
    info->keep_cache = 1;
    return 0;
}
int read_file(const char *, char *buffer, size_t size, off_t offset, struct fuse_file_info *info) {
    auto &bytes = reinterpret_cast<Entry *>(info->fh)->bytes;
    if (offset < 0) return -EINVAL;
    if (static_cast<uint64_t>(offset) >= bytes.size) return 0;
    size = std::min(size, bytes.size-static_cast<size_t>(offset));
    memcpy(buffer, bytes.data+offset, size);
    return size;
}
int read_link(const char *path, char *buffer, size_t size) { return files().link(path, buffer, size); }
int read_directory(const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset,
                   struct fuse_file_info *, enum fuse_readdir_flags) {
    Entry *entry;
    if (int error = files().status(path, entry)) return -error;
    if (!S_ISDIR(entry->mode())) return -ENOTDIR;
    if (!entry->listed) return -EACCES;
    if (offset < 0) return -EINVAL;
    for (size_t i = offset; i < entry->children.size()+2; ++i) {
        const char *name = i == 0 ? "." : i == 1 ? ".." : entry->children[i-2].c_str();
        if (fill(buffer, name, nullptr, i+1, static_cast<fuse_fill_dir_flags>(0))) break;
    }
    return 0;
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
extern "C" void *dcc_gcc_files_start(int channel, const char *mountpoint,
                                      const char *identity, const char *job_root) {
    uid_t uid = getuid(); gid_t gid = getgid();
    if (mkdir(mountpoint, 0700) || unshare(CLONE_NEWUSER | CLONE_NEWNS) ||
        !mapping("/proc/self/setgroups", "deny") ||
        !mapping("/proc/self/uid_map", "0 "+std::to_string(uid)+" 1\n") ||
        !mapping("/proc/self/gid_map", "0 "+std::to_string(gid)+" 1\n") ||
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)) return nullptr;
    auto state = std::make_unique<Mount>();
    struct fuse_operations operations{};
    operations.init = initialize;
    operations.getattr = attributes;
    operations.open = open_file;
    operations.read = read_file;
    operations.readlink = read_link;
    operations.readdir = read_directory;
    char name[] = "distcc-files", options[] = "-oro";
    char *argv[] = {name, options};
    struct fuse_args args = FUSE_ARGS_INIT(2, argv);
    state->fs = fuse_new(&args, &operations, sizeof(operations), state.get());
    fuse_opt_free_args(&args);
    if (!state->fs) { errno = EINVAL; return nullptr; }
    if (fuse_mount(state->fs, mountpoint)) { fuse_destroy(state->fs); return nullptr; }
    state->child = fork();
    if (state->child < 0) {
        fuse_unmount(state->fs); fuse_destroy(state->fs); return nullptr;
    }
    if (!state->child) {
        // This service must not keep the worker's cancellation/result sockets
        // alive. It owns only the FUSE device and its filesystem RPC stream.
        int device = fuse_session_fd(fuse_get_session(state->fs));
        int first = std::min(device, channel), last = std::max(device, channel);
        if (first > 3) close_range(3, first-1, 0);
        if (last > first+1) close_range(first+1, last-1, 0);
        close_range(last+1, ~0U, 0);
        // TMPDIR is already the job root here. Like the Clang frontend, keep
        // immutable snapshots in its parent so job cleanup cannot remove them.
        auto cache_root = std::filesystem::path(job_root).parent_path().string();
        state->files = std::make_unique<Files>(channel, mountpoint, identity, cache_root.c_str());
        int result = fuse_loop(state->fs);
        state->files.reset();
        fuse_destroy(state->fs);
        _exit(result ? 1 : 0);
    }
    return state.release();
}
extern "C" int dcc_gcc_files_stop(void *opaque, uint64_t *cpu_ns, unsigned long long *peak_bytes) {
    std::unique_ptr<Mount> state(static_cast<Mount *>(opaque));
    fuse_unmount(state->fs);
    int status;
    struct rusage usage{};
    pid_t result;
    do { result = wait4(state->child, &status, 0, &usage); } while (result < 0 && errno == EINTR);
    if (result >= 0) {
        *cpu_ns += (uint64_t)(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000000 +
                   (uint64_t)(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1000;
        *peak_bytes += (unsigned long long)usage.ru_maxrss*1024;
    }
    fuse_destroy(state->fs);
    return result >= 0 && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
