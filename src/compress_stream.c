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
    int bound = LZ4_compressBound(DCC_STREAM_CHUNK), ret = 0;
    char *plain = malloc(DCC_STREAM_CHUNK), *encoded = malloc(bound + 8);
    uint32_t header[2] = {0, 0};
    if (!plain || !encoded) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    while (size > 0) {
        int n = size < DCC_STREAM_CHUNK ? (int)size : DCC_STREAM_CHUNK;
        int compressed;
        if ((ret = dcc_readx(in_fd, plain, n))) goto done;
        compressed = LZ4_compress_default(plain, encoded + 8, n, bound);
        if (compressed <= 0) { ret = EXIT_IO_ERROR; goto done; }
        header[0] = htonl(n);
        header[1] = htonl(compressed < n ? compressed : 0);
        memcpy(encoded, header, 8);
        if (compressed >= n) { memcpy(encoded + 8, plain, n); compressed = n; }
        if ((ret = dcc_writex(out_fd, encoded, compressed + 8))) goto done;
        size -= n;
    }
    memset(header, 0, sizeof(header));
    ret = dcc_writex(out_fd, header, sizeof(header));
done:
    free(plain);
    free(encoded);
    return ret;
}

/* out_fd >= 0 writes directly to the caller's output file. Diagnostics use
 * output instead, preserving the existing delayed-publication contract. */
int dcc_r_lz4_stream(int out_fd, struct dcc_buffer *output, int in_fd)
{
    int ret = 0, bound = LZ4_compressBound(DCC_STREAM_CHUNK);
    char *plain = malloc(DCC_STREAM_CHUNK), *encoded = malloc(bound);
    size_t capacity = 0;
    if (!plain || !encoded) { ret = EXIT_OUT_OF_MEMORY; goto done; }
    for (;;) {
        uint32_t header[2], size, compressed;
        if ((ret = dcc_readx(in_fd, header, sizeof(header)))) break;
        size = ntohl(header[0]);
        compressed = ntohl(header[1]);
        if (!size && !compressed) break;
        if (!size || size > DCC_STREAM_CHUNK || compressed >= size) {
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        if ((ret = dcc_readx(in_fd, compressed ? encoded : plain, compressed ? compressed : size))) break;
        if (compressed && LZ4_decompress_safe(encoded, plain, compressed, size) != (int)size) {
            ret = EXIT_PROTOCOL_ERROR;
            break;
        }
        if (out_fd >= 0) {
            if ((ret = dcc_writex(out_fd, plain, size))) break;
        } else {
            size_t needed = output->size + size;
            if (needed < output->size) { ret = EXIT_OUT_OF_MEMORY; break; }
            if (needed > capacity) {
                size_t next = capacity <= SIZE_MAX / 2 ? capacity * 2 : needed;
                char *data;
                if (next < needed) next = needed;
                data = realloc(output->data, next);
                if (!data) { ret = EXIT_OUT_OF_MEMORY; break; }
                capacity = next;
                output->data = data;
            }
            memcpy(output->data + output->size, plain, size);
            output->size += size;
        }
    }
done:
    free(plain);
    free(encoded);
    if (ret && output) dcc_free_buffer(output);
    return ret;
}
