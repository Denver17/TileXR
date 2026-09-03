#include <stdint.h>

#include "tilexr_moonep.h"

int main(void)
{
    TileXRMoonEpTensorV1 tensor = {0};
    TileXRMoonEpPlanV1 plan = {0};
    TileXRMoonEpDispatchArgsV1 dispatch = {0};
    TileXRMoonEpDispatchArgsV2 dispatchV2 = {0};
    TileXRMoonEpPrefetchWeightArgsV1 prefetch = {0};
    TileXRMoonEpReduceGradArgsV1 reduce = {0};
    TileXRMoonEpReduceGradWorkspaceQueryV2 query = {0};
    TileXRMoonEpReduceGradWorkspaceInfoV2 info = {0};
    TileXRMoonEpReduceGradSourceSliceV2 source = {0};
    TileXRMoonEpReduceGradPrepareArgsV2 prepare = {0};
    TileXRMoonEpReduceGradPreparedV2 prepared = 0;
    TileXRMoonEpReduceGradArgsV2 reduceV2 = {0};

    tensor.structSize = (uint32_t)sizeof(tensor);
    tensor.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    tensor.dtype = TILEXR_MOONEP_DTYPE_FLOAT16;
    plan.structSize = (uint32_t)sizeof(plan);
    plan.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    plan.n = 1;
    plan.nvS = 1;
    dispatch.structSize = (uint32_t)sizeof(dispatch);
    dispatch.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    dispatch.hiddenSh = &tensor;
    dispatchV2.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    dispatchV2.hiddenSh = &tensor;
    prefetch.gate = &tensor;
    reduce.input = &tensor;
    reduce.output = &tensor;
    query.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    info.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    source.data = &tensor;
    prepare.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    prepare.sources[0] = source;
    reduceV2.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    reduceV2.prepared = prepared;

    return tensor.dtype == TILEXR_MOONEP_DTYPE_FLOAT16 &&
        plan.nvS == 1 && dispatch.hiddenSh == &tensor &&
        dispatchV2.hiddenSh == &tensor &&
        prefetch.gate == &tensor &&
        reduce.input == &tensor && reduce.output == &tensor &&
        query.abiVersion == TILEXR_MOONEP_ABI_VERSION_V2 &&
        info.abiVersion == TILEXR_MOONEP_ABI_VERSION_V2 &&
        prepare.sources[0].data == &tensor &&
        reduceV2.abiVersion == TILEXR_MOONEP_ABI_VERSION_V2 ? 0 : 1;
}
