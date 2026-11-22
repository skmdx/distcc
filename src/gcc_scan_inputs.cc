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
// Plugin headers already supplied GCC's system layer. A complete source tree
// also contains libcpp/system.h; do not initialize/poison libc a second time.
#define LIBCPP_SYSTEM_H
#include "files.cc"

// -1: file; 0..2: explicit directory; 3..5: default directory; 6: preinclude.
using input_callback = bool (*)(const char *, const struct stat *, int, void *);

extern "C" bool distcc_gcc_plan(cpp_reader *reader, input_callback callback, void *context)
{
    if (!reader) return false;
    for (const auto *dir = reader->quote_include; dir; dir = dir->next)
        if (!callback(dir->name, nullptr, dir->sysp + (dir->user_supplied_p ? 0 : 3), context))
            return false;
    return true;
}

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

extern "C" void distcc_gcc_client_search(cpp_reader *reader)
{
    // The driver has initialized the complete native search chain. The
    // client-provided defaults arrived as explicit include options; remove
    // only the worker's implicit directories. Keeping std_inc unchanged lets
    // GCC itself handle its target preinclude and PCH initialization order.
    cpp_dir *quote = nullptr, *bracket = nullptr, **tail = &quote;
    bool angled = false;
    for (auto *dir = reader->quote_include; dir;) {
        auto *next = dir->next;
        if (dir == reader->bracket_include) angled = true;
        if (dir->user_supplied_p) {
            *tail = dir; tail = &dir->next;
            if (angled && !bracket) bracket = dir;
        }
        dir = next;
    }
    *tail = nullptr;
    reader->quote_include = quote;
    reader->bracket_include = bracket;
}
