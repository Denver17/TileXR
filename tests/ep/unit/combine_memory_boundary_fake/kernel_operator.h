#ifndef TILEXR_COMBINE_BOUNDARY_FAKE_KERNEL_OPERATOR_H
#define TILEXR_COMBINE_BOUNDARY_FAKE_KERNEL_OPERATOR_H
// Bounded sparse CPU GM and UB substitutes. No hardware cache/timing claim.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>
#define __aicore__
#define __global__
#define __gm__
#define GM_ADDR uint8_t *
namespace AscendC {
extern uint32_t testCore;
void ReadGm(uintptr_t address, void *data, uint32_t bytes);
void WriteGm(uintptr_t address, const void *data, uint32_t bytes);
inline uint32_t GetBlockIdx() { return testCore; }
inline std::vector<std::function<void()>> &PendingWrites() { static std::vector<std::function<void()>> writes; return writes; }
enum Pipeline { PIPE_ALL };
template<Pipeline> void PipeBarrier()
{
    for (const auto &write : PendingWrites()) write();
    PendingWrites().clear();
}
enum class TPosition { VECCALC };
template<class T> struct LocalTensor { T *data; uint32_t bytes; };
template<class T> struct GlobalTensor {
    uintptr_t address = 0;
    uint32_t count = 0;
    void SetGlobalBuffer(T *pointer, uint32_t elements) { address = reinterpret_cast<uintptr_t>(pointer); count = elements; }
    GlobalTensor operator[](uint32_t offset) const
    {
        if (offset > count) throw std::runtime_error("GM offset outside declared tensor");
        return {address + offset * sizeof(T), count - offset};
    }
};
template<TPosition> struct TBuf {
    std::vector<uint32_t> data;
    template<class T> LocalTensor<T> Get() { return {reinterpret_cast<T *>(data.data()), uint32_t(data.size() * 4)}; }
};
struct TPipe {
    template<TPosition P> void InitBuffer(TBuf<P> &buffer, uint32_t bytes)
    {
        if (bytes != 32) throw std::runtime_error("unexpected boundary UB capacity");
        buffer.data.assign(bytes / 4, 0xcdcdcdcd);
    }
};
template<class T> void DataCopy(LocalTensor<T> destination, GlobalTensor<T> source, uint32_t count)
{
    if (count > source.count || count * sizeof(T) > destination.bytes || !PendingWrites().empty())
        throw std::runtime_error("GM-to-UB bounds or missing pipeline completion");
    ReadGm(source.address, destination.data, count * sizeof(T));
}
template<class T> void DataCopy(GlobalTensor<T> destination, LocalTensor<T> source, uint32_t count)
{
    if (count > destination.count || count * sizeof(T) > source.bytes)
        throw std::runtime_error("UB-to-GM bounds");
    PendingWrites().push_back([=] { WriteGm(destination.address, source.data, count * sizeof(T)); });
}
template<class T> void Duplicate(LocalTensor<T> destination, T value, uint32_t count)
{
    if (count * sizeof(T) > destination.bytes) throw std::runtime_error("Duplicate UB bounds");
    std::fill(destination.data, destination.data + count, value);
}
}
#endif
