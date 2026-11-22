/* Collect inputs from the actual GCC frontend. SPDX-License-Identifier: GPL-3.0-or-later */
// Let GCC include the standard library before poisoning libc identifiers.
#define INCLUDE_STRING
#define INCLUDE_VECTOR
#include "gcc-plugin.h"
#include "plugin-version.h"
#include "cpplib.h"
extern cpp_reader *parse_in;
#include "langhooks.h"
#include "opts.h"
#include "toplev.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" __attribute__((visibility("default"))) int plugin_is_GPL_compatible;
int plugin_is_GPL_compatible;
extern "C" bool distcc_gcc_input_root(cpp_reader *, const char *);
extern "C" void distcc_gcc_cxx_system(cpp_reader *, const char *);
extern "C" void distcc_gcc_client_search(cpp_reader *);
extern "C" bool distcc_gcc_plan(cpp_reader *,
    bool (*)(const char *, const struct stat *, int, void *), void *);

namespace {
FILE *output = nullptr;
// Header search produces many short records; write them in bounded batches.
char output_buffer[64 * 1024];
bool client_search = false;
void (*preprocess_options)(cpp_reader *) = nullptr;
std::string input_root;
std::vector<std::string> cxx_system_dirs;

// Decode the driver's own option language for compound compile/link jobs.
// The descriptor contains counted, NUL-terminated argv arrays and is replaced
// with counted canonical option groups. No C/C++ or option grammar is copied.
bool driver_options(int descriptor)
{
    struct stat info;
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0)
        return false;
    std::vector<char> data(info.st_size);
    size_t received = 0;
    while (received < data.size()) {
        ssize_t count = read(descriptor, data.data() + received, data.size() - received);
        if (count <= 0) return false;
        received += count;
    }
    if (data.back() || ftruncate(descriptor, 0) || lseek(descriptor, 0, SEEK_SET))
        return false;
    FILE *stream = fdopen(descriptor, "wb");
    if (!stream) return false;
    char *cursor = data.data(), *limit = cursor + data.size();
    unsigned int command = 0;
    while (cursor < limit) {
        char *end;
        unsigned long argc = strtoul(cursor, &end, 10);
        if (*end || !argc || argc > UINT_MAX) { fclose(stream); return false; }
        cursor = end + 1;
        std::vector<const char *> argv;
        for (unsigned long i = 0; i < argc; ++i) {
            if (cursor >= limit) { fclose(stream); return false; }
            argv.push_back(cursor);
            cursor += strlen(cursor) + 1;
        }
        cl_decoded_option *options;
        unsigned int count;
        decode_cmdline_options_to_array(argc, argv.data(),
            command++ == 0 ? CL_DRIVER : ~CL_DRIVER, &options, &count);
        fprintf(stream, "%u%c", count - 1, 0);
        for (unsigned int i = 1; i < count; ++i) {
            const auto &option = options[i];
            fprintf(stream, "%c%c%zu%c", option.opt_index == OPT_SPECIAL_input_file ? 'I' : 'O',
                    0, option.canonical_option_num_elements, 0);
            for (size_t j = 0; j < option.canonical_option_num_elements; ++j)
                fwrite(option.canonical_option[j], 1, strlen(option.canonical_option[j]) + 1, stream);
        }
        free(options);
    }
    return fclose(stream) == 0;
}

struct frontend_plan {
    std::vector<std::string> defaults{"4gcc"}, roots;
};
bool plan_entry(const char *path, const struct stat *, int kind, void *context)
{
    auto &plan = *static_cast<frontend_plan *>(context);
    if (!path || !*path) return false;
    std::string name(path);
    if (kind < 0) {
        auto end = name.find_last_of('/');
        name = end == std::string::npos ? "." : end == 0 ? "/" : name.substr(0, end);
    }
    plan.roots.push_back(name);
    if (kind >= 3) plan.defaults.push_back(std::string(1, kind == 3 ? '7' : kind == 4 ? '5' : '0') + path);
    return true;
}
bool plan_group(const std::vector<std::string> &values)
{
    if (fprintf(output, "ARGC%08x", static_cast<unsigned>(values.size())) < 0) return false;
    for (const auto &value : values)
        if (fprintf(output, "ARGV%08x", static_cast<unsigned>(value.size())) < 0 ||
            fwrite(value.data(), 1, value.size(), output) != value.size()) return false;
    return true;
}

void plan_options(cpp_reader *reader)
{
    if (preprocess_options) preprocess_options(reader);
    frontend_plan plan;
    bool ok = true;
    for (unsigned i = 0; i < save_decoded_options_count; ++i) {
        const auto &option = save_decoded_options[i];
        if ((option.opt_index == OPT_SPECIAL_input_file || option.opt_index == OPT_include ||
             option.opt_index == OPT_imacros) && option.arg)
            ok = ok && plan_entry(option.arg, nullptr, -1, &plan);
    }
    ok = ok && distcc_gcc_plan(reader, plan_entry, &plan) &&
         plan_group(plan.defaults) && plan_group(plan.roots);
    if (fclose(output)) ok = false;
    // GCC has registered its include chains. Stop before opening the source;
    // preprocessing and its diagnostics belong to the actual remote compiler.
    _exit(ok ? 0 : 1);
}

void entered_file(void *, void *)
{
    if (distcc_gcc_input_root(parse_in, input_root.c_str())) {
        if (client_search) distcc_gcc_client_search(parse_in);
        for (const auto &path : cxx_system_dirs)
            distcc_gcc_cxx_system(parse_in, path.c_str());
    }
}


} // namespace

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
            if (!strcmp(arg.key, "client-search") && arg.value && !strcmp(arg.value, "1")) {
                client_search = true;
                continue;
            }
            if (strcmp(arg.key, "cxx-system") || !arg.value || !*arg.value) return 1;
            cxx_system_dirs.emplace_back(arg.value);
        }
        register_callback(arguments->base_name, PLUGIN_INCLUDE_FILE, entered_file, nullptr);
        return 0;
    }
    if (arguments->argc != 1) return 1;
    char *end;
    long descriptor = strtol(arguments->argv[0].value, &end, 10);
    if (*end || descriptor < 3 || descriptor > INT_MAX) return 1;
    if (!strcmp(arguments->argv[0].key, "driver-fd"))
        return driver_options(descriptor) ? 0 : 1;
    if (strcmp(arguments->argv[0].key, "plan-fd")) return 1;
    struct stat info;
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) || lseek(descriptor, 0, SEEK_CUR) != 0) return 1;
    output = fdopen(descriptor, "wb");
    if (!output || setvbuf(output, output_buffer, _IOFBF, sizeof output_buffer)) return 1;
    // Module preprocessing has a language-specific token observer and is not
    // supported by the dependency collector. Never replace that observer.
    if (flag_modules) return 1;
    preprocess_options = lang_hooks.preprocess_options;
    lang_hooks.preprocess_options = plan_options;
    return 0;
}
