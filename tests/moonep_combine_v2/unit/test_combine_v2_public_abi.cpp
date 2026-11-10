/*
 * 编译校验 MoonEP Combine V2 公开工作区查询及算子签名，消费 tilexr_moonep_combine_v2.h。
 */

#include <cstdint>
#include <type_traits>

#include "tilexr_moonep_combine_v2.h"

using CombineWorkspaceQueryV2 = int (*)(int64_t, int64_t, int64_t,
    int64_t, uint32_t, uint64_t *, uint64_t *, uint64_t *);
using CombineV2 = int (*)(void *, const int32_t *, TileXRCommPtr, int64_t,
    int64_t, int64_t, int64_t, uint32_t, uint64_t *, uint32_t, aclrtStream);
using CombineStageV2Fused = int (*)(void *, uint64_t, const int32_t *,
    TileXRCommPtr, TileXRCommPtr, int64_t, int64_t, int64_t, int64_t,
    uint32_t, const void *, void *, const float *, float *, uint32_t,
    aclrtStream);

static_assert(std::is_same<decltype(&TileXRMoonEpCombineGetWorkspaceSizeV2),
    CombineWorkspaceQueryV2>::value, "Combine V2 workspace ABI changed");
static_assert(std::is_same<decltype(&TileXRMoonEpCombineV2),
    CombineV2>::value, "Combine V2 launch ABI changed");
static_assert(std::is_same<decltype(&TileXRMoonEpCombineStageV2Fused),
    CombineStageV2Fused>::value, "Combine V2 fused Stage ABI changed");

int main()
{
    return 0;
}
