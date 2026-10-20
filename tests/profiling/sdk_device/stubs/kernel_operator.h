// CPU-only Ascend C surface substitute. This cannot model device visibility,
// register allocation, scalar cache behavior, or instruction generation.
#ifndef TILEXR_PROFILING_TEST_KERNEL_OPERATOR_H
#define TILEXR_PROFILING_TEST_KERNEL_OPERATOR_H
#include <cstdint>
#define __aicore__
#define __gm__
using GM_ADDR = unsigned char *;
namespace AscendC {
inline uint64_t &ProfilingTestCycle()
{
    static uint64_t cycle = 0;
    return cycle;
}
inline uint64_t GetSystemCycle() { return ProfilingTestCycle(); }
}
#endif
