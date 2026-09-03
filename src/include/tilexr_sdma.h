/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_H
#define TILEXR_SDMA_H

#include "comm_args.h"
#include "tilexr_sdma_types.h"

#if defined(__has_include)
#if __has_include("tilexr_sdma_config.h")
#include "tilexr_sdma_config.h"
#endif
#endif

#ifndef TILEXR_HAVE_PTO_SDMA
#define TILEXR_HAVE_PTO_SDMA 0
#endif

#if TILEXR_ASCENDC_AICORE_COMPILE && \
    (((defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)) || \
      (defined(CATLASS_ARCH) && (CATLASS_ARCH == 3510))) || \
     defined(TILEXR_SDMA_FORCE_A5))
#define TILEXR_SDMA_A5_AICORE_COMPILE 1
#include "tilexr_sdma_a5.h"
#else
#define TILEXR_SDMA_A5_AICORE_COMPILE 0
#endif

#if TILEXR_ASCENDC_AICORE_COMPILE && !TILEXR_SDMA_A5_AICORE_COMPILE && \
    defined(TILEXR_HAVE_PTO_SDMA) && TILEXR_HAVE_PTO_SDMA
#include "tilexr_sdma_compat.h"
#endif

namespace TileXR {

#if TILEXR_ASCENDC_AICORE_COMPILE

__aicore__ inline bool SDMAEnabled(const __gm__ CommArgs* args)
{
    return args != nullptr && ((args->extraFlag & ExtraFlag::SDMA) != 0) && args->sdmaWorkspacePtr != nullptr;
}

__aicore__ inline uint32_t SDMAResolveChannelGroup(uint32_t channelGroupIdx)
{
    if (channelGroupIdx == TILEXR_SDMA_AUTO_CHANNEL_GROUP) {
        return static_cast<uint32_t>(AscendC::GetBlockIdx());
    }
    return channelGroupIdx;
}

__aicore__ inline bool SDMAPrewarmSqPages(
    const __gm__ CommArgs* args, uint32_t channelCount,
    AscendC::LocalTensor<uint8_t> scratch)
{
#if TILEXR_SDMA_A5_AICORE_COMPILE
    return SDMAEnabled(args) && detail::A5SdmaPrewarmSqPages(
        reinterpret_cast<__gm__ uint8_t*>(args->sdmaWorkspacePtr),
        channelCount, scratch);
#else
    (void)args;
    (void)channelCount;
    (void)scratch;
    return false;
#endif
}

__aicore__ inline uint64_t SDMACopyNbi(
    const __gm__ CommArgs* args,
    __gm__ uint8_t* dst,
    __gm__ uint8_t* src,
    uint64_t bytes,
    uint32_t channelGroupIdx = TILEXR_SDMA_AUTO_CHANNEL_GROUP,
    SDMASubmitTrace* submitTrace = nullptr)
{
#if TILEXR_SDMA_A5_AICORE_COMPILE
    if (!SDMAEnabled(args) || dst == nullptr || src == nullptr || bytes == 0) {
        return 0;
    }
    const uint32_t resolvedGroup = SDMAResolveChannelGroup(channelGroupIdx);
    return detail::A5SdmaCopyNbi(
        reinterpret_cast<__gm__ uint8_t*>(args->sdmaWorkspacePtr),
        dst, src, bytes, resolvedGroup, submitTrace);
#elif defined(TILEXR_HAVE_PTO_SDMA) && TILEXR_HAVE_PTO_SDMA
    (void)submitTrace;
    if (!SDMAEnabled(args) || dst == nullptr || src == nullptr || bytes == 0) {
        return 0;
    }
    detail::SDMAScratchTile scratch;
    pto::comm::sdma::SdmaSession session {};
    const uint32_t resolvedGroup = SDMAResolveChannelGroup(channelGroupIdx);
    const bool built = detail::TileXRSDMABuildSession(
        scratch, reinterpret_cast<__gm__ uint8_t*>(args->sdmaWorkspacePtr), session, resolvedGroup);
    if (!built || !session.valid) {
        return 0;
    }
    return detail::TileXRSDMAPostPut(dst, src, bytes, session);
#else
    (void)args;
    (void)dst;
    (void)src;
    (void)bytes;
    (void)channelGroupIdx;
    (void)submitTrace;
    return 0;
#endif
}

__aicore__ inline bool SDMAWait(
    const __gm__ CommArgs* args,
    uint64_t eventHandle,
    uint32_t channelGroupIdx = TILEXR_SDMA_AUTO_CHANNEL_GROUP)
{
#if TILEXR_SDMA_A5_AICORE_COMPILE
    if (eventHandle == 0) {
        return true;
    }
    if (!SDMAEnabled(args)) {
        return false;
    }
    const uint32_t resolvedGroup = SDMAResolveChannelGroup(channelGroupIdx);
    return detail::A5SdmaWaitEvent(
        reinterpret_cast<__gm__ uint8_t*>(args->sdmaWorkspacePtr),
        eventHandle, resolvedGroup);
#elif defined(TILEXR_HAVE_PTO_SDMA) && TILEXR_HAVE_PTO_SDMA
    if (eventHandle == 0) {
        return true;
    }
    if (!SDMAEnabled(args)) {
        return false;
    }
    detail::SDMAScratchTile scratch;
    pto::comm::sdma::SdmaSession session {};
    const uint32_t resolvedGroup = SDMAResolveChannelGroup(channelGroupIdx);
    const bool built = detail::TileXRSDMABuildSession(
        scratch, reinterpret_cast<__gm__ uint8_t*>(args->sdmaWorkspacePtr), session, resolvedGroup);
    if (!built || !session.valid) {
        return false;
    }
    return detail::TileXRSDMAWait(eventHandle, session);
#else
    (void)args;
    (void)eventHandle;
    (void)channelGroupIdx;
    return eventHandle == 0;
#endif
}

#endif // TILEXR_ASCENDC_AICORE_COMPILE

} // namespace TileXR

#endif // TILEXR_SDMA_H
