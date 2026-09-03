#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "tilexr_udma_reg.h"

namespace {

int g_failures = 0;

static_assert(offsetof(TileXR::TileXRUDMAProfileView, infoDev) == 16U,
    "TileXRUDMAProfileView pointer layout changed");
static_assert(sizeof(TileXR::TileXRUDMAProfileView) == 40U,
    "TileXRUDMAProfileView ABI changed");
static_assert(offsetof(TileXR::TileXRUDMAProfileDesc, reserved) == 12U,
    "TileXRUDMAProfileDesc reserved field layout changed");

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK_TRUE failed at line " << __LINE__ << ": " #expr << std::endl; \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(lhs, rhs) \
    do { \
        auto lhsValue = (lhs); \
        auto rhsValue = (rhs); \
        if (lhsValue != rhsValue) { \
            std::cerr << "CHECK_EQ failed at line " << __LINE__ << ": " #lhs " != " #rhs \
                      << " (" << lhsValue << " vs " << rhsValue << ")" << std::endl; \
            ++g_failures; \
        } \
    } while (0)

void TestRemoteAddressCalculation()
{
    TileXR::TileXRUDMARegistry registry = {};
    registry.magic = TileXR::TILEXR_UDMA_REGISTRY_MAGIC;
    registry.version = TileXR::TILEXR_UDMA_REGISTRY_VERSION;
    registry.regionCount = 1;
    registry.rankSize = 2;
    registry.regions[0].base = reinterpret_cast<GM_ADDR>(0x100000);
    registry.regions[0].bytes = 4096;
    registry.regions[1].base = reinterpret_cast<GM_ADDR>(0x200000);
    registry.regions[1].bytes = 2048;

    CHECK_TRUE(TileXR::UDMARegistryValid(&registry, 2));
    CHECK_TRUE(TileXR::UDMARegionContains(&registry, 0, 128, 256));
    CHECK_TRUE(TileXR::UDMARegionContains(&registry, 1, 1024, 1024));
    CHECK_TRUE(!TileXR::UDMARegionContains(&registry, 1, 1024, 1025));
    CHECK_TRUE(!TileXR::UDMARegionContains(&registry, 2, 0, 1));
    CHECK_EQ(reinterpret_cast<uintptr_t>(TileXR::UDMARemoteAddr(&registry, 1, 64)),
             static_cast<uintptr_t>(0x200040));
}

TileXR::TileXRUDMAProfileDesc MakeProfileDesc()
{
    TileXR::TileXRUDMAProfileDesc desc = {};
    desc.regionCount = 4;
    desc.qpBindingCount = 3;
    for (uint32_t region = 0; region < desc.regionCount; ++region) {
        desc.regions[region].base = reinterpret_cast<GM_ADDR>(0x100000 + region * 0x10000);
        desc.regions[region].bytes = 0x8000;
    }
    desc.qpBindings[0] = {0, 1};
    desc.qpBindings[1] = {0, 2};
    desc.qpBindings[2] = {0, 3};
    return desc;
}

void TestProfileDescriptorValidation()
{
    auto desc = MakeProfileDesc();
    CHECK_TRUE(TileXR::UDMAProfileDescValid(&desc, 3));
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&desc, 2));

    auto invalid = desc;
    invalid.regionCount = 0;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = desc;
    invalid.regionCount = TileXR::TILEXR_UDMA_PROFILE_MAX_REGIONS + 1;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = desc;
    invalid.qpBindings[2].remoteRegion = desc.regionCount;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = desc;
    invalid.regions[1].base = nullptr;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = desc;
    invalid.regions[1].bytes = 0;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = desc;
    invalid.regions[1].base = reinterpret_cast<GM_ADDR>(
        std::numeric_limits<uintptr_t>::max() - 7U);
    invalid.regions[1].bytes = 16;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));

    auto backed = desc;
    backed.regions[1].base = reinterpret_cast<GM_ADDR>(0x210000);
    backed.regions[1].bytes = 0x1000;
    backed.regions[1].registrationBase = reinterpret_cast<GM_ADDR>(0x200000);
    backed.regions[1].registrationBytes = 0x20000;
    CHECK_TRUE(TileXR::UDMAProfileDescValid(&backed, 3));
    CHECK_EQ(reinterpret_cast<uintptr_t>(
                 TileXR::UDMAProfileRegistrationBase(backed.regions[1])),
             static_cast<uintptr_t>(0x200000));
    CHECK_EQ(TileXR::UDMAProfileRegistrationBytes(backed.regions[1]),
             UINT64_C(0x20000));

    invalid = backed;
    invalid.regions[1].registrationBytes = 0;
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
    invalid = backed;
    invalid.regions[1].base = reinterpret_cast<GM_ADDR>(0x220000);
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&invalid, 3));
}

void TestProfileContractAgreement()
{
    const auto desc = MakeProfileDesc();
    auto peer = desc;
    peer.regions[0].base = reinterpret_cast<GM_ADDR>(0x900000);
    peer.regions[0].bytes = 0x4000;
    CHECK_TRUE(TileXR::UDMAProfileContractsEqual(desc, peer));
    peer.qpBindings[1].remoteRegion = 3;
    CHECK_TRUE(!TileXR::UDMAProfileContractsEqual(desc, peer));
    peer = desc;
    peer.reserved = static_cast<uint32_t>(
        TileXR::TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL);
    CHECK_TRUE(!TileXR::UDMAProfileContractsEqual(desc, peer));
}

void TestProfileRegistryAddressing()
{
    TileXR::TileXRUDMAProfileRegistry registry = {};
    registry.rankSize = 2;
    registry.regionCount = 4;
    registry.qpCount = 3;
    registry.qpBindings[0] = {0, 1};
    registry.qpBindings[1] = {0, 2};
    registry.qpBindings[2] = {0, 3};
    const size_t rankOneGate = TileXR::TILEXR_UDMA_PROFILE_MAX_REGIONS + 1;
    registry.regions[rankOneGate].base = reinterpret_cast<GM_ADDR>(0x800000);
    registry.regions[rankOneGate].bytes = 4096;

    CHECK_TRUE(TileXR::UDMAProfileRegistryValid(&registry, 2, 4, 3));
    registry.reserved = 99U;
    CHECK_TRUE(!TileXR::UDMAProfileRegistryValid(&registry, 2, 4, 3));
    registry.reserved = 0U;
    CHECK_TRUE(TileXR::UDMAProfileRegionContains(&registry, 1, 1, 1024, 3072));
    CHECK_TRUE(!TileXR::UDMAProfileRegionContains(&registry, 1, 1, 1024, 3073));
    CHECK_TRUE(!TileXR::UDMAProfileRegionContains(&registry, 2, 1, 0, 1));
    CHECK_EQ(reinterpret_cast<uintptr_t>(
                 TileXR::UDMAProfileRemoteAddr(&registry, 1, 1, 64)),
             static_cast<uintptr_t>(0x800040));
}

void TestGroupedProfileClassification()
{
    TileXR::TileXRUDMAProfileDesc desc {};
    desc.reserved = static_cast<uint32_t>(
        TileXR::TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL);
    desc.regionCount = 1;
    desc.qpBindingCount = 39;
    desc.regions[0].base = reinterpret_cast<GM_ADDR>(0x100000);
    desc.regions[0].bytes = 4096;
    CHECK_TRUE(TileXR::UDMAProfileDescValid(&desc, 39));
    CHECK_TRUE(!TileXR::UDMAProfileDescValid(&desc, 32));

    TileXR::TileXRUDMAProfileRegistry registry {};
    registry.reserved = static_cast<uint32_t>(
        TileXR::TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL);
    registry.rankSize = 8;
    registry.regionCount = 1;
    registry.qpCount = 39;
    CHECK_TRUE(TileXR::UDMAGroupedAllToAllProfileValid(&registry, 8));
    registry.qpBindings[38].remoteRegion = 1;
    CHECK_TRUE(!TileXR::UDMAGroupedAllToAllProfileValid(&registry, 8));
    registry.qpBindings[38].remoteRegion = 0;
    registry.reserved = 0U;
    CHECK_TRUE(!TileXR::UDMAGroupedAllToAllProfileValid(&registry, 8));
}

} // namespace

int main()
{
    TestRemoteAddressCalculation();
    TestProfileDescriptorValidation();
    TestProfileContractAgreement();
    TestProfileRegistryAddressing();
    TestGroupedProfileClassification();
    if (g_failures != 0) {
        std::cerr << g_failures << " registry checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR UDMA registry checks passed" << std::endl;
    return 0;
}
