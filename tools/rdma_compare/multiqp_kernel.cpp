#include "kernel_operator.h"
#include "tilexr_rdma_write.h"
#include "bench_abi.h"
#include "case_layout.h"

extern "C" __global__ __aicore__ void tilexr_multiqp_write(
    GM_ADDR viewsGM, GM_ADDR resultsGM, uint64_t sourceOffset, uint64_t targetOffset,
    uint64_t stride, uint32_t bytes, uint32_t count, uint32_t rank,
    uint32_t direction, uint32_t cyclicSlots, uint32_t calibration,
    uint64_t targetStride, uint32_t numSge, uint32_t qps)
{
    const uint32_t q = AscendC::GetBlockIdx();
    if (q >= qps || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE) { return; }
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> scratchBuffer, outputBuffer;
    pipe.InitBuffer(scratchBuffer, TileXR::RDMAWriteScratchBytes(numSge));
    pipe.InitBuffer(outputBuffer, sizeof(BenchResult));
    const auto scratch = scratchBuffer.Get<uint8_t>();
    const auto output = outputBuffer.Get<uint64_t>();
    const auto views = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    const auto view = views + q;
    uint32_t status = 0;
    uint64_t start = 0, end = 0;
    if (calibration) {
        start = AscendC::GetSystemCycle();
        do { end = AscendC::GetSystemCycle(); } while (end - start < 100000000ULL);
    } else if (direction == rank) {
        TileXR::RDMAWriteOptions options;
        options.count = count;
        options.localStride = stride;
        options.remoteStride = targetStride;
        options.cyclicSlots = cyclicSlots;
        options.maxPolls = 10000000U;
        if (numSge == 1U) {
            start = AscendC::GetSystemCycle();
            status = qps == 1U ? TileXR::RDMAWrite(view, sourceOffset, targetOffset, bytes, options, scratch) :
                TileXR::RDMAWriteMultiQp(views, qps, q, sourceOffset, targetOffset, bytes, options, scratch);
            end = AscendC::GetSystemCycle();
        } else {
            TileXRRDMADeviceSge sgeList[TILEXR_RDMA_DEVICE_MAX_SGE];
            for (uint32_t i = 0; i < numSge; ++i) {
                sgeList[i].localOffset = SgeSourceOffset(sourceOffset, bytes, numSge, i);
                sgeList[i].bytes = bytes;
                sgeList[i].reserved = 0U;
            }
            start = AscendC::GetSystemCycle();
            status = qps == 1U ? TileXR::RDMAWriteSge(view, sgeList, numSge, targetOffset, options, scratch) :
                TileXR::RDMAWriteSgeMultiQp(views, qps, q, sgeList, numSge, targetOffset, options, scratch);
            end = AscendC::GetSystemCycle();
        }
    }
    TileXR::RdmaDeviceDetail::QueueIO io(view->queue.state, scratch);
    const auto state = io.LoadState();
    output.SetValue(0, 0x5458524D41544348ULL);
    output.SetValue(1, status); output.SetValue(2, start); output.SetValue(3, end);
    output.SetValue(4, state.producer); output.SetValue(5, state.completed);
    output.SetValue(6, state.cqConsumer);
    output.SetValue(7, uint64_t(state.error) | (uint64_t(state.syndrome) << 32));
    AscendC::GlobalTensor<uint64_t> result;
    result.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultsGM) + uint64_t(q) * 8U, 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopy(result, output, 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE3_S>();
}
