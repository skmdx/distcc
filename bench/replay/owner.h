/* Same-host canary IPC uses the host's public stat ABI, never a wire ABI. */
#pragma once
#include <sys/stat.h>
struct owner_reply {
    int error;
    struct stat attributes;
};
