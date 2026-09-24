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
        {16, 32, 1, 4096, 8}, {64, 384, 6, 98304, 48},
        {128, 384, 6, 98304, 48}, {16, 512, 32, 131072, 256},
        {64, 384, 8, 98304, 64}, {128, 384, 16, 98304, 128},
        {16, 16, 1, 4096, 8}, {16, 1024, 32, 65536, 256},
        {64, 1024, 16, 131072, 128}, {128, 1024, 8, 131072, 64},
        {24, 1008, 32, 98304, 256}
    };
    for (const auto &test : cases) {
        // Bound this fixture's public output capacities for larger expert domains.
        const uint64_t bs = test.experts >= 1008 ? 128U : 256U;
        Check(TileXREpBuildMemoryServerDedupLayout(test.ranks, bs, 7168, test.k, test.experts,
            &layout) == TILEXR_SUCCESS, "supported layout");
        Check(layout.capacityRows == test.rows && layout.relayReadIndexRowStrideBytes == (kEpServerDedupEnable ? test.stride : 0U),
            "capacity and tightly packed RelayReadIndex stride");
        Check(layout.tokenTypeBytes == (kEpServerDedupEnable ? test.rows * 4 : 0U) && layout.destinationIndexBytes == test.rows * 12 &&
            layout.relayReadIndexBytes == (kEpServerDedupEnable ? test.rows * test.stride : 0U),
            "metadata byte sizes");
        Check(layout.sourceMaskBytes == (kEpServerDedupEnable ? bs * 4U : 0U) && layout.xBytes == bs * 7168U * 2U, "input and mask sizes");
        Check(layout.expertIdsBytes == bs * test.k * 4U && layout.weightBytes == layout.expertIdsBytes,
            "route input sizes");
        Check(layout.sendCountsBytes == static_cast<uint64_t>(test.experts) * 4U &&
            layout.expertTokenNumsBytes == static_cast<uint64_t>(test.experts / test.ranks) * 8U,
            "count output sizes");
    }
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 3, 1, 32, &layout) == TILEXR_SUCCESS &&
        layout.xBytes == 6 && layout.expandXBytes == 96 && layout.relayReadIndexBytes == (kEpServerDedupEnable ? 128 : 0), "tight H tail and K=1");
    struct WireCase {
        uint32_t h, k;
        bool quantized;
        uint64_t header, index, stride;
    };
    // Explicit wire positions cover a fitting segment, exact payload boundary,
    // and a segment that must skip the 32B flag and start in the next block.
    const WireCase wireCases[] = {
        {184, 6, false, 384, 416, 512},
        {200, 4, false, 416, 448, 512},
        {200, 6, false, 416, 512, 1024},
        {200, 1, false, 416, 448, 512},
        {200, 32, false, 416, 512, 1024},
        {1, 1, true, 288, 320, 512},
        {1, 6, true, 288, 320, 512},
        {1, 20, true, 288, 320, 512},
        {1, 21, true, 288, 512, 1024},
        {1, 32, true, 288, 512, 1024}
    };
    for (const auto &test : wireCases) {
        Check(TileXREpBuildMemoryServerDedupLayout(16, 1, test.h, test.k, 32,
            &layout, test.quantized) == TILEXR_SUCCESS, "wire boundary layout supported");
        if (!kEpServerDedupEnable) {
            Check(layout.data.headerWireOffset == test.header && layout.data.payloadBytes == layout.data.headerPayloadOffset + 32U &&
                layout.data.tokenStride == 512U, "compact three-field tail drops the relay segment");
        } else {
        Check(layout.data.headerWireOffset == test.header &&
            layout.data.relayReadIndexWireOffset == test.index && layout.data.tokenStride == (kEpServerDedupEnable ? test.stride : 0U),
            "Host caches exact aligned header/index offsets and complete token stride");
        Check(layout.data.headerWireOffset % 32U == 0 && layout.data.relayReadIndexWireOffset % 32U == 0 &&
            layout.data.relayReadIndexWireOffset % 512U + test.k * 8U <= 480U,
            "both segment starts align to 32B and all index bytes precede the flag");
        }
    }
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
        layout.relayReadIndexBytes = 123;
        Check(TileXREpBuildMemoryServerDedupLayout(test[0], test[1], test[2], test[3], test[4],
            &layout) == TILEXR_ERROR_PARA_CHECK_FAIL, "invalid or overflowing layout rejected");
        Check(layout.relayReadIndexBytes == 0 && layout.capacityRows == 0, "failure clears stale layout");
    }
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 1, 1, 32, nullptr) == TILEXR_ERROR_PARA_CHECK_FAIL,
        "null layout rejected");

    Check(TileXREpBuildMemoryServerDedupLayout(128, 128, 7168, 8, 1024, &layout, false) == TILEXR_SUCCESS,
        "compile-time core split builds a valid layout");
    const auto split = TileXREpServerDedupCountLayout(1024);
    Check(split.cntOffset + split.prefixOffset == 610816U &&
        split.cumsumFlagOffset == split.prefixOffset + 393216U &&
        layout.countStateBytes == 1079296U && split.stateBytes <= TILEXR_EP_SERVER_DEDUP_DISPATCH_CONTROL_BYTES,
        "fixed prefix region reserves twice the maximum 1024-expert/48-core footprint");
    for (uint32_t experts : {16U, 96U, 384U, 1008U, 1024U}) {
        const auto actual = TileXREpServerDedupCountLayout(experts);
        Check(actual.cntOffset == split.cntOffset && actual.prefixOffset == split.prefixOffset &&
            actual.cumsumOffset == split.cumsumOffset && actual.cumsumFlagOffset == split.cumsumFlagOffset &&
            actual.counterOffset == split.counterOffset && actual.stateBytes == split.stateBytes,
            "expert count never moves a control table");
        Check(actual.prefixStride == experts * 4U,
            "fixed whole-table reservation retains compact actual-E copies");
    }
    struct Region { uint64_t offset, bytes; };
    const Region regions[] = {
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_BYTES},
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_CUMSUM_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_CUMSUM_BYTES},
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_PREFIX_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_PREFIX_BYTES},
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_READY_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_READY_BYTES},
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNTER_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNTER_BYTES},
        {TILEXR_EP_SERVER_DEDUP_DISPATCH_RESERVED_OFFSET, TILEXR_EP_SERVER_DEDUP_DISPATCH_RESERVED_BYTES},
        {TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET, TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES}
    };
    uint64_t end = 0;
    for (const auto &region : regions) {
        Check(region.offset % 512U == 0 && region.bytes % 512U == 0 && region.offset >= end,
            "fixed tables are aligned and disjoint including unused capacity");
        end = region.offset + region.bytes;
    }
    Check(end == 1372160U && TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET == 1572864U &&
        end <= TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET,
        "shared data follows both fixed control regions");

    // cnt uses bounded UB even though this increment does not allocate hidden buffers.
    Check(TileXREpBuildMemoryServerDedupLayout(128, 4096, 1, 32, 1024, &layout) ==
        TILEXR_ERROR_PARA_CHECK_FAIL, "route scratch exceeding UB rejected");
    const auto countLayout = TileXREpServerDedupCountLayout(1024);
    const uint64_t matrixBytes = uint64_t(kEpServerDedupRecvCoreNum) * kEpServerDedupRecvCoreNum * 32U;
    Check(countLayout.cntOffset == 12800 && countLayout.cumsumOffset == 524288 &&
        kEpServerDedupCountRecordBytes == 32U && kEpServerDedupCountGmStride == 512U &&
        countLayout.prefixStride == 4096,
        "Count has 512B GM slots, compact 32B UB records and a full prefix per receiver");
    Check(TileXREpServerDedupCountSendUbBytes(6U, 384U, 64U) == 1312U,
        "sparse GM slots do not expand the compact count UB reservation");
    Check(countLayout.prefixOffset >= countLayout.cumsumOffset + matrixBytes &&
        countLayout.cumsumFlagOffset >= countLayout.prefixOffset + kEpServerDedupRecvCoreNum * 4096U &&
        countLayout.counterOffset >= countLayout.cntOffset + countLayout.cumsumFlagOffset + matrixBytes &&
        countLayout.counterOffset % 1024U == 0 && countLayout.stateBytes == countLayout.counterOffset + 1024U,
        "both receiver matrices, every prefix and the isolated counter have disjoint storage");
    Check(TileXREpBuildMemoryServerDedupLayout(128, 256, 7168, 6, 384, &layout) == TILEXR_SUCCESS &&
        layout.data.dispatchOffset == 1572864U && layout.data.dispatchBytes == (kEpServerDedupEnable ? 1560281088U : 1509949440U) &&
        layout.data.requiredBytes == (kEpServerDedupEnable ? 1561853952U : 1511522304U),
        "Dispatch requires only fixed controls and its own receive view");
    for (uint64_t peerBytes : {6ULL << 30, 8ULL << 30, 10ULL << 30}) {
        for (int64_t magic = 0; magic <= 5; ++magic) {
            const uint64_t start = TileXREpServerDedupWindowOffset(magic, peerBytes);
            Check(start == (magic % 2 ? peerBytes / 2U : 0), "magic alternates actual capacity halves");
            Check(start == TileXREpServerDedupWindowOffset(magic + 2, peerBytes), "two calls later reuses the same bank");
            Check(start + layout.data.requiredBytes <= peerBytes - TILEXR_FLAG_BUFF_BYTES,
                "either bank preserves the final runtime flag reservation");
        }
    }
    Check(TileXREpBuildMemoryServerDedupLayout(64, 256, 65536, 6, 384, &layout) ==
        TILEXR_SUCCESS && layout.data.requiredBytes > (4ULL << 30),
        "shape builder reports demand without imposing a fixed communication capacity");
    Check(TileXREpBuildMemoryServerDedupLayout(16, 96, 7168, 6, 96, &layout, true) == TILEXR_SUCCESS &&
        layout.data.tokenStride == 8192 && layout.data.scaleBytes == 224 &&
        layout.expandXBytes == layout.capacityRows * 7168 && layout.dynamicScalesBytes == layout.capacityRows * 224,
        "MXFP8 packet and separate output scales");

    alignas(32) uint8_t storage[512] = {};
    Check(TileXREpBuildMemoryServerDedupLayout(16, 1, 3, 1, 32, &layout) == TILEXR_SUCCESS,
        "one-token RelayReadIndex capacity");
    if (kEpServerDedupEnable) {
    EpServerDedupAllocation relayReadIndex {storage + 4, 64, layout.relayReadIndexBytes, 4, 1};
    Check(TileXREpValidateServerDedupAllocations(&relayReadIndex, 1, 1) == TILEXR_ERROR_PARA_CHECK_FAIL,
        "insufficient RelayReadIndex allocation rejected");
    relayReadIndex.bytes = layout.relayReadIndexBytes;
    Check(TileXREpValidateServerDedupAllocations(&relayReadIndex, 1, 1) == TILEXR_SUCCESS,
        "4B-aligned capacityRows allocation fits exactly");
    }
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
