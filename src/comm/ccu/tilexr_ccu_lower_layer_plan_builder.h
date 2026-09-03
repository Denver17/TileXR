/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_LOWER_LAYER_PLAN_BUILDER_H
#define TILEXR_CCU_LOWER_LAYER_PLAN_BUILDER_H

#include "ccu/tilexr_ccu_install_provider.h"
#include "ccu/tilexr_ccu_lower_layer_payloads.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

constexpr uint32_t TILEXR_CCU_REMOTE_PEER_RANK_UNKNOWN = 0xffffffffU;

struct TileXRCcuLowerLayerMsidTokenSpec {
    uint8_t dieId = 0;
    uint32_t msId = 0;
    uint32_t tokenId = 0;
    uint32_t tokenValue = 0;
    bool valid = false;
};

struct TileXRCcuLowerLayerPfeSpec {
    uint8_t dieId = 0;
    uint32_t pfeOffset = 0;
    uint16_t startJettyId = 0;
    uint16_t jettyCount = 0;
    uint16_t startLocalJettyCtxId = 0;
};

struct TileXRCcuLowerLayerJettySpec {
    uint8_t dieId = 0;
    uint32_t pfeId = 0;
    uint16_t startJettyCtxId = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    uint16_t wqeBasicBlockStartId = 0;
};

struct TileXRCcuLowerLayerChannelSpec {
    uint8_t dieId = 0;
    uint32_t channelId = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> remoteEid {};
    uint32_t tpn = 0;
    uint32_t sourcePfeId = 0;
    uint16_t startJettyId = 0;
    uint16_t jettyCount = 0;
    uint32_t memoryTokenId = 0;
    uint32_t memoryTokenValue = 0;
    uint64_t remoteCcuVa = 0;
};

struct TileXRCcuLowerLayerCkeClearSpec {
    uint8_t dieId = 0;
    uint32_t startCkeId = 0;
    uint32_t count = 0;
    bool valid = false;
};

struct TileXRCcuLowerLayerXnClearSpec {
    uint8_t dieId = 0;
    uint32_t startXnId = 0;
    uint32_t count = 0;
    bool valid = false;
};

struct TileXRCcuLowerLayerPlanSpec {
    TileXRCcuLowerLayerMsidTokenSpec msidToken;
    TileXRCcuLowerLayerPfeSpec pfe;
    std::vector<TileXRCcuLowerLayerJettySpec> jettys;
    std::vector<TileXRCcuLowerLayerChannelSpec> channels;
    TileXRCcuLowerLayerXnClearSpec xnClear;
    TileXRCcuLowerLayerCkeClearSpec ckeClear;
    std::vector<TileXRCcuRemoteXnBindingProof> remoteXnBindings;
};

struct TileXRCcuLowerLayerPlanBuilderReport {
    uint32_t msidTokenCount = 0;
    uint32_t pfeCount = 0;
    uint32_t jettyCount = 0;
    uint32_t localJettyCtxCount = 0;
    uint32_t channelCount = 0;
    uint32_t ckeClearCount = 0;
    std::string message;
};

struct TileXRCcuRemoteCcuBufferInfo {
    uint64_t remoteCcuVa = 0;
    uint32_t peerRank = TILEXR_CCU_REMOTE_PEER_RANK_UNKNOWN;
    uint32_t memoryTokenId = 0;
    uint32_t rawMemoryTokenId = 0;
    uint32_t memoryTokenValue = 0;
    uint16_t remoteXnId = 0;
    uint16_t remoteNotifyCke = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> remoteEid {};
    uint32_t tpn = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    uint64_t localDoorbellVa = 0;
    uint32_t localDoorbellTokenId = 0;
    uint32_t localDoorbellTokenValue = 0;
    uint32_t localSqDepth = 0;
    bool endpointRouteVerified = false;
    bool channelResourceOwnerVerified = false;
    bool transportResourceExchangeVerified = false;
};

struct TileXRCcuLowerLayerTransportRoute {
    uint32_t channelId = 0;
    uint32_t peerRank = 0;
    uint16_t remoteXnId = 0;
    uint16_t remoteNotifyCke = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> remoteEid {};
    uint32_t tpn = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    uint64_t localDoorbellVa = 0;
    uint32_t localDoorbellTokenId = 0;
    uint32_t localDoorbellTokenValue = 0;
    uint32_t localSqDepth = 0;
    uint16_t startJettyId = 0;
    uint16_t wqeBasicBlockStartId = 0;
    uint32_t memoryTokenId = 0;
    uint32_t memoryTokenValue = 0;
    uint64_t remoteCcuVa = 0;
    bool endpointRouteVerified = false;
    bool channelResourceOwnerVerified = false;
    bool transportResourceExchangeVerified = false;
};

struct TileXRCcuLowerLayerTransportSnapshot {
    TileXRCcuLowerLayerMsidTokenSpec msidToken;
    uint8_t dieId = 0;
    uint32_t pfeOffset = 0;
    uint32_t pfeId = 0;
    uint16_t startJettyId = 0;
    uint16_t pfeJettyCount = 0;
    uint16_t startLocalJettyCtxId = 0;
    uint32_t xnStartId = 0;
    uint32_t xnCount = 0;
    uint32_t ckeStartId = 0;
    uint32_t ckeCount = 0;
    std::vector<TileXRCcuLowerLayerTransportRoute> routes;
};

int TileXRCcuBuildLowerLayerInstallPlan(
    const TileXRCcuLowerLayerPlanSpec& spec,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report);

int TileXRCcuBuildLowerLayerTransportTemplate(
    const TileXRCcuBasicInfo& basicInfo,
    const TileXRCcuResourceAllocation& allocation,
    const std::vector<TileXRCcuRemoteCcuBufferInfo>& remoteCcuBuffers,
    TileXRCcuLowerLayerTransportSnapshot* snapshot,
    TileXRCcuLowerLayerPlanBuilderReport* report);

int TileXRCcuOverlayVerifiedEndpointRoutes(
    const std::vector<TileXRCcuLowerLayerTransportRoute>& verifiedRoutes,
    TileXRCcuLowerLayerTransportSnapshot* snapshot,
    TileXRCcuLowerLayerPlanBuilderReport* report);

int TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(
    const TileXRCcuLowerLayerTransportSnapshot& snapshot,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_LOWER_LAYER_PLAN_BUILDER_H
