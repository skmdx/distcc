/* Narrow Files/Connection FD-export canary. Not a production service.
 * SPDX-License-Identifier: GPL-2.0-or-later */
extern "C" {
#include <config.h>
#include "distcc.h"
#include "tcp_job.h"
#include "frontend_fs.h"
const char *rs_program_name = "input-owner-canary";
}
#include "../../src/gcc_files.cc"
#include <cstdio>
#include "owner.h"

static void require(bool condition, const char *message) {
    if (!condition) { perror(message); exit(20); }
}
static bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
static void replace_bytes(const std::string &path, const char *bytes) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0600);
    require(fd >= 0 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes), "write fixture");
    close(fd);
}
static int probe(const char *root) {
    std::string base = std::string(root)+"/";
    struct stat before{}, after{}, opened{}, hard{}, other{};
    require(!stat((base+"metadata-only.h").c_str(), &before), "metadata without read");
    require(stat((base+"absent.h").c_str(), &before) < 0 && errno == ENOENT, "absent metadata");
    require(!stat((base+"denied.h").c_str(), &before), "denied metadata");
    require(open((base+"denied.h").c_str(), O_RDONLY) < 0 && errno == EACCES, "denied read");
    require(!stat((base+"revoked.h").c_str(), &before), "metadata before revoke");
    require(!chmod((base+"revoked.h").c_str(), 0), "revoke fixture");
    require(open((base+"revoked.h").c_str(), O_RDONLY) < 0 && errno == EACCES, "revoke must reauthorize");
    require(!stat((base+"value.h").c_str(), &before), "path metadata");
    struct stat copy_before{};
    require(!stat((base+"copy.h").c_str(), &copy_before) && !same(before,copy_before), "distinct metadata identity");
    int first = open((base+"value.h").c_str(), O_RDONLY | O_CLOEXEC);
    int second = open((base+"hard.h").c_str(), O_RDONLY | O_CLOEXEC);
    int duplicate = open((base+"copy.h").c_str(), O_RDONLY | O_CLOEXEC);
    require(first >= 0 && second >= 0 && duplicate >= 0, "open identities");
    require(!fstat(first,&opened) && !stat((base+"value.h").c_str(),&after) &&
            !fstat(second,&hard) && !fstat(duplicate,&other), "stat identities");
    require(same(before,opened) && same(after,opened) && same(opened,hard) &&
            !same(opened,other) && same(copy_before,other), "identity contract");
    require(before.st_mode == opened.st_mode && after.st_mode == opened.st_mode &&
            opened.st_size == after.st_size && (opened.st_mode & 0777) == 0400, "worker object metadata");
    require(!stat((base+"hard.h").c_str(), &after) && same(after,hard), "alias path identity");
    char a[4], b[4];
    require(read(first,a,4) == 4 && read(second,b,4) == 4 && !memcmp(a,b,4), "independent offsets");
    require(fcntl(first,F_GET_SEALS) == 15 && (fcntl(first,F_GETFD) & FD_CLOEXEC), "seals and exec lifetime");
    close(first); close(second); close(duplicate);
    require(!stat((base+"mutable.h").c_str(),&before), "metadata before modification");
    const char *updated = "new and longer input\n";
    replace_bytes(base+"mutable.h", updated);
    first = open((base+"mutable.h").c_str(), O_RDONLY | O_CLOEXEC);
    require(first >= 0 && !fstat(first,&opened) && !stat((base+"mutable.h").c_str(),&after), "read changed input");
    char buffer[64];
    require(same(before,opened) && same(after,opened) && opened.st_size == (off_t)strlen(updated) &&
            read(first,buffer,sizeof(buffer)) == (ssize_t)strlen(updated) &&
            !memcmp(buffer,updated,strlen(updated)), "in-place change before READ");
    close(first);
    require(!stat((base+"replaced.h").c_str(),&before), "metadata before replacement");
    replace_bytes(base+"new.h", updated);
    require(!rename((base+"new.h").c_str(),(base+"replaced.h").c_str()), "replace fixture inode");
    first = open((base+"replaced.h").c_str(), O_RDONLY | O_CLOEXEC);
    require(first >= 0 && !fstat(first,&opened) && !stat((base+"replaced.h").c_str(),&after) &&
            same(after,opened) && !same(before,opened), "replacement identity");
    close(first);
    puts("{\"path_fd_identity\":true,\"metadata_only\":true,\"read_denial\":true,\"revocation\":true,"
         "\"hardlinks\":true,\"distinct_equal_contents\":true,\"independent_offsets\":true,"
         "\"changed_before_read\":true,\"replaced_before_read\":true,\"cloexec\":true}");
    return 0;
}

static bool receive(int fd, void *buffer, size_t size) {
    auto p = static_cast<char *>(buffer);
    while (size) {
        ssize_t n = recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    if (!strcmp(argv[1], "probe")) return probe(argv[4]);
    int channel = atoi(argv[2]);
    if (!strcmp(argv[1], "client")) {
        dcc_job job{};
        if (dcc_frontend_fs_add_root(&job, argv[4])) return 3;
        dcc_frontend_reply reply{}; reply.fd = -1;
        uint32_t header[2];
        while (receive(channel, header, sizeof(header))) {
            if (ntohl(header[0]) != DCC_TCP_FS_QUERY || ntohl(header[1]) > DCC_FS_QUERY_MAX) return 4;
            std::string query(ntohl(header[1]), '\0');
            if (!receive(channel, query.data(), query.size())) return 5;
            if (dcc_frontend_fs_reply(&job, query.data(), query.size(), &reply)) return 6;
            fprintf(stderr, "rpc %u %u %.*s\n", dcc_tcp_get32(query.data()),
                    dcc_tcp_get32(reply.data), int(query.size()-4), query.data()+4);
            if (dcc_writex(channel, reply.data, reply.size)) return 7;
            char buffer[65536];
            for (uint64_t offset = 0; offset < reply.file_size;) {
                ssize_t n = pread(reply.fd, buffer, std::min<uint64_t>(sizeof(buffer), reply.file_size-offset), offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0 || dcc_writex(channel, buffer, n)) return 8;
                offset += n;
            }
            dcc_frontend_fs_reset(&reply);
        }
        dcc_frontend_fs_clear(&reply); dcc_free_argv(job.fs_roots);
        return 0;
    }
    if (strcmp(argv[1], "owner")) return 9;
    int consumer = atoi(argv[3]);
    Files files(channel, "", "input-canary", argv[5], argv[4], "main.c");
    char query[PATH_MAX+1];
    ssize_t size;
    while ((size = recv(consumer, query, sizeof(query)-1, 0)) > 0) {
        query[size] = 0;
        const char *path = query+1;
        int fd = -1;
        owner_reply reply{};
        if (query[0] == 'S') {
            reply.error = files.path_stat(path, reply.attributes);
            fprintf(stderr, "stat %s error=%d inode=%llu size=%lld\n", path, reply.error,
                    (unsigned long long)reply.attributes.st_ino, (long long)reply.attributes.st_size);
        } else if (query[0] == 'O') {
            fd = files.descriptor(path); reply.error = fd < 0 ? errno : 0;
        } else return 12;
        if (fd >= 0) {
            struct stat actual{}; fstat(fd, &actual);
            Entry *entry; files.status(path, entry);
            fprintf(stderr, "export %s client=%llu:%llu payload=%llu:%llu size=%lld seals=%d\n", path,
                    (unsigned long long)number(entry->version.data(),8), (unsigned long long)number(entry->version.data()+8,8),
                    (unsigned long long)actual.st_dev, (unsigned long long)actual.st_ino,
                    (long long)actual.st_size, fcntl(fd, F_GET_SEALS));
        } else if (reply.error) fprintf(stderr, "denied %s %d\n", path, reply.error);
        int result = dcc_send_fd(consumer, &reply, sizeof(reply), fd);
        if (fd >= 0) close(fd);
        if (result) return 10;
    }
    return size < 0 ? 11 : 0;
}
