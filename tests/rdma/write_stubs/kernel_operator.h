/*
 * 功能：用 CPU tensor 和 memcpy 替代 RDMA/Profiling 测试所需的 Ascend C I/O，统计 view、metadata、copy 和 doorbell 访问。
 * 调用关系：WRITE adapter/context 测试经 include 路径选用本 stub；tensor 借用 fixture buffer，ActivityLog 保存访问计数。
 * 边界：同步操作为空实现，st_dev 仅写 CPU 内存；不模拟设备 cache 或 NIC 传输。
 *
 * Purpose: Substitute CPU tensors/memcpy for RDMA/profiling Ascend C I/O and count view, metadata, copy and doorbell accesses.
 * Calls: WRITE adapter/context tests select this stub via include paths; tensors borrow fixture buffers and ActivityLog holds counters.
 * Scope: Synchronization is a no-op and st_dev only writes CPU memory; no device-cache or NIC-transfer model is provided.
 */
#ifndef TILEXR_RDMA_WRITE_TEST_KERNEL_OPERATOR_H
#define TILEXR_RDMA_WRITE_TEST_KERNEL_OPERATOR_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define __aicore__
#define __gm__
#define __NPU_ARCH__ 3510
using GM_ADDR = unsigned char*;
constexpr int PIPE_MTE3 = 0;

namespace RdmaWriteTest {
struct Activity {
    uint32_t publications = 0U;
    uint32_t rings = 0U;
    uint32_t reads = 0U;
    uint64_t state = 0U, pending = 0U;
    uint64_t view = 0U;
    uint32_t viewBytes = 0U, viewInvalidations = 0U;
    uint32_t pendingBytes = 0U, metadataReads = 0U, metadataWrites = 0U;
    bool Metadata(uint64_t address) const {
        return (state != 0U && address == state) ||
            (pending != 0U && address >= pending && address - pending < pendingBytes);
    }
};
inline Activity& ActivityLog() { static Activity log; return log; }
} // namespace RdmaWriteTest

template <typename T>
inline void st_dev(T value, T* address, uint32_t offset)
{
    ++RdmaWriteTest::ActivityLog().rings;
    address[offset] = value;
}

namespace AscendC {
inline uint64_t GetSystemCycle() { static uint64_t cycle = 0U; return ++cycle; }
enum class HardEvent { S_MTE3, MTE3_S, S_MTE2, MTE2_S };
enum class CacheLine { SINGLE_CACHE_LINE };
enum class DcciDst { CACHELINE_OUT };
struct TPipe { int32_t FetchEventID(HardEvent) { return 0; } };
template <HardEvent> inline void SetFlag(int32_t) {}
template <HardEvent> inline void WaitFlag(int32_t) {}
template <int> inline void PipeBarrier() {}

struct DataCopyExtParams {
    uint16_t blockCount;
    uint32_t blockLen;
    int64_t srcStride;
    int64_t dstStride;
    uint32_t reserved;
};
template <typename T> struct DataCopyPadExtParams {
    bool padding;
    uint8_t left;
    uint8_t right;
    T value;
};

template <typename T> class LocalTensor {
public:
    LocalTensor(T* address, uint32_t size) : address_(address), size_(size) {}
    uint32_t GetSize() const { return size_; }
    uint64_t GetPhyAddr() const { return reinterpret_cast<uint64_t>(address_); }
    void SetValue(uint32_t index, T value) const {
        if (index >= size_) { std::abort(); }
        address_[index] = value;
    }
    T GetValue(uint32_t index) const {
        if (index >= size_) { std::abort(); }
        return address_[index];
    }
    LocalTensor operator[](uint32_t offset) const {
        if (offset > size_) { std::abort(); }
        return LocalTensor(address_ + offset, size_ - offset);
    }
    template <typename U> LocalTensor<U> ReinterpretCast() const {
        return LocalTensor<U>(reinterpret_cast<U*>(address_), size_ * sizeof(T) / sizeof(U));
    }
private:
    T* address_;
    uint32_t size_;
};

template <typename T> class GlobalTensor {
public:
    void SetGlobalBuffer(T* address, uint32_t size) { address_ = address; size_ = size; }
    T* GetPhyAddr() const { return address_; }
    uint32_t GetSize() const { return size_; }
private:
    T* address_ = nullptr;
    uint32_t size_ = 0U;
};
template <typename T, CacheLine, DcciDst>
inline void DataCacheCleanAndInvalid(const GlobalTensor<T>& tensor) {
    auto& log = RdmaWriteTest::ActivityLog();
    const auto address = reinterpret_cast<uint64_t>(tensor.GetPhyAddr());
    if (log.view != 0U && address >= log.view && address - log.view < log.viewBytes) {
        ++log.viewInvalidations;
    }
}

template <typename T>
inline void DataCopyPad(const GlobalTensor<T>& target, const LocalTensor<T>& source,
    const DataCopyExtParams& params)
{
    if (params.blockCount != 1U || params.blockLen > target.GetSize() * sizeof(T) ||
        params.blockLen > source.GetSize() * sizeof(T) || (source.GetPhyAddr() & 31U) != 0U) { std::abort(); }
    ++RdmaWriteTest::ActivityLog().publications;
    if (RdmaWriteTest::ActivityLog().Metadata(reinterpret_cast<uint64_t>(target.GetPhyAddr()))) {
        ++RdmaWriteTest::ActivityLog().metadataWrites;
    }
    std::memcpy(target.GetPhyAddr(), reinterpret_cast<const void*>(source.GetPhyAddr()), params.blockLen);
}
template <typename T>
inline void DataCopyPad(const LocalTensor<T>& target, const GlobalTensor<T>& source,
    const DataCopyExtParams& params, const DataCopyPadExtParams<T>&)
{
    if (params.blockCount != 1U || params.blockLen > target.GetSize() * sizeof(T) ||
        params.blockLen > source.GetSize() * sizeof(T) || (target.GetPhyAddr() & 31U) != 0U) { std::abort(); }
    ++RdmaWriteTest::ActivityLog().reads;
    if (RdmaWriteTest::ActivityLog().Metadata(reinterpret_cast<uint64_t>(source.GetPhyAddr()))) {
        ++RdmaWriteTest::ActivityLog().metadataReads;
    }
    std::memcpy(reinterpret_cast<void*>(target.GetPhyAddr()), source.GetPhyAddr(), params.blockLen);
}
} // namespace AscendC

inline AscendC::TPipe* GetTPipePtr() { static AscendC::TPipe pipe; return &pipe; }
#endif
