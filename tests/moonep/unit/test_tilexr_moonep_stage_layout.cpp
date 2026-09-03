#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "comm_args.h"
#include "dispatch_layout.h"
#include "moonep_peer_window.h"

namespace {

int failures = 0;

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expr << '\n'; \
            ++failures; \
        } \
    } while (0)

void CheckStatus(const char *name, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
        ++failures;
    }
}

TileXRMoonEpTensorV1 Tensor(uint32_t dtype, uint32_t rank,
    int64_t d0, int64_t d1, uint64_t elements, uintptr_t address)
{
    TileXRMoonEpTensorV1 tensor {};
    tensor.structSize = sizeof(tensor);
    tensor.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    tensor.data = reinterpret_cast<void *>(address);
    tensor.elementCount = elements;
    tensor.dtype = dtype;
    tensor.rank = rank;
    tensor.shape[0] = d0;
    tensor.shape[1] = d1;
    return tensor;
}

TileXRMoonEpPlanV1 Plan(int64_t r, int64_t s, int64_t k, int64_t nvS)
{
    TileXRMoonEpPlanV1 plan {};
    plan.structSize = sizeof(plan);
    plan.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    plan.n = s * k;
    plan.r = r;
    plan.e = 8;
    plan.b = 2;
    plan.nvS = nvS;
    plan.k = k;
    plan.dst = reinterpret_cast<void *>(uintptr_t {0x2000});
    plan.expertsToCopy = reinterpret_cast<void *>(uintptr_t {0x2100});
    plan.zeroFillRanges = reinterpret_cast<void *>(uintptr_t {0x2200});
    plan.remoteStats = reinterpret_cast<void *>(uintptr_t {0x2300});
    plan.dupGroups = reinterpret_cast<void *>(uintptr_t {0x2400});
    plan.dupLoffs = reinterpret_cast<void *>(uintptr_t {0x2500});
    plan.dupCounts = reinterpret_cast<void *>(uintptr_t {0x2600});
    plan.status = reinterpret_cast<void *>(uintptr_t {0x2700});
    return plan;
}

void TestAlignment()
{
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpAlignDispatchBytes(1) == 32);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpAlignDispatchBytes(32) == 32);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpAlignDispatchBytes(33) == 64);
    CHECK_TRUE(TileXRMoonEp::TileXRMoonEpAlignDispatchBytes(
        std::numeric_limits<uint64_t>::max()) == std::numeric_limits<uint64_t>::max());
}

void TestDispatchPairedLayout()
{
    TileXRMoonEpPlanV1 plan = Plan(2, 3, 2, 8);
    TileXRMoonEpTensorV1 hiddenSh = Tensor(
        TILEXR_MOONEP_DTYPE_BFLOAT16, 2, 3, 17, 51, 0x1000);
    TileXRMoonEpTensorV1 hiddenNvsh = Tensor(
        TILEXR_MOONEP_DTYPE_BFLOAT16, 2, 8, 17, 136, 0x2000);
    TileXRMoonEpTensorV1 weightsSk = Tensor(
        TILEXR_MOONEP_DTYPE_FLOAT32, 2, 3, 2, 6, 0x3000);
    TileXRMoonEpTensorV1 weightsNvs = Tensor(
        TILEXR_MOONEP_DTYPE_FLOAT32, 1, 8, 0, 8, 0x4000);
    TileXRMoonEp::DispatchLayout layout {};
    CheckStatus("paired dispatch", TileXRMoonEp::TileXRMoonEpBuildDispatchLayout(
        1, 2, &plan, &hiddenSh, &weightsSk, &hiddenNvsh, &weightsNvs,
        TILEXR_MOONEP_FLAG_BUILD_DEDUP, &layout), TILEXR_MOONEP_SUCCESS);
    CHECK_TRUE(layout.s == 3 && layout.n == 6 && layout.nvS == 8 && layout.k == 2);
    CHECK_TRUE(layout.hiddenSize == 17 && layout.hiddenRowBytes == 34);
    CHECK_TRUE(layout.hiddenChunkBytes == 34 && layout.hiddenChunkStride == 64);
    CHECK_TRUE(layout.hiddenPayloadBytes == 512 && layout.chunkCount == 1);
    CHECK_TRUE(layout.routeWeightsOffset == 512 && layout.routeWeightsBytes == 32);
    CHECK_TRUE(layout.dedupParentsOffset == 544 && layout.dedupParentsBytes == 32);
    CHECK_TRUE(layout.dedupGroupMapOffset == 576 && layout.windowBytes == 608);

    CheckStatus("hidden-only reuse", TileXRMoonEp::TileXRMoonEpBuildDispatchLayout(
        1, 2, &plan, &hiddenSh, nullptr, &hiddenNvsh, nullptr,
        TILEXR_MOONEP_FLAG_NONE, &layout), TILEXR_MOONEP_SUCCESS);
    CHECK_TRUE(layout.routeWeightsBytes == 0 && layout.dedupParentsBytes == 0 &&
        layout.windowBytes == 512);

    CheckStatus("unpaired weights", TileXRMoonEp::TileXRMoonEpBuildDispatchLayout(
        1, 2, &plan, &hiddenSh, &weightsSk, &hiddenNvsh, nullptr,
        TILEXR_MOONEP_FLAG_NONE, &layout), TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    CheckStatus("unknown flag", TileXRMoonEp::TileXRMoonEpBuildDispatchLayout(
        1, 2, &plan, &hiddenSh, nullptr, &hiddenNvsh, nullptr, UINT64_C(1) << 20,
        &layout), TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
}

void TestChunkingAndPlanAgreement()
{
    TileXRMoonEpPlanV1 plan = Plan(2, 1, 1, 1);
    const int64_t hidden = static_cast<int64_t>(
        TileXR::IPC_BUFF_MAX_SIZE / sizeof(uint16_t)) + 1;
    TileXRMoonEpTensorV1 input = Tensor(TILEXR_MOONEP_DTYPE_BFLOAT16, 2,
        1, hidden, static_cast<uint64_t>(hidden), 0x1000);
    TileXRMoonEpTensorV1 output = Tensor(TILEXR_MOONEP_DTYPE_BFLOAT16, 2,
        1, hidden, static_cast<uint64_t>(hidden), 0x2000);
    TileXRMoonEp::DispatchLayout dispatch {};
    CheckStatus("chunked dispatch", TileXRMoonEp::TileXRMoonEpBuildDispatchLayout(
        0, 2, &plan, &input, nullptr, &output, nullptr, 0, &dispatch),
        TILEXR_MOONEP_SUCCESS);
    CHECK_TRUE(dispatch.chunkCount == 2 &&
        dispatch.hiddenPayloadBytes <= static_cast<uint64_t>(TileXR::IPC_BUFF_MAX_SIZE));

}

} // namespace

int main()
{
    TestAlignment();
    TestDispatchPairedLayout();
    TestChunkingAndPlanAgreement();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
