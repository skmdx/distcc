/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2003, 2004 by Martin Pool
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


/* pump.c - Transfer of bulk data (source, object code) */


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SYS_MMAN_H
#  include <sys/mman.h>
#endif

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "exitcode.h"


/*
 * Receive either compressed or uncompressed bulk data.
 */
void dcc_free_buffer(struct dcc_buffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
}

/* Publish only complete payloads; leave an empty buffer on receive failure. */
int dcc_r_buffer(struct dcc_buffer *buffer, int ifd, unsigned f_size,
                 enum dcc_compress compression)
{
    int ret;
    dcc_free_buffer(buffer);
    if (!f_size)
        return 0;
    if (compression == DCC_COMPRESS_LZO1X)
        return dcc_r_bulk_lzo1x(buffer, ifd, f_size);
    if (compression == DCC_COMPRESS_LZ4)
        return dcc_r_bulk_lz4(buffer, ifd, f_size);
    if (compression == DCC_COMPRESS_LZ4_STREAM)
        return f_size == 0xffffffffU ? dcc_r_lz4_stream(-1, buffer, ifd) : EXIT_PROTOCOL_ERROR;
    if (compression != DCC_COMPRESS_NONE) {
        rs_log_error("impossible compression %d", compression);
        return EXIT_PROTOCOL_ERROR;
    }
    if (!(buffer->data = malloc(f_size)))
        return EXIT_OUT_OF_MEMORY;
    if ((ret = dcc_readx(ifd, buffer->data, f_size))) {
        dcc_free_buffer(buffer);
        return ret;
    }
    buffer->size = f_size;
    return 0;
}

int dcc_r_bulk(int ofd,
               int ifd,
               unsigned f_size,
               enum dcc_compress compression)
{
    struct dcc_buffer buffer = {0};
    int ret;
    if (compression == DCC_COMPRESS_LZ4_STREAM)
        return !f_size ? 0 : f_size == 0xffffffffU ? dcc_r_lz4_stream(ofd, NULL, ifd) : EXIT_PROTOCOL_ERROR;
    if (compression == DCC_COMPRESS_NONE)
        return dcc_pump_readwrite(ofd, ifd, f_size);
    ret = dcc_r_buffer(&buffer, ifd, f_size, compression);
    if (!ret)
        ret = dcc_writex(ofd, buffer.data, buffer.size);
    dcc_free_buffer(&buffer);
    return ret;
}



/**
 * Copy @p n bytes from @p ifd to @p ofd.
 *
 * Does not use sendfile(), so either one may be a socket.
 *
 * In the current code at least one of the files will always be a regular
 * (disk) file, even though it may not be mmapable.  That should mean that
 * writes to it will always complete immediately.  That in turn means that on
 * each pass through the main loop we ought to either completely fill our
 * buffer, or completely drain it, depending on which one is the disk.
 *
 * In future we may put back the ability to feed the compiler from a fifo, in
 * which case it may be that the writes don't complete.
 *
 * We might try selecting on both buffers and handling whichever is ready.
 * This would require some approximation to a circular buffer though, which
 * might be more complex.
 **/
int
dcc_pump_readwrite(int ofd, int ifd, size_t n)
{
    static char buf[262144];    /* we're not recursive */
    char *p;
    ssize_t r_in, r_out, wanted;
    int ret;

    while (n > 0) {
        wanted = (n > sizeof buf) ? (sizeof buf) : n;
        r_in = read(ifd, buf, (size_t) wanted);

        if (r_in == -1 && errno == EAGAIN) {
            if ((ret = dcc_select_for_read(ifd, dcc_get_io_timeout())) != 0)
                return ret;
            else
                continue;
        } else if (r_in == -1 && errno == EINTR) {
            continue;
        } else if (r_in == -1) {
            rs_log_error("failed to read %ld bytes: %s",
                         (long) wanted, strerror(errno));
            return EXIT_IO_ERROR;
        } else if (r_in == 0) {
            rs_log_error("unexpected eof on fd%d", ifd);
            return EXIT_IO_ERROR;
        }

        n -= r_in;
        p = buf;

        /* We now have r_in bytes waiting to go out, starting at p.  Keep
         * going until they're all written out. */

        while (r_in > 0) {
            r_out = write(ofd, p, (size_t) r_in);

            if (r_out == -1 && errno == EAGAIN) {
                if ((ret = dcc_select_for_write(ofd,
                                                dcc_get_io_timeout())) != 0) {
                    return ret;
                } else {
                    continue;
                }
            } else if (r_out == -1 && errno == EINTR) {
                continue;
            } else if (r_out == -1  ||  r_out == 0) {
                rs_log_error("failed to write: %s", strerror(errno));
                return EXIT_IO_ERROR;
            }
            r_in -= r_out;
            p += r_out;
        }
    }

    return 0;
}
