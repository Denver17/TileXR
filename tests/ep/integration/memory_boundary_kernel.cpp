#include "kernel_operator.h"
#include "comm_args.h"

// One AIV per rank. No remote payload access and no Dispatch algorithm.
extern "C" __global__ __aicore__ void tilexr_ep_memory_boundary_test_kernel(
    GM_ADDR commArgs, GM_ADDR result, uint64_t expectedBytes)
{
    using namespace AscendC;
    if (GetBlockIdx() != 0 || commArgs == nullptr || result == nullptr) return;
    const auto *args = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
    if (args->rank < 0 || args->rank >= args->rankSize ||
        args->peerMemBytes != expectedBytes || expectedBytes < 32) return;
    __gm__ uint8_t *local = args->peerMems[args->rank];
    if (local == nullptr) return;

    GlobalTensor<uint32_t> boundary, output;
    // Keep both the byte offset and address arithmetic 64-bit at the 4 GiB boundary.
    boundary.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(local + (expectedBytes - 32)), 8);
    output.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(result), 16);
    TPipe pipe;
    TBuf<TPosition::VECCALC> buffer;
    pipe.InitBuffer(buffer, 32);
    auto data = buffer.Get<uint32_t>();

    DataCopy(data, boundary, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(output, data, 8);
    PipeBarrier<PIPE_ALL>();
    Duplicate<uint32_t>(data, 0xFFFFFFFFU, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(boundary, data, 8);
    PipeBarrier<PIPE_ALL>();
    // Clear UB before the read, so a missing read cannot pass on the write pattern.
    Duplicate<uint32_t>(data, 0U, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(data, boundary, 8);
    PipeBarrier<PIPE_ALL>();
    DataCopy(output[8], data, 8);
    PipeBarrier<PIPE_ALL>();
}
