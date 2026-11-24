/* Let the native GCC frontend read client files through the existing RPC.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#define FUSE_USE_VERSION 312
#include <fuse3/fuse_lowlevel.h>
#include <limits.h>
#include <fcntl.h>
#include <sched.h>
#include <thread>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <map>
#include <filesystem>
#include <set>
#include <vector>
#include <algorithm>
#include <limits>
extern "C" {
#include "file_cache.h"
#include "trace.h"
}
#include "remote_files.h"
#include "gcc_files.h"

namespace {
using dccfs::number;
using dccfs::valid_name;
static int run_session(fuse_session *session) {
    // A signal can arrive after the single-thread loop's exit check but before
    // its blocking read. The MT loop wakes via a semaphore and cancels/joins
    // its reader, including in that window. Keep classic callbacks serial.
    auto *config = fuse_loop_cfg_create();
    if (!config) return -ENOMEM;
    fuse_loop_cfg_set_max_threads(config, 1);
    fuse_loop_cfg_set_clone_fd(config, 0);
    int result = fuse_session_loop_mt(session, config);
    fuse_loop_cfg_destroy(config);
    return result;
}
struct Entry : dccfs::DirectoryListing {
    // The map owns the spelling; entries and keys live until this job ends.
    const std::string *path = nullptr;
    int error = 0;
    std::array<char, DCC_FS_METADATA> version{};
    bool queried = false, searched = false;
    std::set<std::string> hints, observed;
    // A job owns one filesystem snapshot. Repeated FUSE readlink callbacks
    // must not round-trip to the client and reauthorize the same hop.
    dccfs::Buffer link_target;
    unsigned mode() const { return number(version.data()+32, 4); }
};
using Query = dccfs::DirectoryQuery;
static dccfs::SourceHints source_hints(dccfs::Connection &connection, const char *cwd, const char *input) {
    auto source = (std::filesystem::path(cwd)/input).lexically_normal();
    return {connection, source.string(), source.parent_path().string()};
}
class Files {
    dccfs::Connection connection;
    dccfs::SourceHints hints;
    dccfs::SourceHints::Directories predictions;
    int hints_error = 0;
    std::map<std::string, Entry> entries;
    std::map<std::pair<uint64_t, uint64_t>, ino_t> inodes;
public:
    std::string root;
    Files(int fd, std::string root, const char *identity, const char *cache_root,
          const char *cwd, const char *input)
        : connection(fd, dcc_file_cache_records, identity, cache_root),
          hints(source_hints(connection, cwd, input)), root(std::move(root)) {
        hints_error = hints.load(predictions);
    }
    ~Files() {
        if (hints_error) return;
        dccfs::SourceHints::Directories actual;
        for (auto &[path, entry] : entries) {
            if (!entry.searched) continue;
            // Speculative queries do not teach the next job to repeat them.
            // Retain only children the compiler actually requested this time.
            // An empty footprint still records a searched directory: its fresh
            // names can answer absent-header lookups without individual RPCs.
            actual.emplace(path, std::move(entry.observed));
        }
        hints.save_source(connection, actual);
        hints.merge_scope(connection, actual, [&](const auto &dir, auto &names) {
            return dccfs::prune_missing(dir, names, [&](const std::string &path) -> dccfs::HintStatus {
                auto found = entries.find(path);
                if (found == entries.end() || !found->second.queried) return {};
                auto &entry = found->second;
                return {entry.mode(), entry.error, &entry};
            });
        });
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
        if (speculative && hints.learned() && predicted == predictions.end()) {
            query.operation = DCC_FS_ATTRIBUTES;
            query.bytes = path;
            return query.bytes.size() <= DCC_FS_QUERY_MAX-12 ? 0 : (entry.error = ENAMETOOLONG);
        }
        if (predicted != predictions.end()) {
            entry.hints = std::move(predicted->second);
            predictions.erase(predicted);
        }
        // An unknown child upgrades a partial footprint to a complete name index.
        if (entry.listing != Entry::Partial) query.previous = connection.directory_names(path);
        return entry.error = query.encode(entry.hints, scheduled);
    }
    int accept(const Query &query, const dccfs::Buffer &response) {
        auto &entry = entries[query.path];
        entry.queried = true; entry.error = 0;
        entry.listing = query.operation == DCC_FS_ATTRIBUTES ? Entry::Deferred : Entry::Unavailable;
        if (response.size < DCC_FS_METADATA) return entry.error = EPROTO;
        memcpy(entry.version.data(), response.data, DCC_FS_METADATA);
        if (response.size == DCC_FS_METADATA) return 0;
        const std::string prefix = query.path == "/" ? query.path : query.path+'/';
        return entry.error = entry.decode(connection, query, response,
            [&](std::string_view name, unsigned error, const char *metadata) {
                auto &child = entries[prefix + std::string(name)];
                child.error = error; child.queried = true;
                if (!error) memcpy(child.version.data(), metadata, DCC_FS_METADATA);
            });
    }
    int fetch(std::set<std::string> paths, bool speculative = false) {
        while (!paths.empty()) {
            std::vector<Query> pending;
            while (!paths.empty()) {
                auto &query = pending.emplace_back();
                if (int error = prepare(paths, query, speculative)) return error;
                paths.erase(query.path);
            }
            auto error = connection.batch(DCC_FS_DIRECTORIES, pending, [&](const Query &q, std::error_code ec,
                                                                           const dccfs::Buffer &reply) {
                if (!ec) return std::error_code(accept(q, reply), std::generic_category());
                auto &entry = entries[q.path];
                entry.queried = true; entry.error = ec.value();
                return std::error_code{};
            });
            if (error) return error.value();
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
        }
        return 0;
    }
    int prefetch(const char *cwd, char *const *directories) {
        std::set<std::string> paths, roots;
        auto add = [&](const char *name) {
            std::filesystem::path path = "/";
            paths.insert(path.string());
            for (const auto &part : (std::filesystem::path(cwd)/name).relative_path()) {
                if (part.empty() || part == ".") continue;
                if (part == "..") path = path.parent_path();
                else path /= part;
                paths.insert(path.string());
            }
            roots.insert(path.string());
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
            if (parent->second.excludes(name)) {
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
    std::thread service;
    sem_t serial;
    unsigned long uring = 0, classic = 0;
    int status = 0;
    Mount() {
        if (sem_init(&serial, 0, 1)) throw std::system_error(errno, std::generic_category());
    }
    ~Mount() { sem_destroy(&serial); }
};
// sem_wait is cancellable; pthread_mutex_lock is not. libfuse cancels and
// joins queue threads in order, including waiters behind a blocked RPC owner.
// The guard releases ownership on C++ cancellation unwind; later callbacks
// observe the session exit before touching the single Files/RPC owner.
struct Callback {
    Mount &mount;
    explicit Callback(fuse_req_t request) : mount(*static_cast<Mount *>(fuse_req_userdata(request))) {
        while (sem_wait(&mount.serial)) if (errno != EINTR) _exit(120);
        if (fuse_req_is_uring(request)) ++mount.uring; else ++mount.classic;
    }
    ~Callback() { sem_post(&mount.serial); }
    bool stopped() const { return fuse_session_exited(mount.fs); }
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
    // Absolute targets gain the private mount prefix in read_link(), while
    // lookup still returns client metadata. Kernels using page_get_link()
    // truncate cached symlinks to that original st_size. Files already pins
    // each link's reply for the job; leave kernel symlink caching disabled.
    connection->want &= ~FUSE_CAP_CACHE_SYMLINKS;
    connection->want |= connection->capable & FUSE_CAP_OVER_IO_URING;
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
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
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
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
    auto &fs = files(request);
    auto &entry = node(fs, inode);
    if (entry.error) { fuse_reply_err(request, entry.error); return; }
    struct stat st;
    attributes(fs, entry, st);
    fuse_reply_attr(request, &st, 1);
}
void open_file(fuse_req_t request, fuse_ino_t inode, struct fuse_file_info *info) {
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
    if ((info->flags & O_ACCMODE) != O_RDONLY) { fuse_reply_err(request, EROFS); return; }
    auto &fs = files(request);
    Entry *entry = &node(fs, inode);
    const auto previous = entry->version;
    if (int error = fs.open(entry->path->c_str(), entry)) { fuse_reply_err(request, error); return; }
    if (previous != entry->version) {
        // The client may replace/grow an input between LOOKUP and the first
        // authorized READ. Refresh kernel attributes before it bounds reads
        // or mmap by the old size; bytes remain demand-paged from the snapshot.
        auto session = static_cast<Mount *>(fuse_req_userdata(request))->fs;
        if (int error = fuse_lowlevel_notify_inval_inode(session, inode, -1, 0)) {
            fuse_reply_err(request, -error); return;
        }
    }
    info->keep_cache = 1;
    fuse_reply_open(request, info);
}
void read_file(fuse_req_t request, fuse_ino_t inode, size_t size, off_t offset,
               struct fuse_file_info *) {
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
    auto &bytes = node(files(request), inode).bytes;
    if (offset < 0) { fuse_reply_err(request, EINVAL); return; }
    if (static_cast<uint64_t>(offset) >= bytes.size) { fuse_reply_buf(request, nullptr, 0); return; }
    size = std::min(size, bytes.size-static_cast<size_t>(offset));
    // The immutable snapshot already owns the reply body. Low-level FUSE
    // writes it directly, without a second allocation and memcpy per read.
    fuse_reply_buf(request, bytes.data+offset, size);
}
void read_link(fuse_req_t request, fuse_ino_t inode) {
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
    auto &fs = files(request);
    char target[PATH_MAX];
    if (int error = fs.link(node(fs, inode).path->c_str(), target, sizeof(target))) {
        fuse_reply_err(request, -error); return;
    }
    fuse_reply_readlink(request, target);
}
void open_directory(fuse_req_t request, fuse_ino_t inode, struct fuse_file_info *info) {
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
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
    Callback guard(request);
    if (guard.stopped()) { fuse_reply_err(request, EINTR); return; }
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
    Callback guard(request);
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
    char name[] = "distcc-files", options[] = "-oro,io_uring,io_uring_q_depth=1";
    char *argv[] = {name, options};
    struct fuse_args args = FUSE_ARGS_INIT(2, argv);
    state->fs = fuse_session_new(&args, &operations, sizeof(operations), state.get());
    fuse_opt_free_args(&args);
    if (!state->fs) { errno = EINVAL; return nullptr; }
    if (fuse_session_mount(state->fs, mountpoint)) { fuse_session_destroy(state->fs); return nullptr; }
    int error = 0;
    int rpc = fcntl(channel, F_DUPFD_CLOEXEC, 3);
    if (rpc < 0) error = errno;
    else {
        auto cache_root = std::filesystem::path(job_root).parent_path().string();
        state->files = std::make_unique<Files>(rpc, mountpoint, identity, cache_root.c_str(), cwd, input);
        if (fcntl(fuse_session_fd(state->fs), F_SETFD, FD_CLOEXEC)) error = errno;
        else error = state->files->prefetch(cwd, directories);
    }
    if (!error) {
        try {
            state->service = std::thread([mount = state.get()] {
                mount->status = run_session(mount->fs);
                // Release any pending kernel requests if the loop failed.
                if (mount->status) umount2(mount->mountpoint.c_str(), MNT_DETACH);
            });
            return state.release();
        } catch (const std::system_error &e) { error = e.code().value(); }
    }
    fuse_session_unmount(state->fs);
    fuse_session_destroy(state->fs);
    errno = error;
    return nullptr;
}
extern "C" int dcc_gcc_files_stop(void *opaque) {
    std::unique_ptr<Mount> state(static_cast<Mount *>(opaque));
    fuse_session_exit(state->fs);
    int error = umount2(state->mountpoint.c_str(), MNT_DETACH) && errno != EINVAL && errno != ENOENT;
    if (error) perror("distcc GCC filesystem unmount");
    state->service.join();
    fuse_session_destroy(state->fs);
    char traffic[192];
    snprintf(traffic, sizeof(traffic), " DCC_URING done job_pid=%ld uring=%lu classic=%lu status=%d transport=%s",
             (long)getpid(), state->uring, state->classic, state->status,
             state->uring ? "uring" : state->classic ? "classic" : "unused");
    dcc_job_summary_append(traffic);
    return error || state->status ? -1 : 0;
}
