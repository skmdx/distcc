/* Complete compiler command files, independent of the network transport.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "argv_file.h"
#include "exitcode.h"

int dcc_parse_token_int(const char *bytes, const char *expected, unsigned *value)
{
    char number[9], *end;
    if (strlen(expected) != 4 || memcmp(bytes, expected, 4)) return EXIT_PROTOCOL_ERROR;
    memcpy(number, bytes + 4, 8);
    number[8] = 0;
    *value = strtoul(number, &end, 16);
    return end == number + 8 ? 0 : EXIT_PROTOCOL_ERROR;
}

/* The producer hands off a complete file. Strings borrow their received
 * offsets; terminate each only after reading the following token. */
int dcc_argv_file_read(int fd, unsigned groups, struct dcc_argv_file *file)
{
    struct stat info;
    off_t offset = lseek(fd, 0, SEEK_CUR);
    memset(file, 0, sizeof(*file));
    if (offset < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < offset)
        return EXIT_IO_ERROR;
    uintmax_t extent = info.st_size - offset;
    if (extent >= SIZE_MAX) return EXIT_OUT_OF_MEMORY;
    size_t size = extent, cursor = 0;
    if (!groups || groups > size / 12) return EXIT_PROTOCOL_ERROR;
    char *bytes = malloc(size + 1), *terminator = NULL;
    if (!bytes) return EXIT_OUT_OF_MEMORY;
    file->bytes = bytes;
    file->groups = calloc(groups, sizeof(*file->groups));
    if (!file->groups) { dcc_argv_file_close(file); return EXIT_OUT_OF_MEMORY; }
    file->count = groups;
    int ret = EXIT_IO_ERROR;
    while (cursor < size) {
        ssize_t n = read(fd, bytes + cursor, size - cursor);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
        cursor += n;
    }
    cursor = 0;
    ret = EXIT_PROTOCOL_ERROR;
    for (unsigned g = 0; g < groups; ++g) {
        unsigned argc;
        if (size - cursor < 12 || dcc_parse_token_int(bytes + cursor, "ARGC", &argc)) goto out;
        if (terminator) { *terminator = 0; terminator = NULL; }
        cursor += 12;
        if (argc > (size - cursor) / 12) goto out;
        char **argv = file->groups[g] = calloc((size_t)argc + 1, sizeof(*argv));
        if (!argv) { ret = EXIT_OUT_OF_MEMORY; goto out; }
        for (unsigned a = 0; a < argc; ++a) {
            unsigned length;
            if (size - cursor < 12 || dcc_parse_token_int(bytes + cursor, "ARGV", &length)) goto out;
            if (terminator) *terminator = 0;
            cursor += 12;
            if (length > size - cursor || (uintmax_t)length + 1 > SIZE_MAX) goto out;
            argv[a] = bytes + cursor;
            cursor += length;
            terminator = bytes + cursor;
        }
    }
    if (cursor == size) {
        if (terminator) *terminator = 0;
        ret = 0;
    }
out:
    if (ret) dcc_argv_file_close(file);
    return ret;
}

void dcc_argv_file_close(struct dcc_argv_file *file)
{
    for (unsigned g = 0; g < file->count; ++g) free(file->groups[g]);
    free(file->groups);
    free(file->bytes);
    memset(file, 0, sizeof(*file));
}
