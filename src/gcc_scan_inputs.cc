/* GCC's private dependency state adapter. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Build against the exact source and plugin headers of the selected GCC.
 * The compiler owns parsing, macro expansion, feature tests and header search.
 * Including its implementation gives this adapter the actual private types;
 * no copied structure layouts or substitute preprocessor are maintained here.
 * Hidden unused libcpp sections are removed when linking the module.
 */
#include <config.h>
#include "system.h"
#include "intl.h"
#include "files.cc"

// -1 identifies a file record; 0..2 are GCC's directory system-header classes.
using input_callback = bool (*)(const char *, const struct stat *, int, void *);

static const char *input_root;

static char *absolute_input(const char *name, cpp_dir *)
{
    size_t length = strlen(input_root);
    // Command-line paths have already been rooted by distccd. Operands in
    // source files have not. GCC still owns lookup, caching and preprocessing.
    if (!strncmp(name, input_root, length) && name[length] == '/')
        return xstrdup(name);
    return concat(input_root, name, nullptr);
}

extern "C" bool distcc_gcc_input_root(cpp_reader *reader, const char *root)
{
    if (reader->no_search_path.construct == absolute_input) return false;
    input_root = root;
    reader->no_search_path.construct = absolute_input;
    return true;
}

extern "C" void distcc_gcc_cxx_system(cpp_reader *reader, const char *path)
{
    // -isystem uses the extern-C system class. GCC's built-in C++ directories
    // use class 1 instead; restore it before any headers are entered.
    for (auto *dir = reader->quote_include; dir; dir = dir->next)
        if (!strcmp(dir->name, path)) dir->sysp = 1;
}

static bool unsupported_absolute(const char *path)
{
    if (!path || path[0] != '/') return false;
    // Parent traversal can escape the private input tree. Its meaning also
    // depends on symlinks, so lexical normalization would change semantics.
    for (const char *part = path; (part = strchr(part, '/')); ++part)
        if (part[1] == '.' && part[2] == '.' && (!part[3] || part[3] == '/')) return true;
    return false;
}

struct input_visitor {
    input_callback callback;
    void *context;
    bool ok;
};

static int missing_input(void **slot, void *opaque)
{
    auto &visitor = *static_cast<input_visitor *>(opaque);
    const auto *path = static_cast<const char *>(*slot);
    // libcpp records ENOENT here, including directories and ENOTDIR. The
    // publication cache observes these failed candidates and their ancestors;
    // do not repeat every failed filesystem lookup in this process as well.
    visitor.ok = !unsupported_absolute(path) &&
                 visitor.callback(path, nullptr, -1, visitor.context);
    return visitor.ok;
}

extern "C" __attribute__((visibility("default")))
bool distcc_gcc_inputs(cpp_reader *reader, input_callback callback, void *context)
{
    if (!reader || !reader->main_file) return false;
    for (const auto *file = reader->all_files; file; file = file->next_file) {
        if (file->embed || file->pchname || file->header_unit > 0 ||
            unsupported_absolute(file->name)) return false;
        if (file->err_no == ENOENT) continue;
        if (file->err_no || !file->path || !S_ISREG(file->st.st_mode)) return false;
        // libcpp replaces st_size with its decoded buffer length (BOM removal
        // and input-charset conversion). Keep its original file identity and
        // timestamps, but report the raw file size used for transfer.
        struct stat stamp;
        if (stat(file->path, &stamp) || stamp.st_dev != file->st.st_dev ||
            stamp.st_ino != file->st.st_ino || stamp.st_mode != file->st.st_mode ||
            stamp.st_mtim.tv_sec != file->st.st_mtim.tv_sec ||
            stamp.st_mtim.tv_nsec != file->st.st_mtim.tv_nsec ||
            stamp.st_ctim.tv_sec != file->st.st_ctim.tv_sec ||
            stamp.st_ctim.tv_nsec != file->st.st_ctim.tv_nsec ||
            !callback(file->path, &stamp, -1, context)) return false;
    }
    input_visitor visitor{callback, context, true};
    htab_traverse(reader->nonexistent_file_hash, missing_input, &visitor);
    if (!visitor.ok) return false;
    for (const auto *dir = reader->quote_include; dir; dir = dir->next)
        if (!callback(dir->name, nullptr, dir->sysp, context)) return false;
    return visitor.ok;
}
