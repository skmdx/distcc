/* Bounded, independently decodable LZ4 chunks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <lz4.h>
#include "distcc.h"
#include "exitcode.h"

/* Each chunk is (plain size, encoded size), followed by encoded bytes. An
 * encoded size of zero means raw bytes; (0, 0) terminates the file. The file
 * token is UINT32_MAX, so no total compressed length or whole-file buffer is
 * needed. Incompressible chunks never grow by more than the eight-byte header. */
int dcc_x_lz4_stream(int out_fd, int in_fd, off_t size)
{
    int chunk = size < DCC_STREAM_CHUNK ? (int)size : DCC_STREAM_CHUNK;
    int bound = LZ4_compressBound(chunk), ret = 0;
    char *plain = malloc(chunk + 8), *encoded = malloc(bound + 8);
    uint32_t header[2] = {0, 0};
    if (!plain || !encoded) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    while (size > 0) {
        int n = size < DCC_STREAM_CHUNK ? (int)size : DCC_STREAM_CHUNK;
        int compressed;
        char *frame;
        if ((ret = dcc_readx(in_fd, plain + 8, n))) goto done;
        compressed = LZ4_compress_default(plain + 8, encoded + 8, n, bound);
        if (compressed <= 0) { ret = EXIT_IO_ERROR; goto done; }
        header[0] = htonl(n);
        header[1] = htonl(compressed < n ? compressed : 0);
        frame = compressed < n ? encoded : plain;
        memcpy(frame, header, 8);
        if (compressed >= n) compressed = n;
        if ((ret = dcc_writex(out_fd, frame, compressed + 8))) goto done;
        size -= n;
    }
    memset(header, 0, sizeof(header));
    ret = dcc_writex(out_fd, header, sizeof(header));
done:
    free(plain);
    free(encoded);
    return ret;
}

static int reserve_buffer(char **buffer, size_t *capacity, size_t needed)
{
    size_t next;
    char *data;
    if (needed <= *capacity) return 0;
    next = *capacity ? *capacity : 64;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    data = realloc(*buffer, next);
    if (!data) return EXIT_OUT_OF_MEMORY;
    *buffer = data;
    *capacity = next;
    return 0;
}

/* out_fd >= 0 writes directly to the caller's output file. Diagnostics use
 * output instead, preserving the existing delayed-publication contract. */
int dcc_r_lz4_stream(int out_fd, struct dcc_buffer *output, int in_fd)
{
    int ret = 0;
    char *plain = NULL, *encoded = NULL;
    size_t plain_capacity = 0, encoded_capacity = 0, capacity = 0;
    for (;;) {
        uint32_t header[2], size, compressed;
        char *destination;
        if ((ret = dcc_readx(in_fd, header, sizeof(header)))) break;
        size = ntohl(header[0]);
        compressed = ntohl(header[1]);
        if (!size && !compressed) break;
        if (!size || size > DCC_STREAM_CHUNK || compressed >= size) {
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        if (out_fd >= 0) {
            if ((ret = reserve_buffer(&plain, &plain_capacity, size))) break;
            destination = plain;
        } else {
            size_t needed = output->size + size;
            if (needed < output->size) { ret = EXIT_OUT_OF_MEMORY; break; }
            if ((ret = reserve_buffer(&output->data, &capacity, needed))) break;
            destination = output->data + output->size;
        }
        if (compressed && (ret = reserve_buffer(&encoded, &encoded_capacity, compressed))) break;
        if ((ret = dcc_readx(in_fd, compressed ? encoded : destination, compressed ? compressed : size))) break;
        if (compressed && LZ4_decompress_safe(encoded, destination, compressed, size) != (int)size) {
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        if (out_fd >= 0) {
            if ((ret = dcc_writex(out_fd, destination, size))) break;
        } else output->size += size;
    }
    free(plain);
    free(encoded);
    if (ret && output) dcc_free_buffer(output);
    return ret;
}
