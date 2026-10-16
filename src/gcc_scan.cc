/* Collect inputs from the actual GCC frontend. SPDX-License-Identifier: GPL-3.0-or-later */
#include "gcc-plugin.h"
#include "plugin-version.h"
#include "c-family/c-pragma.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" __attribute__((visibility("default"))) int plugin_is_GPL_compatible;
int plugin_is_GPL_compatible;
extern "C" bool distcc_gcc_inputs(cpp_reader *,
                                  bool (*)(const char *, const struct stat *, void *), void *);

namespace {
int output = -1;
size_t bytes = 0;
bool precompiled = false;
void (*read_pch)(cpp_reader *, const char *, int, const char *) = nullptr;

void read_precompiled(cpp_reader *reader, const char *name, int fd, const char *original)
{
    precompiled = true;
    read_pch(reader, name, fd, original);
}

void entered_file(void *, void *)
{
    auto *callbacks = cpp_get_callbacks(parse_in);
    if (!read_pch && callbacks->read_pch) {
        read_pch = callbacks->read_pch;
        callbacks->read_pch = read_precompiled;
    }
}

bool append(const void *data, size_t length)
{
    if (length > 64 * 1024 * 1024 - bytes) return false;
    bytes += length;
    const char *position = static_cast<const char *>(data);
    while (length) {
        ssize_t count = write(output, position, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        position += count;
        length -= count;
    }
    return true;
}

bool input(const char *path, const struct stat *stamp, void *)
{
    char metadata[256];
    int length = stamp ? snprintf(metadata, sizeof metadata, "%llu %llu %llu %llu %lld %lld",
        static_cast<unsigned long long>(stamp->st_dev),
        static_cast<unsigned long long>(stamp->st_ino),
        static_cast<unsigned long long>(stamp->st_mode),
        static_cast<unsigned long long>(stamp->st_size),
        static_cast<long long>(stamp->st_mtim.tv_sec) * 1000000000 + stamp->st_mtim.tv_nsec,
        static_cast<long long>(stamp->st_ctim.tv_sec) * 1000000000 + stamp->st_ctim.tv_nsec)
        : snprintf(metadata, sizeof metadata, "-");
    return length > 0 && static_cast<size_t>(length) < sizeof metadata &&
           append(path, strlen(path) + 1) && append(metadata, length + 1);
}

void finish(void *, void *)
{
    // A terminator is published only after every dependency has been collected.
    // Unsupported inputs never yield a usable manifest. Source diagnostics can
    // still be replayed by the worker when input collection was complete.
    if (!precompiled && append("DCGS1", 6) &&
        distcc_gcc_inputs(parse_in, input, nullptr)) append("", 1);
}
}

extern "C" __attribute__((visibility("default")))
int plugin_init(struct plugin_name_args *arguments, struct plugin_gcc_version *version)
{
    if (!plugin_default_version_check(version, &gcc_version) ||
        strcmp(version->datestamp, DCC_GCC_SOURCE_DATE) || arguments->argc != 1 ||
        strcmp(arguments->argv[0].key, "output-fd") || !arguments->argv[0].value) return 1;
    char *end;
    long descriptor = strtol(arguments->argv[0].value, &end, 10);
    if (*end || descriptor < 3 || descriptor > INT_MAX) return 1;
    output = static_cast<int>(descriptor);
    struct stat info;
    if (fstat(output, &info) || !S_ISREG(info.st_mode) || lseek(output, 0, SEEK_CUR) != 0) return 1;
    register_callback(arguments->base_name, PLUGIN_FINISH, finish, nullptr);
    register_callback(arguments->base_name, PLUGIN_INCLUDE_FILE, entered_file, nullptr);
    return 0;
}
