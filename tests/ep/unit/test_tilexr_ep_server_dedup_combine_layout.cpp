#include <limits>
#include "server_dedup_combine_test_fixture.h"

using namespace TileXREp;
using namespace TileXR;
using ServerDedupTest::Check;
int main()
{
    EpMemoryServerDedupCombineLayout layout {};
    Check(TileXREpBuildMemoryServerDedupCombineLayout(64, 4, 13, 6, 192, &layout) == TILEXR_SUCCESS,
        "supported metadata layout");
    Check(layout.globalBs == 256 && layout.capacityRows == 768 && layout.expertOutBytes == 19968 &&
        layout.assistInfoBytes == 9216 && layout.sendCountsBytes == 768 && layout.expertScalesBytes == 96 &&
        layout.tokenTypeBytes == 3072 && layout.relayReadIndexRowStrideBytes == 48 &&
        layout.relayReadIndexBytes == 36864 && layout.sourceMaskBytes == 16 && layout.yOutBytes == 104,
        "independent expected five-table and tensor sizes");
    for (int64_t ranks : {32, 64, 128}) {
        for (int64_t k : {1, 6, 32}) {
            Check(TileXREpBuildMemoryServerDedupCombineLayout(ranks, 96, 7168, k, ranks, &layout) == TILEXR_SUCCESS,
                "L=1 remains supported with full K relay stride");
            Check(layout.capacityRows == uint64_t(ranks * 96) &&
                layout.relayReadIndexBytes == uint64_t(ranks * 96 * k * 8), "capacity uses min(K,L)");
        }
    }
    Check(TileXREpBuildMemoryServerDedupCombineLayout(16, 4096, 1, 32, 1024, &layout) == TILEXR_SUCCESS,
        "Combine does not inherit Dispatch count UB restriction");
    for (int i = 0; i < 12; ++i) {
        int64_t r = 64, b = 4, h = 13, k = 6, e = 192;
        switch (i) {
            case 0: r = 8; break;
            case 1: r = 136; break;
            case 2: r = 17; break;
            case 3: b = 0; break;
            case 4: h = 0; break;
            case 5: h = int64_t(UINT32_MAX) + 1; break;
            case 6: k = 33; break;
            case 7: e = 193; break;
            case 8: e = 1088; break;
            case 9: b = int64_t(UINT32_MAX); break;
            case 10: k = -1; break;
            case 11: r = 128; b = UINT32_MAX; h = UINT32_MAX; k = 1; e = 128; break;
        }
        Check(TileXREpBuildMemoryServerDedupCombineLayout(r, b, h, k, e, &layout) == TILEXR_ERROR_PARA_CHECK_FAIL,
            "shape, narrowing and multiplication overflow rejected");
        Check(layout.capacityRows == 0 && layout.expertOutBytes == 0, "no partial layout on failure");
    }
    Check(TileXREpBuildMemoryServerDedupCombineLayout(64, 4, 13, 6, 192, nullptr) == TILEXR_ERROR_PARA_CHECK_FAIL,
        "null layout rejected");
    Check(TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET == 782336 &&
        TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES == 53248 &&
        TILEXR_EP_SERVER_DEDUP_COMBINE_PREPARE_DONE_BYTES == 2048 &&
        TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET == 2048 &&
        TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_BYTES == 24576 &&
        TILEXR_EP_SERVER_DEDUP_COMBINE_RESERVED_OFFSET + TILEXR_EP_SERVER_DEDUP_COMBINE_RESERVED_BYTES == 53248 &&
        TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET == 1048576, "fixed shared control partition");
    EpServerDedupCombineDataLayout data {};
    Check(TileXREpBuildServerDedupCombineDataLayout(16, 96, 7168, 6, 96, 15360, &data) == TILEXR_SUCCESS,
        "explicit result-slot geometry without choosing a wire dtype");
    Check(data.resultOffset == 1048576 && data.resultBytes == 8847360 && data.stageOffset == 9895936 &&
        data.stageRowBytes == 14336 && data.stageRows == 9216 && data.stageBytes == 132120576 &&
        data.requiredWindowBytes == 142016512, "result and E*BS stage expected offsets");
    Check(TileXREpBuildServerDedupCombineDataLayout(64, 4, 13, 6, 192, 1024, &data) == TILEXR_SUCCESS &&
        data.stageRowBytes == 512 && data.stageOffset == 1048576 + 24576, "other wire stride and padded stage");
    for (uint64_t slot : {uint64_t(0), uint64_t(513), UINT64_MAX - 511U}) {
        Check(TileXREpBuildServerDedupCombineDataLayout(64, 4, 13, 6, 192, slot, &data) == TILEXR_ERROR_PARA_CHECK_FAIL,
            "invalid or overflowing result stride rejected");
        Check(data.requiredWindowBytes == 0, "no partial data layout on failure");
    }
    return CombineTest::Finish("layout");
}
