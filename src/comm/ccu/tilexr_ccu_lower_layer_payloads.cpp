/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_lower_layer_payloads.h"

#include <algorithm>
#include <cstring>

namespace TileXR {
namespace {

constexpr uint32_t TOKEN_VALUE_VALID = 1;
constexpr uint32_t DOORBELL_ADDR_TYPE_VA = 1;
constexpr uint32_t DOORBELL_TOKEN_VALUE_VALID = 1;
constexpr uint32_t CCU_WQE_NUM_PER_SQE = 4;

void ResetReport(TileXRCcuLowerLayerPayloadReport* report)
{
    if (report != nullptr) {
        report->message.clear();
    }
}

int Fail(TileXRCcuLowerLayerPayloadReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void Store16(uint8_t* raw, uint32_t offset, uint16_t value)
{
    raw[offset] = static_cast<uint8_t>(value & 0xffU);
    raw[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

uint16_t Log2PowerOfTwo(uint32_t value)
{
    uint16_t log2 = 0;
    while (value > 1U) {
        value >>= 1U;
        ++log2;
    }
    return log2;
}

bool IsPowerOfTwo(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

bool IsEidEmpty(const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& eid)
{
    return std::all_of(eid.begin(), eid.end(), [](uint8_t byte) { return byte == 0; });
}

} // namespace

int TileXRCcuBuildPfeCtx(
    const TileXRCcuPfeCtxSpec& spec,
    TileXRCcuPfeCtx* ctx,
    TileXRCcuLowerLayerPayloadReport* report)
{
    ResetReport(report);
    if (ctx == nullptr) {
        return Fail(report, "missing output CCU PFE context");
    }
    std::memset(ctx->raw, 0, sizeof(ctx->raw));
    if (spec.startJettyId == 0 || spec.jettyCount == 0 || spec.jettyCount > 128U ||
        spec.startLocalJettyCtxId >= 128U) {
        return Fail(report, "invalid CCU PFE context spec");
    }

    Store16(ctx->raw, 0, spec.startJettyId);
    const uint16_t word = static_cast<uint16_t>(
        ((spec.jettyCount - 1U) & 0x7fU) |
        ((static_cast<uint32_t>(spec.startLocalJettyCtxId) & 0x7fU) << 7U));
    Store16(ctx->raw, 2, word);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildLocalJettyCtx(
    const TileXRCcuLocalJettyCtxSpec& spec,
    TileXRCcuLocalJettyCtxData* ctx,
    TileXRCcuLowerLayerPayloadReport* report)
{
    ResetReport(report);
    if (ctx == nullptr) {
        return Fail(report, "missing output CCU local jetty context");
    }
    std::memset(ctx->raw, 0, sizeof(ctx->raw));
    const uint32_t wqeBasicBlocks = spec.sqDepth * CCU_WQE_NUM_PER_SQE;
    if (spec.pfeId > 0xfU || spec.dieId > 1U || spec.doorbellVa == 0 ||
        spec.sqDepth == 0 || !IsPowerOfTwo(wqeBasicBlocks)) {
        return Fail(report, "invalid CCU local jetty context spec");
    }

    Store16(ctx->raw, 0, static_cast<uint16_t>(spec.doorbellVa & 0xffffU));
    Store16(ctx->raw, 2, static_cast<uint16_t>((spec.doorbellVa >> 16U) & 0xffffU));
    Store16(ctx->raw, 4, static_cast<uint16_t>((spec.doorbellVa >> 32U) & 0xffffU));
    Store16(ctx->raw, 6, static_cast<uint16_t>((spec.doorbellVa >> 48U) & 0xffffU));

    Store16(ctx->raw, 8, static_cast<uint16_t>(
        (spec.pfeId & 0xfU) |
        ((static_cast<uint32_t>(spec.dieId) & 0x1U) << 4U) |
        (DOORBELL_ADDR_TYPE_VA << 5U) |
        (DOORBELL_TOKEN_VALUE_VALID << 6U) |
        ((spec.doorbellTokenId & 0xffU) << 8U)));
    Store16(ctx->raw, 10, static_cast<uint16_t>(
        ((spec.doorbellTokenId >> 8U) & 0xfffU) |
        ((spec.doorbellTokenValue & 0xfU) << 12U)));
    Store16(ctx->raw, 12, static_cast<uint16_t>((spec.doorbellTokenValue >> 4U) & 0xffffU));
    Store16(ctx->raw, 14, static_cast<uint16_t>(
        ((spec.doorbellTokenValue >> 20U) & 0xfffU) |
        ((static_cast<uint32_t>(Log2PowerOfTwo(wqeBasicBlocks)) & 0xfU) << 12U)));
    Store16(ctx->raw, 22, static_cast<uint16_t>(
        (static_cast<uint32_t>(spec.wqeBasicBlockStartId) & 0xfU) << 12U));
    Store16(ctx->raw, 24, static_cast<uint16_t>((spec.wqeBasicBlockStartId >> 4U) & 0xffU));
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildChannelCtxV1(
    const TileXRCcuChannelCtxV1Spec& spec,
    TileXRCcuChannelCtxDataV1* ctx,
    TileXRCcuLowerLayerPayloadReport* report)
{
    ResetReport(report);
    if (ctx == nullptr) {
        return Fail(report, "missing output CCU channel context v1");
    }
    std::memset(ctx->raw, 0, sizeof(ctx->raw));
    if (IsEidEmpty(spec.remoteEid) || spec.sourcePfeId > 0xfU || spec.startJettyId == 0 ||
        spec.jettyCount == 0 || spec.jettyCount > 128U || spec.dieId > 1U ||
        spec.remoteCcuVa == 0) {
        return Fail(report, "invalid CCU channel context v1 spec");
    }

    std::copy(spec.remoteEid.begin(), spec.remoteEid.end(), ctx->raw);
    Store16(ctx->raw, 16, static_cast<uint16_t>(spec.tpn & 0xffffU));
    Store16(ctx->raw, 18, static_cast<uint16_t>(
        ((spec.tpn >> 16U) & 0xffU) |
        ((spec.sourcePfeId & 0xfU) << 8U) |
        ((static_cast<uint32_t>(spec.startJettyId) & 0xfU) << 12U)));
    const uint32_t jettyNumMinusOne = spec.jettyCount - 1U;
    Store16(ctx->raw, 20, static_cast<uint16_t>(
        ((static_cast<uint32_t>(spec.startJettyId) >> 4U) & 0xfffU) |
        ((jettyNumMinusOne & 0xfU) << 12U)));
    Store16(ctx->raw, 22, static_cast<uint16_t>(
        ((jettyNumMinusOne >> 4U) & 0x7U) |
        ((static_cast<uint32_t>(spec.dieId) & 0x1U) << 3U) |
        ((spec.memoryTokenId & 0xfffU) << 4U)));
    Store16(ctx->raw, 24, static_cast<uint16_t>(
        ((spec.memoryTokenId >> 12U) & 0xffU) |
        ((spec.memoryTokenValue & 0xffU) << 8U)));
    Store16(ctx->raw, 26, static_cast<uint16_t>((spec.memoryTokenValue >> 8U) & 0xffffU));

    const uint64_t dstVa = spec.remoteCcuVa >> TILEXR_CCU_REMOTE_CCU_VA_SHIFT;
    Store16(ctx->raw, 28, static_cast<uint16_t>(
        ((spec.memoryTokenValue >> 24U) & 0xffU) |
        ((dstVa & 0xffU) << 8U)));
    Store16(ctx->raw, 30, static_cast<uint16_t>((dstVa >> 8U) & 0xffffU));
    Store16(ctx->raw, 32, static_cast<uint16_t>((dstVa >> 24U) & 0xffffU));
    Store16(ctx->raw, 34, static_cast<uint16_t>(
        ((dstVa >> 40U) & 0x1U) |
        (TOKEN_VALUE_VALID << 1U)));
    return TILEXR_SUCCESS;
}

} // namespace TileXR
