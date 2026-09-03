/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_ra_custom_channel_provider.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace TileXR {
namespace {

constexpr int TILEXR_CCU_ROCE_EAGAIN = 128101;
constexpr uint32_t TILEXR_CCU_RA_EAGAIN_RETRY_COUNT = 100;
constexpr uint32_t TILEXR_CCU_RA_EAGAIN_RETRY_INTERVAL_MS = 100;

void ResetReport(TileXRCcuRaCustomChannelProviderReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuRaCustomChannelProviderReport{};
    }
}

int Fail(TileXRCcuRaCustomChannelProviderReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void FillReport(
    uint32_t devicePhyId,
    bool initialized,
    const std::string& message,
    TileXRCcuRaCustomChannelProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->devicePhyId = devicePhyId;
    report->initialized = initialized;
    report->message = message;
}

} // namespace

int TileXRCcuRaCustomChannelProvider::Init(
    uint32_t devicePhyId,
    TileXRCcuRaCustomChannelFunc raCustomChannel,
    TileXRCcuRaCustomChannelProviderReport* report)
{
    TileXRCcuRaCustomChannelInvoker invoker;
    if (raCustomChannel != nullptr) {
        invoker =
            [raCustomChannel](
                TileXRCcuRaInfo info,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out) -> int {
                return raCustomChannel(
                    info,
                    const_cast<TileXRCcuCustomChannelIn*>(&in),
                    out);
            };
    }
    return InitCallable(devicePhyId, invoker, report);
}

int TileXRCcuRaCustomChannelProvider::InitCallable(
    uint32_t devicePhyId,
    TileXRCcuRaCustomChannelInvoker raCustomChannel,
    TileXRCcuRaCustomChannelProviderReport* report)
{
    ResetReport(report);
    if (!raCustomChannel) {
        initialized_ = false;
        return Fail(report, "missing RA custom channel function");
    }
    devicePhyId_ = devicePhyId;
    raCustomChannel_ = raCustomChannel;
    initialized_ = true;
    FillReport(devicePhyId_, initialized_, "ok", report);
    return TILEXR_SUCCESS;
}

int TileXRCcuRaCustomChannelProvider::CreateAdapter(
    TileXRCcuDriverAdapter* adapter,
    TileXRCcuDriverAdapterReport* report)
{
    if (adapter == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!initialized_ || !raCustomChannel_) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    return adapter->Init(devicePhyId_, &TileXRCcuRaCustomChannelProvider::AdapterCallback, this, report);
}

int TileXRCcuRaCustomChannelProvider::AdapterCallback(
    uint32_t devicePhyId,
    const TileXRCcuCustomChannelIn& in,
    TileXRCcuCustomChannelOut* out,
    void* userData)
{
    auto* provider = static_cast<TileXRCcuRaCustomChannelProvider*>(userData);
    if (provider == nullptr || !provider->raCustomChannel_ || out == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    TileXRCcuRaInfo info {};
    info.mode = TILEXR_CCU_NETWORK_OFFLINE;
    info.phyId = devicePhyId;
    for (uint32_t retry = 0; retry <= TILEXR_CCU_RA_EAGAIN_RETRY_COUNT; ++retry) {
        std::memset(out, 0, sizeof(*out));
        const int ret = provider->raCustomChannel_(info, in, out);
        if (ret != TILEXR_CCU_ROCE_EAGAIN || retry == TILEXR_CCU_RA_EAGAIN_RETRY_COUNT) {
            return ret;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(TILEXR_CCU_RA_EAGAIN_RETRY_INTERVAL_MS));
    }
    return TILEXR_CCU_ROCE_EAGAIN;
}

} // namespace TileXR
