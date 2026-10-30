/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_CLANG_CODEGEN_H
#define DCC_CLANG_CODEGEN_H
#include <optional>
namespace clang { class CompilerInstance; }

// Serial execution has no split/merge cost. Beyond two parts the model is
// convex, so stop as soon as another part no longer reduces completion time.
// Until overhead has been observed, two parts are the smallest measurement.
inline unsigned dcc_codegen_parallelism(double serial, double overhead,
                                       unsigned useful, bool overhead_known) {
    if (!(serial > 0) || useful < 2) return 1;
    if (!overhead_known) return 2;
    unsigned selected = 1;
    double shortest = serial;
    for (unsigned parts = 2; parts <= useful; ++parts) {
        double seconds = serial / parts + overhead * parts;
        if (seconds >= shortest) break;
        selected = parts;
        shortest = seconds;
    }
    return selected;
}

void dcc_clang_codegen_init();
std::optional<bool> dcc_clang_codegen(clang::CompilerInstance &);
#endif
