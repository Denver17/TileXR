#include <cstring>
#include <type_traits>

#include "ep_memory_server_dedup_protocol.h"
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

    return ServerDedupTest::Finish("protocol");
}
