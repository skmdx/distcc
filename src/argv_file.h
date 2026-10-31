/* Complete compiler command files shared by the C client and LLVM helpers. */
#ifndef DCC_ARGV_FILE_H
#define DCC_ARGV_FILE_H

struct dcc_argv_file {
    char *bytes;
    char ***groups;
    unsigned count;
};
/* Group vectors borrow strings from bytes. Close the owner, never free the
 * individual strings. An unsuccessful read leaves an empty owner. */
int dcc_argv_file_read(int, unsigned, struct dcc_argv_file *);
void dcc_argv_file_close(struct dcc_argv_file *);
struct dcc_argv_file_api {
    int (*read)(int, unsigned, struct dcc_argv_file *);
    void (*close)(struct dcc_argv_file *);
    int (*write)(int, const char *, const char *, char **);
};
#endif
