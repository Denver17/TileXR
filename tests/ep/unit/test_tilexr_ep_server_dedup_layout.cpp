#include <limits>

#include "ep_memory_server_dedup_layout.h"
#include "ep_memory_server_dedup_count_layout.h"
#include "server_dedup_test_checks.h"
#include "tilexr_types.h"

int main()
{
    using namespace TileXREp;
    using namespace TileXR;
    using ServerDedupTest::Check;
    EpMemoryServerDedupLayout layout {};
    struct Case { int64_t ranks; int64_t experts; int64_t k; uint64_t rows; uint64_t stride; };
    const Case cases[] = {
        {16, 32, 1, 4096, 32}, {64, 384, 6, 98304, 64},
        {128, 384, 6, 98304, 64}, {16, 512, 32, 131072, 256},
        {64, 384, 8, 98304, 64}, {128, 384, 16, 98304, 128},
        {16, 16, 1, 4096, 32}, {16, 1024, 32, 65536, 256},
        {64, 1024, 16, 131072, 128}, {128, 1024, 8, 131072, 64},
        {24, 1008, 32, 98304, 256}
    };
    for (const auto &test : cases) {
        // Larger expert domains need fewer tokens to fit one 4 GiB bank.
        const uint64_t bs = test.experts >= 1008 ? 128U : 256U;
        Check(TileXREpBuildMemoryServerDedupLayout(test.ranks, bs, 7168, test.k, test.experts,
            &layout) == TILEXR_SUCCESS, "supported layout");
        Check(layout.capacityRows == test.rows && layout.groupRowStrideBytes == test.stride,
            "capacity and group stride");
        Check(layout.assistExBytes == test.rows * 32 && layout.groupBytes == 8 * bs * test.stride,
            "metadata byte sizes");
        Check(layout.sourceMaskBytes == bs * 4U && layout.xBytes == bs * 7168U * 2U, "input and mask sizes");
        Check(layout.expertIdsBytes == bs * test.k * 4U && layout.weightBytes == layout.expertIdsBytes,
            "route input sizes");
        Check(layout.sendCountsBytes == static_cast<uint64_t>(test.experts) * 4U &&
            layout.expertTokenNumsBytes == static_cast<uint64_t>(test.experts / test.ranks) * 8U,
            "count output sizes");
    }
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 3, 1, 32, &layout) == TILEXR_SUCCESS &&
        layout.xBytes == 6 && layout.expandXBytes == 96 && layout.groupBytes == 256, "tight H tail and K=1");
    const int64_t invalid[][5] = {
        {0, 1, 1, 1, 32}, {8, 1, 1, 1, 32}, {17, 1, 1, 1, 34}, {136, 1, 1, 1, 272},
        {16, 0, 1, 1, 32}, {16, -1, 1, 1, 32}, {16, 1, 0, 1, 32}, {16, 1, -1, 1, 32},
        {16, 1, 1, 0, 32}, {16, 1, 1, 33, 32}, {16, 1, 1, 17, 16}, {16, 1, 1, 1, 1040},
        {16, 1, 1, 1, 33}, {16, 1, 1, 1, -32},
        {16, std::numeric_limits<int64_t>::max(), 1, 1, 32},
        {16, 1, std::numeric_limits<int64_t>::max(), 1, 32},
        {128, std::numeric_limits<int64_t>::max() / 128, 1, 32, 4096}
    };
    for (const auto &test : invalid) {
        layout.groupBytes = 123;
        Check(TileXREpBuildMemoryServerDedupLayout(test[0], test[1], test[2], test[3], test[4],
            &layout) == TILEXR_ERROR_PARA_CHECK_FAIL, "invalid or overflowing layout rejected");
        Check(layout.groupBytes == 0 && layout.capacityRows == 0, "failure clears stale layout");
    }
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 1, 1, 32, nullptr) == TILEXR_ERROR_PARA_CHECK_FAIL,
        "null layout rejected");

    // cnt uses bounded UB even though this increment does not allocate hidden buffers.
    Check(TileXREpBuildMemoryServerDedupLayout(128, 4096, 1, 32, 1024, &layout) ==
        TILEXR_ERROR_PARA_CHECK_FAIL, "route scratch exceeding UB rejected");
    const auto countLayout = TileXREpServerDedupCountLayout(1024);
    Check(countLayout.cntOffset == 12800 && countLayout.cumsumOffset == 32768 &&
        countLayout.prefixOffset == 40960 && countLayout.prefixStride == 4096 &&
        countLayout.cumsumFlagOffset == 106496 && countLayout.counterOffset == 128000 && countLayout.stateBytes == 129024,
        "per-window count state and one isolated counter fit the control prefix");
    Check(TileXREpBuildMemoryServerDedupLayout(128, 256, 7168, 6, 384, &layout) == TILEXR_SUCCESS &&
        layout.data.sourceOffset == 2097152 && layout.data.sourceBytes == 23592960 &&
        layout.data.dispatchOffset == 25690112 && layout.data.dispatchBytes == 1560281088 &&
        layout.data.relayOffset == 1585971200 && layout.data.requiredBytes == 1774714880,
        "fixed control prefix followed by token and return reservations");
    Check(countLayout.stateBytes <= kEpServerDedupControlBytes &&
        kEpServerDedupPeerBytes == UINT64_C(8589934592), "8 GiB domain has room for both control prefixes");
    for (int64_t magic = 1; magic <= 5; ++magic) {
        const uint64_t start = TileXREpServerDedupWindowOffset(magic);
        Check(start == (magic % 2 ? UINT64_C(4294967296) : 0), "magic alternates exact 4 GiB windows");
        Check(start == TileXREpServerDedupWindowOffset(magic + 2), "two calls later reuses the same bank");
        Check(start + layout.data.requiredBytes <= kEpServerDedupPeerBytes - TILEXR_FLAG_BUFF_BYTES,
            "either bank preserves the final runtime flag reservation");
    }
    Check(TileXREpBuildMemoryServerDedupLayout(64, 256, 65536, 6, 384, &layout) ==
        TILEXR_ERROR_PARA_CHECK_FAIL, "oversized single-round payload is rejected even with 8 GiB total");
    Check(TileXREpBuildMemoryServerDedupLayout(16, 96, 7168, 6, 96, &layout, true) == TILEXR_SUCCESS &&
        layout.data.tokenStride == 8192 && layout.data.scaleBytes == 224 &&
        layout.expandXBytes == layout.capacityRows * 7168 && layout.dynamicScalesBytes == layout.capacityRows * 224,
        "MXFP8 packet and separate output scales");

    alignas(32) uint8_t storage[256] = {};
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 3, 1, 32, &layout) == TILEXR_SUCCESS,
        "one-token group capacity");
    EpServerDedupAllocation group {storage, 32, layout.groupBytes, 32, 1};
    Check(TileXREpValidateServerDedupAllocations(&group, 1, 1) == TILEXR_ERROR_PARA_CHECK_FAIL,
        "old BS-row group allocation is insufficient");
    group.bytes = sizeof(storage);
    Check(TileXREpValidateServerDedupAllocations(&group, 1, 1) == TILEXR_SUCCESS,
        "8*BS-row group allocation fits exactly");
    const EpServerDedupAllocation valid[] = {
        {storage, 64, 64, 32, 1}, {storage + 64, 32, 4, 4, 1}, {storage + 96, 160, 128, 32, 1}
    };
    Check(TileXREpValidateServerDedupAllocations(valid, 3, 1) == TILEXR_SUCCESS, "adjacent disjoint buffers");
    for (int test = 0; test < 9; ++test) {
        EpServerDedupAllocation records[] = {valid[0], valid[1], valid[2]};
        switch (test) {
            case 0: records[0].bytes = 63; break;
            case 1: records[1].data = storage + 32; break;
            case 2: records[2].deviceId = 0; break;
            case 3: records[0].data = storage + 1; break;
            case 4: records[0].data = nullptr; break;
            case 5: records[0].alignment = 0; break;
            case 6: records[0].alignment = 3; break;
            case 7: records[0].requiredBytes = 0; break;
            case 8: records[0].data = reinterpret_cast<void *>(std::numeric_limits<uintptr_t>::max() - 31);
                break;
        }
        Check(TileXREpValidateServerDedupAllocations(records, 3, 1) == TILEXR_ERROR_PARA_CHECK_FAIL,
            "invalid allocation record rejected");
    }
    Check(TileXREpValidateServerDedupAllocations(nullptr, 1, 1) == TILEXR_ERROR_PARA_CHECK_FAIL, "null records");
    Check(TileXREpValidateServerDedupAllocations(valid, 0, 1) == TILEXR_ERROR_PARA_CHECK_FAIL, "empty records");
    Check(TileXREpValidateServerDedupAllocations(valid, 3, -1) == TILEXR_ERROR_PARA_CHECK_FAIL, "invalid device");
    return ServerDedupTest::Finish("layout");
}
