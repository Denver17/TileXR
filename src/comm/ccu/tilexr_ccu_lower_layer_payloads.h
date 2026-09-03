/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_LOWER_LAYER_PAYLOADS_H
#define TILEXR_CCU_LOWER_LAYER_PAYLOADS_H

#include "ccu/tilexr_ccu_abi_constants.h"
#include "ccu/tilexr_ccu_driver_adapter.h"

#include <array>
#include <cstdint>
#include <string>

namespace TileXR {

struct TileXRCcuPfeCtxSpec {
    uint16_t startJettyId = 0;
    uint16_t jettyCount = 0;
    uint16_t startLocalJettyCtxId = 0;
};

struct TileXRCcuLocalJettyCtxSpec {
    uint8_t dieId = 0;
    uint32_t pfeId = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    uint16_t wqeBasicBlockStartId = 0;
};

struct TileXRCcuChannelCtxV1Spec {
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> remoteEid {};
    uint32_t tpn = 0;
    uint32_t sourcePfeId = 0;
    uint16_t startJettyId = 0;
    uint16_t jettyCount = 0;
    uint8_t dieId = 0;
    uint32_t memoryTokenId = 0;
    uint32_t memoryTokenValue = 0;
    uint64_t remoteCcuVa = 0;
};

struct TileXRCcuLowerLayerPayloadReport {
    std::string message;
};

int TileXRCcuBuildPfeCtx(
    const TileXRCcuPfeCtxSpec& spec,
    TileXRCcuPfeCtx* ctx,
    TileXRCcuLowerLayerPayloadReport* report);

int TileXRCcuBuildLocalJettyCtx(
    const TileXRCcuLocalJettyCtxSpec& spec,
    TileXRCcuLocalJettyCtxData* ctx,
    TileXRCcuLowerLayerPayloadReport* report);

int TileXRCcuBuildChannelCtxV1(
    const TileXRCcuChannelCtxV1Spec& spec,
    TileXRCcuChannelCtxDataV1* ctx,
    TileXRCcuLowerLayerPayloadReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_LOWER_LAYER_PAYLOADS_H
