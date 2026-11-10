/*
 * URMA Dispatch 的 credit token 与有界源槽布局；Host 校验组数，Device 用相同偏移发布信用。
 */
#ifndef TILEXR_MOONEP_DISPATCH_CREDIT_H
#define TILEXR_MOONEP_DISPATCH_CREDIT_H

#include <cstdint>

#include "comm_args.h"
#include "dispatch_common.h"

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_MOONEP_CREDIT_INLINE __attribute__((always_inline)) inline __aicore__
#else
#define TILEXR_MOONEP_CREDIT_INLINE inline
#endif

namespace TileXRMoonEp {

constexpr uint64_t kDispatchCreditStrideBytes = 512U;
constexpr uint32_t kDispatchCreditPingPongSlots = 2U;
constexpr uint32_t kDispatchCreditGroupBits = 16U;
constexpr uint64_t kDispatchCreditGroupMask =
    (UINT64_C(1) << kDispatchCreditGroupBits) - 1U;
constexpr uint64_t kDispatchCreditMaxMagic =
    UINT64_MAX >> kDispatchCreditGroupBits;
constexpr uint64_t kDispatchCreditPlaneBytes =
    static_cast<uint64_t>(TileXR::CREDIT_IPC_SLOT_BYTES);
constexpr uint64_t kDispatchCreditBytes =
    static_cast<uint64_t>(TileXR::DISPATCH_CREDIT_IPC_BYTES);
constexpr uint32_t kDispatchCreditMaxGroupCount = 16U;
constexpr uint64_t kDispatchCreditSourceStrideBytes = 64U;
constexpr uint64_t kDispatchCreditSourceBytes =
    static_cast<uint64_t>(kDispatchAivCoreCount) *
    kDispatchCreditMaxGroupCount * kDispatchCreditSourceStrideBytes;

static_assert(kDispatchCreditStrideBytes ==
        static_cast<uint64_t>(TileXR::CREDIT_IPC_STRIDE),
    "Dispatch and communicator credit strides differ");
static_assert(kDispatchCreditBytes ==
        kDispatchCreditPingPongSlots * kDispatchCreditPlaneBytes,
    "Dispatch credit ping-pong layout changed");
static_assert(kDispatchCreditMaxGroupCount * kDispatchDefaultGroupWidth >=
        static_cast<uint32_t>(TileXR::TILEXR_MAX_RANK_SIZE),
    "Dispatch credit source slots do not cover default-width groups");

TILEXR_MOONEP_CREDIT_INLINE bool DispatchCreditToken(
    int64_t magic, uint32_t group, uint64_t &token)
{
    token = 0U;
    if (magic <= 0 || static_cast<uint64_t>(magic) > kDispatchCreditMaxMagic ||
        group >= kDispatchCreditGroupMask) {
        return false;
    }
    token = (static_cast<uint64_t>(magic) << kDispatchCreditGroupBits) |
        (static_cast<uint64_t>(group) + 1U);
    return true;
}

TILEXR_MOONEP_CREDIT_INLINE uint64_t DispatchCreditPlaneOffset(int64_t magic)
{
    return magic <= 0 ? UINT64_MAX :
        (static_cast<uint64_t>(magic) & 1U) * kDispatchCreditPlaneBytes;
}

TILEXR_MOONEP_CREDIT_INLINE uint64_t DispatchCreditEntryOffset(
    uint32_t destinationRank)
{
    return destinationRank >= static_cast<uint32_t>(TileXR::TILEXR_MAX_RANK_SIZE) ?
        UINT64_MAX :
        static_cast<uint64_t>(destinationRank) * kDispatchCreditStrideBytes;
}

TILEXR_MOONEP_CREDIT_INLINE uint64_t DispatchCreditSourceOffset(
    uint32_t core, uint32_t group)
{
    return core >= kDispatchAivCoreCount ||
        group >= kDispatchCreditMaxGroupCount ? UINT64_MAX :
        (static_cast<uint64_t>(core) * kDispatchCreditMaxGroupCount + group) *
            kDispatchCreditSourceStrideBytes;
}

TILEXR_MOONEP_CREDIT_INLINE bool DispatchCreditRequired(uint32_t group)
{
    return group != 0U;
}

TILEXR_MOONEP_CREDIT_INLINE bool DispatchCreditReady(
    uint64_t observed, uint64_t expected)
{
    return expected != 0U && observed >= expected;
}

} // namespace TileXRMoonEp

#undef TILEXR_MOONEP_CREDIT_INLINE

#endif // TILEXR_MOONEP_DISPATCH_CREDIT_H
