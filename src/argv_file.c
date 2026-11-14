/* Complete compiler command files, independent of the network transport.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "argv_file.h"
#include "exitcode.h"

/* Tokens are four-byte protocol constants; their wire values are eight hex digits. */
static void encode_token(char *data, const char *token, unsigned value)
{
    static const char hex[] = "0123456789abcdef";
    memcpy(data, token, 4);
    for (unsigned i = 0; i < 8; ++i) data[4 + i] = hex[(value >> (28 - i * 4)) & 15];
}

int dcc_argv_encode(const char *argc_token, const char *argv_token, char **argv,
                    char **data, size_t *length)
{
    unsigned argc = 0;
    size_t size = 12;
    *data = NULL;
    *length = 0;
    while (argv[argc]) {
        size_t n = strlen(argv[argc]);
        if (argc == UINT_MAX || n > UINT_MAX || n > SIZE_MAX - 13 || size > SIZE_MAX - 13 - n)
            return EXIT_PROTOCOL_ERROR;
        size += 12 + n;
        ++argc;
    }
    char *buffer = malloc(size);
    if (!buffer) return EXIT_OUT_OF_MEMORY;
    encode_token(buffer, argc_token, argc);
    char *p = buffer + 12;
    for (unsigned i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]);
        encode_token(p, argv_token, (unsigned)n);
        memcpy(p + 12, argv[i], n);
        p += 12 + n;
    }
    *data = buffer;
    *length = size;
    return 0;
}

int dcc_parse_token_int(const char *bytes, const char *expected, unsigned *value)
{
    if (memcmp(bytes, expected, 4)) return EXIT_PROTOCOL_ERROR;
    unsigned result = 0;
    unsigned i = 4;
    /* Legacy peers right-align hexadecimal values in this fixed-width field. */
    while (i < 12 && bytes[i] == ' ') ++i;
    if (i == 12) return EXIT_PROTOCOL_ERROR;
    for (; i < 12; ++i) {
        unsigned digit = (unsigned char)bytes[i] - '0';
        if (digit > 9) {
            digit = ((unsigned char)bytes[i] | 32U) - 'a';
            if (digit > 5) return EXIT_PROTOCOL_ERROR;
            digit += 10;
        }
        result = (result << 4) | digit;
    }
    *value = result;
    return 0;
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
