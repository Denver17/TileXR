#include "tilexr/checker/collective_trace_runner.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tilexr/checker/allgather_hdb_trace_shim.h"
#include "tilexr/checker/trace_runtime.h"

namespace tilexr {
namespace checker {

TraceScheduleSpec GetAllGatherHierarchyDoubleRingScheduleSpec(
    const CheckerCase &test_case) {
    TraceScheduleSpec spec;
    spec.name = "allgather_hierarchy_double_ring_stage_major";
    spec.rank_size = test_case.rank_size;
    spec.block_num = STAGE_NUM * test_case.rank_size;
    spec.visits = static_cast<size_t>(spec.block_num) * static_cast<size_t>(test_case.rank_size);
    spec.loop_order = TraceScheduleLoopOrder::kBlockMajor;
    spec.peer_window_mode = TracePeerWindowMode::kPeerSlotZeroBase;
    return spec;
}

CheckerStatus RunAllGatherHierarchyDoubleRingTrace(RankWorld *world,
                                                   const CheckerCase &test_case) {
    if (world == nullptr) {
        return CheckerStatus::Fail("rank world is null");
    }

    TraceRuntime runtime(world, test_case);
    const size_t output_bytes =
        static_cast<size_t>(test_case.rank_size) * static_cast<size_t>(test_case.count) *
        sizeof(int32_t);
    RegisterCollectiveTraceRanges(world, test_case, &runtime, output_bytes);
    const std::vector<std::vector<GM_ADDR> > original_peer_mems =
        InstallCollectiveTracePeerMems(world, test_case);
    TraceRuntime::SetCurrent(&runtime);

    const TraceScheduleSpec schedule =
        GetAllGatherHierarchyDoubleRingScheduleSpec(test_case);
    for (int block_idx = 0; block_idx < schedule.block_num; ++block_idx) {
        for (int rank = 0; rank < test_case.rank_size; ++rank) {
            runtime.SetKernelContext(rank, block_idx, schedule.block_num);
            TileXR::CommArgs &args = world->HostArgs(rank);
            AllGatherHierarchyDoubleRing<uint8_t> op(rank, test_case.rank_size, args.extraFlag);
            op.Init(world->UserInput(rank).data_ptr(), world->UserOutput(rank).data_ptr(),
                    reinterpret_cast<GM_ADDR>(&args),
                    static_cast<int64_t>(test_case.count) * static_cast<int64_t>(sizeof(int32_t)),
                    static_cast<int64_t>(test_case.magic), TileXR::Op::COPYONLY, 0, 0,
                    nullptr, 0, nullptr);
            op.Process();
        }
    }

    CheckerStatus materialize_status =
        runtime.ApplyMaterializer("allgather_hierarchy_double_ring_int32");
    TraceRuntime::SetCurrent(nullptr);
    RestoreCollectiveTracePeerMems(world, test_case, original_peer_mems);
    return materialize_status;
}

}  // namespace checker
}  // namespace tilexr
