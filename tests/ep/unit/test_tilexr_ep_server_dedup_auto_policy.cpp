#include "ep_memory_server_dedup_layout.h"
#include "ep_memory_server_dedup_combine_profile.h"
#include "server_dedup_combine_test_fixture.h"

int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    static_assert(kEpServerDedupEnable && kEpServerDedupAutoEnable, "auto uses dedup layout");
    for (uint32_t bs : {1U, 31U, 32U, 33U, 256U}) {
        Check(EpServerDedupEnabled(bs) == (bs > 32U), "original BS selects execution at boundary");
        EpMemoryServerDedupLayout dispatch {};
        EpMemoryServerDedupCombineLayout combine {};
        Check(TileXREpBuildMemoryServerDedupLayout(16, bs, 224, 6, 96, &dispatch) == TileXR::TILEXR_SUCCESS,
            "auto Dispatch layout is valid");
        Check(TileXREpBuildMemoryServerDedupCombineLayout(16, bs, 224, 6, 96, &combine) == TileXR::TILEXR_SUCCESS,
            "auto Combine layout is valid");
        // Independent dedup geometry, including unused small-BS metadata/stage.
        Check(dispatch.data.tokenStride == 1024U && dispatch.sourceMaskBytes == bs * 4U &&
            dispatch.relayReadIndexBytes == 16ULL * bs * 6U * 48U,
            "direct mode preserves complete dedup wire and metadata allocation");
        Check(combine.data.stageRows == 96ULL * bs && combine.data.stageBytes == 96ULL * bs * 512U &&
            combine.sourceMaskBytes == bs * 4U && combine.relayReadIndexBytes == dispatch.relayReadIndexBytes,
            "Combine stage and auxiliary tables preserve dedup geometry");
        Check(EpCombineProfilingRecordsPerLaunch(bs, 6U) >=
            EpCombineM5TokenCount(bs) * (8U + 4U * 6U) + 128U,
            "profiling reservation covers the direct per-loop records");
    }
    return CombineTest::Finish("auto-policy");
}
