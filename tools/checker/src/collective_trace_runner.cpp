#include "tilexr/checker/collective_trace_runner.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tilexr/checker/trace_runtime.h"
#include "tilexr/checker/allreduce_big_data_trace_shim.h"

namespace tilexr {
namespace checker {

namespace {

constexpr uintptr_t kTraceIpcBase = static_cast<uintptr_t>(0x100000000000ULL);

}  // namespace

size_t TraceCommVirtualBytes() {
    return static_cast<size_t>(TileXR::IPC_DATA_OFFSET + TileXR::IPC_BUFF_MAX_SIZE);
}

uintptr_t TraceCommVirtualBase(int rank_size, int owner_rank, int slot) {
    const size_t comm_virtual_bytes = TraceCommVirtualBytes();
    const size_t linear_slot = static_cast<size_t>(owner_rank * rank_size + slot);
    return kTraceIpcBase + linear_slot * comm_virtual_bytes;
}

TraceScheduleSpec GetAllReduceBigDataScheduleSpec(const CheckerCase &test_case) {
    TraceScheduleSpec spec;
    spec.name = "allreduce_big_data_block_major";
    spec.rank_size = test_case.rank_size;
    spec.block_num = test_case.rank_size * 2;
    spec.visits = static_cast<size_t>(spec.block_num) * static_cast<size_t>(test_case.rank_size);
    spec.loop_order = TraceScheduleLoopOrder::kBlockMajor;
    spec.peer_window_mode = TracePeerWindowMode::kPeerSlotZeroBase;
    return spec;
}

void RegisterCollectiveTraceRanges(RankWorld *world, const CheckerCase &test_case,
                                   TraceRuntime *runtime, size_t output_bytes) {
    const size_t input_bytes = static_cast<size_t>(test_case.count) * sizeof(int32_t);
    for (int rank = 0; rank < test_case.rank_size; ++rank) {
        runtime->AddRange(world->UserInput(rank).data_ptr(), input_bytes,
                          BufferRole::kUserInput, rank, rank);
        runtime->AddRange(world->UserOutput(rank).data_ptr(), output_bytes,
                          BufferRole::kUserOutput, rank, rank);
        for (int slot = 0; slot < test_case.rank_size; ++slot) {
            ByteBuffer &comm = world->CommData(rank, slot);
            runtime->AddVirtualRange(TraceCommVirtualBase(test_case.rank_size, rank, slot),
                                     comm.data_ptr(), TraceCommVirtualBytes(), comm.size(),
                                     BufferRole::kCommData, rank, slot);
        }
    }
}

std::vector<std::vector<GM_ADDR> > InstallCollectiveTracePeerMems(
    RankWorld *world, const CheckerCase &test_case) {
    std::vector<std::vector<GM_ADDR> > original(
        static_cast<size_t>(test_case.rank_size),
        std::vector<GM_ADDR>(static_cast<size_t>(test_case.rank_size), nullptr));
    for (int rank = 0; rank < test_case.rank_size; ++rank) {
        TileXR::CommArgs &args = world->HostArgs(rank);
        for (int peer = 0; peer < test_case.rank_size; ++peer) {
            original[static_cast<size_t>(rank)][static_cast<size_t>(peer)] =
                args.peerMems[peer];
            args.peerMems[peer] =
                reinterpret_cast<GM_ADDR>(TraceCommVirtualBase(test_case.rank_size, peer, 0));
        }
    }
    return original;
}

void RestoreCollectiveTracePeerMems(RankWorld *world, const CheckerCase &test_case,
                                    const std::vector<std::vector<GM_ADDR> > &original) {
    for (int rank = 0; rank < test_case.rank_size; ++rank) {
        TileXR::CommArgs &args = world->HostArgs(rank);
        for (int peer = 0; peer < test_case.rank_size; ++peer) {
            args.peerMems[peer] =
                original[static_cast<size_t>(rank)][static_cast<size_t>(peer)];
        }
    }
}

CheckerStatus RunAllReduceBigDataTrace(RankWorld *world, const CheckerCase &test_case) {
    if (world == nullptr) {
        return CheckerStatus::Fail("rank world is null");
    }
    TraceRuntime runtime(world, test_case);
    const size_t output_bytes = static_cast<size_t>(test_case.count) * sizeof(int32_t);
    RegisterCollectiveTraceRanges(world, test_case, &runtime, output_bytes);
    const std::vector<std::vector<GM_ADDR> > original_peer_mems =
        InstallCollectiveTracePeerMems(world, test_case);
    TraceRuntime::SetCurrent(&runtime);

    const TraceScheduleSpec schedule = GetAllReduceBigDataScheduleSpec(test_case);
    for (int block_idx = 0; block_idx < schedule.block_num; ++block_idx) {
        for (int rank = 0; rank < test_case.rank_size; ++rank) {
            runtime.SetKernelContext(rank, block_idx, schedule.block_num);
            TileXR::CommArgs &args = world->HostArgs(rank);
            AllReduceBigData<int32_t> op(rank, test_case.rank_size, args.extraFlag);
            op.Init(world->UserInput(rank).data_ptr(), world->UserOutput(rank).data_ptr(),
                    reinterpret_cast<GM_ADDR>(&args), test_case.count,
                    static_cast<int64_t>(test_case.magic), TileXR::Op::ADD, 0, 0,
                    nullptr, 0, nullptr);
            op.Process();
        }
    }

    CheckerStatus materialize_status =
        runtime.ApplyMaterializer("allreduce_big_data_sum_int32");
    TraceRuntime::SetCurrent(nullptr);
    RestoreCollectiveTracePeerMems(world, test_case, original_peer_mems);
    return materialize_status;
}

}  // namespace checker
}  // namespace tilexr
