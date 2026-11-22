/* Auxiliary compiler inputs, owned and interpreted by Clang.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_CLANG_INPUTS_H
#define DCC_CLANG_INPUTS_H
#include <clang/Basic/CodeGenOptions.h>
template<class Visitor>
void dcc_clang_profile_inputs(clang::CodeGenOptions &options, Visitor visit) {
    for (auto *path : {&options.ProfileInstrumentUsePath, &options.SampleProfileFile,
                       &options.MemoryProfileUsePath, &options.ProfileRemappingFile})
        if (!path->empty()) visit(*path);
}
#endif
