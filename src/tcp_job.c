/* Bounded wire records, shared by the TCP client and server.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <lz4.h>
#include "tcp_job.h"

int dcc_tcp_local_address(int port, struct sockaddr_un *address)
{
    const char *prefix = getenv("DISTCC_LOCAL_SOCKET");
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    return !prefix || prefix[0] != '/' ||
        snprintf(address->sun_path, sizeof(address->sun_path), "%s-%d.sock", prefix, port) >=
            (int)sizeof(address->sun_path) ? -1 : 0;
}

int dcc_tcp_local_listener(int port)
{
    struct sockaddr_un address;
    struct stat st;
    if (dcc_tcp_local_address(port, &address)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (!lstat(address.sun_path, &st)) {
        /* Never replace another owner's path or a live listener. */
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
            !connect(fd, (struct sockaddr *)&address, sizeof(address)) || errno != ECONNREFUSED ||
            unlink(address.sun_path)) { close(fd); return -1; }
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
        chmod(address.sun_path, 0666) || listen(fd, 128)) { close(fd); return -1; }
    return fd;
}

uint32_t dcc_tcp_get32(const void *p)
{
    const unsigned char *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
uint64_t dcc_tcp_get64(const void *p)
{
    return (uint64_t)dcc_tcp_get32(p) << 32 | dcc_tcp_get32((const char *)p + 4);
}
void dcc_tcp_put32(void *p, uint32_t n)
{
    unsigned char *b = p;
    b[0] = n >> 24; b[1] = n >> 16; b[2] = n >> 8; b[3] = n;
}
void dcc_tcp_put64(void *p, uint64_t n)
{
    dcc_tcp_put32(p, n >> 32);
    dcc_tcp_put32((char *)p + 4, n);
}
void dcc_tcp_frame_clear(struct dcc_tcp_frame *f)
{
    if (f->fd_plus_one) close(f->fd_plus_one - 1);
    free(f->data);
    memset(f, 0, sizeof(*f));
}
int dcc_tcp_frame_read(int fd, struct dcc_tcp_frame *f, size_t available)
{
    for (;;) {
        size_t remaining;
        void *where;
        if (f->used < 8) {
            remaining = 8 - f->used;
            where = f->header + f->used;
        } else {
            if (!f->data) {
                uint32_t kind;
                f->type = dcc_tcp_get32(f->header);
                f->size = dcc_tcp_get32(f->header + 4);
                kind = f->type & ~DCC_TCP_FLAGS;
                if (kind < DCC_TCP_BEGIN || kind > DCC_TCP_FS_REPLY ||
                    f->size > available ||
                    f->size > (kind == DCC_TCP_BEGIN ? DCC_JOB_METADATA_MAX + 8U :
                               kind == DCC_TCP_NEED ? DCC_JOB_INPUT_MAX * 4U : DCC_TCP_CHUNK + 8U) ||
                    ((f->type & DCC_TCP_FLAGS) && kind != DCC_TCP_INPUT &&
                     kind != DCC_TCP_OUTPUT && kind != DCC_TCP_FS_REPLY))
                    return -1;
                f->data = malloc(f->size ? f->size : 1);
                if (!f->data) return -1;
            }
            remaining = f->size - (f->used - 8);
            if (!remaining) return 1;
            where = f->data + f->used - 8;
        }
        struct iovec iov = {where, remaining};
        union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int) * 4)]; } control;
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = control.data, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        int invalid = msg.msg_flags & MSG_CTRUNC;
        if (n >= 0) for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) { invalid = 1; continue; }
            unsigned count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(c);
            for (unsigned i = 0; i < count; ++i) {
                if (f->fd_plus_one || f->used || invalid) { close(fds[i]); invalid = 1; }
                else f->fd_plus_one = fds[i] + 1;
            }
        }
        if (n >= 0 && invalid) return -1;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;
        f->used += n;
    }
}
unsigned char *dcc_tcp_frame_make(uint32_t type, const void *data, size_t size, size_t *length)
{
    unsigned char *p;
    if (size > DCC_JOB_METADATA_MAX + 8U || !(p = malloc(size + 8))) return NULL;
    dcc_tcp_put32(p, type);
    dcc_tcp_put32(p + 4, size);
    if (size) memcpy(p + 8, data, size);
    *length = size + 8;
    return p;
}
int dcc_tcp_unpack(const struct dcc_tcp_frame *f, unsigned *index, void *buffer,
                   const void **data, size_t *length)
{
    if (f->size < 8) return -1;
    *index = dcc_tcp_get32(f->data);
    *length = dcc_tcp_get32(f->data + 4);
    if (*length > DCC_TCP_CHUNK || (!*length && !(f->type & DCC_TCP_FINAL))) return -1;
    if (f->type & DCC_TCP_COMPRESSED) {
        if (f->size - 8 >= *length || LZ4_decompress_safe((char *)f->data + 8,
                buffer, f->size - 8, *length) != (int)*length) return -1;
        *data = buffer;
    } else {
        if (f->size - 8 != *length) return -1;
        *data = f->data + 8;
    }
    return 0;
}
size_t dcc_tcp_encode(uint32_t type, unsigned index, const void *data,
                       size_t length, int final, void *storage)
{
    unsigned char *p = storage;
    int compressed = 0;
    /* A destination shorter than raw makes incompressible blocks cheap. */
    if (length > 16) compressed = LZ4_compress_default(data, (char *)p + 16, length, length - 1);
    if (compressed) type |= DCC_TCP_COMPRESSED;
    if (final) type |= DCC_TCP_FINAL;
    dcc_tcp_put32(p, type);
    dcc_tcp_put32(p + 4, 8 + (compressed ? (size_t)compressed : length));
    dcc_tcp_put32(p + 8, index);
    dcc_tcp_put32(p + 12, length);
    return 16 + compressed;
}
unsigned char *dcc_tcp_file_chunk(uint32_t type, unsigned index, int fd,
                                  off_t offset, size_t length, int final, int immutable, size_t *size)
{
    struct stat st;
    long page = sysconf(_SC_PAGESIZE);
    if (length > DCC_TCP_CHUNK || offset < 0 || page <= 0 || fstat(fd, &st) ||
        st.st_size < offset || (uint64_t)(st.st_size - offset) < length) return NULL;
    size_t delta = offset % page;
    void *mapping = MAP_FAILED;
    unsigned char *snapshot = NULL;
    const void *data = NULL;
    unsigned char *p = malloc(length + 16);
    if (!p) return NULL;
    if (length && immutable) {
        mapping = mmap(NULL, delta + length, PROT_READ, MAP_PRIVATE, fd, offset - delta);
        if (mapping == MAP_FAILED) { free(p); return NULL; }
        data = (char *)mapping + delta;
    } else if (length) {
        snapshot = malloc(length + 16);
        if (!snapshot || pread(fd, snapshot + 16, length, offset) != (ssize_t)length) {
            free(snapshot); free(p); return NULL;
        }
        data = snapshot + 16;
    }
    *size = dcc_tcp_encode(type, index, data, length, final, p);
    if (mapping != MAP_FAILED) munmap(mapping, delta + length);
    if (snapshot && *size == 16) {
        memcpy(snapshot, p, 16); free(p);
        *size += length;
        return snapshot;
    }
    free(snapshot);
    return p;
}
