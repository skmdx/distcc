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

using input_callback = bool (*)(const char *, const struct stat *, void *);

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
    visitor.ok = visitor.callback(path, nullptr, visitor.context);
    return visitor.ok;
}

extern "C" __attribute__((visibility("default")))
bool distcc_gcc_inputs(cpp_reader *reader, input_callback callback, void *context)
{
    if (!reader || !reader->main_file) return false;
    for (const auto *file = reader->all_files; file; file = file->next_file) {
        if (file->embed || file->pchname || file->header_unit > 0 ||
            (file != reader->main_file && !file->implicit_preinclude &&
             file->name && file->name[0] == '/')) return false;
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
            !callback(file->path, &stamp, context)) return false;
    }
    input_visitor visitor{callback, context, true};
    htab_traverse(reader->nonexistent_file_hash, missing_input, &visitor);
    return visitor.ok;
}
