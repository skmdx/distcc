/* The native helpers consume the same command codec as the C client. */
#ifndef DCC_COMPILER_COMMAND_H
#define DCC_COMPILER_COMMAND_H
extern "C" {
#include "../argv_file.h"
}

class CompilerCommand {
    dcc_argv_file file{};
public:
    CompilerCommand() = default;
    CompilerCommand(const CompilerCommand &) = delete;
    CompilerCommand &operator=(const CompilerCommand &) = delete;
    ~CompilerCommand() { dcc_argv_file_close(&file); }
    bool read(int fd, unsigned groups) {
        return !dcc_argv_file_read(fd, groups, &file);
    }
    char **operator[](unsigned group) const { return file.groups[group]; }
};
#endif
