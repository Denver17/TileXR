#pragma once
// CPU substitutes for production count arithmetic and the strided GM protocol.
// Optional deferred source reads exercise UB ownership; this is not device timing
// or a cache/coherence model.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <array>
#include <functional>

#define __aicore__
#define __global__
#if defined(TILEXR_TEST_GM_ADDRESS_SPACE)
// Clang syntax checks preserve GM pointer types; CPU execution leaves them flat.
#define __gm__ __attribute__((address_space(1)))
#else
#define __gm__
#endif
#define __ubuf__
#define __mix__(a, b)
#define GM_ADDR __gm__ uint8_t *
using half = uint16_t;
using bfloat16_t = uint16_t;

namespace AscendC {
extern uint32_t testCore;
extern uint64_t testCopies;
// Map sparse synthetic GM addresses to bounded Host storage. Real pointer inputs
// stay unchanged; this lets tests cover >4 GiB offsets without huge allocations.
inline std::function<void *(uintptr_t)> &GmAddressResolver() {
    static std::function<void *(uintptr_t)> resolver;
    return resolver;
}
inline void *ResolveGm(uintptr_t address) {
    return GmAddressResolver() ? GmAddressResolver()(address) : reinterpret_cast<void *>(address);
}
inline std::function<void(const void *, const void *, uint32_t)> &GmWriteObserver() {
    static std::function<void(const void *, const void *, uint32_t)> observer;
    return observer;
}
inline uint32_t GetBlockIdx() { return testCore; }
inline std::function<void(const void *, uint32_t)> &GmReadObserver() {
    static std::function<void(const void *, uint32_t)> observer;
    return observer;
}
inline uint32_t GetBlockNum() { return 64; }
inline uint64_t GetSystemCycle() { static uint64_t ticks = 100; return ++ticks; }
inline uint64_t &TestSyncOperations() { static uint64_t count = 0; return count; }
template<class A, class B> inline uint32_t Ceil(A a, B b) { return uint32_t((a + b - 1) / b); }
enum class HardEvent { MTE2_V, MTE2_S, V_MTE2, V_S, S_V, S_MTE3, V_MTE3, MTE3_V, MTE3_S, MTE2_MTE3, MTE3_MTE2 };
enum Pipeline { PIPE_ALL, PIPE_V, PIPE_MTE3 };
inline bool &DeferMte3() { static bool defer = false; return defer; }
inline std::vector<std::function<void()>> &PendingMte3() {
    static std::vector<std::function<void()>> pending;
    return pending;
}
inline void CompleteMte3() {
    auto pending = std::move(PendingMte3());
    PendingMte3().clear();
    for (const auto &copy : pending) copy();
}
inline void IssueMte3(std::function<void()> copy) {
    if (DeferMte3()) PendingMte3().push_back(std::move(copy));
    else copy();
}
using TEventID = uint32_t;
inline std::function<void(HardEvent, TEventID, bool)> &EventObserver() {
    static std::function<void(HardEvent, TEventID, bool)> observer;
    return observer;
}
inline std::function<void(const void *, const void *, uint32_t)> &PaddedWriteObserver() {
    static std::function<void(const void *, const void *, uint32_t)> observer;
    return observer;
}
template<HardEvent> std::array<bool, 8> &Events() { static std::array<bool, 8> state {}; return state; }
template<HardEvent E> void SetFlag(TEventID id) {
    ++TestSyncOperations();
    if (id >= 8 || Events<E>()[id]) throw std::runtime_error("duplicate or invalid event publication");
    Events<E>()[id] = true;
    if (EventObserver()) EventObserver()(E, id, true);
}
template<HardEvent E> void WaitFlag(TEventID id) {
    ++TestSyncOperations();
    if (id >= 8 || !Events<E>()[id]) throw std::runtime_error("waiting on an unpublished event");
    Events<E>()[id] = false;
    if (E == HardEvent::MTE3_V || E == HardEvent::MTE3_S || E == HardEvent::MTE3_MTE2)
        CompleteMte3();
    if (EventObserver()) EventObserver()(E, id, false);
}
template<Pipeline P> void PipeBarrier() {
    ++TestSyncOperations();
    if (P == PIPE_ALL || P == PIPE_MTE3) CompleteMte3();
}
inline uint64_t &TestCtrlSpr() { static uint64_t value = UINT64_C(0x1000000000000008); return value; }
template<int First, int Last> int64_t GetCtrlSpr() {
    return (TestCtrlSpr() >> First) & ((UINT64_C(1) << (Last - First + 1)) - 1U);
}
template<int First, int Last> void SetCtrlSpr(int64_t value) {
    const uint64_t mask = ((UINT64_C(1) << (Last - First + 1)) - 1U) << First;
    TestCtrlSpr() = (TestCtrlSpr() & ~mask) | ((uint64_t(value) << First) & mask);
}
inline void SyncAll() {}
template<class T> inline T AtomicAdd(__gm__ T *address, T value) {
    T *mapped = static_cast<T *>(ResolveGm(reinterpret_cast<uintptr_t>(address)));
    T old = *mapped;
    *mapped += value;
    return old;
}
enum class RoundMode { CAST_TRUNC, CAST_RINT };
struct fp8_e4m3fn_t {};
struct fp8_e5m2_t {};

struct Storage { std::vector<uint8_t> bytes; bool alive = true; };
template<class T> struct LocalTensor {
    std::shared_ptr<Storage> storage;
    std::size_t offset = 0;
    std::size_t length = SIZE_MAX;
    uint8_t *Data(std::size_t bytes) const {
        if (!storage || !storage->alive || bytes > length || offset + bytes > storage->bytes.size())
            throw std::runtime_error("UB bounds/lifetime violation");
        return storage->bytes.data() + offset;
    }
    T GetValue(std::size_t i) const { T value; std::memcpy(&value, Data((i + 1) * sizeof(T)) + i * sizeof(T), sizeof(T)); return value; }
    void SetValue(std::size_t i, T value) const { std::memcpy(Data((i + 1) * sizeof(T)) + i * sizeof(T), &value, sizeof(T)); }
    T operator()(std::size_t i) const { return GetValue(i); }
    uint64_t GetPhyAddr() const { return reinterpret_cast<uint64_t>(Data(0)); }
    LocalTensor operator[](std::size_t i) const {
        if (i * sizeof(T) > length) throw std::runtime_error("UB tensor offset overflow");
        return {storage, offset + i * sizeof(T), length - i * sizeof(T)};
    }
    template<class U> LocalTensor<U> ReinterpretCast() const { return {storage, offset, length}; }
};
template<class T> struct GlobalTensor {
    T *pointer = nullptr;
    void SetGlobalBuffer(__gm__ T *p, std::size_t = 0) {
        // Enforce the device API's GM argument before mapping to CPU backing.
        pointer = static_cast<T *>(ResolveGm(reinterpret_cast<uintptr_t>(p)));
    }
    GlobalTensor operator[](std::size_t i) const { return {pointer + i}; }
};
enum class CacheLine { SINGLE_CACHE_LINE };
enum class DcciDst { CACHELINE_OUT };
template<class T, CacheLine, DcciDst> void DataCacheCleanAndInvalid(GlobalTensor<T>) {}
template<int = 0> struct TBuf {
    std::shared_ptr<Storage> storage;
    std::size_t offset = 0, bytes = 0;
    template<class T> LocalTensor<T> Get() { return {storage, offset, bytes}; }
    template<class T> LocalTensor<T> GetWithOffset(uint32_t count, uint32_t relativeOffset) {
        if (relativeOffset + uint64_t(count) * sizeof(T) > bytes)
            throw std::runtime_error("UB buffer offset overflow");
        return {storage, offset + relativeOffset, bytes - relativeOffset};
    }
};
enum class QuePosition { VECIN };
enum class TPosition { VECIN, VECCALC };
template<QuePosition, int> struct TQue {
    std::vector<LocalTensor<uint8_t>> slots;
    uint32_t next = 0, state = 0;
    template<class T> LocalTensor<T> AllocTensor() {
        if (state != 0) throw std::runtime_error("queue slot reused before FreeTensor");
        state = 1; return slots.at(next).template ReinterpretCast<T>();
    }
    template<class T> void EnQue(LocalTensor<T>) {
        if (state != 1) throw std::runtime_error("queue enqueue order");
        state = 2;
    }
    template<class T> LocalTensor<T> DeQue() {
        if (state != 2) throw std::runtime_error("queue dequeue order");
        state = 3; return slots.at(next).template ReinterpretCast<T>();
    }
    template<class T> void FreeTensor(LocalTensor<T>) {
        if (state != 3) throw std::runtime_error("queue free order");
        state = 0; next = (next + 1) % slots.size();
    }
};
template<TPosition P, uint32_t N = 4> struct TBufPool {
    std::shared_ptr<Storage> storage;
    uint64_t base = 0, capacity = 0, used = 0;
    uint32_t buffers = 0;
    uint64_t Allocate(uint32_t size, uint32_t count = 1) {
        const uint32_t aligned = (size + 31U) / 32U * 32U;
        if (!size || used + uint64_t(aligned) * count > capacity || buffers + count > N)
            throw std::runtime_error("UB pool capacity/buffer overflow");
        const uint64_t offset = base + used;
        used += uint64_t(aligned) * count;
        buffers += count;
        if (storage->bytes.size() < base + used) storage->bytes.resize(base + used, 0xcd);
        return offset;
    }
    template<QuePosition Q, int M> void InitBuffer(TQue<Q, M> &q, uint32_t count, uint32_t size) {
        const uint64_t offset = Allocate(size, count);
        for (uint32_t i = 0; i < count; ++i)
            q.slots.push_back({storage, offset + uint64_t(i) * ((size + 31U) / 32U * 32U), size});
    }
    void InitBuffer(TBuf<> &b, uint32_t size) {
        b.offset = Allocate(size); b.bytes = size; b.storage = storage;
    }

};
struct TPipe {
    std::vector<std::shared_ptr<Storage>> buffers;
    uint64_t bytes = 0;
    void InitBuffer(TBuf<> &buf, uint64_t size) {
        if (!size || size % 32 || bytes + size > 190 * 1024) throw std::runtime_error("UB allocation overflow/alignment");
        buf.storage = std::make_shared<Storage>();
        buf.storage->bytes.assign(size, 0xcd);
        buf.offset = 0; buf.bytes = size;
        buffers.push_back(buf.storage);
        bytes += size;
    }
    TEventID FetchEventID(HardEvent) { return 0; }
    template<TPosition P, uint32_t N> void InitBufPool(TBufPool<P, N> &pool, uint32_t size) {
        if (!size || size % 32 || bytes + size > 190 * 1024) throw std::runtime_error("UB pool overflow/alignment");
        pool.storage = std::make_shared<Storage>();
        // Reserve stable addresses without touching all 190 KiB on thousands of simulated cores.
        pool.storage->bytes.reserve(size);
        pool.capacity = size;
        buffers.push_back(pool.storage);
        bytes += size;
    }
    template<TPosition P, uint32_t N, TPosition Q, uint32_t M>
    void InitBufPool(TBufPool<P, N> &pool, uint32_t size, TBufPool<Q, M> &other) {
        if (size > other.capacity || N > M) throw std::runtime_error("invalid UB pool alias");
        pool.base = other.base; pool.capacity = size; pool.storage = other.storage;
    }
    void Reset() {
        CompleteMte3();
        for (auto &b : buffers) b->alive = false;
        buffers.clear(); bytes = 0;
        // CANN 9.1 A5 InitSocStateImpl, also called by TPipe::Reset.
        TestCtrlSpr() = UINT64_C(0x1000000000000008) | (TestCtrlSpr() & (UINT64_C(1) << 48));
    }
};
struct DataCopyParams { uint16_t blockCount, blockLen, srcStride, dstStride; };
struct DataCopyExtParams { uint16_t blockCount; uint32_t blockLen, srcStride, dstStride, rsv; };
struct CopyRepeatParams { uint8_t dstBlkStride, srcBlkStride, dstRepStride, srcRepStride; };
template<class T> void Copy(LocalTensor<T> dst, LocalTensor<T> src, uint64_t mask, uint8_t repeats, CopyRepeatParams p) {
    // A5 CopyImpl rejects b8 UB-to-UB copies (CANN dav_3510 diagnostic).
    static_assert(sizeof(T) == 2 || sizeof(T) == 4, "UB Copy requires b16/b32 elements");
    if (mask == 0 || mask > 256 / sizeof(T)) throw std::runtime_error("invalid Copy element mask");
    for (uint32_t r = 0; r < repeats; ++r) for (uint32_t i = 0; i < mask; ++i) {
        const uint32_t block = i / (32 / sizeof(T)), within = i % (32 / sizeof(T));
        dst.SetValue((r * p.dstRepStride + block * p.dstBlkStride) * 32 / sizeof(T) + within,
            src.GetValue((r * p.srcRepStride + block * p.srcBlkStride) * 32 / sizeof(T) + within));
    }
}
template<class T> struct DataCopyPadExtParams { bool isPad; uint8_t leftPadding, rightPadding; T paddingValue; };
template<class T> void DataCopy(LocalTensor<T> dst, GlobalTensor<T> src, DataCopyParams p) {
    if (GmReadObserver()) GmReadObserver()(src.pointer, p.blockCount * p.blockLen * 32U);
    ++testCopies;
    for (uint32_t b = 0; b < p.blockCount; ++b) {
        const auto to = dst[uint64_t(b) * (p.blockLen + p.dstStride) * 32 / sizeof(T)];
        std::memcpy(to.Data(p.blockLen * 32), reinterpret_cast<uint8_t *>(src.pointer) +
            uint64_t(b) * (p.blockLen + p.srcStride) * 32, p.blockLen * 32);
    }
}
template<class T> void DataCopy(GlobalTensor<T> dst, LocalTensor<T> src, DataCopyParams p) {
    ++testCopies;
    IssueMte3([=]() {
        for (uint32_t b = 0; b < p.blockCount; ++b) {
            const auto from = src[uint64_t(b) * (p.blockLen + p.srcStride) * 32 / sizeof(T)];
            std::memcpy(reinterpret_cast<uint8_t *>(dst.pointer) + uint64_t(b) * (p.blockLen + p.dstStride) * 32,
                from.Data(p.blockLen * 32), p.blockLen * 32);
        }
    });
}

template<class T> void DataCopy(LocalTensor<T> dst, GlobalTensor<T> src, uint32_t count) {
    if (GmReadObserver()) GmReadObserver()(src.pointer, count * sizeof(T));
    ++testCopies; std::memcpy(dst.Data(count * sizeof(T)), src.pointer, count * sizeof(T));
}
template<class T> void DataCopy(GlobalTensor<T> dst, LocalTensor<T> src, uint32_t count) {
    ++testCopies;
    IssueMte3([=]() {
        if (GmWriteObserver()) GmWriteObserver()(dst.pointer, src.Data(count * sizeof(T)), count * sizeof(T));
        std::memcpy(dst.pointer, src.Data(count * sizeof(T)), count * sizeof(T));
    });
}
template<class T> void DataCopyPad(LocalTensor<T> dst, GlobalTensor<T> src, DataCopyExtParams p, DataCopyPadExtParams<T>) {
    if (p.blockCount != 1) throw std::runtime_error("unexpected padded block count");
    if (GmReadObserver()) GmReadObserver()(src.pointer, p.blockLen);
    ++testCopies; std::memcpy(dst.Data(p.blockLen), src.pointer, p.blockLen);
}
template<class T> void DataCopyPad(GlobalTensor<T> dst, LocalTensor<T> src, DataCopyExtParams p) {
    if (src.offset % 32U) throw std::runtime_error("unaligned UB DataCopyPad source");
    ++testCopies;
    IssueMte3([=]() {
        // CANN 9.1 UB->GM: source stride counts 32B blocks, destination stride bytes.
        const uint64_t sourceStep = (p.blockLen + 31U) / 32U * 32U + uint64_t(p.srcStride) * 32U;
        for (uint32_t block = 0; block < p.blockCount; ++block) {
            if (PaddedWriteObserver()) PaddedWriteObserver()(
                reinterpret_cast<uint8_t *>(dst.pointer) + uint64_t(block) * (p.blockLen + p.dstStride),
                src.template ReinterpretCast<uint8_t>()[uint64_t(block) * sourceStep].Data(p.blockLen), p.blockLen);
            std::memcpy(reinterpret_cast<uint8_t *>(dst.pointer) + uint64_t(block) * (p.blockLen + p.dstStride),
                src.template ReinterpretCast<uint8_t>()[uint64_t(block) * sourceStep].Data(p.blockLen), p.blockLen);
        }
    });
}

template<class T> void Duplicate(LocalTensor<T> dst, T value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) dst.SetValue(i, value);
}
// CANN 9.1's bit-mask overload requires a mutable mask array.
template<class T> void Duplicate(const LocalTensor<T> &dst, const T &value, uint64_t mask[],
    const uint8_t repeats, const uint16_t blockStride, const uint8_t repeatStride) {
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
    for (uint32_t row = 0; row < p.outter; ++row) {
        T sum = 0;
        for (uint32_t i = 0; i < p.n; ++i) sum += src.GetValue(row * p.inner + i);
        dst.SetValue(row, sum);
    }
}
inline std::function<void(SumParams)> &SumObserver() {
    static std::function<void(SumParams)> observer;
    return observer;
}
template<class T> void Sum(LocalTensor<T> dst, LocalTensor<T> src, LocalTensor<uint8_t> tmp, SumParams p) {
    const auto overlaps = [](const std::shared_ptr<Storage> &a, uint64_t aStart, uint64_t aBytes,
                             const std::shared_ptr<Storage> &b, uint64_t bStart, uint64_t bBytes) {
        return a == b && aStart < bStart + bBytes && bStart < aStart + aBytes;
    };
    if (!p.outter || !p.n || p.n > p.inner || p.inner * sizeof(T) % 32U ||
        overlaps(dst.storage, dst.offset, dst.length, src.storage, src.offset, src.length) ||
        overlaps(dst.storage, dst.offset, dst.length, tmp.storage, tmp.offset, tmp.length) ||
        overlaps(src.storage, src.offset, src.length, tmp.storage, tmp.offset, tmp.length))
        throw std::runtime_error("invalid explicit Sum shape or overlapping storage");
    // Bound the full vector loads in CANN 9.1/A5, even for masked-off elements.
    // This is an access-range model, not a model of device ordering or timing.
    constexpr uint32_t vl = 256U / sizeof(T);
    const uint32_t repeats = (p.n + vl - 1U) / vl;
    src.Data(((p.outter - 1U) * p.inner + repeats * vl) * sizeof(T));
    if (repeats > 1U) {
        const uint32_t stride = ((p.inner + vl - 1U) / vl + 31U) / 32U * 32U;
        tmp.Data(((p.outter - 1U) * stride + ((repeats + vl - 1U) / vl) * vl) * sizeof(T));
    }
    Sum(dst, src, p);
    if (SumObserver()) SumObserver()(p);
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
