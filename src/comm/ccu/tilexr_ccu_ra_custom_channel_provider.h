/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_RA_CUSTOM_CHANNEL_PROVIDER_H
#define TILEXR_CCU_RA_CUSTOM_CHANNEL_PROVIDER_H

#include "ccu/tilexr_ccu_driver_adapter.h"
#include "ccu/tilexr_ccu_hccp_types.h"

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <functional>
#include <string>

namespace TileXR {

struct TileXRCcuRaCustomChannelProviderReport {
    uint32_t devicePhyId = 0;
    bool initialized = false;
    std::string message;
};

class TileXRCcuRaCustomChannelProvider {
private:
    using TileXRCcuRaCustomChannelInvoker = std::function<int(
        TileXRCcuRaInfo,
        const TileXRCcuCustomChannelIn&,
        TileXRCcuCustomChannelOut*)>;

public:
    int Init(
        uint32_t devicePhyId,
        TileXRCcuRaCustomChannelFunc raCustomChannel,
        TileXRCcuRaCustomChannelProviderReport* report);

    template <typename RaInfoT>
    int Init(
        uint32_t devicePhyId,
        int (*raCustomChannel)(RaInfoT, void*, void*),
        TileXRCcuRaCustomChannelProviderReport* report)
    {
        if (raCustomChannel == nullptr) {
            return InitCallable(devicePhyId, TileXRCcuRaCustomChannelInvoker {}, report);
        }
        TileXRCcuRaCustomChannelInvoker invoker =
            [raCustomChannel](
                TileXRCcuRaInfo info,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out) -> int {
                RaInfoT compatInfo {};
                const size_t copyBytes = sizeof(compatInfo) < sizeof(info) ? sizeof(compatInfo) : sizeof(info);
                std::memcpy(&compatInfo, &info, copyBytes);
                return raCustomChannel(compatInfo, const_cast<TileXRCcuCustomChannelIn*>(&in), out);
            };
        return InitCallable(devicePhyId, invoker, report);
    }

    int CreateAdapter(TileXRCcuDriverAdapter* adapter, TileXRCcuDriverAdapterReport* report);

private:
    int InitCallable(
        uint32_t devicePhyId,
        TileXRCcuRaCustomChannelInvoker raCustomChannel,
        TileXRCcuRaCustomChannelProviderReport* report);

    static int AdapterCallback(
        uint32_t devicePhyId,
        const TileXRCcuCustomChannelIn& in,
        TileXRCcuCustomChannelOut* out,
        void* userData);

    uint32_t devicePhyId_ = 0;
    TileXRCcuRaCustomChannelInvoker raCustomChannel_;
    bool initialized_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_RA_CUSTOM_CHANNEL_PROVIDER_H
