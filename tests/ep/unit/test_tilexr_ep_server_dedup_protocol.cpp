#include <cstring>
#include <type_traits>

#include "ep_memory_server_dedup_protocol.h"
#include "ep_memory_server_dedup_profile.h"
#include "tilexr_types.h"
#include "server_dedup_test_checks.h"

int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    static_assert(std::is_standard_layout<EpServerDedupAssistEx>::value, "AssistEx is a wire record");
    static_assert(std::is_trivially_copyable<EpServerDedupAssistEx>::value, "AssistEx permits bit copies");
    static_assert(std::is_standard_layout<EpServerDedupGroupMember>::value, "Group member is a wire record");
    const uint32_t words[8] = {17U, 23U, 31U, 0xBF400000U, 0xFFFFFFFFU, 5U, 8U, 29U};
    EpServerDedupAssistEx assist {};
    std::memcpy(&assist, words, sizeof(words));
    Check(assist.srcRank == 17 && assist.srcTokenId == 23 && assist.k == 31, "source/token/k word order");
    Check(assist.forwardCount == -1 && assist.forwardEntryCount == 5, "signed forwarding fields");
    Check(assist.relayRank == 8 && assist.forwardTokenIdx == 29, "relay identity word order");
    float weight = 0;
    std::memcpy(&weight, &assist.weightBits, sizeof(weight));
    Check(weight == -0.75f, "FP32 weight bit pattern is preserved");
    uint32_t roundTrip[8] = {};
    std::memcpy(roundTrip, &assist, sizeof(assist));
    Check(std::memcmp(words, roundTrip, sizeof(words)) == 0, "all eight words round-trip");

    // A group has a firstHopRow followed immediately by dense pairs, including local returns.
    uint32_t group[8] = {42U, 99U, 0x3F800000U, 101U, 0x80000000U};
    EpServerDedupGroupMember members[2] = {};
    std::memcpy(members, reinterpret_cast<const uint8_t *>(group) + kEpServerDedupGroupMembersOffset,
        sizeof(members));
    Check(members[0].recvSlot == 99U && members[0].weightBits == 0x3F800000U, "first dense member");
    Check(members[1].recvSlot == 101U && members[1].weightBits == 0x80000000U, "second member preserves negative zero");
    Check((uint32_t(1) << 31U) == 0x80000000U, "sourceMask includes original k=31");

    static_assert(std::is_standard_layout<EpServerDedupProfileRecord>::value, "Profile record is a wire record");
    static_assert(std::is_trivially_copyable<EpServerDedupProfileRecord>::value, "Profile record permits bit copies");
    static_assert(kEpServerDedupProfileOffset + kEpServerDedupProfileMaxCores *
        sizeof(EpServerDedupProfileRecord) <=
        static_cast<uint64_t>(TileXR::TILEXR_MEMORY_DOMAIN_BUFFER_SIZE) * 1024U * 1024U,
        "Profile records must remain before MemoryDomain flags");
    const EpServerDedupProfileRecord profile {kEpServerDedupProfileMarker, kEpServerDedupProfileVersion,
        sizeof(EpServerDedupProfileRecord), 127U, 199U, 200U, 0U, UINT64_C(0x100000002),
        UINT64_C(0x100000004), UINT64_C(0x100000008), UINT64_C(0x10000000c), 0U};
    uint64_t profileWords[8] = {};
    std::memcpy(profileWords, &profile, sizeof(profile));
    Check(profileWords[3] == UINT64_C(0x100000002) && profileWords[4] == UINT64_C(0x100000004) &&
        profileWords[5] == UINT64_C(0x100000008) && profileWords[6] == UINT64_C(0x10000000c),
        "Profile generation and raw ticks preserve their 64-bit positions");
    return ServerDedupTest::Finish("protocol");
}
