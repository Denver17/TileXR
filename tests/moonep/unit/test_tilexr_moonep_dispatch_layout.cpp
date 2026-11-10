/*
 * 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。
 */
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "dispatch_layout.h"
#include "comm_args.h"
#include "dispatch_diagnostics.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) do { if (!(expr)) { std::cerr << "CHECK_TRUE line " \
    << __LINE__ << ": " #expr << std::endl; ++g_failures; } } while (0)
#define CHECK_EQ(lhs, rhs) do { const auto lhsValue = (lhs); const auto rhsValue = (rhs); \
    if (lhsValue != rhsValue) { std::cerr << "CHECK_EQ line " << __LINE__ \
    << ": " #lhs " != " #rhs << std::endl; ++g_failures; } } while (0)

struct Range {
    uint64_t offset;
    uint64_t bytes;
};

void CheckDisjointLayout(const TileXRMoonEp::MoonEpDispatchUrmaLayout &layout)
{
    const std::vector<Range> ranges {
        {layout.hidden.sourceOffset, layout.hidden.sourceBytes},
        {layout.hidden.scratchOffset, layout.hidden.scratchBytes},
        {layout.weight.sourceOffset, layout.weight.sourceBytes},
        {layout.weight.scratchOffset, layout.weight.scratchBytes},
        {layout.completionFlagsOffset, layout.completionFlagsBytes},
        {layout.signalOffset, layout.signalBytes},
        {layout.hiddenDfxOffset, layout.dfxBytes},
        {layout.weightDfxOffset, layout.dfxBytes},
        {layout.kernelStatusOffset, layout.kernelStatusBytes},
    };
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const Range &current = ranges[index];
        CHECK_TRUE(current.bytes > 0U);
        CHECK_TRUE(current.offset <= layout.totalBytes);
        CHECK_TRUE(current.bytes <= layout.totalBytes - current.offset);
        for (std::size_t other = index + 1; other < ranges.size(); ++other) {
            const Range &candidate = ranges[other];
            CHECK_TRUE(current.offset + current.bytes <= candidate.offset ||
                candidate.offset + candidate.bytes <= current.offset);
        }
    }
}

void TestReferenceShape()
{
    TileXRMoonEp::MoonEpDispatchUrmaLayout layout {};
    CHECK_EQ(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        128, 128, 16, 3584, 2048,
        &layout), TileXR::TILEXR_SUCCESS);
    CHECK_EQ(layout.routeCount, 2048);
    CHECK_EQ(layout.destinationCapacity, 2048);
    CHECK_EQ(layout.hidden.rowBytes, UINT64_C(7168));
    CHECK_EQ(layout.hidden.sourceBytes, UINT64_C(917504));
    CHECK_EQ(layout.hidden.scratchSlotBytes, UINT64_C(14680064));
    CHECK_EQ(layout.weight.rowBytes, UINT64_C(4));
    CHECK_EQ(layout.weight.sourceBytes, UINT64_C(8192));
    CHECK_EQ(layout.weight.scratchSlotBytes, UINT64_C(8192));
    CHECK_EQ(layout.hidden.sourceOffset, UINT64_C(0));
    CHECK_TRUE(layout.hidden.scratchOffset >=
        layout.hidden.sourceOffset + layout.hidden.sourceBytes);
    CHECK_TRUE(layout.weight.sourceOffset >=
        layout.hidden.scratchOffset + layout.hidden.scratchBytes);
    CHECK_TRUE(layout.weight.scratchOffset >=
        layout.weight.sourceOffset + layout.weight.sourceBytes);
    CHECK_TRUE(layout.commonOffset >=
        layout.weight.scratchOffset + layout.weight.scratchBytes);
    CHECK_EQ(layout.completionFlagsBytes, UINT64_C(8192));
    CHECK_EQ(layout.creditBytes, UINT64_C(2) * TileXR::TILEXR_MAX_RANK_SIZE * 512U);
    CHECK_EQ(layout.creditSourceBytes, UINT64_C(64) * 16U * 64U);
    CHECK_TRUE(layout.creditOffset >=
        layout.completionFlagsOffset + layout.completionFlagsBytes);
    CHECK_TRUE(layout.creditSourceOffset >=
        layout.creditOffset + layout.creditBytes);
    CHECK_TRUE(layout.signalOffset >=
        layout.creditSourceOffset + layout.creditSourceBytes);
    CHECK_EQ(layout.dfxBytes,
        UINT64_C(64) * sizeof(TileXRMoonEp::DispatchDfxRecord));
    CHECK_EQ(layout.totalBytes, UINT64_C(30) * 1024U * 1024U);
    CHECK_TRUE(layout.requiredBytes <= layout.totalBytes);
    CHECK_EQ(layout.totalBytes % TileXRMoonEp::kDispatchRegistrationAlignmentBytes,
        UINT64_C(0));
    CheckDisjointLayout(layout);

    TileXRMoonEp::MoonEpDispatchUrmaLayout expanded = layout;
    const uint64_t oldCommonOffset = expanded.commonOffset;
    CHECK_EQ(TileXRMoonEp::TileXRMoonEpBindDispatchUrmaWorkspace(
        layout.totalBytes + TileXRMoonEp::kDispatchRegistrationAlignmentBytes,
        &expanded), TileXR::TILEXR_SUCCESS);
    CHECK_EQ(expanded.commonOffset,
        oldCommonOffset + TileXRMoonEp::kDispatchRegistrationAlignmentBytes);
    CheckDisjointLayout(expanded);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBindDispatchUrmaWorkspace(
        layout.totalBytes - TileXRMoonEp::kDispatchRegistrationAlignmentBytes,
        &layout) != TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBindDispatchUrmaWorkspace(
        layout.totalBytes + 64U, &layout) != TileXR::TILEXR_SUCCESS);
}

void TestFailuresAndModes()
{
    TileXRMoonEp::MoonEpDispatchUrmaLayout layout {};
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        0, 1, 1, 1, 1, &layout) !=
        TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        TileXR::TILEXR_MAX_RANK_SIZE + 1, 1, 1, 1, 1, &layout) !=
        TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(1,
        std::numeric_limits<int64_t>::max(), 2, 1,
        std::numeric_limits<int64_t>::max(), &layout) != TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        1, 8, 2, 64, 15, &layout) != TileXR::TILEXR_SUCCESS);
    CHECK_EQ(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        1, 8, 2, 64, 32, &layout),
        TileXR::TILEXR_SUCCESS);
    CHECK_EQ(layout.routeCount, 16);
    CHECK_EQ(layout.destinationCapacity, 32);
    CHECK_EQ(layout.hidden.scratchSlotBytes, UINT64_C(4096));
    CHECK_EQ(layout.weight.scratchSlotBytes, UINT64_C(128));
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpGetActiveDispatchUrmaLayout(
        layout, TileXRMoonEp::DispatchPayloadMode::Hidden) == &layout.hidden);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpGetActiveDispatchUrmaLayout(
        layout, TileXRMoonEp::DispatchPayloadMode::RouteWeight) == &layout.weight);

    CHECK_EQ(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        1, 1, 1, 1, 1, &layout), TileXR::TILEXR_SUCCESS);
    CheckDisjointLayout(layout);
    CHECK_EQ(layout.hidden.rowBytes, UINT64_C(2));
    CHECK_EQ(layout.weight.rowBytes, UINT64_C(4));

    CHECK_EQ(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        3, 3, 2, 3, 7, &layout), TileXR::TILEXR_SUCCESS);
    CheckDisjointLayout(layout);
    CHECK_EQ(layout.routeCount, 6);
    CHECK_EQ(layout.destinationCapacity, 7);
    CHECK_EQ(layout.hidden.rowBytes, UINT64_C(6));
}

} // namespace

int main()
{
    TestReferenceShape();
    TestFailuresAndModes();
    return g_failures == 0 ? 0 : 1;
}
