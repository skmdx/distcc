/* The namespace of an LLVM index belongs to its producer. Physical names
 * below are private snapshots or descriptors, never names from the index. */
#pragma once
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Error.h>
#include <vector>

struct LtoInputs {
    std::string cwd;
    std::string output;
    llvm::StringMap<int> files;
    std::string manifest;
    std::string localIndex;
    unsigned partitions = 0;
    llvm::Expected<int> lookup(llvm::StringRef name) const;
};
int dcc_lto_compile(const LtoInputs &, llvm::ArrayRef<const char *>, int output);
std::vector<std::string> dcc_lto_cc1(llvm::ArrayRef<const char *> driver);
llvm::Error dcc_localize_elf(int fd, llvm::ArrayRef<std::string> names, llvm::raw_ostream &output);
llvm::Error dcc_link_elf(llvm::ArrayRef<const char *> args, int capture);
bool dcc_lto_init(bool client);
int dcc_lto_local(const char *description, unsigned operation);
int dcc_lto_plan(const char *description, int output);
int dcc_lto_remote(int command, unsigned operation);
