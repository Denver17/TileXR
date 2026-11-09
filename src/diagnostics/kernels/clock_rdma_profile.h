/*
 * 功能：记录 RDMA WRITE 的发布窗口，并在启用 profiling 时累计发送和 CQ polling 的 cycle 计数。
 * 调用关系：clock_sync_kernel.h 将 trace 传给 RDMA queue；计数布局定义在 tilexr_straggler_layout.h。
 * 资源与生命周期：trace 借用 observer 或 UB record；profile 宏使用调用方的 TPipe 分配 UB 并写入 GM。
 * Purpose: Capture RDMA WRITE publication windows and optionally count send/CQ polling cycles.
 * Calls: clock_sync_kernel.h passes traces to the queue using tilexr_straggler_layout.h records.
 * Lifetime: Traces borrow observers/UB records; profile macros allocate UB through the caller's TPipe and export to GM.
 */
#ifndef TILEXR_DIAGNOSTICS_CLOCK_RDMA_PROFILE_H
#define TILEXR_DIAGNOSTICS_CLOCK_RDMA_PROFILE_H

// Profiling is a separate build: normal kernels retain the existing send path.
#ifndef TILEXR_DIAGNOSTICS_CLOCK_PROFILE
#define TILEXR_DIAGNOSTICS_CLOCK_PROFILE 0
#endif

namespace TileXR {
// The NTP send timestamp brackets device publication, never remote completion.
// In particular a synchronous CQ wait must not move t0/t2 into the future.
template <typename Observer>
class ClockRdmaPublicationTrace {
public:
    __aicore__ inline explicit ClockRdmaPublicationTrace(Observer &observer) : observer_(observer) {}
    __aicore__ inline void Stage(RdmaWire::BatchTraceStage stage)
    {
        observer_.Stage(stage);
        if (stage == RdmaWire::BatchTraceStage::WqesPublished) {
            begin_ = static_cast<uint64_t>(AscendC::GetSystemCycle());
        } else if (stage == RdmaWire::BatchTraceStage::DoorbellIssued) {
            end_ = static_cast<uint64_t>(AscendC::GetSystemCycle());
        }
    }
    __aicore__ inline void WqeStart() { observer_.WqeStart(); }
    __aicore__ inline void WqeEncoded() { observer_.WqeEncoded(); }
    __aicore__ inline void WqePublished() { observer_.WqePublished(); }
    __aicore__ inline void PollStart() { observer_.PollStart(); }
    __aicore__ inline void PollRead() { observer_.PollRead(); }
    __aicore__ inline void PollDecoded(bool pending) { observer_.PollDecoded(pending); }
    __aicore__ inline uint64_t Midpoint() const { return begin_ + (end_ - begin_) / 2u; }
private:
    Observer &observer_;
    uint64_t begin_ = 0, end_ = 0;
};
} // namespace TileXR

#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
namespace TileXR {
class ClockRdmaSendTrace {
public:
    __aicore__ inline explicit ClockRdmaSendTrace(__ubuf__ TileXRClockRdmaSendProfile *record)
        : record_(record) {}
    __aicore__ inline void Stage(RdmaWire::BatchTraceStage stage)
    {
        const uint32_t index = static_cast<uint32_t>(stage);
        stamps_[index] = static_cast<uint64_t>(AscendC::GetSystemCycle());
    }
    __aicore__ inline void WqeStart() { wqeStart_ = Now(); }
    __aicore__ inline void WqeEncoded() { wqeEncoded_ = Now(); }
    __aicore__ inline void WqePublished() { wqePublished_ = Now(); }
    __aicore__ inline void PollStart() { pollStart_ = Now(); }
    __aicore__ inline void PollRead() { pollRead_ = Now(); }
    __aicore__ inline void PollDecoded(bool pending)
    {
        const uint64_t end = Now();
        ++record_->polls;
        record_->pending += pending ? 1u : 0u;
        pollReadCycles_ += pollRead_ - pollStart_;
        pollDecodeCycles_ += end - pollRead_;
    }
    __aicore__ inline void Finish(uint64_t start, uint64_t staged, uint64_t end, uint32_t status,
                                  bool deferred)
    {
        ++record_->sends;
        record_->failures += status == TILEXR_RDMA_DEVICE_SUCCESS ? 0u : 1u;
        Add(0, end - start);
        Add(1, staged - start);
        if (status != TILEXR_RDMA_DEVICE_SUCCESS) { return; }
        Add(2, Stamp(RdmaWire::BatchTraceStage::ViewLoaded) - Stamp(RdmaWire::BatchTraceStage::Enter));
        Add(3, Stamp(RdmaWire::BatchTraceStage::Validated) - Stamp(RdmaWire::BatchTraceStage::ViewLoaded));
        Add(4, Stamp(RdmaWire::BatchTraceStage::Reserved) - Stamp(RdmaWire::BatchTraceStage::Validated));
        Add(5, Stamp(RdmaWire::BatchTraceStage::StopPublished) - Stamp(RdmaWire::BatchTraceStage::Reserved));
        Add(6, wqeEncoded_ - wqeStart_);
        Add(7, wqePublished_ - wqeEncoded_);
        Add(8, Stamp(RdmaWire::BatchTraceStage::DoorbellIssued) - Stamp(RdmaWire::BatchTraceStage::WqesPublished));
        if (!deferred) {
            Add(9, Stamp(RdmaWire::BatchTraceStage::FirstCqe) - Stamp(RdmaWire::BatchTraceStage::DoorbellIssued));
            Add(10, pollReadCycles_);
            Add(11, pollDecodeCycles_);
            Add(12, Stamp(RdmaWire::BatchTraceStage::Retired) - Stamp(RdmaWire::BatchTraceStage::CqDrained));
        }
        Add(13, end - Stamp(deferred ? RdmaWire::BatchTraceStage::DoorbellIssued : RdmaWire::BatchTraceStage::Retired));
    }
private:
    __aicore__ inline uint64_t Now() { return static_cast<uint64_t>(AscendC::GetSystemCycle()); }
    __aicore__ inline uint64_t Stamp(RdmaWire::BatchTraceStage stage)
    { return stamps_[static_cast<uint32_t>(stage)]; }
    __aicore__ inline void Add(uint32_t index, uint64_t value)
    {
        record_->sum[index] += value;
        if (record_->sends == 1 || value < record_->minimum[index]) { record_->minimum[index] = value; }
        if (value > record_->maximum[index]) { record_->maximum[index] = value; }
    }
    __ubuf__ TileXRClockRdmaSendProfile *record_;
    uint64_t stamps_[static_cast<uint32_t>(RdmaWire::BatchTraceStage::Count)] = {};
    uint64_t wqeStart_ = 0, wqeEncoded_ = 0, wqePublished_ = 0;
    uint64_t pollStart_ = 0, pollRead_ = 0, pollReadCycles_ = 0, pollDecodeCycles_ = 0;
};
} // namespace TileXR
#define TILEXR_CLOCK_PROFILE_INIT(pipe, magicValue) \
    AscendC::TBuf<AscendC::QuePosition::VECCALC> profileStorage; \
    pipe.InitBuffer(profileStorage, sizeof(TileXR::TileXRClockRdmaProfile)); \
    auto *profile = reinterpret_cast<__ubuf__ TileXR::TileXRClockRdmaProfile *>( \
        profileStorage.Get<uint8_t>().GetPhyAddr()); \
    for (uint32_t i = 0; i < sizeof(*profile) / sizeof(uint64_t); ++i) { \
        reinterpret_cast<__ubuf__ uint64_t *>(profile)[i] = 0; \
    } \
    profile->magic = TileXR::TILEXR_CLOCK_RDMA_PROFILE_MAGIC; \
    profile->generation = magicValue; profile->bytes = sizeof(*profile);
#define TILEXR_CLOCK_PROFILE_FLUSH(memory) \
    if (memory != nullptr) { \
        TileXR::TileXRStragglerWriteGm(memory + TileXR::TILEXR_CLOCK_RDMA_PROFILE_OFFSET, \
            reinterpret_cast<__ubuf__ uint8_t *>(profile), sizeof(*profile)); \
    }
#else
#define TILEXR_CLOCK_PROFILE_INIT(pipe, magicValue) \
    __ubuf__ TileXR::TileXRClockRdmaProfile *profile = nullptr;
#define TILEXR_CLOCK_PROFILE_FLUSH(memory)
#endif
#endif
