#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROTOCOL_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace TileXREp {

constexpr uint32_t kEpServerDedupRanksPerServer = 8U;
constexpr uint32_t kEpServerDedupAlignmentBytes = 32U;
constexpr uint32_t kEpServerDedupMaxTopK = 32U;

// Weight words preserve FP32 bits; these records are not a public descriptor.
struct alignas(32) EpServerDedupAssistEx {
    int32_t srcRank;
    int32_t srcTokenId;
    int32_t k;
    uint32_t weightBits;
    int32_t forwardCount;
    int32_t forwardEntryCount;
    int32_t relayRank;
    int32_t forwardTokenIdx; // First-hop output row on relayRank; terminals inherit it.
};

struct EpServerDedupGroupMember {
    uint32_t recvSlot;
    uint32_t weightBits;
};

struct EpServerDedupForwardEntry {
    int32_t destinationExpertId;
    int32_t destinationQ;
    int32_t originalK;
    uint32_t weightBits;
};

struct EpServerDedupDirectTask {
    uint32_t tokenOffset;
    uint32_t originalK;
    uint32_t expertId;
    uint32_t q;
};
static_assert(sizeof(EpServerDedupForwardEntry) == 16U, "forward entry ABI");
static_assert(sizeof(EpServerDedupDirectTask) == 16U, "source direct task ABI");

constexpr uint32_t kEpServerDedupGroupMembersOffset = sizeof(int32_t);

static_assert(sizeof(EpServerDedupAssistEx) == 32U, "AssistEx must occupy eight words");
static_assert(alignof(EpServerDedupAssistEx) == 32U, "AssistEx alignment must be 32B");
static_assert(offsetof(EpServerDedupAssistEx, srcRank) == 0U, "srcRank offset");
static_assert(offsetof(EpServerDedupAssistEx, srcTokenId) == 4U, "srcTokenId offset");
static_assert(offsetof(EpServerDedupAssistEx, k) == 8U, "k offset");
static_assert(offsetof(EpServerDedupAssistEx, weightBits) == 12U, "weight offset");
static_assert(offsetof(EpServerDedupAssistEx, forwardCount) == 16U, "forwardCount offset");
static_assert(offsetof(EpServerDedupAssistEx, forwardEntryCount) == 20U, "entry count offset");
static_assert(offsetof(EpServerDedupAssistEx, relayRank) == 24U, "relayRank offset");
static_assert(offsetof(EpServerDedupAssistEx, forwardTokenIdx) == 28U, "forwardTokenIdx offset");
static_assert(sizeof(EpServerDedupGroupMember) == 8U, "Group member must occupy two words");
static_assert(offsetof(EpServerDedupGroupMember, weightBits) == 4U, "Member weight offset");

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_PROTOCOL_H
