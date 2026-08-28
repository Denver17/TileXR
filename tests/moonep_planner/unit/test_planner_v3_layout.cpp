#include <cstdlib>
#include <iostream>

#include "comm_args.h"
#include "planner_common.h"
#include "planner_layout.h"
#include "tilexr_types.h"

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++failures;
    }
}

void SetBlockDimOverride(const char *value)
{
#ifdef _WIN32
    _putenv_s("TILEXR_MOONEP_PLANNER_BLOCK_DIM", value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv("TILEXR_MOONEP_PLANNER_BLOCK_DIM");
    } else {
        setenv("TILEXR_MOONEP_PLANNER_BLOCK_DIM", value, 1);
    }
#endif
}

} // namespace

int main()
{
    TileXRMoonEpV3::PlannerLayout layout {};
    SetBlockDimOverride("64");
    Check(TileXRMoonEpV3::TileXRMoonEpBuildPlannerLayout(
        128, 8, 1, 128, 1, 1, &layout) == TileXR::TILEXR_SUCCESS,
        "128-rank Planner V3 layout rejected 64 AIV blocks");
    Check(layout.blockDim == 64, "128-rank Planner V3 blockDim mismatch");
    Check(layout.dstLocalOffset % TileXRMoonEpV3::kPlannerWorkspaceAlignment == 0,
        "Planner V3 dstLocal offset is not aligned");
    Check(layout.dstLocalOffset + static_cast<uint64_t>(layout.nvS) * sizeof(int32_t) <=
            layout.workspaceBytes,
        "Planner V3 dstLocal exceeds workspace");
    Check(layout.peerDstOffset >= 128U * sizeof(int32_t),
        "Planner V3 peer dst overlaps tokens-per-expert publication");
    const uint64_t peerRowBytes = TileXRMoonEpV3::TileXRMoonEpAlignUp(
        layout.peerDstOffset + static_cast<uint64_t>(layout.routeCount) * sizeof(int32_t),
        TileXRMoonEpV3::kPlannerPeerTransferBytes);
    Check(layout.peerPublicationBytes == 128U * peerRowBytes,
        "Planner V3 peer publication does not reserve one row per source rank");

    Check(TileXRMoonEpV3::TileXRMoonEpBuildPlannerLayout(
        8, 256, 4, 32, 2, 128, &layout) == TileXR::TILEXR_SUCCESS,
        "PR113 Planner V3 layout rejected");
    Check(layout.nvS == 2040, "PR113 Planner V3 NvS mismatch");
    Check(layout.workspaceBytes >=
            layout.dstLocalOffset + 2040U * sizeof(int32_t),
        "PR113 Planner V3 dstLocal allocation is incomplete");

    Check(TileXRMoonEpV3::TileXRMoonEpBuildPlannerLayout(
        128, 1048448, 1, 128, 1, 1, &layout) == TileXR::TILEXR_SUCCESS,
        "Planner V3 rejected a peer publication that exactly fits the IPC data window");
    Check(layout.peerPublicationBytes == static_cast<uint64_t>(TileXR::IPC_BUFF_MAX_SIZE),
        "Planner V3 exact-fit peer publication size mismatch");
    Check(TileXRMoonEpV3::TileXRMoonEpBuildPlannerLayout(
        128, 1048449, 1, 128, 1, 1, &layout) != TileXR::TILEXR_SUCCESS,
        "Planner V3 accepted peer publication beyond the IPC data window");

    SetBlockDimOverride("65");
    Check(TileXRMoonEpV3::TileXRMoonEpBuildPlannerLayout(
        128, 8, 1, 128, 1, 1, &layout) != TileXR::TILEXR_SUCCESS,
        "Planner V3 accepted blockDim above the AIV limit");
    SetBlockDimOverride(nullptr);
    return failures == 0 ? 0 : 1;
}
