/*
 * 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "combine_v2_layout.h"
#include "comm_args.h"
#include "tilexr_moonep.h"

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void CheckStatus(int actual, int expected, const char *message)
{
    if (actual != expected) {
        std::cerr << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

void TestTargetLayout()
{
    using namespace TileXRMoonEp;

    CombineV2Layout layout {};
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        8192, 3584, 16, 131072, TILEXR_MOONEP_DTYPE_BFLOAT16, &layout),
        TILEXR_MOONEP_SUCCESS, "target layout");
    Check(layout.slots == 131072U, "target slots mismatch");
    Check(layout.rowBytes == 7168U, "target row bytes mismatch");
    Check(layout.expertBytes == 939524096U, "target expert bytes mismatch");
    Check(layout.scratchOffset[0] == layout.expertBytes,
        "scratch epoch 0 offset mismatch");
    Check(layout.scratchOffset[1] == layout.scratchOffset[0] + layout.expertBytes,
        "scratch epoch 1 offset mismatch");
    Check(layout.doneBytes == 32768U, "done bytes mismatch");
    Check(layout.controlSourceOffset == 2818605056ULL,
        "control source offset mismatch");
    Check(layout.failureOffset == 2818607104ULL,
        "failure offset mismatch");
    Check(layout.controlSourceBytes == 2048U, "control source bytes mismatch");
    Check(layout.failureBytes == 2048U, "failure bytes mismatch");
    Check(layout.collectiveStatusOffset == 2818609152ULL,
        "collective status offset mismatch");
    Check(layout.collectiveStatusBytes == 2048U,
        "collective status bytes mismatch");
    Check(layout.outputOffset == 2818611200ULL,
        "target output offset mismatch");
    Check(layout.outputOffset ==
            layout.collectiveStatusOffset + layout.collectiveStatusBytes,
        "output offset mismatch");
    Check(layout.outputBytes == 58720256U, "output bytes mismatch");
    Check(layout.totalBytes == 2879389696ULL, "target total bytes mismatch");
}

void TestSmallAndInvalidLayouts()
{
    using namespace TileXRMoonEp;
    CombineV2Layout layout {};
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        8, 3584, 16, 128, TILEXR_MOONEP_DTYPE_BFLOAT16, &layout),
        TILEXR_MOONEP_SUCCESS, "small layout");
    Check(layout.expertBytes == 917504U, "small expert bytes mismatch");
    Check(layout.totalBytes == 4194304U, "small total bytes mismatch");
    Check(layout.totalBytes % kCombineV2RegistrationAlignmentBytes == 0U,
        "workspace is not registration aligned");
    Check(layout.outputOffset >=
            layout.collectiveStatusOffset + layout.collectiveStatusBytes &&
        layout.outputOffset + layout.outputBytes <= layout.totalBytes,
        "small output overlaps control workspace");

    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        256, 1024, 4, 2040, TILEXR_MOONEP_DTYPE_BFLOAT16, &layout),
        TILEXR_MOONEP_SUCCESS, "PR113 hidden layout");
    Check(layout.rowBytes == 2048U && layout.slots == 2040U,
        "PR113 hidden layout mismatch");
    Check(layout.outputBytes == 524288U,
        "PR113 hidden output bytes mismatch");
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        256, 1, 4, 2040, TILEXR_MOONEP_DTYPE_FLOAT32, &layout),
        TILEXR_MOONEP_SUCCESS, "PR113 route-weight layout");
    Check(layout.rowBytes == sizeof(float), "route-weight row size mismatch");
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        8192, 3584, 16, 131071, TILEXR_MOONEP_DTYPE_BFLOAT16, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "undersized slots");
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        8192, 3584, 16, 131072, TILEXR_MOONEP_DTYPE_FLOAT16, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "unsupported dtype");
    CheckStatus(TileXRMoonEpBuildCombineV2Layout(
        8, 3584, 16, 128, TILEXR_MOONEP_DTYPE_BFLOAT16, nullptr),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "null layout");
}

void TestWeightMemoryLayout()
{
    using namespace TileXRMoonEp;
    CombineV2WeightLayout layout {};
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(131072, 64, &layout),
        TILEXR_MOONEP_SUCCESS, "weight memory layout");
    Check(layout.recordOffset == 0U &&
        layout.recordEpochBytes == 131072U * 16U &&
        layout.recordBytes == 2U * layout.recordEpochBytes,
        "weight record layout mismatch");
    Check(layout.doneOffset == layout.recordBytes &&
        layout.doneEpochBytes == 64U * 64U &&
        layout.doneBytes == 2U * layout.doneEpochBytes,
        "weight done layout mismatch");
    Check(layout.totalBytes == layout.doneOffset + layout.doneBytes,
        "weight total layout mismatch");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(129, 8, &layout),
        TILEXR_MOONEP_SUCCESS, "unaligned weight memory layout");
    Check(layout.recordEpochBytes == 2112U &&
        layout.recordBytes == 4224U && layout.doneOffset == 4224U &&
        layout.doneOffset % 64U == 0U &&
        layout.doneEpochBytes == 512U && layout.doneBytes == 1024U,
        "weight record alignment mismatch");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(0, 64, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "zero weight slots");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(128, 0, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "zero weight world");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(128, TileXR::TILEXR_MAX_RANK_SIZE + 1, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "oversized weight world");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(
        static_cast<int64_t>(TileXR::IPC_BUFF_MAX_SIZE / 16U), 128, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "weight layout beyond IPC capacity");
    CheckStatus(TileXRMoonEpBuildCombineV2WeightLayout(INT64_MAX, 128, &layout),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "overflowing weight layout");
}

} // namespace

int main()
{
    TestTargetLayout();
    TestSmallAndInvalidLayouts();
    TestWeightMemoryLayout();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
