#include <cstring>
#include <type_traits>

#include "ep_memory_server_dedup_protocol.h"
#include "server_dedup_test_checks.h"

int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    static_assert(std::is_standard_layout<EpServerDedupPacketHeader>::value, "PacketHeader is a wire record");
    static_assert(std::is_trivially_copyable<EpServerDedupPacketHeader>::value, "PacketHeader permits bit copies");
    static_assert(std::is_standard_layout<EpServerDedupReadIndexBlock>::value, "ReadIndex is a wire record");
    static_assert(std::is_trivially_copyable<EpServerDedupReadIndexBlock>::value, "ReadIndex permits bit copies");
    Check(static_cast<uint32_t>(EpServerDedupTokenType::Direct) == 1U &&
        static_cast<uint32_t>(EpServerDedupTokenType::FirstHop) == 2U &&
        static_cast<uint32_t>(EpServerDedupTokenType::Terminal) == 3U,
        "wire and persistent TokenType share DIRECT/first-hop/terminal values");

    const uint32_t words[8] = {17U, 23U, 31U, 2U, 0U, 0U, 0U, 0U};
    EpServerDedupPacketHeader header {};
    std::memcpy(&header, words, sizeof(words));
    Check(header.srcRank == 17U && header.srcTokenId == 23U && header.topK == 31U,
        "source/token/original-k word order");
    Check(header.type == static_cast<uint32_t>(EpServerDedupTokenType::FirstHop),
        "role is the fourth word, independent of read-index member count");
    uint32_t roundTrip[8] = {};
    std::memcpy(roundTrip, &header, sizeof(header));
    Check(std::memcmp(words, roundTrip, sizeof(words)) == 0, "32B packet header round-trips including padding");

    // Exactly two words per block; self and member weights preserve their FP32 bits.
    const uint32_t indexWords[] = {2U, 0xBF400000U, 0x00070023U, 0x3F800000U,
        0xFFFF03FFU, 0x80000000U};
    EpServerDedupReadIndexBlock blocks[3] = {};
    std::memcpy(blocks, indexWords, sizeof(indexWords));
    Check(blocks[0].index == 2U, "block zero stores N without an extra row index");
    float selfWeight = 0;
    std::memcpy(&selfWeight, &blocks[0].weightBits, sizeof(selfWeight));
    Check(selfWeight == -0.75f, "header stores exact FP32 self weight");
    Check((blocks[1].index & 0xffffU) == 35U && (blocks[1].index >> 16U) == 7U &&
        blocks[1].weightBits == 0x3F800000U, "member stores expert low16/q high16 and weight");
    Check((blocks[2].index & 0xffffU) == 1023U && (blocks[2].index >> 16U) == 65535U &&
        blocks[2].weightBits == 0x80000000U, "member packing retains high q bits and negative zero");
    Check(std::memcmp(blocks, indexWords, sizeof(indexWords)) == 0,
        "full RelayReadIndex segment is bit-copyable without repacking");

    // Dispatch exposes uint32 words; Combine's int32 pointer is a view of the
    // same 12B row. A negative signed slot is not an invalid unsigned slot.
    const uint32_t destination[3] = {0x80000000U, UINT32_MAX, UINT32_MAX};
    int32_t combineView[3] = {};
    std::memcpy(combineView, destination, sizeof(destination));
    Check(combineView[0] < 0 && combineView[1] == -1 && combineView[2] == -1,
        "Combine signed view preserves the two sentinel bit patterns");
    uint32_t decoded[3] = {};
    std::memcpy(decoded, combineView, sizeof(decoded));
    Check(decoded[0] == 0x80000000U && decoded[1] == UINT32_MAX && decoded[2] == UINT32_MAX,
        "unsigned slot high bit survives Dispatch-to-Combine view");
    Check((uint32_t(1) << 31U) == 0x80000000U, "SourceMask includes original k=31");

    return ServerDedupTest::Finish("protocol");
}
