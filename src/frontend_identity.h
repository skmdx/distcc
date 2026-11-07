/* Cache authority follows the client's boot and read credentials.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
static int dcc_compare_gid(const void *a, const void *b)
{
    gid_t x = *(const gid_t *)a, y = *(const gid_t *)b;
    return (x > y) - (x < y);
}
static char *dcc_frontend_identity(void)
{
    char boot[64];
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!file) return NULL;
    int ok = fgets(boot, sizeof(boot), file) != NULL;
    fclose(file);
    int count = getgroups(0, NULL);
    if (!ok || count < 0) return NULL;
    gid_t *groups = (gid_t *)malloc((count + 1) * sizeof(*groups));
    if (!groups) return NULL;
    if (getgroups(count, groups) != count) { free(groups); return NULL; }
    qsort(groups, count, sizeof(*groups), dcc_compare_gid);
    size_t capacity = sizeof(boot) + 80 + (size_t)count * 24;
    char *identity = (char *)malloc(capacity);
    if (identity) {
        size_t used = snprintf(identity, capacity, "distcc frontend files:%s:%lu:%lu",
                               boot, (unsigned long)geteuid(), (unsigned long)getegid());
        for (int i = 0; i < count; ++i)
            used += snprintf(identity + used, capacity - used, ":%lu", (unsigned long)groups[i]);
    }
    free(groups);
    return identity;
}
