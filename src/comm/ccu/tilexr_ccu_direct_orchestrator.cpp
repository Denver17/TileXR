/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_direct_orchestrator.h"

#include "ccu/tilexr_ccu_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace TileXR {
namespace {

constexpr const char* TILEXR_CCU_DIRECT_KNOWN_MISSING_INSTALL_SURFACES =
    "remote XN install provider is missing";
constexpr uint16_t TILEXR_CCU_TRACE_LOAD_SQE_ARGS_TO_X_HEADER = 0x0001U;
constexpr uint16_t TILEXR_CCU_TRACE_LOAD_IMD_TO_GSA_HEADER = 0x0002U;
constexpr uint16_t TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER = 0x0003U;
constexpr uint16_t TILEXR_CCU_TRACE_SET_CKE_HEADER = 0x0802U;
constexpr uint16_t TILEXR_CCU_TRACE_CLEAR_CKE_HEADER = 0x0804U;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_RMT_MEM_TO_LOC_MEM_HEADER = 0x1008U;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_RMT_MEM_HEADER = 0x1009U;
constexpr uint16_t TILEXR_CCU_TRACE_SYNC_CKE_HEADER = 0x100bU;
constexpr uint16_t TILEXR_CCU_TRACE_SYNC_XN_HEADER = 0x100dU;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_VALID_SHIFT = 52ULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_ID_SHIFT = 32ULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_ID_MASK = 0xfffffULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_VALUE_MASK = 0xffffffffULL;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT = 7U;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT = 3U;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT = 2U;

void ResetReport(TileXRCcuDirectInstallReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuDirectInstallReport{};
    }
}

void ClearAttempt(TileXRCcuDirectInstallAttempt* attempt)
{
    if (attempt != nullptr) {
        *attempt = TileXRCcuDirectInstallAttempt{};
    }
}

int Fail(TileXRCcuDirectInstallAttempt* attempt, TileXRCcuDirectInstallReport* report, const std::string& message)
{
    ClearAttempt(attempt);
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool HasRepositoryInstallInputs(const TileXRCcuDirectInstallOptions& options)
{
    return options.driverAdapter != nullptr &&
        options.repositoryMemoryOps.alloc != nullptr &&
        options.repositoryMemoryOps.copyHostToDevice != nullptr &&
        options.repositoryMemoryOps.free != nullptr;
}

bool SyncXnMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXn ||
        mode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly;
}

bool HasNonZeroArgs(const TileXRCcuTaskWindow& window)
{
    for (uint64_t arg : window.args) {
        if (arg != 0) {
            return true;
        }
    }
    return false;
}

bool ContainsRange(uint16_t outerStart, uint16_t outerCount, uint16_t innerStart, uint32_t innerCount)
{
    if (outerCount == 0 || innerCount == 0) {
        return false;
    }
    const uint32_t outerBegin = outerStart;
    const uint32_t outerEnd = outerBegin + outerCount;
    const uint32_t innerBegin = innerStart;
    const uint32_t innerEnd = innerBegin + innerCount;
    return innerBegin >= outerBegin && innerEnd <= outerEnd;
}

bool RangesOverlap(uint16_t firstStart, uint32_t firstCount, uint16_t secondStart, uint32_t secondCount)
{
    const uint32_t firstEnd = static_cast<uint32_t>(firstStart) + firstCount;
    const uint32_t secondEnd = static_cast<uint32_t>(secondStart) + secondCount;
    return static_cast<uint32_t>(firstStart) < secondEnd && static_cast<uint32_t>(secondStart) < firstEnd;
}

TileXRCcuRange MakeRange(uint8_t dieId, uint16_t startId, uint16_t count)
{
    TileXRCcuRange range;
    range.dieId = dieId;
    range.startId = startId;
    range.num = count;
    return range;
}

uint64_t PackCcuSqeToken(uint32_t tokenId, uint32_t tokenValue, bool valid)
{
    const uint64_t validBits = valid ? 1ULL : 0ULL;
    return (validBits << TILEXR_CCU_PACKED_TOKEN_VALID_SHIFT) |
        ((static_cast<uint64_t>(tokenId) & TILEXR_CCU_PACKED_TOKEN_ID_MASK) << TILEXR_CCU_PACKED_TOKEN_ID_SHIFT) |
        (static_cast<uint64_t>(tokenValue) & TILEXR_CCU_PACKED_TOKEN_VALUE_MASK);
}

uint16_t ReadLe16(const uint8_t* raw, uint32_t offset)
{
    return static_cast<uint16_t>(raw[offset]) |
        static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1U]) << 8U);
}

uint64_t DecodeChannelRemoteCcuVa(const TileXRCcuChannelCtxDataV1& ctx)
{
    const uint16_t word28 = ReadLe16(ctx.raw, 28);
    const uint16_t word34 = ReadLe16(ctx.raw, 34);
    const uint64_t dstVa =
        ((static_cast<uint64_t>(word28) >> 8U) & 0xffULL) |
        (static_cast<uint64_t>(ReadLe16(ctx.raw, 30)) << 8U) |
        (static_cast<uint64_t>(ReadLe16(ctx.raw, 32)) << 24U) |
        ((static_cast<uint64_t>(word34) & 0x1ULL) << 40U);
    return dstVa << TILEXR_CCU_REMOTE_CCU_VA_SHIFT;
}

int PopulateHcommStyleSqeTaskArgs(
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr ||
        attempt->plan.taskWindows.size() < 2 ||
        !SyncXnMode(attempt->plan.barrierMode)) {
        return TILEXR_SUCCESS;
    }

    TileXRCcuTaskWindow& sqeLoadTask = attempt->plan.taskWindows[0];
    if (sqeLoadTask.argSize != TILEXR_CCU_SQE_ARGS_LEN ||
        sqeLoadTask.instCnt == 0 ||
        attempt->specInfo.resourceAddr == 0) {
        return TILEXR_SUCCESS;
    }
    if (HasNonZeroArgs(sqeLoadTask)) {
        return TILEXR_SUCCESS;
    }

    sqeLoadTask.args.assign(TILEXR_CCU_SQE_ARGS_LEN, 0);
    sqeLoadTask.args[0] = attempt->specInfo.resourceAddr;
    sqeLoadTask.args[1] = attempt->specInfo.resourceAddr;

    if (!attempt->preparedLowerLayerPlan.msidTokens.empty()) {
        const auto& token = attempt->preparedLowerLayerPlan.msidTokens[0];
        sqeLoadTask.args[2] = PackCcuSqeToken(token.tokenId, token.tokenValue, true);
    }
    for (const auto& channel : attempt->preparedLowerLayerPlan.channels) {
        const uint64_t remoteCcuVa = DecodeChannelRemoteCcuVa(channel.ctx);
        if (remoteCcuVa != 0) {
            sqeLoadTask.args[3] = remoteCcuVa;
            break;
        }
    }

    if (report != nullptr) {
        report->message.clear();
    }
    return TILEXR_SUCCESS;
}

bool DirectTraceEnabled()
{
    const char* value = std::getenv("TILEXR_CCU_DIRECT_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

uint16_t TraceSlot(uint64_t word, uint32_t slot)
{
    return static_cast<uint16_t>((word >> (slot * 16U)) & 0xffffU);
}

uint16_t TraceRead16(const uint8_t* raw, uint32_t offset)
{
    return ReadLe16(raw, offset);
}

uint64_t TraceReadDoorbellVa(const TileXRCcuLocalJettyCtxData& ctx)
{
    uint64_t value = 0;
    for (uint32_t word = 0; word < 4U; ++word) {
        value |= static_cast<uint64_t>(TraceRead16(ctx.raw, word * 2U)) << (word * 16U);
    }
    return value;
}

uint64_t TraceLoadImmediate(const TileXRCcuInstr& instr)
{
    return (instr.words[0] >> 32U) | ((instr.words[1] & 0xffffffffULL) << 32U);
}

void TraceDecodedPfeCtx(size_t index, const TileXRCcuPfeInstall& pfe)
{
    const uint16_t word = TraceRead16(pfe.ctx.raw, 2);
    const uint16_t jettyCountMinusOne = word & 0x7fU;
    std::cerr << "TileXRDirectCcuTrace lowerLayerPfe[" << index << "]"
              << " decoded=PfeCtx"
              << " dieId=" << static_cast<uint32_t>(pfe.dieId)
              << " pfeOffset=" << pfe.pfeOffset
              << " startTaJettyId=" << TraceRead16(pfe.ctx.raw, 0)
              << " jettyCount=" << static_cast<uint32_t>(jettyCountMinusOne) + 1U
              << " jettyCountMinusOne=" << jettyCountMinusOne
              << " startLocalJettyCtxId=" << ((word >> 7U) & 0x7fU)
              << "\n";
}

void TraceDecodedLocalJettyCtx(size_t jettyIndex, size_t ctxIndex, const TileXRCcuLocalJettyCtxData& ctx)
{
    const uint16_t word8 = TraceRead16(ctx.raw, 8);
    const uint16_t word10 = TraceRead16(ctx.raw, 10);
    const uint16_t word14 = TraceRead16(ctx.raw, 14);
    const uint16_t word22 = TraceRead16(ctx.raw, 22);
    const uint16_t word24 = TraceRead16(ctx.raw, 24);
    const uint32_t tokenId =
        ((word8 >> 8U) & 0xffU) |
        ((static_cast<uint32_t>(word10) & 0xfffU) << 8U);
    const uint32_t tokenValue =
        ((word10 >> 12U) & 0xfU) |
        (static_cast<uint32_t>(TraceRead16(ctx.raw, 12)) << 4U) |
        ((static_cast<uint32_t>(word14) & 0xfffU) << 20U);
    const uint32_t wqeBasicBlockShift = (word14 >> 12U) & 0xfU;
    const uint32_t wqeBasicBlockCount = 1U << wqeBasicBlockShift;
    const uint32_t wqeBasicBlockStartId =
        ((word22 >> 12U) & 0xfU) |
        ((static_cast<uint32_t>(word24) & 0xffU) << 4U);

    std::cerr << "TileXRDirectCcuTrace lowerLayerJettyCtx[" << jettyIndex << "," << ctxIndex << "]"
              << " decoded=LocalJettyCtx"
              << " doorbellVa=" << std::hex << std::showbase << TraceReadDoorbellVa(ctx)
              << " doorbellTokenId=" << tokenId
              << " doorbellTokenValue=" << tokenValue
              << std::dec << std::noshowbase
              << " pfeId=" << (word8 & 0xfU)
              << " ioDieId=" << ((word8 >> 4U) & 0x1U)
              << " doorbellAddrType=" << ((word8 >> 5U) & 0x1U)
              << " tokenValueValid=" << ((word8 >> 6U) & 0x1U)
              << " sqeBasicBlockLeftShifts=" << wqeBasicBlockShift
              << " wqeBasicBlockCount=" << wqeBasicBlockCount
              << " inferredSqDepth=" << (wqeBasicBlockCount / 4U)
              << " wqeBasicBlockStartId=" << wqeBasicBlockStartId
              << " pi=" << TraceRead16(ctx.raw, 16)
              << " ci=" << TraceRead16(ctx.raw, 18)
              << " maxCi=" << TraceRead16(ctx.raw, 20)
              << " oooCqeCnt=" << (word22 & 0xfffU)
              << " doorbellSendState=" << ((word24 >> 8U) & 0x3U)
              << "\n";
}

void TraceDecodedChannelCtxV1(size_t index, const TileXRCcuChannelInstall& channel)
{
    const uint16_t word16 = TraceRead16(channel.ctx.raw, 16);
    const uint16_t word18 = TraceRead16(channel.ctx.raw, 18);
    const uint16_t word20 = TraceRead16(channel.ctx.raw, 20);
    const uint16_t word22 = TraceRead16(channel.ctx.raw, 22);
    const uint16_t word24 = TraceRead16(channel.ctx.raw, 24);
    const uint16_t word28 = TraceRead16(channel.ctx.raw, 28);
    const uint16_t word34 = TraceRead16(channel.ctx.raw, 34);

    const uint32_t tpn = word16 | ((static_cast<uint32_t>(word18) & 0xffU) << 16U);
    const uint16_t startJettyId =
        ((word18 >> 12U) & 0xfU) |
        static_cast<uint16_t>((word20 & 0xfffU) << 4U);
    const uint16_t jettyCountMinusOne =
        ((word20 >> 12U) & 0xfU) |
        static_cast<uint16_t>((word22 & 0x7U) << 4U);
    const uint32_t tokenId =
        ((static_cast<uint32_t>(word22) >> 4U) & 0xfffU) |
        ((static_cast<uint32_t>(word24) & 0xffU) << 12U);
    const uint32_t tokenValue =
        ((static_cast<uint32_t>(word24) >> 8U) & 0xffU) |
        (static_cast<uint32_t>(TraceRead16(channel.ctx.raw, 26)) << 8U) |
        ((static_cast<uint32_t>(word28) & 0xffU) << 24U);
    const uint64_t dstVa =
        ((static_cast<uint64_t>(word28) >> 8U) & 0xffULL) |
        (static_cast<uint64_t>(TraceRead16(channel.ctx.raw, 30)) << 8U) |
        (static_cast<uint64_t>(TraceRead16(channel.ctx.raw, 32)) << 24U) |
        ((static_cast<uint64_t>(word34) & 0x1ULL) << 40U);
    const uint64_t remoteCcuVa = dstVa << TILEXR_CCU_REMOTE_CCU_VA_SHIFT;

    std::cerr << "TileXRDirectCcuTrace lowerLayerChannel[" << index << "]"
              << " decoded=ChannelCtxV1"
              << " dieId=" << static_cast<uint32_t>(channel.dieId)
              << " channelId=" << channel.channelId
              << " tpn=" << std::hex << std::showbase << tpn
              << " memoryTokenId=" << tokenId
              << " memoryTokenValue=" << tokenValue
              << " dstVaShifted=" << dstVa
              << " remoteCcuVa=" << remoteCcuVa
              << std::dec << std::noshowbase
              << " sourcePfeId=" << ((word18 >> 8U) & 0xfU)
              << " startTaJettyId=" << startJettyId
              << " jettyCount=" << static_cast<uint32_t>(jettyCountMinusOne) + 1U
              << " jettyCountMinusOne=" << jettyCountMinusOne
              << " ioDieId=" << ((word22 >> 3U) & 0x1U)
              << " tokenValueValid=" << ((word34 >> 1U) & 0x1U)
              << " remoteEid=";
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        std::cerr << std::hex << std::setw(2) << std::setfill('0') << std::noshowbase
                  << static_cast<uint32_t>(channel.ctx.raw[i]);
    }
    std::cerr << std::dec << std::setfill(' ') << "\n";
}

void TraceDecodedInstr(const char* label, size_t index, const TileXRCcuInstr& instr)
{
    const uint16_t opcode = TraceSlot(instr.words[0], 0);
    std::cerr << "TileXRDirectCcuTrace " << label << "[" << index << "] ";
    switch (opcode) {
        case TILEXR_CCU_TRACE_LOAD_SQE_ARGS_TO_X_HEADER:
            std::cerr << "decoded=LoadSqeArgsToX"
                      << " xnId=" << TraceSlot(instr.words[0], 1)
                      << " sqeArgId=" << TraceSlot(instr.words[0], 2);
            break;
        case TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER:
            std::cerr << "decoded=LoadImdToXn"
                      << " xnId=" << TraceSlot(instr.words[0], 1)
                      << " immediate=" << std::hex << std::showbase << TraceLoadImmediate(instr)
                      << std::dec << std::noshowbase
                      << " secFlag=" << TraceSlot(instr.words[1], 2);
            break;
        case TILEXR_CCU_TRACE_LOAD_IMD_TO_GSA_HEADER:
            std::cerr << "decoded=LoadImdToGSA"
                      << " gsaId=" << TraceSlot(instr.words[0], 1)
                      << " immediate=" << std::hex << std::showbase << TraceLoadImmediate(instr)
                      << std::dec << std::noshowbase;
            break;
        case TILEXR_CCU_TRACE_TRANS_RMT_MEM_TO_LOC_MEM_HEADER: {
            const uint16_t control = TraceSlot(instr.words[1], 3);
            const uint16_t flags = TraceSlot(instr.words[2], 3);
            std::cerr << "decoded=TransRmtMemToLocMem"
                      << " remoteGsa=" << TraceSlot(instr.words[0], 3)
                      << " remoteXn=" << TraceSlot(instr.words[1], 0)
                      << " localGsa=" << TraceSlot(instr.words[0], 1)
                      << " localXn=" << TraceSlot(instr.words[0], 2)
                      << " lengthXn=" << TraceSlot(instr.words[1], 1)
                      << " channelId=" << TraceSlot(instr.words[1], 2)
                      << " clearType=" << (flags & 0x1U)
                      << " lengthEn=" << ((flags >> 1U) & 0x1U)
                      << " reduceEn=" << ((flags >> 2U) & 0x1U)
                      << " reduceDataType=" << ((control >> 8U) & 0xfU)
                      << " reduceOpCode=" << ((control >> 12U) & 0xfU)
                      << " setCkeId=" << TraceSlot(instr.words[3], 0)
                      << " setCkeMask=" << TraceSlot(instr.words[3], 1)
                      << " waitCkeId=" << TraceSlot(instr.words[3], 2)
                      << " waitCkeMask=" << TraceSlot(instr.words[3], 3);
            break;
        }
        case TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_RMT_MEM_HEADER: {
            const uint16_t control = TraceSlot(instr.words[1], 3);
            const uint16_t flags = TraceSlot(instr.words[2], 3);
            std::cerr << "decoded=TransLocMemToRmtMem"
                      << " localGsa=" << TraceSlot(instr.words[0], 3)
                      << " localXn=" << TraceSlot(instr.words[1], 0)
                      << " remoteGsa=" << TraceSlot(instr.words[0], 1)
                      << " remoteXn=" << TraceSlot(instr.words[0], 2)
                      << " lengthXn=" << TraceSlot(instr.words[1], 1)
                      << " channelId=" << TraceSlot(instr.words[1], 2)
                      << " clearType=" << (flags & 0x1U)
                      << " lengthEn=" << ((flags >> 1U) & 0x1U)
                      << " reduceEn=" << ((flags >> 2U) & 0x1U)
                      << " reduceDataType=" << ((control >> 8U) & 0xfU)
                      << " reduceOpCode=" << ((control >> 12U) & 0xfU)
                      << " setCkeId=" << TraceSlot(instr.words[3], 0)
                      << " setCkeMask=" << TraceSlot(instr.words[3], 1)
                      << " waitCkeId=" << TraceSlot(instr.words[3], 2)
                      << " waitCkeMask=" << TraceSlot(instr.words[3], 3);
            break;
        }
        case TILEXR_CCU_TRACE_SYNC_XN_HEADER:
            std::cerr << "decoded=SyncXn"
                      << " remoteXn=" << TraceSlot(instr.words[0], 1)
                      << " localXn=" << TraceSlot(instr.words[0], 2)
                      << " channelId=" << TraceSlot(instr.words[1], 0)
                      << " notifyCke=" << TraceSlot(instr.words[1], 1)
                      << " notifyMask=" << TraceSlot(instr.words[1], 2)
                      << " traceFlag=" << std::hex << std::showbase << instr.words[2]
                      << std::dec << std::noshowbase
                      << " setCkeId=" << TraceSlot(instr.words[3], 0)
                      << " setCkeMask=" << TraceSlot(instr.words[3], 1)
                      << " waitCkeId=" << TraceSlot(instr.words[3], 2)
                      << " waitCkeMask=" << TraceSlot(instr.words[3], 3);
            break;
        case TILEXR_CCU_TRACE_SYNC_CKE_HEADER:
            std::cerr << "decoded=SyncCke"
                      << " remoteCke=" << TraceSlot(instr.words[0], 1)
                      << " localCke=" << TraceSlot(instr.words[0], 2)
                      << " localCkeMask=" << TraceSlot(instr.words[0], 3)
                      << " channelId=" << TraceSlot(instr.words[1], 0)
                      << " clearType=" << TraceSlot(instr.words[2], 3)
                      << " setCkeId=" << TraceSlot(instr.words[3], 0)
                      << " setCkeMask=" << TraceSlot(instr.words[3], 1)
                      << " waitCkeId=" << TraceSlot(instr.words[3], 2)
                      << " waitCkeMask=" << TraceSlot(instr.words[3], 3);
            break;
        case TILEXR_CCU_TRACE_SET_CKE_HEADER:
            std::cerr << "decoded=SetCke"
                      << " clearType=" << TraceSlot(instr.words[0], 1)
                      << " ckeId=" << TraceSlot(instr.words[0], 2)
                      << " mask=" << TraceSlot(instr.words[0], 3)
                      << " waitCkeId=" << TraceSlot(instr.words[1], 0)
                      << " waitMask=" << TraceSlot(instr.words[1], 1);
            break;
        case TILEXR_CCU_TRACE_CLEAR_CKE_HEADER:
            std::cerr << "decoded=ClearCke"
                      << " clearType=" << TraceSlot(instr.words[0], 1)
                      << " ckeId=" << TraceSlot(instr.words[0], 2)
                      << " mask=" << TraceSlot(instr.words[0], 3)
                      << " waitCkeId=" << TraceSlot(instr.words[1], 0)
                      << " waitMask=" << TraceSlot(instr.words[1], 1);
            break;
        default:
            std::cerr << "decoded=Unknown opcode=" << std::hex << std::showbase << opcode
                      << std::dec << std::noshowbase;
            break;
    }
    std::cerr << "\n";
}

void TraceInstr(const char* label, size_t index, const TileXRCcuInstr& instr)
{
    std::cerr << "TileXRDirectCcuTrace " << label << "[" << index << "] words="
              << std::hex << std::showbase
              << instr.words[0] << "," << instr.words[1] << ","
              << instr.words[2] << "," << instr.words[3]
              << std::dec << std::noshowbase << "\n";
    TraceDecodedInstr(label, index, instr);
}

void TraceDirectInstallAttempt(const TileXRCcuDirectInstallAttempt& attempt)
{
    if (!DirectTraceEnabled()) {
        return;
    }

    std::cerr << "TileXRDirectCcuTrace begin"
              << " missionId=" << static_cast<uint32_t>(attempt.plan.mission.missionId)
              << " missionKey=" << std::hex << std::showbase << attempt.plan.mission.key
              << std::dec << std::noshowbase
              << " dieId=" << static_cast<uint32_t>(attempt.plan.mission.dieId)
              << "\n";
    for (size_t i = 0; i < attempt.plan.syncResources.size(); ++i) {
        const auto& resource = attempt.plan.syncResources[i];
        std::cerr << "TileXRDirectCcuTrace syncResource[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(resource.dieId)
                  << " channelId=" << resource.channelId
                  << " localXn=" << resource.localXn
                  << " remoteXn=" << resource.remoteXn
                  << " notifyCke=" << resource.notifyCke
                  << " localWaitCke=" << resource.localWaitCke
                  << " localWaitMask=" << resource.localWaitMask
                  << " remoteNotifyMask=" << resource.remoteNotifyMask
                  << " sourceCke=" << resource.sourceCke
                  << " sourceCkeMask=" << resource.sourceCkeMask
                  << "\n";
    }
    for (size_t i = 0; i < attempt.preparedLowerLayerPlan.remoteXnBindings.size(); ++i) {
        const auto& proof = attempt.preparedLowerLayerPlan.remoteXnBindings[i];
        std::cerr << "TileXRDirectCcuTrace remoteXnBinding[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(proof.dieId)
                  << " channelId=" << proof.channelId
                  << " localXn=" << proof.localXn
                  << " remoteXn=" << proof.remoteXn
                  << " notifyCke=" << proof.notifyCke
                  << " localWaitCke=" << proof.localWaitCke
                  << " peerRank=" << proof.peerRank
                  << " peerExchangeObserved=" << (proof.peerExchangeObserved ? 1 : 0)
                  << " endpointRouteVerified=" << (proof.endpointRouteVerified ? 1 : 0)
                  << " channelResourceOwnerVerified=" << (proof.channelResourceOwnerVerified ? 1 : 0)
                  << " transportResourceExchangeVerified=" << (proof.transportResourceExchangeVerified ? 1 : 0)
                  << "\n";
    }
    for (size_t i = 0; i < attempt.preparedLowerLayerPlan.pfes.size(); ++i) {
        const auto& pfe = attempt.preparedLowerLayerPlan.pfes[i];
        std::cerr << "TileXRDirectCcuTrace lowerLayerPfe[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(pfe.dieId)
                  << " pfeOffset=" << pfe.pfeOffset
                  << " ctx=";
        for (uint8_t byte : pfe.ctx.raw) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<uint32_t>(byte);
        }
        std::cerr << std::dec << std::setfill(' ') << "\n";
        TraceDecodedPfeCtx(i, pfe);
    }
    for (size_t i = 0; i < attempt.preparedLowerLayerPlan.channels.size(); ++i) {
        const auto& channel = attempt.preparedLowerLayerPlan.channels[i];
        std::cerr << "TileXRDirectCcuTrace lowerLayerChannel[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(channel.dieId)
                  << " channelId=" << channel.channelId
                  << " ctx=";
        for (uint8_t byte : channel.ctx.raw) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<uint32_t>(byte);
        }
        std::cerr << std::dec << std::setfill(' ') << "\n";
        TraceDecodedChannelCtxV1(i, channel);
    }
    for (size_t i = 0; i < attempt.preparedLowerLayerPlan.jettys.size(); ++i) {
        const auto& jetty = attempt.preparedLowerLayerPlan.jettys[i];
        std::cerr << "TileXRDirectCcuTrace lowerLayerJetty[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(jetty.dieId)
                  << " startJettyCtxId=" << jetty.startJettyCtxId
                  << " ctxCount=" << jetty.ctxs.size()
                  << "\n";
        for (size_t j = 0; j < jetty.ctxs.size(); ++j) {
            std::cerr << "TileXRDirectCcuTrace lowerLayerJettyCtx[" << i << "," << j << "] ctx=";
            for (uint8_t byte : jetty.ctxs[j].raw) {
                std::cerr << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<uint32_t>(byte);
            }
            std::cerr << std::dec << std::setfill(' ') << "\n";
            TraceDecodedLocalJettyCtx(i, j, jetty.ctxs[j]);
        }
    }
    for (size_t i = 0; i < attempt.plan.taskWindows.size(); ++i) {
        const auto& window = attempt.plan.taskWindows[i];
        std::cerr << "TileXRDirectCcuTrace taskWindow[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(window.dieId)
                  << " instStartId=" << window.instStartId
                  << " instCnt=" << window.instCnt
                  << " argSize=" << window.argSize
                  << " args=";
        for (size_t arg = 0; arg < window.args.size(); ++arg) {
            if (arg != 0) {
                std::cerr << ",";
            }
            std::cerr << std::hex << std::showbase << window.args[arg]
                      << std::dec << std::noshowbase;
        }
        std::cerr << "\n";
    }
    const auto& tracedTasks = attempt.submitTasks.empty() ? attempt.package.tasks : attempt.submitTasks;
    for (size_t i = 0; i < tracedTasks.size(); ++i) {
        const auto& task = tracedTasks[i];
        std::cerr << "TileXRDirectCcuTrace task[" << i << "]"
                  << " dieId=" << static_cast<uint32_t>(task.dieId)
                  << " missionId=" << static_cast<uint32_t>(task.missionId)
                  << " timeout=" << task.timeout
                  << " instStartId=" << task.instStartId
                  << " instCnt=" << task.instCnt
                  << " argSize=" << task.argSize
                  << " key=" << std::hex << std::showbase << task.key
                  << std::dec << std::noshowbase
                  << " args=";
        for (uint32_t arg = 0; arg < TILEXR_CCU_SQE_ARGS_LEN; ++arg) {
            if (arg != 0) {
                std::cerr << ",";
            }
            std::cerr << std::hex << std::showbase << task.args[arg]
                      << std::dec << std::noshowbase;
        }
        std::cerr << "\n";
    }
    for (size_t i = 0; i < attempt.package.program.sqeLoad.size(); ++i) {
        TraceInstr("program.sqeLoad", i, attempt.package.program.sqeLoad[i]);
    }
    for (size_t i = 0; i < attempt.package.program.sync.size(); ++i) {
        TraceInstr("program.sync", i, attempt.package.program.sync[i]);
    }
    std::cerr << "TileXRDirectCcuTrace end\n";
}

void ApplySplitCkeOptions(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuResourceSpec* resourceSpec)
{
    if (resourceSpec == nullptr) {
        return;
    }
    if (options.localWaitCkeStartId != 0 || options.localWaitCkeCount != 0) {
        resourceSpec->localWaitCkeStartId =
            options.localWaitCkeStartId == 0 ? options.ckeStartId : options.localWaitCkeStartId;
        resourceSpec->localWaitCkeCount =
            options.localWaitCkeCount == 0 ? resourceSpec->ckeCount : options.localWaitCkeCount;
    }
    if (options.remoteNotifyCkeStartId != 0 || options.remoteNotifyCkeCount != 0) {
        resourceSpec->remoteNotifyCkeStartId =
            options.remoteNotifyCkeStartId == 0 ? options.ckeStartId : options.remoteNotifyCkeStartId;
        resourceSpec->remoteNotifyCkeCount =
            options.remoteNotifyCkeCount == 0 ? resourceSpec->ckeCount : options.remoteNotifyCkeCount;
    }
}

void ApplyRemoteXnOptions(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuResourceSpec* resourceSpec)
{
    if (resourceSpec == nullptr) {
        return;
    }
    if (options.remoteXnStartId != 0 || options.remoteXnCount != 0) {
        resourceSpec->remoteXnStartId = options.remoteXnStartId;
        resourceSpec->remoteXnCount =
            options.remoteXnCount == 0 ? resourceSpec->xnCount : options.remoteXnCount;
    }
}

int PrepareLowerLayerPlanIfNeeded(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (options.prepareLowerLayerPlan == nullptr) {
        return TILEXR_SUCCESS;
    }
    if (attempt == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuLowerLayerInstallPlan plan;
    TileXRCcuLowerLayerPlanBuilderReport planReport;
    const int ret = options.prepareLowerLayerPlan(
        attempt->allocation,
        &plan,
        &planReport,
        options.lowerLayerPlanUserData);
    attempt->lowerLayerPlanReport = planReport;
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = planReport.message.empty() ?
                "failed to prepare direct CCU lower-layer install plan" :
                planReport.message;
        }
        return ret;
    }
    attempt->preparedLowerLayerPlan = plan;
    return TILEXR_SUCCESS;
}

int ReconcileProducerPlanWithLowerLayerProof(
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr || attempt->preparedLowerLayerPlan.remoteXnBindings.empty()) {
        return TILEXR_SUCCESS;
    }
    if (attempt->preparedLowerLayerPlan.remoteXnBindings.size() != attempt->plan.syncResources.size()) {
        if (report != nullptr) {
            report->message = "lower-layer remote XN proof count does not match producer sync resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    for (auto& resource : attempt->plan.syncResources) {
        bool matched = false;
        for (const auto& proof : attempt->preparedLowerLayerPlan.remoteXnBindings) {
            if (proof.dieId != resource.dieId ||
                proof.channelId != resource.channelId ||
                proof.localXn != resource.localXn ||
                !proof.peerExchangeObserved) {
                continue;
            }
            resource.remoteXn = proof.remoteXn;
            resource.notifyCke = proof.notifyCke;
            if (proof.localWaitCke != 0) {
                resource.localWaitCke = proof.localWaitCke;
            }
            matched = true;
            break;
        }
        if (!matched) {
            if (report != nullptr) {
                report->message = "lower-layer remote XN proof does not cover producer sync resource";
            }
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

int ConfigureDirectMemoryCopyResources(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr || attempt->plan.syncResources.size() != 1 || attempt->plan.taskWindows.size() != 1) {
        if (report != nullptr) {
            report->message = "memory copy direct CCU plan requires one sync resource and one task";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (options.gsaStartId == 0 || attempt->resourceSpec.gsaCount < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT) {
        if (report != nullptr) {
            report->message = "memory copy direct CCU requires a kernel-local GSA resource window";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint8_t dieId = attempt->specInfo.dieId;
    const uint16_t localXnStart = attempt->allocation.localXn.startId;
    if (!ContainsRange(
            attempt->resourceSpec.xnStartId,
            attempt->resourceSpec.xnCount,
            localXnStart,
            TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT)) {
        if (report != nullptr) {
            report->message = "memory copy direct CCU local XN window is too small";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    uint16_t remoteXnStart = attempt->allocation.remoteXn.startId;
    if (RangesOverlap(
            localXnStart,
            TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT,
            remoteXnStart,
            1U)) {
        remoteXnStart = static_cast<uint16_t>(localXnStart + TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT);
    }
    if (!ContainsRange(attempt->resourceSpec.xnStartId, attempt->resourceSpec.xnCount, remoteXnStart, 1U)) {
        if (report != nullptr) {
            report->message = "memory copy direct CCU remote XN window is outside the XN resource range";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    attempt->allocation.localXn =
        MakeRange(dieId, localXnStart, static_cast<uint16_t>(TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT));
    attempt->allocation.localGsa =
        MakeRange(dieId, options.gsaStartId, static_cast<uint16_t>(TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT));
    attempt->allocation.remoteXn = MakeRange(dieId, remoteXnStart, 1U);
    attempt->plan.kernelLocalXn = attempt->allocation.localXn;
    attempt->plan.kernelLocalGsa = attempt->allocation.localGsa;
    attempt->plan.syncResources[0].remoteXn = remoteXnStart;
    attempt->plan.taskWindows[0].instCnt =
        static_cast<uint16_t>(std::max<uint32_t>(
            attempt->plan.taskWindows[0].instCnt,
            TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT));
    return TILEXR_SUCCESS;
}

int BuildDirectMemoryCopyLaunchPackage(
    const TileXRCcuDirectMemoryCopySpec& memoryCopy,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr || attempt->plan.syncResources.empty()) {
        if (report != nullptr) {
            report->message = "missing direct CCU memory copy producer resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const TileXRCcuSyncResource& resource = attempt->plan.syncResources[0];
    if (attempt->plan.kernelLocalGsa.num < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT ||
        attempt->plan.kernelLocalXn.num < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT) {
        if (report != nullptr) {
            report->message = "missing direct CCU memory copy GSA/XN resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuMemoryCopySpec copySpec;
    copySpec.direction = memoryCopy.direction;
    copySpec.localGsa = attempt->plan.kernelLocalGsa.startId;
    copySpec.remoteGsa = static_cast<uint16_t>(attempt->plan.kernelLocalGsa.startId + 1U);
    copySpec.localXn = attempt->plan.kernelLocalXn.startId;
    copySpec.remoteXn = static_cast<uint16_t>(attempt->plan.kernelLocalXn.startId + 1U);
    copySpec.lengthXn = static_cast<uint16_t>(attempt->plan.kernelLocalXn.startId + 2U);
    copySpec.localAddr = memoryCopy.localAddr;
    copySpec.localToken = memoryCopy.localToken;
    copySpec.remoteAddr = memoryCopy.remoteAddr;
    copySpec.remoteToken = memoryCopy.remoteToken;
    copySpec.lengthBytes = memoryCopy.lengthBytes;
    copySpec.channelId = resource.channelId;
    copySpec.completionCke = resource.localWaitCke == 0 ? resource.notifyCke : resource.localWaitCke;
    copySpec.completionMask = resource.localWaitMask == 0 ? 1U : resource.localWaitMask;

    TileXRCcuProgram program;
    TileXRCcuMemoryProgramReport memoryReport;
    if (TileXRCcuBuildMemoryCopyProgram(copySpec, &program.sync, &memoryReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = memoryReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuRepositoryImage repository;
    TileXRCcuRepositoryReport repositoryReport;
    if (TileXRCcuBuildRepositoryImage(attempt->plan, program, &repository, &repositoryReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = repositoryReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    std::vector<TileXRCcuTask> tasks;
    TileXRCcuProducerPlanReport planReport;
    if (TileXRCcuBuildTasks(attempt->plan, &tasks, &planReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = planReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    attempt->package.plan = attempt->plan;
    attempt->package.program = program;
    attempt->package.repository = repository;
    attempt->package.tasks = tasks;
    attempt->package.installScope = TileXRCcuLaunchInstallScope {};
    attempt->package.requiresHardwareInstall = true;
    return TILEXR_SUCCESS;
}

void FillReportFromAttempt(const TileXRCcuDirectInstallAttempt& attempt, TileXRCcuDirectInstallReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->pipelineBuilt = true;
    report->installAttempted = attempt.installReport.installAttempted;
    report->installSucceeded = attempt.installReport.installSucceeded;
    report->submitReady = attempt.providerReport.submitReady;
    report->requiredInstallSurfaceCount = attempt.installReport.requiredInstallSurfaceCount;
    report->publicVerifiedInstallSurfaceCount = attempt.installReport.publicVerifiedInstallSurfaceCount;
    report->missingInstallSurfaceCount = attempt.installReport.missingInstallSurfaceCount;
    report->taskCount = static_cast<uint32_t>(attempt.package.tasks.size());
    report->submitTaskCount = static_cast<uint32_t>(attempt.submitTasks.size());
}

void ResetSubmitReport(TileXRCcuDirectSubmitReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuDirectSubmitReport{};
    }
}

void TraceFinalRuntimeTask(size_t taskIndex, const TileXRCcuTask& task)
{
    if (!DirectTraceEnabled()) {
        return;
    }
    std::cerr << "TileXRDirectCcuTrace finalRuntimeTask[" << taskIndex << "]"
              << " dieId=" << static_cast<uint32_t>(task.dieId)
              << " missionId=" << static_cast<uint32_t>(task.missionId)
              << " timeout=" << task.timeout
              << " instStartId=" << task.instStartId
              << " instCnt=" << task.instCnt
              << " key=0x" << std::hex << std::nouppercase << task.key
              << std::dec
              << " argSize=" << task.argSize;
    for (uint32_t arg = 0; arg < TILEXR_CCU_SQE_ARGS_LEN; ++arg) {
        std::cerr << " args[" << arg << "]=0x"
                  << std::hex << std::nouppercase << task.args[arg]
                  << std::dec;
    }
    std::cerr << "\n";
}

std::string FormatSubmitTaskFailure(
    size_t taskIndex,
    int ret,
    const TileXRCcuTask& task,
    const TileXRCcuRuntimeSubmitReport* runtimeReport)
{
    const TileXRCcuTask& diagnosticTask =
        (runtimeReport != nullptr && runtimeReport->finalTaskCaptured) ?
            runtimeReport->finalTask :
            task;
    std::ostringstream oss;
    oss << "direct CCU submit failed task=" << taskIndex
        << " ret=" << ret;
    if (runtimeReport != nullptr && runtimeReport->runtimeLaunchAttempted) {
        oss << " rtRet=" << runtimeReport->runtimeRet;
    }
    oss << " dieId=" << static_cast<uint32_t>(diagnosticTask.dieId)
        << " missionId=" << static_cast<uint32_t>(diagnosticTask.missionId)
        << " timeout=" << diagnosticTask.timeout
        << " instStartId=" << diagnosticTask.instStartId
        << " instCnt=" << diagnosticTask.instCnt
        << " key=0x" << std::hex << std::nouppercase << diagnosticTask.key
        << std::dec
        << " argSize=" << diagnosticTask.argSize;
    for (uint32_t arg = 0; arg < TILEXR_CCU_SQE_ARGS_LEN; ++arg) {
        oss << " args[" << arg << "]=0x"
            << std::hex << std::nouppercase << diagnosticTask.args[arg]
            << std::dec;
    }
    return oss.str();
}

int ReturnWithAttemptStatus(
    int ret,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt != nullptr) {
        FillReportFromAttempt(*attempt, report);
        if (report != nullptr) {
            if (attempt->providerReport.submitReady) {
                report->message = attempt->providerReport.message;
            } else if (!attempt->installReport.message.empty()) {
                report->message = attempt->installReport.message;
            } else {
                report->message = attempt->providerReport.message.empty() ?
                    TILEXR_CCU_DIRECT_KNOWN_MISSING_INSTALL_SURFACES :
                    attempt->providerReport.message;
            }
        }
    }
    return ret;
}

} // namespace

int TileXRCcuSubmitPreparedTasks(
    const std::vector<TileXRCcuTask>& submitTasks,
    void* stream,
    TileXRCcuTaskSubmitFn submitFn,
    void* submitUserData,
    TileXRCcuDirectSubmitReport* report)
{
    ResetSubmitReport(report);
    if (submitTasks.empty()) {
        if (report != nullptr) {
            report->message = "missing prepared direct CCU submit tasks";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (stream == nullptr) {
        if (report != nullptr) {
            report->taskCount = static_cast<uint32_t>(submitTasks.size());
            report->message = "missing runtime stream for direct CCU submit";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    uint32_t submitted = 0;
    for (size_t i = 0; i < submitTasks.size(); ++i) {
        TileXRCcuTask taskForSubmit = submitTasks[i];

        TileXRCcuRuntimeSubmitReport runtimeReport;
        const bool useDefaultSubmit = submitFn == nullptr;
        const int ret = useDefaultSubmit ?
            TileXRCcuSubmitTaskWithReport(taskForSubmit, stream, &runtimeReport) :
            submitFn(taskForSubmit, stream, submitUserData);
        if (useDefaultSubmit && runtimeReport.finalTaskCaptured) {
            TraceFinalRuntimeTask(i, runtimeReport.finalTask);
        }
        if (ret != TILEXR_SUCCESS) {
            if (report != nullptr) {
                report->taskCount = static_cast<uint32_t>(submitTasks.size());
                report->submittedTaskCount = submitted;
                report->message = FormatSubmitTaskFailure(
                    i,
                    ret,
                    taskForSubmit,
                    useDefaultSubmit ? &runtimeReport : nullptr);
            }
            return ret;
        }
        ++submitted;
    }

    if (report != nullptr) {
        report->submitted = true;
        report->taskCount = static_cast<uint32_t>(submitTasks.size());
        report->submittedTaskCount = submitted;
        report->message = "direct CCU prepared tasks submitted";
    }
    return TILEXR_SUCCESS;
}

int RunDirectInstallAttemptImpl(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectMemoryCopySpec* memoryCopy,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    ResetReport(report);
    if (attempt == nullptr) {
        return Fail(nullptr, report, "missing output direct CCU install attempt");
    }
    ClearAttempt(attempt);

    if (options.basicInfo == nullptr) {
        return Fail(attempt, report, "missing direct CCU basic info");
    }
    if (options.provider.empty()) {
        return Fail(attempt, report, "missing direct CCU install provider");
    }
    if (!options.offlineOnly && !HasRepositoryInstallInputs(options)) {
        return Fail(attempt, report, "missing direct CCU repository install inputs");
    }
    attempt->repositoryMemoryOps = options.repositoryMemoryOps;
    attempt->repositoryMemoryUserData = options.repositoryMemoryUserData;

    TileXRCcuSpecsReport specsReport;
    int ret = TileXRCcuDecodeBasicInfo(*options.basicInfo, &attempt->specInfo, &specsReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(attempt, report, specsReport.message);
    }

    ret = TileXRCcuBuildResourceSpec(
        attempt->specInfo,
        options.missionStartId,
        options.instructionStartId,
        options.xnStartId,
        options.ckeStartId,
        options.channelStartId,
        &attempt->resourceSpec,
        &specsReport,
        options.gsaStartId);
    if (ret != TILEXR_SUCCESS) {
        return Fail(attempt, report, specsReport.message);
    }
    attempt->resourceSpec.missionInstructionStartId = options.missionInstructionStartId;
    ApplyRemoteXnOptions(options, &attempt->resourceSpec);
    ApplySplitCkeOptions(options, &attempt->resourceSpec);

    attempt->resourceRequest.sqeArgCount = memoryCopy == nullptr ? options.sqeArgCount : 0U;
    attempt->resourceRequest.syncResourceCount = memoryCopy == nullptr ? options.syncResourceCount : 1U;
    attempt->resourceRequest.syncInstructionCount = memoryCopy == nullptr ?
        options.syncInstructionCount :
        std::max<uint32_t>(options.syncInstructionCount, TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT);
    attempt->resourceRequest.bindingsPerSyncResource = options.bindingsPerSyncResource;
    attempt->resourceRequest.barrierMode = options.barrierMode;

    TileXRCcuResourceAllocator allocator;
    if (allocator.Init(attempt->resourceSpec) != TILEXR_SUCCESS) {
        return Fail(attempt, report, "failed to initialize direct CCU resource allocator");
    }

    TileXRCcuResourceAllocatorReport allocatorReport;
    ret = allocator.Allocate(
        attempt->resourceRequest,
        &attempt->plan,
        &attempt->allocation,
        &allocatorReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(attempt, report, allocatorReport.message);
    }

    if (memoryCopy != nullptr) {
        ret = ConfigureDirectMemoryCopyResources(options, attempt, report);
        if (ret != TILEXR_SUCCESS) {
            return Fail(
                attempt,
                report,
                report == nullptr || report->message.empty() ?
                    "failed to configure direct CCU memory copy resources" :
                    report->message);
        }
    }

    ret = PrepareLowerLayerPlanIfNeeded(options, attempt, report);
    if (ret != TILEXR_SUCCESS) {
        return Fail(
            attempt,
            report,
            report == nullptr || report->message.empty() ?
                "failed to prepare direct CCU lower-layer install plan" :
                report->message);
    }

    ret = ReconcileProducerPlanWithLowerLayerProof(attempt, report);
    if (ret != TILEXR_SUCCESS) {
        return Fail(
            attempt,
            report,
            report == nullptr || report->message.empty() ?
                "failed to reconcile direct CCU lower-layer peer resources" :
                report->message);
    }
    if (memoryCopy == nullptr) {
        ret = PopulateHcommStyleSqeTaskArgs(attempt, report);
        if (ret != TILEXR_SUCCESS) {
            return Fail(attempt, report, "failed to populate direct CCU SQE task arguments");
        }
    }

    TileXRCcuLaunchPackageReport packageReport;
    ret = memoryCopy == nullptr ?
        TileXRCcuBuildLaunchPackage(attempt->plan, &attempt->package, &packageReport) :
        BuildDirectMemoryCopyLaunchPackage(*memoryCopy, attempt, report);
    if (ret != TILEXR_SUCCESS) {
        return Fail(
            attempt,
            report,
            memoryCopy == nullptr ? packageReport.message :
                (report == nullptr || report->message.empty() ?
                    "failed to build direct CCU memory copy launch package" :
                    report->message));
    }

    TraceDirectInstallAttempt(*attempt);

    ret = TileXRCcuBindLaunchPackageInstallScope(
        &attempt->package,
        options.deviceId,
        options.rank,
        options.provider);
    if (ret != TILEXR_SUCCESS) {
        return Fail(attempt, report, "failed to bind direct CCU launch install scope");
    }

    TileXRCcuInstallManifestReport manifestReport;
    ret = TileXRCcuBuildInstallManifest(attempt->package, &attempt->manifest, &manifestReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(attempt, report, manifestReport.message);
    }

    TileXRCcuInstallRequest installRequest;
    installRequest.package = &attempt->package;
    installRequest.manifest = &attempt->manifest;
    installRequest.deviceId = options.deviceId;
    installRequest.rank = options.rank;
    installRequest.provider = options.provider;
    installRequest.offlineOnly = options.offlineOnly;
    installRequest.driverAdapter = options.driverAdapter;
    installRequest.repositoryMemoryOps = options.repositoryMemoryOps;
    installRequest.repositoryMemoryUserData = options.repositoryMemoryUserData;
    installRequest.repositoryInstallOptions = options.repositoryInstallOptions;
    installRequest.repositoryReceipt = &attempt->repositoryReceipt;
    installRequest.installOrder = options.installOrder;
    installRequest.lowerLayerPlan =
        options.prepareLowerLayerPlan == nullptr ? options.lowerLayerPlan : &attempt->preparedLowerLayerPlan;

    const int installRet = TileXRCcuInstallHardware(
        installRequest,
        &attempt->evidence,
        &attempt->installReport);

    const int submitRet = TileXRCcuPrepareSubmitTasks(
        attempt->package,
        attempt->evidence,
        &attempt->submitTasks,
        &attempt->providerReport);
    if (submitRet == TILEXR_SUCCESS) {
        return ReturnWithAttemptStatus(TILEXR_SUCCESS, attempt, report);
    }
    if (installRet != TILEXR_SUCCESS) {
        return ReturnWithAttemptStatus(installRet, attempt, report);
    }
    return ReturnWithAttemptStatus(submitRet, attempt, report);
}

int TileXRCcuRunDirectInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    return RunDirectInstallAttemptImpl(options, nullptr, attempt, report);
}

int TileXRCcuRunDirectMemoryCopyInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectMemoryCopySpec& memoryCopy,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (memoryCopy.localAddr == 0 || memoryCopy.localToken == 0 ||
        memoryCopy.remoteAddr == 0 || memoryCopy.remoteToken == 0 ||
        memoryCopy.lengthBytes == 0) {
        ResetReport(report);
        ClearAttempt(attempt);
        if (report != nullptr) {
            report->message = "invalid direct CCU memory copy address/token inputs";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return RunDirectInstallAttemptImpl(options, &memoryCopy, attempt, report);
}

int TileXRCcuReleaseDirectInstallAttemptResources(TileXRCcuDirectInstallAttempt& attempt)
{
    if (attempt.repositoryReceipt.deviceInstructionPtr == nullptr) {
        attempt.repositoryReceipt = TileXRCcuRepositoryInstallReceipt{};
        attempt.repositoryReleaseReport = TileXRCcuRepositoryReport{};
        attempt.repositoryReleaseReport.message = "ok";
        return TILEXR_SUCCESS;
    }
    return TileXRCcuReleaseRepositoryInstallReceipt(
        attempt.repositoryReceipt,
        attempt.repositoryMemoryOps,
        attempt.repositoryMemoryUserData,
        &attempt.repositoryReleaseReport);
}

} // namespace TileXR
