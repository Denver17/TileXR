// 四个 AIV block 执行 Add4，以具名事件记录共享边界；Host 传入独立 SDK descriptor，mapping 定义离线配对。
// Run Add4 on four AIV blocks and record named shared-boundary events; the Host supplies the SDK descriptor and the mapping defines offline pairing.
#include "kernel_operator.h"
#include <tilexr/profiling.h>

namespace {

constexpr uint32_t kElementsPerCore = 2048U;
constexpr uint32_t kRecordCount = 17U;
constexpr uint32_t kTileBytes = kElementsPerCore * sizeof(float);

constexpr uint32_t kEventTotalBegin = 1U;
constexpr uint32_t kEventTotalEnd = 2U;
constexpr uint32_t kEventCopyInputBegin = 10U;
constexpr uint32_t kEventCopyInputEnd = 11U;
constexpr uint32_t kEventAccumulateBegin = 20U;
constexpr uint32_t kEventAccumulateEnd = 21U;
constexpr uint32_t kEventCopyOutBegin = 30U;
constexpr uint32_t kEventCopyOutEnd = 31U;
constexpr uint32_t kEventPartialSum0End = 201U;
constexpr uint32_t kEventPartialSum1End = 202U;
constexpr uint32_t kEventPartialSum2End = 203U;

__aicore__ inline void WaitInputCopy()
{
    AscendC::SetFlag<AscendC::HardEvent::MTE2_V>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_V>(EVENT_ID0);
}

__aicore__ inline void WaitOutputCopy()
{
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

} // namespace

extern "C" __global__ __aicore__ void tilexr_profiled_add4_kernel(
    GM_ADDR input0GM, GM_ADDR input1GM, GM_ADDR input2GM, GM_ADDR input3GM,
    GM_ADDR outputGM, GM_ADDR profileGM)
{
    const uint32_t core = static_cast<uint32_t>(AscendC::GetBlockIdx());
    const uint64_t elementOffset = static_cast<uint64_t>(core) * kElementsPerCore;

    AscendC::GlobalTensor<float> input0;
    AscendC::GlobalTensor<float> input1;
    AscendC::GlobalTensor<float> input2;
    AscendC::GlobalTensor<float> input3;
    AscendC::GlobalTensor<float> output;
    input0.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(input0GM) + elementOffset,
        kElementsPerCore);
    input1.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(input1GM) + elementOffset,
        kElementsPerCore);
    input2.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(input2GM) + elementOffset,
        kElementsPerCore);
    input3.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(input3GM) + elementOffset,
        kElementsPerCore);
    output.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(outputGM) + elementOffset,
        kElementsPerCore);

    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> input0Buf;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> input1Buf;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> input2Buf;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> input3Buf;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> outputBuf;
    pipe.InitBuffer(input0Buf, kTileBytes);
    pipe.InitBuffer(input1Buf, kTileBytes);
    pipe.InitBuffer(input2Buf, kTileBytes);
    pipe.InitBuffer(input3Buf, kTileBytes);
    pipe.InitBuffer(outputBuf, kTileBytes);
    auto input0Local = input0Buf.Get<float>();
    auto input1Local = input1Buf.Get<float>();
    auto input2Local = input2Buf.Get<float>();
    auto input3Local = input3Buf.Get<float>();
    auto outputLocal = outputBuf.Get<float>();
    TileXR::Profiling::Recorder<kRecordCount> tilexrProfiler;

    tilexrProfiler.RecordMarker<0U, kEventTotalBegin>();

    tilexrProfiler.RecordMarker<1U, kEventCopyInputBegin>();
    AscendC::DataCopy(input0Local, input0, kElementsPerCore);
    WaitInputCopy();
    tilexrProfiler.RecordMarker<2U, kEventCopyInputEnd>();

    tilexrProfiler.RecordMarker<3U, kEventCopyInputBegin>();
    AscendC::DataCopy(input1Local, input1, kElementsPerCore);
    WaitInputCopy();
    tilexrProfiler.RecordMarker<4U, kEventCopyInputEnd>();

    tilexrProfiler.RecordMarker<5U, kEventCopyInputBegin>();
    AscendC::DataCopy(input2Local, input2, kElementsPerCore);
    WaitInputCopy();
    tilexrProfiler.RecordMarker<6U, kEventCopyInputEnd>();

    tilexrProfiler.RecordMarker<7U, kEventCopyInputBegin>();
    AscendC::DataCopy(input3Local, input3, kElementsPerCore);
    WaitInputCopy();
    tilexrProfiler.RecordMarker<8U, kEventCopyInputEnd>();

    tilexrProfiler.RecordMarker<9U, kEventAccumulateBegin>();
    // mapping.v2 shares event 20 across the parent and all partial sums.

    AscendC::Add(outputLocal, input0Local, input1Local, kElementsPerCore);
    AscendC::PipeBarrier<PIPE_V>();
    tilexrProfiler.RecordMarker<10U, kEventPartialSum0End>();

    AscendC::Add(outputLocal, outputLocal, input2Local, kElementsPerCore);
    AscendC::PipeBarrier<PIPE_V>();
    tilexrProfiler.RecordMarker<11U, kEventPartialSum1End>();

    AscendC::Add(outputLocal, outputLocal, input3Local, kElementsPerCore);
    AscendC::PipeBarrier<PIPE_V>();
    tilexrProfiler.RecordMarker<12U, kEventPartialSum2End>();
    tilexrProfiler.RecordMarker<13U, kEventAccumulateEnd>();

    tilexrProfiler.RecordMarker<14U, kEventCopyOutBegin>();
    AscendC::SetFlag<AscendC::HardEvent::V_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::V_MTE3>(EVENT_ID0);
    AscendC::DataCopy(output, outputLocal, kElementsPerCore);
    WaitOutputCopy();
    tilexrProfiler.RecordMarker<15U, kEventCopyOutEnd>();
    tilexrProfiler.RecordMarker<16U, kEventTotalEnd>();

    tilexrProfiler.Flush(profileGM, core);
}
