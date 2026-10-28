/* Collect inputs from the actual GCC frontend. SPDX-License-Identifier: GPL-3.0-or-later */
// Let GCC include the standard library before poisoning libc identifiers.
#define INCLUDE_STRING
#define INCLUDE_VECTOR
#include "gcc-plugin.h"
#include "plugin-version.h"
#include "c-family/c-pragma.h"
#include "langhooks.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" __attribute__((visibility("default"))) int plugin_is_GPL_compatible;
int plugin_is_GPL_compatible;
extern "C" bool distcc_gcc_inputs(cpp_reader *,
                                  bool (*)(const char *, const struct stat *, int, void *), void *);
extern "C" bool distcc_gcc_input_root(cpp_reader *, const char *);
extern "C" void distcc_gcc_cxx_system(cpp_reader *, const char *);

namespace {
FILE *output = nullptr;
// Header search produces many short records; write them in bounded batches.
char output_buffer[64 * 1024];
bool precompiled = false;
std::string input_root;
std::vector<std::string> cxx_system_dirs;
void (*read_pch)(cpp_reader *, const char *, int, const char *) = nullptr;

uintptr_t scan_tokens(cpp_reader *reader, const cpp_token *, uintptr_t)
{
    // Explicit directives-only preprocessing has a different native scanner.
    if (cpp_get_options(reader)->directives_only) return 0;
    // GCC calls this hook after initializing the preprocessor, before streaming
    // its first token. Consume the native stream here without formatting ordinary
    // tokens. cpp_scan_nooutput is unsuitable: it skips ordinary macro uses,
    // whose side effects (notably __COUNTER__) can change later dependencies.
    cpp_get_callbacks(reader)->line_change = nullptr;
    for (;;) {
        location_t location;
        const cpp_token *token = cpp_get_token_with_location(reader, &location);
        input_location = location;
        if (token->type == CPP_EOF) {
            // Leave EOF for the caller; lexing again after the input buffer has
            // been popped is invalid. GCC's token lookahead owns its lifetime.
            _cpp_backup_tokens(reader, 1);
            break;
        }
        // Keep the same early pragma dispatch as GCC's token_streamer, including
        // diagnostic push/pop. All parsing and expansion remain in the frontend.
        if (token->type == CPP_PRAGMA && token->val.pragma >= PRAGMA_FIRST_EXTERNAL)
            c_pp_invoke_early_pragma_handler(token->val.pragma);
    }
    return 0;
}

void read_precompiled(cpp_reader *reader, const char *name, int fd, const char *original)
{
    precompiled = true;
    read_pch(reader, name, fd, original);
}

void entered_file(void *, void *)
{
    if (!input_root.empty()) {
        if (distcc_gcc_input_root(parse_in, input_root.c_str()))
            for (const auto &path : cxx_system_dirs)
                distcc_gcc_cxx_system(parse_in, path.c_str());
        return;
    }
    auto *callbacks = cpp_get_callbacks(parse_in);
    if (!read_pch && callbacks->read_pch) {
        read_pch = callbacks->read_pch;
        callbacks->read_pch = read_precompiled;
    }
}

bool append(const void *data, size_t length)
{
    return fwrite(data, 1, length, output) == length;
}

bool input(const char *path, const struct stat *stamp, int kind, void *)
{
    char metadata[256];
    int length = stamp ? snprintf(metadata, sizeof metadata, "%llu %llu %llu %llu %lld %lld",
        static_cast<unsigned long long>(stamp->st_dev),
        static_cast<unsigned long long>(stamp->st_ino),
        static_cast<unsigned long long>(stamp->st_mode),
        static_cast<unsigned long long>(stamp->st_size),
        static_cast<long long>(stamp->st_mtim.tv_sec) * 1000000000 + stamp->st_mtim.tv_nsec,
        static_cast<long long>(stamp->st_ctim.tv_sec) * 1000000000 + stamp->st_ctim.tv_nsec)
        : kind < 0 ? snprintf(metadata, sizeof metadata, "-")
                   : snprintf(metadata, sizeof metadata, "D%d", kind);
    return length > 0 && static_cast<size_t>(length) < sizeof metadata &&
           append(path, strlen(path) + 1) && append(metadata, length + 1);
}

void finish(void *, void *)
{
    // A terminator is published only after every dependency has been collected.
    // Unsupported inputs never yield a usable manifest. Source diagnostics can
    // still be replayed by the worker when input collection was complete.
    if (!precompiled && append("DCGS2", 6) &&
        distcc_gcc_inputs(parse_in, input, nullptr) && !fflush(output)) append("", 1);
    fclose(output);
}
}

extern "C" __attribute__((visibility("default")))
int plugin_init(struct plugin_name_args *arguments, struct plugin_gcc_version *version)
{
    if (!plugin_default_version_check(version, &gcc_version) ||
        strcmp(version->datestamp, DCC_GCC_SOURCE_DATE) || arguments->argc < 1 ||
        !arguments->argv[0].value) return 1;
    if (!strcmp(arguments->argv[0].key, "input-root")) {
        const char *root = arguments->argv[0].value;
        size_t length = strlen(root);
        if (length < 2 || root[0] != '/' || root[length - 1] == '/') return 1;
        input_root = root;
        for (int i = 1; i < arguments->argc; ++i) {
            const auto &arg = arguments->argv[i];
            if (strcmp(arg.key, "cxx-system") || !arg.value || arg.value[0] != '/') return 1;
            cxx_system_dirs.emplace_back(arg.value);
        }
        register_callback(arguments->base_name, PLUGIN_INCLUDE_FILE, entered_file, nullptr);
        return 0;
    }
    if (arguments->argc != 1 || strcmp(arguments->argv[0].key, "output-fd")) return 1;
    char *end;
    long descriptor = strtol(arguments->argv[0].value, &end, 10);
    if (*end || descriptor < 3 || descriptor > INT_MAX) return 1;
    struct stat info;
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || lseek(descriptor, 0, SEEK_CUR) != 0) return 1;
    output = fdopen(descriptor, "wb");
    if (!output || setvbuf(output, output_buffer, _IOFBF, sizeof output_buffer)) return 1;
    // Module preprocessing has a language-specific token observer and is not
    // supported by the dependency collector. Never replace that observer.
    if (flag_modules) return 1;
    lang_hooks.preprocess_token = scan_tokens;
    register_callback(arguments->base_name, PLUGIN_FINISH, finish, nullptr);
    register_callback(arguments->base_name, PLUGIN_INCLUDE_FILE, entered_file, nullptr);
    return 0;
}
