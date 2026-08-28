#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "reduce_grad_common.h"
#include "reduce_grad_layout.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++g_failures;
    }
}

void CheckStatus(const std::string &label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void TestEqualProjectionAllocationAndWorkspace()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {
        UINT64_C(14) << 20, UINT64_C(14) << 20, UINT64_C(14) << 20};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("8-rank layout", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        3, 8, 384, 48, rows, 8, 0, &layout), TileXR::TILEXR_SUCCESS);

    Check(layout.projectionQpCounts[0] == 3 &&
        layout.projectionQpCounts[1] == 3 &&
        layout.projectionQpCounts[2] == 2,
        "equal projection bytes must receive stable 3/3/2 QP allocation");
    Check(layout.projectionQpBase[0] == 0 && layout.projectionQpBase[1] == 3 &&
        layout.projectionQpBase[2] == 6,
        "projection QP bases must be contiguous");
    for (uint32_t qp = 0; qp < 8; ++qp) {
        const uint32_t expected = qp < 3 ? 0 : (qp < 6 ? 1 : 2);
        Check(layout.qpProjection[qp] == expected,
            "QP-to-projection mapping is not stable");
    }

    Check(layout.chunkBytes == TileXRMoonEp::kReduceGradDefaultChunkBytes,
        "default chunk must be 8 MiB");
    Check(layout.laneStateBytes ==
            8 * TileXRMoonEp::kReduceGradLaneStateStrideBytes &&
        layout.stagingOffset == layout.laneStateBytes,
        "lane state sizing mismatch");
    Check(layout.bankStrideBytes == UINT64_C(64) << 20 &&
        layout.laneStrideBytes == UINT64_C(128) << 20,
        "rank-sized bank strides mismatch");
    Check(layout.workspaceBytes == UINT64_C(1026) << 20,
        "8-rank owner-pull workspace mismatch");
    Check(layout.workspaceBytes % TileXRMoonEp::kReduceGradWorkspaceAlignment == 0,
        "workspace must retain 2 MiB registration alignment");
    Check(layout.blockDim == TileXRMoonEp::kReduceGradMaxAivBlockCount,
        "default launch must use all AIV blocks");
}

void TestProportionalAllocationIsStable()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {
        UINT64_C(16) << 20, UINT64_C(8) << 20, UINT64_C(4) << 20};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("weighted layout", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 16, 384, 24, rows, 8, UINT64_C(2) << 20, &layout),
        TileXR::TILEXR_SUCCESS);
    Check(layout.projectionQpCounts[0] == 5 &&
        layout.projectionQpCounts[1] == 2 &&
        layout.projectionQpCounts[2] == 1,
        "weighted QP allocation must follow projection bytes with stable ties");
    Check(layout.bankStrideBytes == UINT64_C(32) << 20 &&
        layout.laneStrideBytes == UINT64_C(64) << 20 &&
        layout.workspaceBytes == UINT64_C(514) << 20,
        "16-rank 2 MiB workspace arithmetic mismatch");
    Check(layout.chunkCounts[0] == 32 && layout.chunkCounts[1] == 16 &&
        layout.chunkCounts[2] == 8,
        "projection chunk counts mismatch");
}

void TestSharedDomainUsesBoundedActiveLanes()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {
        UINT64_C(14) << 20, UINT64_C(14) << 20, UINT64_C(14) << 20};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("48-QP shared-domain layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 4, 4, 1, rows, 48, UINT64_C(8) << 20, &layout),
        TileXR::TILEXR_SUCCESS);
    Check(layout.transportQpCount == 48 && layout.qpCount == 3 &&
        layout.laneCount == 3,
        "shared domain must expose all transport QPs and use three active lanes");
    Check(layout.lanePhysicalQps[0] == 0 && layout.lanePhysicalQps[1] == 1 &&
        layout.lanePhysicalQps[2] == 16,
        "shared-domain lanes must preserve the measured 6/6/2 route mapping");
    Check(layout.projectionQpCounts[0] == 1 &&
        layout.projectionQpCounts[1] == 1 &&
        layout.projectionQpCounts[2] == 1,
        "48-QP shared domain must allocate one active lane per projection");
    Check(layout.workspaceBytes == UINT64_C(194) << 20,
        "inactive shared-domain QPs must not increase ReduceGrad workspace");
    Check(layout.barrierPhysicalQp == TileXRMoonEp::kReduceGradInvalidPhysicalQp &&
        layout.barrierBytes == 0,
        "same-node layout must retain the peer-memory barrier");
}

void TestCrossNodeBarrierReservesPhysicalQp()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {
        UINT64_C(14) << 20, UINT64_C(14) << 20, UINT64_C(14) << 20};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("48-QP cross-node layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 16, 8, 384, 24, rows, 48, UINT64_C(8) << 20, &layout),
        TileXR::TILEXR_SUCCESS);
    Check(layout.qpCount == 3 && layout.laneCount == 3 &&
        layout.lanePhysicalQps[0] == 0 && layout.lanePhysicalQps[1] == 1 &&
        layout.lanePhysicalQps[2] == 16 && layout.barrierPhysicalQp == 47,
        "shared cross-node layout must retain 0/1/16 lanes and reserve QP47");
    Check(layout.barrierBytes ==
            16 * TileXR::TILEXR_UDMA_CACHE_LINE_SIZE &&
        layout.barrierOffset >= layout.stagingOffset +
            layout.laneCount * layout.laneStrideBytes &&
        layout.workspaceBytes >= layout.barrierOffset + layout.barrierBytes,
        "cross-node barrier token area must fit after the payload");

    CheckStatus("8-QP cross-node layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 16, 8, 384, 24, rows, 8, UINT64_C(8) << 20, &layout),
        TileXR::TILEXR_SUCCESS);
    Check(layout.qpCount == 7 && layout.laneCount == 7 &&
        layout.barrierPhysicalQp == 7,
        "dedicated cross-node layout must reserve the final physical QP");

    CheckStatus("4-QP cross-node layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 16, 8, 384, 24, rows, 4, UINT64_C(8) << 20, &layout),
        TileXR::TILEXR_SUCCESS);
    Check(layout.qpCount == 3 && layout.barrierPhysicalQp == 3,
        "minimum supported cross-node layout must preserve three projections");

    CheckStatus("3-QP cross-node layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 16, 8, 384, 24, rows, 3, UINT64_C(8) << 20, &layout),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);
}

void TestMinimumRankCount()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {64, 128, 256};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("one-rank layout", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 1, 8, 8, rows, 3, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("three-rank layout", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 3, 6, 2, rows, 3, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("four-rank layout", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 4, 8, 2, rows, 3, 0, &layout), TileXR::TILEXR_SUCCESS);
    Check(layout.bankStrideBytes == UINT64_C(32) << 20 &&
        layout.laneStrideBytes == UINT64_C(64) << 20 &&
        layout.workspaceBytes == UINT64_C(194) << 20,
        "4-rank workspace arithmetic mismatch");
}

void TestSlotsMayExceedExpertsPerRank()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {1024, 1024, 1024};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("native dedicated-suite layout",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 8, 64, 14, rows, 8, 0, &layout), TileXR::TILEXR_SUCCESS);
    Check(layout.expertsPerRank == 8 && layout.prefetchSlots == 14,
        "ReduceGrad must support source slots independently of local expert count");
}

void TestChunkAlignment()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {1024, 2048, 4096};
    TileXRMoonEp::ReduceGradLayout layout {};
    const uint64_t requested = (UINT64_C(2) << 20) + 1;
    CheckStatus("unaligned chunk", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 64, 8, rows, 3, requested, &layout), TileXR::TILEXR_SUCCESS);
    Check(layout.chunkBytes == requested + 511,
        "chunk must align up to the 512-byte UDMA boundary");
}

void TestInvalidInputsAndOverflow()
{
    const uint64_t rows[TileXRMoonEp::kReduceGradProjectionCount] = {1, 1, 1};
    TileXRMoonEp::ReduceGradLayout layout {};
    CheckStatus("null output", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, rows, 3, 0, nullptr), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("bad rank", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        8, 8, 16, 2, rows, 3, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("nondivisible experts", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 17, 2, rows, 3, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("too few QPs", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, rows, 2, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("too many transport QPs", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, rows, 33, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("oversized chunk", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, rows, 3, UINT64_C(1) << 32, &layout),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("aligned chunk overflow", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, rows, 3, UINT32_MAX, &layout),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckStatus("contributor index overflow",
        TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
            0, 8, 64, std::numeric_limits<int32_t>::max() / 8 + 1,
            rows, 3, 0, &layout), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    uint64_t overflowRows[TileXRMoonEp::kReduceGradProjectionCount] = {
        std::numeric_limits<uint64_t>::max(), 1, 1};
    CheckStatus("row byte overflow", TileXRMoonEp::TileXRMoonEpBuildReduceGradLayout(
        0, 8, 16, 2, overflowRows, 3, 0, &layout),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

} // namespace

int main()
{
    TestEqualProjectionAllocationAndWorkspace();
    TestProportionalAllocationIsStable();
    TestSharedDomainUsesBoundedActiveLanes();
    TestCrossNodeBarrierReservesPhysicalQp();
    TestMinimumRankCount();
    TestSlotsMayExceedExpertsPerRank();
    TestChunkAlignment();
    TestInvalidInputsAndOverflow();
    return g_failures == 0 ? 0 : 1;
}
