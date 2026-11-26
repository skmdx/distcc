/* Per-user broker discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *dcc_broker_path(void);
const char *dcc_broker_path_in(const char *run_dir);
#ifdef __cplusplus
}
#endif
