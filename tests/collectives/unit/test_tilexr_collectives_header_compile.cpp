#include "tilexr_collectives.h"

namespace {

using AllGatherFn = int (*)(void *, void *, int64_t, TileXR::TileXRDataType, TileXRCommPtr, aclrtStream);
using AllToAllFn = int (*)(void *, void *, int64_t, TileXR::TileXRDataType, TileXRCommPtr, aclrtStream);
using AllReduceFn = int (*)(void *, void *, int64_t, TileXR::TileXRDataType, TileXR::TileXRReduceOp,
                            TileXRCommPtr, aclrtStream);
using ReduceScatterFn = int (*)(void *, void *, int64_t, TileXR::TileXRDataType, TileXR::TileXRReduceOp,
                                TileXRCommPtr, aclrtStream);
using BroadcastFn = int (*)(void *, int64_t, TileXR::TileXRDataType, int, TileXRCommPtr, aclrtStream);

} // namespace

int main()
{
    AllGatherFn allGather = &TileXRAllGather;
    AllToAllFn allToAll = &TileXRAllToAll;
    AllReduceFn allReduce = &TileXRAllReduce;
    ReduceScatterFn reduceScatter = &TileXRReduceScatter;
    BroadcastFn broadcast = &TileXRBroadcast;
    const bool reduceOpsPresent =
        TileXR::TILEXR_REDUCE_SUM != TileXR::TILEXR_REDUCE_RESERVED &&
        TileXR::TILEXR_REDUCE_MAX != TileXR::TILEXR_REDUCE_RESERVED &&
        TileXR::TILEXR_REDUCE_MIN != TileXR::TILEXR_REDUCE_RESERVED &&
        TileXR::TILEXR_REDUCE_PROD != TileXR::TILEXR_REDUCE_RESERVED;

    return (allGather != nullptr && allToAll != nullptr && allReduce != nullptr &&
            reduceScatter != nullptr && broadcast != nullptr && reduceOpsPresent) ? 0 : 1;
}
