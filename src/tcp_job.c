/* Bounded wire records, shared by the TCP client and server.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <lz4.h>
#include "tcp_job.h"

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
                if (kind < DCC_TCP_BEGIN || kind > DCC_TCP_COMPLETE ||
                    f->size > available ||
                    f->size > (kind == DCC_TCP_BEGIN ? DCC_JOB_METADATA_MAX + 8U :
                               kind == DCC_TCP_NEED ? DCC_JOB_INPUT_MAX * 4U : DCC_TCP_CHUNK + 8U) ||
                    ((f->type & DCC_TCP_FLAGS) && kind != DCC_TCP_INPUT && kind != DCC_TCP_OUTPUT))
                    return -1;
                f->data = malloc(f->size ? f->size : 1);
                if (!f->data) return -1;
            }
            remaining = f->size - (f->used - 8);
            if (!remaining) return 1;
            where = f->data + f->used - 8;
        }
        ssize_t n = recv(fd, where, remaining, MSG_DONTWAIT);
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
unsigned char *dcc_tcp_chunk(uint32_t type, unsigned index, const void *data,
                             size_t length, int final, size_t *size)
{
    unsigned char *p;
    if (length > DCC_TCP_CHUNK || !(p = malloc(length + 16))) return NULL;
    *size = dcc_tcp_encode(type, index, data, length, final, p);
    if (*size == 16 && length) { memcpy(p + 16, data, length); *size += length; }
    return p;
}
