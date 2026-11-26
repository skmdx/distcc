/* Per-user broker discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "broker_path.h"

const char *dcc_broker_path_in(const char *run_dir)
{
    const char *configured = getenv("DISTCC_BROKER");
    if (configured) return *configured ? configured : NULL;
    if (getenv("DISTCC_HOSTS") || getenv("DISTCC_DIR")) return NULL;
    const char *home = getenv("HOME");
    if (home) {
        char hosts[PATH_MAX];
        int length = snprintf(hosts, sizeof(hosts), "%s/.distcc/hosts", home);
        if (length >= 0 && length < (int)sizeof(hosts) && access(hosts, R_OK) == 0) return NULL;
    }
    uid_t uid = geteuid();
    static _Thread_local char path[PATH_MAX];
    struct passwd entry, *user;
    for (size_t size = 1024; size <= SIZE_MAX / 2; size *= 2) {
        char *buffer = malloc(size);
        if (!buffer) return NULL;
        int rc = getpwuid_r(uid, &entry, buffer, size, &user);
        int length = -1;
        if (!rc && user && user->pw_name && *user->pw_name && !strchr(user->pw_name, '/'))
            length = snprintf(path, sizeof(path), "%s/distcc-broker-%s.socket", run_dir, user->pw_name);
        free(buffer);
        if (rc == ERANGE) continue;
        struct stat st;
        return length >= 0 && length < (int)sizeof(path) && !lstat(path, &st) &&
               S_ISSOCK(st.st_mode) && st.st_uid == uid && !(st.st_mode & 0077) ? path : NULL;
    }
    return NULL;
}

const char *dcc_broker_path(void)
{
    return dcc_broker_path_in("/run");
}
