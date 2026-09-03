#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "udma/tilexr_udma_config.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK_TRUE failed at line " << __LINE__ << ": " #expr << std::endl; \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(lhs, rhs) \
    do { \
        const auto lhsValue = (lhs); \
        const auto rhsValue = (rhs); \
        if (lhsValue != rhsValue) { \
            std::cerr << "CHECK_EQ failed at line " << __LINE__ << ": " #lhs " != " #rhs << std::endl; \
            ++g_failures; \
        } \
    } while (0)

bool ParseOk(const char* text, TileXR::UDMAQpConfig& config)
{
    std::string error;
    const auto status = TileXR::ParseUDMAQpRouteSpec(text, config, &error);
    if (status != TileXR::UDMAQpConfigParseStatus::SUCCESS) {
        std::cerr << "unexpected parse error for '" << (text == nullptr ? "<null>" : text)
                  << "': " << error << std::endl;
        ++g_failures;
        return false;
    }
    return true;
}

std::string RepeatedRouteSpec(uint32_t count)
{
    std::string result;
    for (uint32_t i = 0; i < count; ++i) {
        if (!result.empty()) {
            result += ',';
        }
        result += i < 16U ? "port_count:6" : "port_count:2";
    }
    return result;
}

void ExpectInvalid(const char* text)
{
    TileXR::UDMAQpConfig config;
    std::string error;
    CHECK_EQ(TileXR::ParseUDMAQpRouteSpec(text, config, &error),
             TileXR::UDMAQpConfigParseStatus::INVALID);
    CHECK_TRUE(!error.empty());
    CHECK_TRUE(!config.explicitConfig);
    CHECK_TRUE(config.routes.empty());
}

void TestLegacyConfig()
{
    for (const char* text : {static_cast<const char*>(nullptr), "", "  \t\r\n "}) {
        TileXR::UDMAQpConfig config;
        if (!ParseOk(text, config)) {
            continue;
        }
        CHECK_TRUE(!config.explicitConfig);
        CHECK_TRUE(config.routes.empty());
        CHECK_EQ(TileXR::UDMAQpConfigQpCount(config), 1U);

        const auto wire = TileXR::BuildUDMAQpConfigWireDescriptor(
            config, TileXR::UDMAQpConfigParseStatus::SUCCESS);
        CHECK_EQ(wire.qpCount, 1U);
        CHECK_EQ(wire.routeRules[0].selectorKind, 0U);
        CHECK_TRUE(TileXR::ValidateUDMAQpConfigWireDescriptor(wire));
    }
}

void TestNormalizedRules()
{
    TileXR::UDMAQpConfig config;
    CHECK_TRUE(ParseOk(" topology ", config));
    CHECK_TRUE(config.explicitConfig);
    CHECK_EQ(config.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(config.routes[0].selector, TileXR::UDMAQpRouteSelector::TOPOLOGY);
    CHECK_EQ(config.routes[0].value, 0U);

    CHECK_TRUE(ParseOk(" port_count:6 , port_count:2 ", config));
    CHECK_EQ(TileXR::UDMAQpConfigQpCount(config), 2U);
    CHECK_EQ(config.routes[0].selector, TileXR::UDMAQpRouteSelector::PORT_COUNT);
    CHECK_EQ(config.routes[0].value, 6U);
    CHECK_EQ(config.routes[1].value, 2U);

    CHECK_TRUE(ParseOk("port_count:6,port_count:6,port_count:2", config));
    CHECK_EQ(config.routes.size(), static_cast<size_t>(3));
    CHECK_EQ(config.routes[0].value, config.routes[1].value);

    const std::string routes48 = RepeatedRouteSpec(48U);
    CHECK_TRUE(ParseOk(routes48.c_str(), config));
    CHECK_EQ(TileXR::UDMAQpConfigQpCount(config), 48U);
    CHECK_EQ(config.routes[15].value, 6U);
    CHECK_EQ(config.routes[16].value, 2U);
    CHECK_EQ(config.routes[47].value, 2U);
}

void TestInvalidRules()
{
    ExpectInvalid(",");
    ExpectInvalid(",topology");
    ExpectInvalid("topology,");
    ExpectInvalid("topology,,port_count:2");
    ExpectInvalid("unknown");
    ExpectInvalid("port_count");
    ExpectInvalid("port_count:");
    ExpectInvalid("port_count:0");
    ExpectInvalid("port_count:-1");
    ExpectInvalid("port_count:+1");
    ExpectInvalid("port_count: 2");
    ExpectInvalid("port_count:2x");
    ExpectInvalid("port_count:4294967296");
    const std::string routes49 = RepeatedRouteSpec(49U);
    ExpectInvalid(routes49.c_str());
}

void TestWireDescriptor()
{
    TileXR::UDMAQpConfig config;
    CHECK_TRUE(ParseOk("port_count:6,port_count:6,port_count:2", config));
    const auto wire = TileXR::BuildUDMAQpConfigWireDescriptor(
        config, TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_TRUE(TileXR::ValidateUDMAQpConfigWireDescriptor(wire));
    CHECK_EQ(wire.qpCount, 3U);
    CHECK_EQ(wire.routeRules[0].selectorKind,
             static_cast<uint32_t>(TileXR::UDMAQpRouteSelector::PORT_COUNT));
    CHECK_EQ(wire.routeRules[1].selectorValue, 6U);
    CHECK_EQ(wire.routeRules[2].selectorValue, 2U);

    TileXR::UDMAQpConfig roundTrip;
    CHECK_TRUE(TileXR::UDMAQpConfigFromWireDescriptor(wire, roundTrip));
    CHECK_TRUE(roundTrip.explicitConfig);
    CHECK_EQ(roundTrip.routes.size(), config.routes.size());
    CHECK_EQ(roundTrip.routes[1].value, config.routes[1].value);

    auto mismatch = wire;
    mismatch.routeRules[1].selectorValue = 2;
    CHECK_TRUE(!TileXR::UDMAQpConfigWireDescriptorsEqual(wire, mismatch));
    CHECK_TRUE(TileXR::UDMAQpConfigWireDescriptorsEqual(wire, wire));

    auto invalid = wire;
    invalid.qpCount = 49;
    CHECK_TRUE(!TileXR::ValidateUDMAQpConfigWireDescriptor(invalid));
    invalid = wire;
    invalid.routeRules[7].selectorKind = 99;
    CHECK_TRUE(!TileXR::ValidateUDMAQpConfigWireDescriptor(invalid));

    const auto parseFailure = TileXR::BuildUDMAQpConfigWireDescriptor(
        config, TileXR::UDMAQpConfigParseStatus::INVALID);
    CHECK_EQ(parseFailure.qpCount, 0U);
    CHECK_TRUE(TileXR::ValidateUDMAQpConfigWireDescriptor(parseFailure));
    CHECK_TRUE(!TileXR::UDMAQpConfigFromWireDescriptor(parseFailure, roundTrip));
}

void TestEnvironmentConfig()
{
    CHECK_EQ(setenv(TileXR::TILEXR_UDMA_QP_ROUTE_SPEC_ENV, "port_count:6,topology", 1), 0);
    TileXR::UDMAQpConfig config;
    std::string error;
    CHECK_EQ(TileXR::LoadUDMAQpConfigFromEnv(config, &error),
             TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_TRUE(config.explicitConfig);
    CHECK_TRUE(!config.sharedQp);
    CHECK_EQ(config.routes.size(), static_cast<size_t>(2));
    CHECK_EQ(config.routes[0].value, 6U);
    CHECK_EQ(config.routes[1].selector, TileXR::UDMAQpRouteSelector::TOPOLOGY);

    CHECK_EQ(unsetenv(TileXR::TILEXR_UDMA_QP_ROUTE_SPEC_ENV), 0);
}

void TestSharedQpDomainConfig()
{
    TileXR::UDMAQpConfig config = TileXR::BuildUDMASharedQpConfig();
    CHECK_TRUE(config.explicitConfig);
    CHECK_TRUE(config.sharedQp);
    CHECK_TRUE(TileXR::ValidateUDMASharedQpConfig(config));
    CHECK_EQ(config.routes.size(),
             static_cast<size_t>(TileXR::TILEXR_UDMA_MAX_QP_COUNT));
    for (uint32_t qp = 0; qp < TileXR::TILEXR_UDMA_MAX_QP_COUNT; ++qp) {
        CHECK_EQ(config.routes[qp].selector,
                 TileXR::UDMAQpRouteSelector::PORT_COUNT);
        CHECK_EQ(config.routes[qp].value,
                 qp < TileXR::TILEXR_UDMA_SHARED_QP_SIX_PORT_COUNT ?
                     TileXR::TILEXR_UDMA_SHARED_QP_SIX_PORT_VALUE :
                     TileXR::TILEXR_UDMA_SHARED_QP_TWO_PORT_VALUE);
    }

    const auto wire = TileXR::BuildUDMAQpConfigWireDescriptor(
        config, TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_EQ(wire.qpCount, TileXR::TILEXR_UDMA_MAX_QP_COUNT);
    CHECK_EQ(wire.sharedQp, 1U);
    CHECK_TRUE(TileXR::ValidateUDMAQpConfigWireDescriptor(wire));
    TileXR::UDMAQpConfig roundTrip;
    CHECK_TRUE(TileXR::UDMAQpConfigFromWireDescriptor(wire, roundTrip));
    CHECK_TRUE(roundTrip.sharedQp);
    CHECK_EQ(roundTrip.routes.size(), config.routes.size());

    auto invalidConfig = config;
    invalidConfig.routes.pop_back();
    CHECK_TRUE(!TileXR::ValidateUDMASharedQpConfig(invalidConfig));
    invalidConfig = config;
    invalidConfig.routes[0].selector = TileXR::UDMAQpRouteSelector::TOPOLOGY;
    CHECK_TRUE(!TileXR::ValidateUDMASharedQpConfig(invalidConfig));

    auto invalidWire = wire;
    invalidWire.qpCount = TileXR::TILEXR_UDMA_MAX_QP_COUNT - 1U;
    CHECK_TRUE(!TileXR::ValidateUDMAQpConfigWireDescriptor(invalidWire));
    invalidWire = wire;
    invalidWire.routeRules[16].selectorValue =
        TileXR::TILEXR_UDMA_SHARED_QP_SIX_PORT_VALUE;
    CHECK_TRUE(!TileXR::ValidateUDMAQpConfigWireDescriptor(invalidWire));

    auto grouped = TileXR::BuildUDMASharedQpConfig(
        TileXR::TILEXR_UDMA_GROUPED_SHARED_QP_COUNT);
    CHECK_TRUE(TileXR::ValidateUDMASharedQpConfig(grouped));
    CHECK_TRUE(TileXR::IsUDMAGroupedQpProfile(grouped));
    CHECK_EQ(grouped.routes.size(),
             static_cast<size_t>(TileXR::TILEXR_UDMA_GROUPED_SHARED_QP_COUNT));
    const auto groupedWire = TileXR::BuildUDMAQpConfigWireDescriptor(
        grouped, TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_EQ(groupedWire.qpCount, TileXR::TILEXR_UDMA_GROUPED_SHARED_QP_COUNT);
    CHECK_TRUE(TileXR::ValidateUDMAQpConfigWireDescriptor(groupedWire));
    CHECK_TRUE(TileXR::UDMAQpConfigFromWireDescriptor(groupedWire, roundTrip));
    CHECK_EQ(roundTrip.routes.size(), grouped.routes.size());
    for (uint32_t qp = 0U;
         qp < TileXR::TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT; ++qp) {
        CHECK_EQ(grouped.routes[qp].selector,
                 TileXR::UDMAQpRouteSelector::PORT_COUNT);
    }
    for (uint32_t slot = 0U;
         slot < TileXR::TILEXR_UDMA_GROUPED_DIRECT_QP_COUNT; ++slot) {
        const auto& route = grouped.routes[
            TileXR::TILEXR_UDMA_GROUPED_DIRECT_QP_BASE + slot];
        CHECK_EQ(route.selector, TileXR::UDMAQpRouteSelector::LOCAL_PEER_SLOT);
        CHECK_EQ(route.value, slot);
    }
    auto invalidGrouped = grouped;
    invalidGrouped.routes[TileXR::TILEXR_UDMA_GROUPED_DIRECT_QP_BASE].selector =
        TileXR::UDMAQpRouteSelector::PORT_COUNT;
    CHECK_TRUE(!TileXR::IsUDMAGroupedQpProfile(invalidGrouped));
    CHECK_TRUE(!TileXR::IsUDMAGroupedQpProfile(
        TileXR::BuildUDMASharedQpConfig(
            TileXR::TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT)));
    CHECK_TRUE(!TileXR::IsUDMAGroupedQpProfile(
        TileXR::BuildUDMASharedQpConfig(TileXR::TILEXR_UDMA_MAX_QP_COUNT)));

    CHECK_EQ(setenv(TileXR::TILEXR_UDMA_SHARED_QP_COUNT_ENV, "32", 1), 0);
    CHECK_EQ(TileXR::LoadUDMASharedQpConfigFromEnv(grouped, nullptr),
             TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_EQ(grouped.routes.size(),
             static_cast<size_t>(TileXR::TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT));
    CHECK_EQ(setenv(TileXR::TILEXR_UDMA_SHARED_QP_COUNT_ENV, "39", 1), 0);
    CHECK_EQ(TileXR::LoadUDMASharedQpConfigFromEnv(grouped, nullptr),
             TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_EQ(grouped.routes.size(),
             static_cast<size_t>(TileXR::TILEXR_UDMA_GROUPED_SHARED_QP_COUNT));
    CHECK_EQ(setenv(TileXR::TILEXR_UDMA_SHARED_QP_COUNT_ENV, "31", 1), 0);
    std::string error;
    CHECK_EQ(TileXR::LoadUDMASharedQpConfigFromEnv(grouped, &error),
             TileXR::UDMAQpConfigParseStatus::INVALID);
    CHECK_TRUE(!error.empty());
    CHECK_EQ(unsetenv(TileXR::TILEXR_UDMA_SHARED_QP_COUNT_ENV), 0);
    CHECK_EQ(TileXR::LoadUDMASharedQpConfigFromEnv(grouped, nullptr),
             TileXR::UDMAQpConfigParseStatus::SUCCESS);
    CHECK_EQ(grouped.routes.size(),
             static_cast<size_t>(TileXR::TILEXR_UDMA_MAX_QP_COUNT));
}

} // namespace

int main()
{
    TestLegacyConfig();
    TestNormalizedRules();
    TestInvalidRules();
    TestWireDescriptor();
    TestEnvironmentConfig();
    TestSharedQpDomainConfig();
    if (g_failures != 0) {
        std::cerr << g_failures << " UDMA route configuration checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR UDMA route configuration checks passed" << std::endl;
    return 0;
}
