/* CPU fixture for the real RDMA device adapter. memcpy/no-op cache/events do
 * not model device ordering, visibility, instructions or NIC execution. */
#ifndef TILEXR_RDMA_WRITE_TEST_KERNEL_OPERATOR_H
#define TILEXR_RDMA_WRITE_TEST_KERNEL_OPERATOR_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define __aicore__
#define __gm__
#define __NPU_ARCH__ 3510
constexpr int PIPE_MTE3 = 0;

namespace RdmaWriteTest {
struct Activity {
    uint32_t publications = 0U;
    uint32_t rings = 0U;
    uint32_t reads = 0U;
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
inline void DataCacheCleanAndInvalid(const GlobalTensor<T>&) {}

template <typename T>
inline void DataCopyPad(const GlobalTensor<T>& target, const LocalTensor<T>& source,
    const DataCopyExtParams& params)
{
    if (params.blockCount != 1U || params.blockLen > target.GetSize() * sizeof(T) ||
        params.blockLen > source.GetSize() * sizeof(T)) { std::abort(); }
    ++RdmaWriteTest::ActivityLog().publications;
    std::memcpy(target.GetPhyAddr(), reinterpret_cast<const void*>(source.GetPhyAddr()), params.blockLen);
}
template <typename T>
inline void DataCopyPad(const LocalTensor<T>& target, const GlobalTensor<T>& source,
    const DataCopyExtParams& params, const DataCopyPadExtParams<T>&)
{
    if (params.blockCount != 1U || params.blockLen > target.GetSize() * sizeof(T) ||
        params.blockLen > source.GetSize() * sizeof(T)) { std::abort(); }
    ++RdmaWriteTest::ActivityLog().reads;
    std::memcpy(reinterpret_cast<void*>(target.GetPhyAddr()), source.GetPhyAddr(), params.blockLen);
}
} // namespace AscendC

inline AscendC::TPipe* GetTPipePtr() { static AscendC::TPipe pipe; return &pipe; }
#endif
