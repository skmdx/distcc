/* Boolean environment options. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdlib.h>
#include <string.h>

static inline int dcc_getenv_bool(const char *name, int default_value)
{
    const char *value = getenv(name);
    if (value && !strcmp(value, "0")) return 0;
    if (value && !strcmp(value, "1")) return 1;
    return default_value;
}
