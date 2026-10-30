/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_CLANG_CODEGEN_H
#define DCC_CLANG_CODEGEN_H
#include <optional>
namespace clang { class CompilerInstance; }
void dcc_clang_codegen_init();
std::optional<bool> dcc_clang_codegen(clang::CompilerInstance &);
#endif
