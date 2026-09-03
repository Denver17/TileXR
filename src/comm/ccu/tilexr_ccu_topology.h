/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_TOPOLOGY_H
#define TILEXR_CCU_TOPOLOGY_H

#include "ccu/tilexr_ccu_hccp_types.h"
#include "tilexr_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRCcuPeerEidRoute {
    uint32_t peerDevicePhyId = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> localEid {};
    std::string localPort;
    uint32_t tpType = TILEXR_CCU_HCCP_TP_TYPE_RTP;
};

int TileXRCcuResolvePeerEidRoutes(
    const std::string& rootInfoPath,
    uint32_t localDevicePhyId,
    const std::vector<uint32_t>& peerDevicePhyIds,
    std::vector<TileXRCcuPeerEidRoute>* routes,
    std::string* message);

} // namespace TileXR

#endif // TILEXR_CCU_TOPOLOGY_H
