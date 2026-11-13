/* Local planner dependency journal. No compiler state or input bytes are cached. */
#pragma once
#include <stdint.h>
#include <sys/stat.h>

enum dcc_plan_operation { DCC_PLAN_STAT=1, DCC_PLAN_LSTAT, DCC_PLAN_ACCESS,
    DCC_PLAN_OPEN, DCC_PLAN_DIRECTORY, DCC_PLAN_LINK, DCC_PLAN_REALPATH, DCC_PLAN_STREAM,
    DCC_PLAN_ACCESS_AT };
struct dcc_plan_dependency {
    uint32_t operation, flags, length, target_length;
    int32_t error;
    struct stat metadata;
};
#define DCC_PLAN_JOURNAL_LIMIT 24576
static inline int dcc_plan_stat_equal(unsigned operation, const struct stat *a, const struct stat *b)
{
    if (a->st_dev!=b->st_dev || a->st_ino!=b->st_ino || a->st_mode!=b->st_mode ||
        a->st_uid!=b->st_uid || a->st_gid!=b->st_gid) return 0;
    /* GCC search-path probes test directory existence/type/identity, not its
     * entries. Outputs created beside headers must not invalidate these probes.
     * opendir/open still retain the content version; specific child probes and
     * access checks independently retain missing names and search permissions. */
    if (S_ISDIR(a->st_mode) && (operation==DCC_PLAN_STAT || operation==DCC_PLAN_LSTAT ||
                              operation==DCC_PLAN_REALPATH)) return 1;
    return a->st_nlink==b->st_nlink &&
        a->st_size==b->st_size && a->st_mtim.tv_sec==b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec==b->st_mtim.tv_nsec && a->st_ctim.tv_sec==b->st_ctim.tv_sec &&
        a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
/* Header bytes: driver finished, planner finished, recorder invalid, reserved. */
#ifdef __cplusplus
extern "C" {
#endif
void dcc_plan_record_begin(int descriptor, int driver);
void dcc_plan_record_finish(int status);
#ifndef __cplusplus
char *dcc_plan_key(char **argv, const char *input);
int dcc_plan_memo_decode(const char *memo, int output, unsigned *dependencies);
char *dcc_plan_memo_encode(int journal, int output, unsigned *dependencies);
#endif
#ifdef __cplusplus
}
#endif
