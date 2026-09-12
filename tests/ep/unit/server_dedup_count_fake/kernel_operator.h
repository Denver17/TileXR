#pragma once
// Synchronous CPU substitutes for exercising the production count arithmetic and
// strided GM protocol. They do not model device events, caches or MTE ordering.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#define __aicore__
#define __global__
#define __gm__
#define GM_ADDR uint8_t *

namespace AscendC {
extern uint32_t testCore;
extern uint64_t testCopies;
inline uint32_t GetBlockIdx() { return testCore; }
inline uint32_t GetBlockNum() { return 64; }
template<class A, class B> inline uint32_t Ceil(A a, B b) { return uint32_t((a + b - 1) / b); }
enum class HardEvent { MTE2_V, MTE2_S, V_MTE2, V_S, S_V, S_MTE3, V_MTE3, MTE3_V, MTE3_S };
enum Pipeline { PIPE_ALL, PIPE_V, PIPE_MTE3 };
using TEventID = uint32_t;
template<HardEvent> void SetFlag(TEventID) {}
template<HardEvent> void WaitFlag(TEventID) {}
template<Pipeline> void PipeBarrier() {}
template<int, int> int64_t GetCtrlSpr() { return 0; }
template<int, int> void SetCtrlSpr(int64_t) {}

struct Storage { std::vector<uint8_t> bytes; bool alive = true; };
template<class T> struct LocalTensor {
    std::shared_ptr<Storage> storage;
    std::size_t offset = 0;
    uint8_t *Data(std::size_t bytes) const {
        if (!storage || !storage->alive || offset + bytes > storage->bytes.size())
            throw std::runtime_error("UB bounds/lifetime violation");
        return storage->bytes.data() + offset;
    }
    T GetValue(std::size_t i) const { T value; std::memcpy(&value, Data((i + 1) * sizeof(T)) + i * sizeof(T), sizeof(T)); return value; }
    void SetValue(std::size_t i, T value) const { std::memcpy(Data((i + 1) * sizeof(T)) + i * sizeof(T), &value, sizeof(T)); }
    T operator()(std::size_t i) const { return GetValue(i); }
    LocalTensor operator[](std::size_t i) const { return {storage, offset + i * sizeof(T)}; }
    template<class U> LocalTensor<U> ReinterpretCast() const { return {storage, offset}; }
};
template<class T> struct GlobalTensor {
    T *pointer = nullptr;
    void SetGlobalBuffer(T *p) { pointer = p; }
    GlobalTensor operator[](std::size_t i) const { return {pointer + i}; }
};
template<int = 0> struct TBuf {
    std::shared_ptr<Storage> storage;
    template<class T> LocalTensor<T> Get() { return {storage, 0}; }
    template<class T> LocalTensor<T> GetWithOffset(uint32_t, uint32_t offset) { return {storage, offset}; }
};
struct TPipe {
    std::vector<std::shared_ptr<Storage>> buffers;
    uint64_t bytes = 0;
    void InitBuffer(TBuf<> &buf, uint64_t size) {
        if (!size || size % 32 || bytes + size > 190 * 1024) throw std::runtime_error("UB allocation overflow/alignment");
        buf.storage = std::make_shared<Storage>();
        buf.storage->bytes.assign(size, 0xcd);
        buffers.push_back(buf.storage);
        bytes += size;
    }
    TEventID FetchEventID(HardEvent) { return 0; }
    void Reset() { for (auto &b : buffers) b->alive = false; buffers.clear(); bytes = 0; }
};
struct DataCopyParams { uint16_t blockCount, blockLen, srcStride, dstStride; };
struct DataCopyExtParams { uint16_t blockCount; uint32_t blockLen, srcStride, dstStride, rsv; };
template<class T> struct DataCopyPadExtParams { bool isPad; uint8_t leftPadding, rightPadding; T paddingValue; };
template<class T> void DataCopy(LocalTensor<T> dst, GlobalTensor<T> src, DataCopyParams p) {
    ++testCopies;
    for (uint32_t b = 0; b < p.blockCount; ++b) {
        const auto to = dst[uint64_t(b) * (p.blockLen + p.dstStride) * 32 / sizeof(T)];
        std::memcpy(to.Data(p.blockLen * 32), reinterpret_cast<uint8_t *>(src.pointer) +
            uint64_t(b) * (p.blockLen + p.srcStride) * 32, p.blockLen * 32);
    }
}
template<class T> void DataCopy(GlobalTensor<T> dst, LocalTensor<T> src, DataCopyParams p) {
    ++testCopies;
    for (uint32_t b = 0; b < p.blockCount; ++b) {
        const auto from = src[uint64_t(b) * (p.blockLen + p.srcStride) * 32 / sizeof(T)];
        std::memcpy(reinterpret_cast<uint8_t *>(dst.pointer) + uint64_t(b) * (p.blockLen + p.dstStride) * 32,
            from.Data(p.blockLen * 32), p.blockLen * 32);
    }
}
template<class T> void DataCopy(LocalTensor<T> dst, GlobalTensor<T> src, uint32_t count) {
    ++testCopies; std::memcpy(dst.Data(count * sizeof(T)), src.pointer, count * sizeof(T));
}
template<class T> void DataCopy(GlobalTensor<T> dst, LocalTensor<T> src, uint32_t count) {
    ++testCopies; std::memcpy(dst.pointer, src.Data(count * sizeof(T)), count * sizeof(T));
}
template<class T> void DataCopyPad(LocalTensor<T> dst, GlobalTensor<T> src, DataCopyExtParams p, DataCopyPadExtParams<T>) {
    if (p.blockCount != 1) throw std::runtime_error("unexpected padded block count");
    ++testCopies; std::memcpy(dst.Data(p.blockLen), src.pointer, p.blockLen);
}
template<class T> void DataCopyPad(GlobalTensor<T> dst, LocalTensor<T> src, DataCopyExtParams p) {
    if (p.blockCount != 1) throw std::runtime_error("unexpected padded block count");
    ++testCopies; std::memcpy(dst.pointer, src.Data(p.blockLen), p.blockLen);
}
template<class T> void Duplicate(LocalTensor<T> dst, T value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) dst.SetValue(i, value);
}
template<class T> void Duplicate(LocalTensor<T> dst, T value, const uint64_t mask[2], uint32_t repeats, uint32_t blockStride, uint32_t repeatStride) {
    for (uint32_t r = 0; r < repeats; ++r) for (uint32_t i = 0; i < 256 / sizeof(T); ++i)
        if ((mask[i / 64] >> (i % 64)) & 1U)
            dst.SetValue(r * repeatStride * 32 / sizeof(T) + (i / (32 / sizeof(T))) * blockStride * (32 / sizeof(T)) + i % (32 / sizeof(T)), value);
}
template<class T> void Duplicate(LocalTensor<T> dst, T value, uint32_t mask, uint32_t repeats, uint32_t blockStride, uint32_t repeatStride) {
    uint64_t bits[2] = {mask >= 64 ? UINT64_MAX : (uint64_t(1) << mask) - 1, 0};
    Duplicate(dst, value, bits, repeats, blockStride, repeatStride);
}
template<class T> void Sub(LocalTensor<T> dst, LocalTensor<T> a, LocalTensor<T> b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst.SetValue(i, a.GetValue(i) - b.GetValue(i));
}
template<class T> void Abs(LocalTensor<T> dst, LocalTensor<T> src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst.SetValue(i, std::abs(src.GetValue(i)));
}
template<class T> void Mins(LocalTensor<T> dst, LocalTensor<T> src, T value, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst.SetValue(i, std::min(src.GetValue(i), value));
}
template<class T> void ReduceSum(LocalTensor<T> dst, LocalTensor<T> src, LocalTensor<T>, uint32_t n) {
    T sum = 0; for (uint32_t i = 0; i < n; ++i) sum += src.GetValue(i); dst.SetValue(0, sum);
}
template<class T> void ReduceSum(LocalTensor<T> dst, LocalTensor<T> src, LocalTensor<T>, uint32_t mask, uint32_t repeats, uint32_t stride) {
    T sum = 0;
    for (uint32_t r = 0; r < repeats; ++r) for (uint32_t i = 0; i < mask; ++i)
        sum += src.GetValue(r * stride * 32 / sizeof(T) + i);
    dst.SetValue(0, sum);
}
struct SumParams { uint32_t outter, inner, n; };
template<class T> void Sum(LocalTensor<T> dst, LocalTensor<T> src, SumParams p) {
    T sum = 0; for (uint32_t i = 0; i < p.n; ++i) sum += src.GetValue(i); dst.SetValue(0, sum);
}
enum class CMPMODE { GE, LT };
template<class T> void Compares(LocalTensor<uint8_t> dst, LocalTensor<T> src, T value, CMPMODE mode, uint32_t n) {
    Duplicate<uint8_t>(dst, 0, (n + 7) / 8);
    for (uint32_t i = 0; i < n; ++i) if (mode == CMPMODE::GE ? src.GetValue(i) >= value : src.GetValue(i) < value)
        dst.SetValue(i / 8, dst.GetValue(i / 8) | (1U << (i % 8)));
}
inline void And(LocalTensor<uint16_t> dst, LocalTensor<uint16_t> a, LocalTensor<uint16_t> b, int32_t n) {
    for (int32_t i = 0; i < n; ++i) dst.SetValue(i, a.GetValue(i) & b.GetValue(i));
}
struct GatherMaskParams { uint16_t src0BlockStride, repeatTimes, src0RepeatStride, src1RepeatStride; };
template<class T> void GatherMask(LocalTensor<T> dst, LocalTensor<T> src, LocalTensor<uint32_t> mask,
    bool, uint32_t count, GatherMaskParams p, uint64_t &selected) {
    selected = 0;
    for (uint32_t r = 0; r < p.repeatTimes; ++r) for (uint32_t i = 0; i < count; ++i)
        if ((mask.GetValue(i / 32) >> (i % 32)) & 1U)
            dst.SetValue(selected++, src.GetValue(r * p.src0RepeatStride * 32 / sizeof(T) + i));
}
} // namespace AscendC
