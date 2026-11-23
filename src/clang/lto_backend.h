/* The namespace of an LLVM index belongs to its producer. Physical names
 * below are private snapshots or descriptors, never names from the index. */
#pragma once
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Error.h>

struct LtoInputs {
    std::string cwd;
    std::string output;
    llvm::StringMap<int> files;
    std::string manifest;
    unsigned partitions = 0;
    llvm::Expected<int> lookup(llvm::StringRef name) const;
};
int dcc_lto_compile(const LtoInputs &, llvm::ArrayRef<const char *>, int output);
bool dcc_lto_init(bool client);
int dcc_lto_local(const char *description, unsigned operation);
int dcc_lto_plan(const char *description, int output);
int dcc_lto_remote(int command, unsigned operation);
