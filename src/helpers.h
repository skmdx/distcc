#pragma once
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif
int dcc_is_launcher(const char *basename);
int dcc_is_distcc_path(const char *path);
/* Return the override or write the absolute path of a private executable. */
char *dcc_helper_path(const char *name, char path[PATH_MAX]);
#ifdef __cplusplus
}
#endif
