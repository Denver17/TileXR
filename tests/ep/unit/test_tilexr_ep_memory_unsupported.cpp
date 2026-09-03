#include <iostream>

#include "tilexr_ep.h"
#include "tilexr_types.h"

int main()
{
    const int dispatchRet = TileXRMoeEpDispatchMemory(
        nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        TileXR::TILEXR_DATA_TYPE_FP16, TileXR::TILEXR_DATA_TYPE_FP16, nullptr);
    const int combineRet = TileXRMoeEpCombineMemory(
        nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        nullptr, TileXR::TILEXR_DATA_TYPE_FP16, nullptr);

    if (dispatchRet != TileXR::TILEXR_ERROR_NOT_SUPPORT ||
        combineRet != TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        std::cerr << "non-A5 EP Memory APIs must return TILEXR_ERROR_NOT_SUPPORT"
                  << std::endl;
        return 1;
    }
    return 0;
}
