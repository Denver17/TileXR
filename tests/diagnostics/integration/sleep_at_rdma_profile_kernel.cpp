/*
 * 功能：发送 64 个 4 KiB WRITE 并等待完成，写回队列状态，仅在入口和出口记录 marker。
 * 调用关系：Host helper 注册外置 binary 后启动一个 AIV；kernel 调用 RDMAWrite、RDMAWait 和 profiling SDK。
 * 资源与生命周期：UB context 在本次调用中保存队列进度；MR、结果 buffer 和采集内存由 Host 持有。
 */
#include "kernel_operator.h"
#include "tilexr_rdma_write.h"
#include "tilexr/profiling.h"
#include "sleep_at_rdma_profile.h"

extern "C" __global__ __aicore__ void tilexr_sleep_at_rdma_profile(
    GM_ADDR viewGM, GM_ADDR resultGM, GM_ADDR profileGM)
{
    using namespace SleepAtRdmaProfile;
    TileXR::Profiling::Recorder<2> recorder;
    recorder.RecordMarker<0, kEntry>();
    const auto *view = reinterpret_cast<const __gm__ TileXRRDMADeviceView *>(viewGM);
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> workBuffer, contextBuffer, resultBuffer;
    pipe.InitBuffer(workBuffer, TileXR::RDMAWriteWorkScratchBytes());
    pipe.InitBuffer(contextBuffer, TileXR::RDMAWriteContextBytes(view->queue.pendingCapacity));
    pipe.InitBuffer(resultBuffer, 64);
    const auto scratch = workBuffer.Get<uint8_t>();
    TileXR::RDMAWriteContext context(contextBuffer.Get<uint8_t>());
    uint32_t status = context.Restore(view);
    const uint64_t before = context.Loaded() ? context.State().producer : 0;
    TileXR::RDMAWriteOptions options;
    options.count = kWqes;
    options.localStride = kBytes;
    options.remoteStride = kBytes;
    options.postBatch = kWqes;
    options.maxPolls = 10000000;
    if (status == 0) {
        status = TileXR::RDMAWrite(view, kSource, kTarget, kBytes, options, scratch, context);
    }
    if (status == 0) { status = TileXR::RDMAWait(view, scratch, context, options.maxPolls); }
    if (context.Loaded()) {
        const uint32_t saved = context.Save();
        if (status == 0) { status = saved; }
    }
    const auto state = context.Loaded() ? context.State() : TileXRRDMADeviceState {};
    const auto result = resultBuffer.Get<uint64_t>();
    result.SetValue(0, kMagic);
    result.SetValue(1, status);
    result.SetValue(2, before);
    result.SetValue(3, state.producer);
    result.SetValue(4, state.completed);
    result.SetValue(5, state.cqConsumer);
    result.SetValue(6, state.error);
    result.SetValue(7, state.syndrome);
    AscendC::GlobalTensor<uint64_t> output;
    output.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(resultGM), 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopy(output, result, 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE3_S>();
    recorder.RecordMarker<1, kExit>();
    recorder.Flush(profileGM, 0);
}
