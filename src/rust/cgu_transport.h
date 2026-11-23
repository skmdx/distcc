/* Each request carries input and reply descriptors. A successful reply carries the object. */
#pragma once
#include "workload.h"

struct dcc_cgu_request {
    char version[sizeof(dcc_work_hint::cgu_backend)];
    char target[sizeof(dcc_work_hint::cgu_backend)];
    char phase[sizeof("thin-postlink")];
    uint64_t identity; /* Hash of the rustc CGU module identifier. */
};
