/*
 * 功能：为单 owner Ascend C kernel 分配连续的 output bundle，读取 Pool 准备的 ring snapshot。
 * 调用关系：compact Dispatch 和 pool 测试 kernel 调用此入口；空间不足时通过 mailbox
 * 请求 Host 增长并在当前 kernel 等待回复，随后发布 allocation handle、地址或错误。
 * 资源与生命周期：Pool 持有 segment 和 mailbox；本入口从调用者的 TPipe 申请 UB scratch，
 * 由调用者 reset 或销毁 TPipe。可选 recorder 由调用者持有，本入口仅写 marker。
 */
#ifndef TILEXR_ELASTIC_MEMORY_DEVICE_H
#define TILEXR_ELASTIC_MEMORY_DEVICE_H
#include "kernel_operator.h"
#include "tilexr_elastic_memory_abi.h"
#include "tilexr_elastic_memory_profile.h"
#define TILEXR_ELASTIC_INLINE __aicore__ inline
#include "allocator.h"
#undef TILEXR_ELASTIC_INLINE
namespace TileXR { namespace Elastic {
namespace DeviceDetail {
template<AscendC::HardEvent Event> __aicore__ inline void Sync()
{
    auto id = GetTPipePtr()->FetchEventID(Event);
    AscendC::SetFlag<Event>(id);
    AscendC::WaitFlag<Event>(id);
}
__aicore__ inline uint64_t ReadWord(uint64_t base, uint32_t index)
{
    return reinterpret_cast<volatile __gm__ uint64_t*>(base)[index];
}
__aicore__ inline void Invalidate(uint64_t base, uint32_t index)
{
    AscendC::GlobalTensor<uint64_t> global;
    global.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(base));
    AscendC::DataCacheCleanAndInvalid<uint64_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
                                    AscendC::DcciDst::CACHELINE_OUT>(global[index]);
}
// Every transfer completes before the scratch buffer or payload can be reused.
// Publish uses the default 64-byte batch even when state copies use larger UB.
template<uint32_t ScratchBytes = 64>
__aicore__ inline void WriteWords(uint64_t base, const uint64_t* values, uint32_t count,
                                  AscendC::LocalTensor<uint64_t>& scratch)
{
    static_assert(ScratchBytes >= 64 && ScratchBytes % 32 == 0,
                  "ElasticMemory scratch must be at least 64 bytes and 32-byte aligned");
    constexpr uint32_t scratchWords = ScratchBytes / sizeof(uint64_t);
    AscendC::GlobalTensor<uint64_t> global;
    global.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(base), count);
    for (uint32_t offset = 0; offset < count; offset += scratchWords) {
        const uint32_t words = count - offset < scratchWords ? count - offset : scratchWords;
        for (uint32_t i = 0; i < words; ++i) scratch.SetValue(i, values[offset + i]);
        Sync<AscendC::HardEvent::S_MTE3>();
        if (words % 4 == 0) AscendC::DataCopy(global[offset], scratch, words);
        else {
            AscendC::DataCopyExtParams params{1, words * 8, 0, 0, 0};
            AscendC::DataCopyPad(global[offset], scratch, params);
        }
        Sync<AscendC::HardEvent::MTE3_S>();
    }
}
__aicore__ inline void Publish(uint64_t base, uint64_t* words, uint64_t sequence,
                              AscendC::LocalTensor<uint64_t>& scratch)
{
    words[0] = 0;
    WriteWords(base, words, 8, scratch);
    words[0] = sequence;
    WriteWords(base, words, 8, scratch);
}
} // namespace DeviceDetail

// One owner per FIFO call. The immutable HBM snapshot contains a 64-byte
// header followed by the preferred ring. Read both in one 128-byte MTE2 copy;
// additional rings are touched only after Capacity. No persistent table writeback.
template<uint32_t ScratchBytes = 128>
__aicore__ inline uint64_t AllocateBundle(DeviceView view, uint64_t bytes, uint64_t alignment,
                                         uint64_t rows, uint64_t* address,
                                         Profile::Recorder* profiler = nullptr)
{
    static_assert(ScratchBytes >= 128 && ScratchBytes % 32 == 0,
                  "ElasticMemory ring allocation requires at least 128 bytes of UB");
    using namespace DeviceDetail;
    Profile::Mark<Profile::AllocatorBegin>(profiler);
    if (address == nullptr || view.state == 0 || view.mailbox == 0) {
        Profile::Mark<Profile::AllocatorEnd>(profiler);
        return Invalid;
    }
    *address = 0;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> buffer;
    GetTPipePtr()->InitBuffer(buffer, ScratchBytes);
    auto scratch = buffer.Get<uint64_t>();
    Profile::Mark<Profile::StateLoadBegin>(profiler);
    AscendC::GlobalTensor<uint64_t> snapshot;
    snapshot.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(view.state), sizeof(PoolState) / 8);
    Sync<AscendC::HardEvent::S_MTE2>();
    AscendC::DataCopy(scratch, snapshot, 16);
    Sync<AscendC::HardEvent::MTE2_S>();
    const uint64_t version = scratch.GetValue(0);
    const uint64_t sequence = scratch.GetValue(1);
    const uint64_t segmentCount = scratch.GetValue(2);
    const uint64_t reservations = scratch.GetValue(3);
    RingState ring;
    uint64_t* words = reinterpret_cast<uint64_t*>(&ring);
    for (uint32_t i = 0; i < 8; ++i) words[i] = scratch.GetValue(8 + i);
    Profile::Mark<Profile::StateLoadEnd>(profiler);
    uint64_t error = version == kAbiVersion && sequence && segmentCount <= kMaxSegments &&
        reservations <= kMaxAllocations ? Success : Invalid;
    if (error == Success && bytes && reservations == kMaxAllocations) error = MetadataFull;
    Handle handle{};
    Profile::Mark<Profile::TryAllocateBegin>(profiler);
    if (error == Success) error = TryAllocateRing(ring, bytes, alignment, sequence, &handle, address);
    Profile::Mark<Profile::TryAllocateEnd>(profiler);
    if (error == Capacity && segmentCount > 1) {
        Profile::Mark<Profile::SegmentSearchBegin>(profiler);
        for (uint32_t slot = 1; slot < segmentCount && error == Capacity; ++slot) {
            Sync<AscendC::HardEvent::S_MTE2>();
            AscendC::DataCopy(scratch, snapshot[8 + slot * 8], 8);
            Sync<AscendC::HardEvent::MTE2_S>();
            for (uint32_t i = 0; i < 8; ++i) words[i] = scratch.GetValue(i);
            error = TryAllocateRing(ring, bytes, alignment, sequence, &handle, address);
        }
        Profile::Mark<Profile::SegmentSearchEnd>(profiler);
    }
    if (error == Capacity) {
        Profile::Mark<Profile::GrowthWaitBegin>(profiler);
        uint64_t request[8] = {0, bytes, alignment, 0, 0, 0, 0, 0};
        Publish(view.mailbox, request, sequence, scratch);
        uint64_t replySequence = 0;
        // The volatile read prevents loop-invariant caching; DCCI makes the
        // independently published Host reply visible to this resident kernel.
        while (replySequence != sequence) {
            Invalidate(view.mailbox, 8);
            replySequence = ReadWord(view.mailbox, 8);
        }
        const uint64_t replyError = ReadWord(view.mailbox, 12);
        error = replyError <= ProtocolFailure ? replyError : static_cast<uint64_t>(ProtocolFailure);
        if (error == Success) {
            ring = RingState{};
            ring.segment = Segment{ReadWord(view.mailbox, 9), ReadWord(view.mailbox, 10),
                                   ReadWord(view.mailbox, 11)};
            error = TryAllocateRing(ring, bytes, alignment, sequence, &handle, address);
            // One growth reply promises a sufficient segment. Never retry it.
            if (error != Success) error = ProtocolFailure;
        }
        Profile::Mark<Profile::GrowthWaitEnd>(profiler);
    }
    Profile::Mark<Profile::StateWritebackBegin>(profiler);
    if (error == Success && bytes) WriteWords(view.mailbox + 256, words, 8, scratch);
    Profile::Mark<Profile::StateWritebackEnd>(profiler);
    if (error != Success) { handle = Handle{}; *address = 0; }
    uint64_t output[8] = {0, error, handle.segmentId, handle.offset, handle.bytes,
                          handle.generation, *address, rows};
    Profile::Mark<Profile::ResultPublishBegin>(profiler);
    Publish(view.mailbox + 128, output, sequence, scratch);
    Profile::Mark<Profile::ResultPublishEnd>(profiler);
    Profile::Mark<Profile::AllocatorEnd>(profiler);
    return error;
}
} }
#endif
