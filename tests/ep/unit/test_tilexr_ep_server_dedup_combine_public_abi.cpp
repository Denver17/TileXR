#include <type_traits>
#include "server_dedup_combine_test_fixture.h"
using CombineMemory = int (*)(void *, int32_t *, int32_t *, float *, bool *, int64_t, void *, TileXRCommPtr,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, void *, TileXR::TileXRDataType, aclrtStream);
using CombineDedup = int (*)(void *, int32_t *, int32_t *, float *, bool *, int64_t, void *, TileXRCommPtr,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, void *, TileXR::TileXRDataType, aclrtStream, const uint32_t *, const void *, const uint32_t *);
static_assert(std::is_same<decltype(&TileXRMoeEpCombineMemory), CombineMemory>::value, "Memory ABI unchanged");
static_assert(std::is_same<decltype(&TileXRMoeEpCombineMemoryServerDedup), CombineDedup>::value, "27 parameter ABI");
namespace {
TileXR::CommArgs g_args {};
int g_magicCalls = 0;
}
namespace TileXREp {
bool TileXREpIsA5Soc() { return true; }
// Public ABI is tested independently of runtime; C2 launch tests link the real launcher.
int TileXREpLaunchCombineMemoryServerDedupKernel(const EpMemoryServerDedupCombineParams &,
    const EpHostLaunchContext &, const EpMemoryServerDedupCombineLayout &)
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
    CombineTest::InitComm(g_args);
    auto params = CombineTest::ValidParams();
    uint16_t output[52];
    for (auto &x : output) { x = 0x55AA; }
    params.combine.yOut = output;
    ServerDedupTest::Check(CombineTest::Invoke(params) == TileXR::TILEXR_ERROR_NOT_SUPPORT,
        "ABI propagates the test launcher result");
    for (auto x : output) { ServerDedupTest::Check(x == 0x55AA, "C1 preserves yOut"); }
    ServerDedupTest::Check(g_magicCalls == 0, "public Host entry does not consume magic outside the launcher");
    return CombineTest::Finish("public_abi");
}
