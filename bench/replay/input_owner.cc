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
    char path[PATH_MAX];
    ssize_t size;
    while ((size = recv(consumer, path, sizeof(path)-1, 0)) > 0) {
        path[size] = 0;
        int fd = files.descriptor(path), error = fd < 0 ? errno : 0;
        if (fd >= 0) {
            struct stat actual{}; fstat(fd, &actual);
            Entry *entry; files.status(path, entry);
            fprintf(stderr, "export %s client=%llu:%llu payload=%llu:%llu size=%lld seals=%d\n", path,
                    (unsigned long long)number(entry->version.data(),8), (unsigned long long)number(entry->version.data()+8,8),
                    (unsigned long long)actual.st_dev, (unsigned long long)actual.st_ino,
                    (long long)actual.st_size, fcntl(fd, F_GET_SEALS));
        } else fprintf(stderr, "denied %s %d\n", path, error);
        int result = dcc_send_fd(consumer, &error, sizeof(error), fd);
        if (fd >= 0) close(fd);
        if (result) return 10;
    }
    return size < 0 ? 11 : 0;
}
