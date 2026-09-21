#include "kernel_operator.h"
#include "comm_args.h"
#include "tilexr_types.h"
#include "combine_memory_boundary_test.h"

// Test-only local access. No remote payload, business operator or runtime flag access.
extern "C" __global__ __aicore__ void tilexr_ep_combine_memory_boundary_test_kernel(
    GM_ADDR commArgs, GM_ADDR result, uint64_t expectedBytes, uint32_t window, uint32_t index)
{
    using namespace AscendC;
    using namespace TileXRCombineMemoryBoundaryTest;
    static_assert(kRuntimeReservedBytes == TileXR::TILEXR_FLAG_BUFF_BYTES, "probe excludes the runtime reserve");
    if (GetBlockIdx() != 0 || commArgs == nullptr || result == nullptr ||
        reinterpret_cast<uint64_t>(commArgs) % 8U != 0 ||
        reinterpret_cast<uint64_t>(result) % kProbeBytes != 0 ||
        reinterpret_cast<uint64_t>(result) > UINT64_MAX - kResultBytes ||
        expectedBytes != kPeerBytes || !ValidProbe(window, index)) return;
    const auto *args = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
    if (args->rank < 0 || args->rankSize <= 0 || args->rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        args->rank >= args->rankSize || args->peerMemBytes != expectedBytes) return;
    __gm__ uint8_t *local = args->peerMems[args->rank];
    const uint64_t base = reinterpret_cast<uint64_t>(local);
    const uint64_t offset = ProbeOffset(window, index);
    if (local == nullptr || base % 512U != 0 || base > UINT64_MAX - expectedBytes ||
        !LegalRange(offset, kProbeBytes)) return;
    const uint64_t resultBase = reinterpret_cast<uint64_t>(result);
    if (resultBase < base + expectedBytes && base < resultBase + kResultBytes) return;

    GlobalTensor<uint32_t> target, output;
    target.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(local + offset), 8);
    output.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(result), 16);
    TPipe pipe;
    TBuf<TPosition::VECCALC> buffer;
    pipe.InitBuffer(buffer, kProbeBytes);
    auto data = buffer.Get<uint32_t>();

    DataCopy(data, target, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(output, data, 8);
    PipeBarrier<PIPE_ALL>();
    Duplicate<uint32_t>(data, ProbePattern(window, index), 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(target, data, 8);
    PipeBarrier<PIPE_ALL>();
    Duplicate<uint32_t>(data, 0U, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(data, target, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(output[8], data, 8);
    PipeBarrier<PIPE_ALL>();
}
