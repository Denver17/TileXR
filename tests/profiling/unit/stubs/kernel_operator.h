#ifndef TILEXR_PROFILING_TEST_KERNEL_OPERATOR_H
#define TILEXR_PROFILING_TEST_KERNEL_OPERATOR_H

// CPU substitute for record bounds and OFF behavior only; no cache, pipeline,
// timing or device resource model is implied.
#include <cstdint>
#define __aicore__
#define __gm__

namespace AscendC {
inline uint64_t &CycleReads()
{
    static uint64_t reads = 0U;
    return reads;
}
inline uint64_t GetSystemCycle() { return ++CycleReads(); }
inline uint32_t GetBlockIdx() { return 0U; }
}

#endif
