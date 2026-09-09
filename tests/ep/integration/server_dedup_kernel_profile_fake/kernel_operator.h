#pragma once
// CPU-only Ascend C API substitute for the separately compiled kernel behavior test.
#include <cstdint>
#include <cstddef>
#define __aicore__
#define __global__
#define __gm__
using GM_ADDR = uint8_t *;
namespace AscendC {
extern uint32_t testCore;
extern uint32_t testCores;
extern uint64_t testTicks;
extern uint32_t testCleans;
inline uint64_t GetSystemCycle() { return ++testTicks; }
inline uint32_t GetBlockIdx() { return testCore; }
inline uint32_t GetBlockNum() { return testCores; }
enum class CacheLine { SINGLE_CACHE_LINE };
enum class DcciDst { CACHELINE_OUT };
template<typename T> struct GlobalTensor {
    T *pointer;
    void SetGlobalBuffer(T *p, std::size_t) { pointer = p; }
    T &operator[](std::size_t i) { return pointer[i]; }
};
template<typename T, CacheLine, DcciDst> void DataCacheCleanAndInvalid(T &) { ++testCleans; }
}
