#ifndef TILEXR_EP_SERVER_DEDUP_COMBINE_TEST_FIXTURE_H
#define TILEXR_EP_SERVER_DEDUP_COMBINE_TEST_FIXTURE_H

#include <cstdio>
#include "ep_memory_server_dedup_combine_host.h"
#include "server_dedup_test_checks.h"

namespace CombineTest {
inline TileXREp::EpMemoryServerDedupCombineParams ValidParams()
{
    TileXREp::EpMemoryServerDedupCombineParams params {};
    auto &p = params.combine;
    // Deliberately unmapped addresses: Host must not dereference device tensors.
    p.expertOut = reinterpret_cast<void *>(0x100000);
    p.assistInfoForCombine = reinterpret_cast<int32_t *>(0x200004);
    p.epRecvCounts = reinterpret_cast<int32_t *>(0x300004);
    p.expertScales = reinterpret_cast<float *>(0x400004);
    p.yOut = reinterpret_cast<void *>(0x500002);
    params.tokenType = reinterpret_cast<uint32_t *>(0x600004);
    params.relayReadIndex = reinterpret_cast<void *>(0x700004);
    params.sourceMask = reinterpret_cast<uint32_t *>(0x800004);
    p.comm = reinterpret_cast<TileXRCommPtr>(0x900000);
    p.stream = reinterpret_cast<aclrtStream>(0xA00000);
    p.bs = 4;
    p.h = 13;
    p.topK = 6;
    p.moeExpertNum = 192;
    p.epWorldSize = 64;
    p.epRankId = 9;
    p.tpWorldSize = 1;
    p.globalBs = 256;
    p.dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
    return params;
}
inline void InitComm(TileXR::CommArgs &args)
{
    args = {};
    args.rankSize = 64;
    args.rank = 9;
    // ValidParams: shared prefix + 24 result slots + 768 padded stage rows.
    args.peerMemBytes = 2ULL * (TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
        24ULL * 512U + 768ULL * 512U + TileXR::TILEXR_FLAG_BUFF_BYTES);
    for (auto &peer : args.peerMems) { peer = reinterpret_cast<GM_ADDR>(0x1000000); }
}
inline int Invoke(const TileXREp::EpMemoryServerDedupCombineParams &params)
{
    const auto &p = params.combine;
    return TileXRMoeEpCombineMemoryServerDedup(p.expertOut, p.assistInfoForCombine, p.epRecvCounts,
        p.expertScales, p.xActiveMask, p.activeMaskType, p.sharedExpertX, p.comm, p.bs, p.h, p.topK,
        p.moeExpertNum, p.epWorldSize, p.epRankId, p.tpWorldSize, p.tpRankId, p.expertShardType,
        p.sharedExpertNum, p.sharedExpertRankNum, p.quantMode, p.globalBs, p.yOut, p.dtype, p.stream,
        params.tokenType, params.relayReadIndex, params.sourceMask);
}
inline int Finish(const char *test)
{
    if (ServerDedupTest::Failures() != 0) { return 1; }
    std::printf("EP_SERVER_DEDUP_COMBINE_ABI_HOST_PASS test=%s\n", test);
    return 0;
}
} // namespace CombineTest
#endif
