/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_CONFIG_H
#define TILEXR_UDMA_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace TileXR {

constexpr char TILEXR_UDMA_QP_ROUTE_SPEC_ENV[] = "TILEXR_UDMA_QP_ROUTE_SPEC";
constexpr char TILEXR_UDMA_SHARED_QP_COUNT_ENV[] = "TILEXR_UDMA_SHARED_QP_COUNT";
constexpr uint32_t TILEXR_UDMA_QP_CONFIG_WIRE_VERSION = 5;
constexpr uint32_t TILEXR_UDMA_MAX_QP_COUNT = 48;
constexpr uint32_t TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT = 32;
constexpr uint32_t TILEXR_UDMA_GROUPED_DIRECT_QP_BASE =
    TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT;
constexpr uint32_t TILEXR_UDMA_GROUPED_DIRECT_QP_COUNT = 7;
constexpr uint32_t TILEXR_UDMA_GROUPED_SHARED_QP_COUNT =
    TILEXR_UDMA_GROUPED_DIRECT_QP_BASE + TILEXR_UDMA_GROUPED_DIRECT_QP_COUNT;
constexpr uint32_t TILEXR_UDMA_SHARED_QP_SIX_PORT_COUNT = 16;
constexpr uint32_t TILEXR_UDMA_SHARED_QP_SIX_PORT_VALUE = 6;
constexpr uint32_t TILEXR_UDMA_SHARED_QP_TWO_PORT_VALUE = 2;

enum class UDMAQpRouteSelector : uint32_t {
    TOPOLOGY = 1,
    PORT_COUNT = 2,
    LOCAL_PEER_SLOT = 3,
};

struct UDMAQpRouteRule {
    UDMAQpRouteSelector selector = UDMAQpRouteSelector::TOPOLOGY;
    uint32_t value = 0;
};

struct UDMAQpConfig {
    bool explicitConfig = false;
    bool sharedQp = false;
    std::vector<UDMAQpRouteRule> routes;
};

enum class UDMAQpConfigParseStatus : uint32_t {
    SUCCESS = 0,
    INVALID = 1,
};

struct UDMAQpRouteWireRule {
    uint32_t selectorKind = 0;
    uint32_t selectorValue = 0;
};

struct UDMAQpConfigWireDescriptor {
    uint32_t version = TILEXR_UDMA_QP_CONFIG_WIRE_VERSION;
    uint32_t parseStatus = static_cast<uint32_t>(UDMAQpConfigParseStatus::SUCCESS);
    uint32_t qpCount = 1;
    uint32_t sharedQp = 0;
    UDMAQpRouteWireRule routeRules[TILEXR_UDMA_MAX_QP_COUNT] = {};
};

static_assert(sizeof(UDMAQpConfigWireDescriptor) == 400,
              "UDMA QP configuration wire descriptor layout changed");
static_assert(std::is_standard_layout<UDMAQpConfigWireDescriptor>::value &&
                  std::is_trivially_copyable<UDMAQpConfigWireDescriptor>::value,
              "UDMA QP configuration wire descriptor must be byte-copyable");

UDMAQpConfigParseStatus ParseUDMAQpRouteSpec(
    const char* routeSpec, UDMAQpConfig& config, std::string* error = nullptr);

UDMAQpConfigParseStatus LoadUDMAQpConfigFromEnv(
    UDMAQpConfig& config, std::string* error = nullptr);

UDMAQpConfig BuildUDMASharedQpConfig(
    uint32_t qpCount = TILEXR_UDMA_MAX_QP_COUNT);

UDMAQpConfigParseStatus LoadUDMASharedQpConfigFromEnv(
    UDMAQpConfig& config, std::string* error = nullptr);

bool ValidateUDMASharedQpConfig(
    const UDMAQpConfig& config, std::string* error = nullptr);

bool IsUDMAGroupedQpProfile(const UDMAQpConfig& config);

uint32_t UDMAQpConfigQpCount(const UDMAQpConfig& config);

UDMAQpConfigWireDescriptor BuildUDMAQpConfigWireDescriptor(
    const UDMAQpConfig& config, UDMAQpConfigParseStatus parseStatus);

bool ValidateUDMAQpConfigWireDescriptor(
    const UDMAQpConfigWireDescriptor& descriptor, std::string* error = nullptr);

bool UDMAQpConfigWireDescriptorsEqual(
    const UDMAQpConfigWireDescriptor& lhs, const UDMAQpConfigWireDescriptor& rhs);

bool UDMAQpConfigFromWireDescriptor(
    const UDMAQpConfigWireDescriptor& descriptor, UDMAQpConfig& config,
    std::string* error = nullptr);

} // namespace TileXR

#endif // TILEXR_UDMA_CONFIG_H
