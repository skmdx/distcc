/* The calling thread's complete Linux affinity mask, including large hosts. */
#pragma once
#include <cerrno>
#include <sched.h>
#include <vector>

namespace rust {
using CpuMask = std::vector<unsigned long>;
inline CpuMask affinity() {
    CpuMask mask(sizeof(cpu_set_t) / sizeof(unsigned long));
    while (sched_getaffinity(0, mask.size() * sizeof(mask[0]), reinterpret_cast<cpu_set_t *>(mask.data()))) {
        if (errno != EINVAL)
            return {};
        mask.resize(mask.size() * 2);
    }
    return mask;
}
}
