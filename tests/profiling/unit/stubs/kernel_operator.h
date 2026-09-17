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
inline uint64_t &CacheCleanCalls()
{
    static uint64_t calls = 0U;
    return calls;
}
inline uint32_t GetBlockIdx() { return 0U; }
template <typename T> struct GlobalTensor {
    void SetGlobalBuffer(T *, uint32_t) {}
};
enum class CacheLine { SINGLE_CACHE_LINE };
enum class DcciDst { CACHELINE_OUT };
template <typename T, CacheLine, DcciDst>
inline void DataCacheCleanAndInvalid(const GlobalTensor<T> &) { ++CacheCleanCalls(); }
}

#endif
