/* One private seqpacket per CGU, carrying input, output and reply descriptors. */
#pragma once
#include "workload.h"

struct dcc_cgu_request {
    char version[sizeof(dcc_work_hint::cgu_backend)];
    char target[sizeof(dcc_work_hint::cgu_backend)];
    char phase[sizeof("thin-postlink")];
};
