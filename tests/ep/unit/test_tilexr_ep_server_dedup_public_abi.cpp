#include <type_traits>

#include "tilexr_ep.h"
#include "ep_memory_server_dedup_launch.h"
#include "server_dedup_test_checks.h"

using DispatchMemory = int (*)(void *, int32_t *, void *, bool *, int64_t, void *, TileXRCommPtr,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, void *, void *, int32_t *, int64_t *, int32_t *, int32_t *,
    void *, TileXR::TileXRDataType, TileXR::TileXRDataType, aclrtStream);
using DispatchServerDedup = int (*)(void *, int32_t *, void *, bool *, int64_t, void *, TileXRCommPtr,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, void *, void *, int32_t *, int64_t *, int32_t *, int32_t *,
    void *, uint32_t *, uint32_t *, uint32_t *, float *, TileXR::TileXRDataType, TileXR::TileXRDataType, aclrtStream);
static_assert(std::is_same<decltype(&TileXRMoeEpDispatchMemory), DispatchMemory>::value, "Old Memory ABI unchanged");
static_assert(std::is_same<decltype(&TileXRMoeEpDispatchMemoryServerDedup), DispatchServerDedup>::value,
    "Independent ABI has three metadata outputs and separate source weights");

namespace {
TileXR::CommArgs g_args {};
bool g_a5 = true;
int g_magicCalls = 0;
}
namespace TileXREp {
bool TileXREpIsA5Soc() { return g_a5; }
// ABI consumer 使用测试替身；生产提交行为由 kernel_launch 和安装包测试覆盖。
int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &,
    const EpHostLaunchContext &, const EpMemoryServerDedupLayout &)
{
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}
}
int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&args)
{
    args = &g_args;
    return TileXR::TILEXR_SUCCESS;
}
int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &args)
{
    args = reinterpret_cast<GM_ADDR>(0x1000);
    return TileXR::TILEXR_SUCCESS;
}
int TileXRCommNextMagic(TileXRCommPtr, int64_t *)
{
    ++g_magicCalls;
    return TileXR::TILEXR_SUCCESS;
}

int main()
{
    using ServerDedupTest::Check;
    g_args.rankSize = 16;
    g_args.peerMemBytes = TileXREp::kEpServerDedupPeerBytes;
    for (auto &peer : g_args.peerMems) { peer = reinterpret_cast<GM_ADDR>(0x1000); }
    alignas(32) uint16_t x[1] = {123};
    int32_t expertId[1] = {0};
    float expertScale[1] = {1.0f};
    float sourceWeight[1] = {-1.0f};
    alignas(32) uint16_t output[16] = {456};
    alignas(32) uint32_t assist[16 * 8] = {789};
    uint32_t mask[1] = {321};
    alignas(32) uint32_t group[8] = {654};
    int64_t expertCounts[2] = {987};
    int32_t counts[32] = {741};
    DispatchServerDedup entry = &TileXRMoeEpDispatchMemoryServerDedup;
    for (bool a5 : {true, false}) {
        g_a5 = a5;
        Check(entry(x, expertId, nullptr, nullptr, TileXREp::TILEXR_EP_ACTIVE_MASK_NONE, expertScale,
            reinterpret_cast<TileXRCommPtr>(0x1000), 1, 1, 1, 32, 16, 0, 1, 0, 0, 0, 0, 0, 16, 0,
            output, nullptr, nullptr, expertCounts, counts, nullptr, nullptr, assist, mask, group, sourceWeight,
            TileXR::TILEXR_DATA_TYPE_FP16, TileXR::TILEXR_DATA_TYPE_RESERVED,
            reinterpret_cast<aclrtStream>(0x2000)) == TileXR::TILEXR_ERROR_NOT_SUPPORT,
            "ABI consumer propagates submit result and rejects non-A5");
    }
    Check(output[0] == 456 && assist[0] == 789 && mask[0] == 321 && group[0] == 654 &&
        expertCounts[0] == 987 && counts[0] == 741 && x[0] == 123, "ABI consumer preserves outputs");
    Check(g_magicCalls == 0, "ABI consumer does not consume magic outside submit");
    return ServerDedupTest::Finish("public_abi");
}
