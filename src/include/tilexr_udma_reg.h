/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_REG_H
#define TILEXR_UDMA_REG_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "comm_args.h"

namespace TileXR {

constexpr uint32_t TILEXR_UDMA_REGISTRY_MAGIC = 0x54585255U; // TXRU
constexpr uint32_t TILEXR_UDMA_REGISTRY_VERSION = 1U;
constexpr uint32_t TILEXR_UDMA_MAX_REGIONS = 1U;
constexpr uint32_t TILEXR_UDMA_PROFILE_MAGIC = 0x54585052U; // TXPR
constexpr uint32_t TILEXR_UDMA_PROFILE_VERSION = 2U;
constexpr uint32_t TILEXR_UDMA_PROFILE_MAX_REGIONS = 8U;
constexpr uint32_t TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS = 48U;

enum class TileXRUDMAProfileKind : uint32_t {
    GENERIC = 0U,
    A5_GROUPED_ALLTOALL = 1U,
};

struct TileXRUDMARegionDesc {
    GM_ADDR base = nullptr;
    uint64_t bytes = 0;
};

struct TileXRUDMARegistry {
    uint32_t magic = TILEXR_UDMA_REGISTRY_MAGIC;
    uint32_t version = TILEXR_UDMA_REGISTRY_VERSION;
    uint32_t rankSize = 0;
    uint32_t regionCount = 0;
    TileXRUDMARegionDesc regions[TILEXR_MAX_RANK_SIZE] = {};
};

struct TileXRUDMAProfileRegionDesc {
    GM_ADDR base = nullptr;
    uint64_t bytes = 0;
    GM_ADDR registrationBase = nullptr;
    uint64_t registrationBytes = 0;
};

struct TileXRUDMAProfileQpBinding {
    uint32_t localRegion = 0;
    uint32_t remoteRegion = 0;
};

struct TileXRUDMAProfileDesc {
    uint32_t version = TILEXR_UDMA_PROFILE_VERSION;
    uint32_t regionCount = 0;
    uint32_t qpBindingCount = 0;
    uint32_t reserved = 0;
    TileXRUDMAProfileRegionDesc regions[TILEXR_UDMA_PROFILE_MAX_REGIONS] = {};
    TileXRUDMAProfileQpBinding qpBindings[TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS] = {};
};

struct TileXRUDMAProfileRegistry {
    uint32_t magic = TILEXR_UDMA_PROFILE_MAGIC;
    uint32_t version = TILEXR_UDMA_PROFILE_VERSION;
    uint32_t rankSize = 0;
    uint32_t regionCount = 0;
    uint32_t qpCount = 0;
    uint32_t reserved = 0;
    TileXRUDMAProfileQpBinding qpBindings[TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS] = {};
    TileXRUDMAProfileRegionDesc
        regions[TILEXR_MAX_RANK_SIZE * TILEXR_UDMA_PROFILE_MAX_REGIONS] = {};
};

struct TileXRUDMAProfileView {
    uint32_t version = TILEXR_UDMA_PROFILE_VERSION;
    uint32_t rankSize = 0;
    uint32_t regionCount = 0;
    uint32_t qpCount = 0;
    GM_ADDR infoDev = nullptr;
    GM_ADDR registryDev = nullptr;
    const TileXRUDMAProfileRegistry* registryHost = nullptr;
};

inline bool UDMAProfileRegionValid(const TileXRUDMAProfileRegionDesc& region)
{
    if (region.base == nullptr || region.bytes == 0) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(region.base);
    if (region.bytes > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max() - base)) {
        return false;
    }
    if (region.registrationBase == nullptr && region.registrationBytes == 0) {
        return true;
    }
    if (region.registrationBase == nullptr || region.registrationBytes == 0) {
        return false;
    }
    const uintptr_t registrationBase =
        reinterpret_cast<uintptr_t>(region.registrationBase);
    if (region.registrationBytes > static_cast<uint64_t>(
            std::numeric_limits<uintptr_t>::max() - registrationBase) ||
        base < registrationBase) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(base - registrationBase);
    return offset <= region.registrationBytes &&
        region.bytes <= region.registrationBytes - offset;
}

inline GM_ADDR UDMAProfileRegistrationBase(const TileXRUDMAProfileRegionDesc& region)
{
    return region.registrationBase == nullptr ? region.base : region.registrationBase;
}

inline uint64_t UDMAProfileRegistrationBytes(const TileXRUDMAProfileRegionDesc& region)
{
    return region.registrationBase == nullptr ? region.bytes : region.registrationBytes;
}

inline bool UDMAProfileDescValid(const TileXRUDMAProfileDesc* desc, uint32_t expectedQpCount)
{
    const auto kind = desc == nullptr ? TileXRUDMAProfileKind::GENERIC :
        static_cast<TileXRUDMAProfileKind>(desc->reserved);
    if (desc == nullptr || desc->version != TILEXR_UDMA_PROFILE_VERSION ||
        desc->regionCount == 0 || desc->regionCount > TILEXR_UDMA_PROFILE_MAX_REGIONS ||
        expectedQpCount == 0 || expectedQpCount > TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS ||
        desc->qpBindingCount != expectedQpCount ||
        (kind != TileXRUDMAProfileKind::GENERIC &&
         kind != TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL) ||
        (kind == TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL &&
         (expectedQpCount != 39U || desc->regionCount != 1U))) {
        return false;
    }
    for (uint32_t region = 0; region < desc->regionCount; ++region) {
        if (!UDMAProfileRegionValid(desc->regions[region])) {
            return false;
        }
    }
    for (uint32_t qp = 0; qp < desc->qpBindingCount; ++qp) {
        if (desc->qpBindings[qp].localRegion >= desc->regionCount ||
            desc->qpBindings[qp].remoteRegion >= desc->regionCount) {
            return false;
        }
    }
    return true;
}

inline bool UDMAProfileContractsEqual(
    const TileXRUDMAProfileDesc& lhs, const TileXRUDMAProfileDesc& rhs)
{
    if (lhs.version != rhs.version || lhs.reserved != rhs.reserved ||
        lhs.regionCount != rhs.regionCount ||
        lhs.qpBindingCount != rhs.qpBindingCount) {
        return false;
    }
    for (uint32_t qp = 0; qp < lhs.qpBindingCount; ++qp) {
        if (lhs.qpBindings[qp].localRegion != rhs.qpBindings[qp].localRegion ||
            lhs.qpBindings[qp].remoteRegion != rhs.qpBindings[qp].remoteRegion) {
            return false;
        }
    }
    return true;
}

inline bool UDMAProfileRegistryValid(const TileXRUDMAProfileRegistry* registry,
    int expectedRankSize, uint32_t expectedRegionCount, uint32_t expectedQpCount = 0)
{
    if (registry == nullptr || registry->magic != TILEXR_UDMA_PROFILE_MAGIC ||
        registry->version != TILEXR_UDMA_PROFILE_VERSION || expectedRankSize <= 0 ||
        expectedRankSize > TILEXR_MAX_RANK_SIZE ||
        registry->rankSize != static_cast<uint32_t>(expectedRankSize) ||
        expectedRegionCount == 0 || expectedRegionCount > TILEXR_UDMA_PROFILE_MAX_REGIONS ||
        registry->regionCount != expectedRegionCount || registry->qpCount == 0 ||
        registry->qpCount > TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS ||
        (registry->reserved != static_cast<uint32_t>(TileXRUDMAProfileKind::GENERIC) &&
         registry->reserved != static_cast<uint32_t>(
             TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL)) ||
        (expectedQpCount != 0 && registry->qpCount != expectedQpCount)) {
        return false;
    }
    for (uint32_t qp = 0; qp < registry->qpCount; ++qp) {
        if (registry->qpBindings[qp].localRegion >= registry->regionCount ||
            registry->qpBindings[qp].remoteRegion >= registry->regionCount) {
            return false;
        }
    }
    return true;
}

inline bool UDMAGroupedAllToAllProfileValid(
    const TileXRUDMAProfileRegistry* registry, int expectedRankSize)
{
    if (!UDMAProfileRegistryValid(registry, expectedRankSize, 1U, 39U) ||
        registry->reserved != static_cast<uint32_t>(
            TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL)) {
        return false;
    }
    for (uint32_t qp = 0U; qp < registry->qpCount; ++qp) {
        if (registry->qpBindings[qp].localRegion != 0U ||
            registry->qpBindings[qp].remoteRegion != 0U) {
            return false;
        }
    }
    return true;
}

inline const TileXRUDMAProfileRegionDesc* UDMAProfileRegion(
    const TileXRUDMAProfileRegistry* registry, int rank, uint32_t region)
{
    if (registry == nullptr || rank < 0 || static_cast<uint32_t>(rank) >= registry->rankSize ||
        region >= registry->regionCount || registry->rankSize > TILEXR_MAX_RANK_SIZE ||
        registry->regionCount > TILEXR_UDMA_PROFILE_MAX_REGIONS) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(rank) * TILEXR_UDMA_PROFILE_MAX_REGIONS + region;
    return &registry->regions[index];
}

inline bool UDMAProfileRegionContains(const TileXRUDMAProfileRegistry* registry,
    int rank, uint32_t region, uint64_t byteOffset, uint64_t byteCount)
{
    const auto* desc = UDMAProfileRegion(registry, rank, region);
    if (desc == nullptr || !UDMAProfileRegionValid(*desc) || byteOffset > desc->bytes) {
        return false;
    }
    return byteCount <= desc->bytes - byteOffset;
}

inline GM_ADDR UDMAProfileRemoteAddr(const TileXRUDMAProfileRegistry* registry,
    int rank, uint32_t region, uint64_t byteOffset)
{
    const auto* desc = UDMAProfileRegion(registry, rank, region);
    if (desc == nullptr || byteOffset > desc->bytes) {
        return nullptr;
    }
    return desc->base + byteOffset;
}

inline bool UDMARegistryValid(const TileXRUDMARegistry *registry, int expectedRankSize)
{
    return registry != nullptr &&
           registry->magic == TILEXR_UDMA_REGISTRY_MAGIC &&
           registry->version == TILEXR_UDMA_REGISTRY_VERSION &&
           expectedRankSize >= 0 &&
           registry->rankSize == static_cast<uint32_t>(expectedRankSize) &&
           registry->rankSize <= TILEXR_MAX_RANK_SIZE &&
           registry->regionCount > 0 &&
           registry->regionCount <= TILEXR_UDMA_MAX_REGIONS;
}

inline bool UDMARegionContains(const TileXRUDMARegistry *registry, int rank, uint64_t byteOffset, uint64_t byteCount)
{
    if (!UDMARegistryValid(registry, static_cast<int>(registry == nullptr ? 0 : registry->rankSize))) {
        return false;
    }
    if (rank < 0 || static_cast<uint32_t>(rank) >= registry->rankSize) {
        return false;
    }
    const auto &region = registry->regions[rank];
    if (region.base == nullptr || byteOffset > region.bytes) {
        return false;
    }
    return byteCount <= region.bytes - byteOffset;
}

inline GM_ADDR UDMARemoteAddr(const TileXRUDMARegistry *registry, int rank, uint64_t byteOffset)
{
    if (registry == nullptr || rank < 0 || static_cast<uint32_t>(rank) >= registry->rankSize) {
        return nullptr;
    }
    return registry->regions[rank].base + byteOffset;
}

} // namespace TileXR

#endif // TILEXR_UDMA_REG_H
