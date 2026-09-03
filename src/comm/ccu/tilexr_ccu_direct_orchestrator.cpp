/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_direct_orchestrator.h"

#include "ccu/tilexr_ccu_alltoall_program.h"
#include "ccu/tilexr_ccu_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
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
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MEM_HEADER = 0x100aU;
constexpr uint16_t TILEXR_CCU_TRACE_SYNC_CKE_HEADER = 0x100bU;
constexpr uint16_t TILEXR_CCU_TRACE_SYNC_XN_HEADER = 0x100dU;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_VALID_SHIFT = 52ULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_ID_SHIFT = 32ULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_ID_MASK = 0xfffffULL;
constexpr uint64_t TILEXR_CCU_PACKED_TOKEN_VALUE_MASK = 0xffffffffULL;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT = 7U;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT = 3U;
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT = 2U;
constexpr uint32_t TILEXR_CCU_DIRECT_ALLTOALL_SYNC_RESOURCE_COUNT = 3U;
constexpr uint32_t TILEXR_CCU_DIRECT_ALLTOALL_MAX_RANK_SIZE = 64U;
constexpr uint32_t TILEXR_CCU_DIRECT_ALLTOALL_CKE_MASK_BITS = 16U;
constexpr uint32_t TILEXR_CCU_DIRECT_ALLTOALL_XN_BINDINGS_PER_CHANNEL = 3U;

uint32_t DirectAllToAllMeshPeerCount(uint32_t rankSize)
{
    return rankSize >= 2U && rankSize <= TILEXR_CCU_DIRECT_ALLTOALL_MAX_RANK_SIZE ? rankSize - 1U : 0U;
}

uint32_t DirectAllToAllMeshCompletionCkeCount(uint32_t rankSize)
{
    const uint32_t peers = DirectAllToAllMeshPeerCount(rankSize);
    return peers == 0 ? 0U : (peers + TILEXR_CCU_DIRECT_ALLTOALL_CKE_MASK_BITS - 1U) /
        TILEXR_CCU_DIRECT_ALLTOALL_CKE_MASK_BITS;
}
constexpr uint32_t TILEXR_CCU_DIRECT_SIGNAL_INSTRUCTION_COUNT = 5U;
constexpr uint32_t TILEXR_CCU_DIRECT_WAIT_INSTRUCTION_COUNT = 5U;
constexpr uint32_t TILEXR_CCU_DIRECT_SIGNAL_WAIT_INSTRUCTION_COUNT = 6U;
constexpr uint32_t TILEXR_CCU_DIRECT_SYNC_XN_PING_INSTRUCTION_COUNT = 2U;

uint32_t DirectAllToAll2RankInstructionCapacity(uint64_t bytes)
{
    if (bytes == 0 || bytes % TILEXR_CCU_ALLTOALL_BLOCK_BYTES != 0) {
        return 0;
    }
    const uint64_t blocks = bytes / TILEXR_CCU_ALLTOALL_BLOCK_BYTES;
    const uint64_t instructions = 7ULL + blocks * 7ULL;
    return instructions > std::numeric_limits<uint32_t>::max() ?
        0U : static_cast<uint32_t>(instructions);
}

uint32_t SyncXnPingAllocationInstructionCount(uint32_t syncResourceCount)
{
    if (syncResourceCount > std::numeric_limits<uint32_t>::max() / 2U) {
        return std::numeric_limits<uint32_t>::max();
    }
    return syncResourceCount * 2U;
}

uint32_t DirectAllToAllMeshInstructionCount(uint32_t rankSize, uint64_t chunkBytes)
{
    const uint64_t peers = DirectAllToAllMeshPeerCount(rankSize);
    const uint64_t completionCkes = DirectAllToAllMeshCompletionCkeCount(rankSize);
    if (peers == 0 || chunkBytes == 0 || chunkBytes % TILEXR_CCU_ALLTOALL_BLOCK_BYTES != 0) {
        return 0;
    }
    const uint64_t blocks = chunkBytes / TILEXR_CCU_ALLTOALL_BLOCK_BYTES;
    const uint64_t preSync = 3ULL + peers * 3ULL;
    const uint64_t perBlock = peers * 6ULL + 9ULL + completionCkes;
    const uint64_t postSync = peers * 2ULL;
    const uint64_t instructions = preSync + blocks * perBlock + postSync + 1ULL;
    return instructions > std::numeric_limits<uint32_t>::max() ? 0U : static_cast<uint32_t>(instructions);
}

bool DirectAllToAllMeshCapacityFits(
    const TileXRCcuResourceSpec& resources,
    uint32_t rankSize,
    uint32_t instructionCount,
    std::string* message)
{
    const auto require = [message](const char* resource, uint32_t requested, uint32_t available) {
        if (requested <= available) {
            return true;
        }
        if (message != nullptr) {
            std::ostringstream stream;
            stream << "insufficient alltoall mesh " << resource
                   << " resources requested=" << requested
                   << " available=" << available;
            *message = stream.str();
        }
        return false;
    };
    const uint32_t missionInstructionStart = resources.missionInstructionStartId == 0 ?
        resources.instructionStartId : resources.missionInstructionStartId;
    const uint32_t repositoryPrefix = missionInstructionStart - resources.instructionStartId;
    const uint32_t localWaitCkeCount = resources.localWaitCkeCount == 0 ?
        resources.ckeCount : resources.localWaitCkeCount;
    const uint32_t remoteNotifyCkeCount = resources.remoteNotifyCkeCount == 0 ?
        resources.ckeCount : resources.remoteNotifyCkeCount;
    const uint32_t peers = DirectAllToAllMeshPeerCount(rankSize);
    const uint32_t completionCkes = DirectAllToAllMeshCompletionCkeCount(rankSize);
    const uint32_t localXns = std::max<uint32_t>(peers, TILEXR_CCU_DIRECT_ALLTOALL_XN_BINDINGS_PER_CHANNEL);
    const uint32_t remoteXns = TILEXR_CCU_DIRECT_ALLTOALL_XN_BINDINGS_PER_CHANNEL;
    return peers != 0U && completionCkes != 0U && require("mission", 1U, resources.missionCount) &&
        require("instruction", repositoryPrefix + instructionCount, resources.instructionCount) &&
        require("GSA", TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT, resources.gsaCount) &&
        require("local XN", localXns, resources.xnCount) &&
        require("remote XN", remoteXns,
            resources.remoteXnCount == 0 ?
                (resources.xnCount > localXns ? resources.xnCount - localXns : 0U) :
                resources.remoteXnCount) &&
        require("local CKE", peers + 1U + completionCkes,
            localWaitCkeCount) &&
        require("remote CKE", peers,
            remoteNotifyCkeCount) &&
        require("channel", peers, resources.channelCount);
}

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
        case TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MEM_HEADER: {
            const uint16_t control = TraceSlot(instr.words[1], 3);
            const uint16_t flags = TraceSlot(instr.words[2], 3);
            std::cerr << "decoded=TransLocMemToLocMem"
                      << " sourceGsa=" << TraceSlot(instr.words[0], 3)
                      << " sourceXn=" << TraceSlot(instr.words[1], 0)
                      << " destinationGsa=" << TraceSlot(instr.words[0], 1)
                      << " destinationXn=" << TraceSlot(instr.words[0], 2)
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

int ConfigureDirectAllToAll2RankResources(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectAllToAll2RankSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    const uint32_t instructionCapacity = DirectAllToAll2RankInstructionCapacity(alltoall.bytes);
    if (attempt == nullptr || instructionCapacity == 0 ||
        attempt->plan.syncResources.size() != TILEXR_CCU_DIRECT_ALLTOALL_SYNC_RESOURCE_COUNT ||
        attempt->plan.taskWindows.size() != 1) {
        if (report != nullptr) {
            report->message = "alltoall direct CCU plan requires three sync resources and one task";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (options.gsaStartId == 0 || attempt->resourceSpec.gsaCount < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT) {
        if (report != nullptr) {
            report->message = "alltoall direct CCU requires a kernel-local GSA resource window";
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
            report->message = "alltoall direct CCU local XN window is too small";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    attempt->allocation.localXn =
        MakeRange(dieId, localXnStart, static_cast<uint16_t>(TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT));
    attempt->allocation.localGsa =
        MakeRange(dieId, options.gsaStartId, static_cast<uint16_t>(TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT));
    attempt->plan.kernelLocalXn = attempt->allocation.localXn;
    attempt->plan.kernelLocalGsa = attempt->allocation.localGsa;
    attempt->plan.taskWindows[0].instCnt =
        static_cast<uint16_t>(std::max<uint32_t>(
            attempt->plan.taskWindows[0].instCnt,
            instructionCapacity));
    return TILEXR_SUCCESS;
}

int ConfigureDirectAllToAllMeshResources(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectAllToAllMeshSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    const uint32_t peerCount = DirectAllToAllMeshPeerCount(alltoall.rankSize);
    const uint32_t completionCkeCount = DirectAllToAllMeshCompletionCkeCount(alltoall.rankSize);
    const uint32_t instructionCount = DirectAllToAllMeshInstructionCount(alltoall.rankSize, alltoall.chunkBytes);
    if (attempt == nullptr || instructionCount == 0 ||
        attempt->plan.syncResources.size() != peerCount ||
        attempt->plan.taskWindows.size() != 1U) {
        if (report != nullptr) {
            report->message = "alltoall mesh direct CCU plan requires rankSize-1 peer resources and one task";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (options.gsaStartId == 0 || attempt->resourceSpec.gsaCount < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT) {
        if (report != nullptr) {
            report->message = "alltoall mesh direct CCU requires two kernel-local GSA resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (attempt->allocation.localXn.num < peerCount || attempt->allocation.remoteXn.num < peerCount ||
        attempt->allocation.localWaitCke.num < peerCount || attempt->allocation.remoteNotifyCke.num < peerCount ||
        attempt->allocation.channels.num < peerCount ||
        attempt->allocation.sourceCke.num < 1U + completionCkeCount) {
        if (report != nullptr) {
            report->message = "alltoall mesh direct CCU allocation is missing XN/CKE/channel resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    attempt->allocation.localGsa = MakeRange(
        attempt->specInfo.dieId,
        options.gsaStartId,
        static_cast<uint16_t>(TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT));
    attempt->plan.kernelLocalGsa = attempt->allocation.localGsa;
    attempt->plan.taskWindows[0].instCnt = static_cast<uint16_t>(instructionCount);
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
    if (DirectTraceEnabled()) {
        const auto tracePackedToken = [](uint64_t token, const char* label) {
            std::cerr << " " << label << "=0x" << std::hex << token
                      << " " << label << "Valid=" << ((token >> TILEXR_CCU_PACKED_TOKEN_VALID_SHIFT) & 0x1ULL)
                      << " " << label << "Id=0x"
                      << ((token >> TILEXR_CCU_PACKED_TOKEN_ID_SHIFT) & TILEXR_CCU_PACKED_TOKEN_ID_MASK)
                      << " " << label << "Value=0x"
                      << (token & TILEXR_CCU_PACKED_TOKEN_VALUE_MASK)
                      << std::dec;
        };
        std::cerr << "TileXRDirectCcuTrace memoryCopySpec"
                  << " direction=" << static_cast<int>(copySpec.direction)
                  << " localGsa=" << copySpec.localGsa
                  << " localXn=" << copySpec.localXn
                  << " remoteGsa=" << copySpec.remoteGsa
                  << " remoteXn=" << copySpec.remoteXn
                  << " lengthXn=" << copySpec.lengthXn
                  << " channelId=" << copySpec.channelId
                  << " completionCke=" << copySpec.completionCke
                  << " completionMask=" << copySpec.completionMask
                  << " localAddr=0x" << std::hex << copySpec.localAddr
                  << " remoteAddr=0x" << copySpec.remoteAddr
                  << " lengthBytes=0x" << copySpec.lengthBytes
                  << std::dec;
        tracePackedToken(copySpec.localToken, "localToken");
        tracePackedToken(copySpec.remoteToken, "remoteToken");
        std::cerr << std::endl;
    }

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

int BuildDirectAllToAll2RankLaunchPackage(
    const TileXRCcuDirectAllToAll2RankSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr ||
        attempt->plan.syncResources.size() != TILEXR_CCU_DIRECT_ALLTOALL_SYNC_RESOURCE_COUNT) {
        if (report != nullptr) {
            report->message = "missing direct CCU alltoall producer resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (attempt->plan.kernelLocalGsa.num < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT ||
        attempt->plan.kernelLocalXn.num < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_XN_COUNT) {
        if (report != nullptr) {
            report->message = "missing direct CCU alltoall GSA/XN resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const TileXRCcuSyncResource& copyResource = attempt->plan.syncResources[0];
    const TileXRCcuSyncResource& preResource = attempt->plan.syncResources[1];
    const TileXRCcuSyncResource& postResource = attempt->plan.syncResources[2];
    const bool preSyncOnCopyRoute =
        std::getenv("TILEXR_CCU_DIRECT_ALLTOALL_PRE_SYNC_ON_COPY_ROUTE") != nullptr;
    const bool preSyncPeerLocalXn =
        std::getenv("TILEXR_CCU_DIRECT_ALLTOALL_PRE_SYNC_PEER_LOCAL_XN") != nullptr;
    const uint16_t preSyncRemoteAddrXn =
        preSyncPeerLocalXn ? preResource.localXn : preResource.remoteXn;

    TileXRCcuAllToAll2RankProgramSpec alltoallSpec;
    alltoallSpec.localRank = alltoall.localRank;
    alltoallSpec.localSendAddr = alltoall.localSendAddr;
    alltoallSpec.localSendToken = alltoall.localSendToken;
    alltoallSpec.localRecvAddr = alltoall.localRecvAddr;
    alltoallSpec.localRecvToken = alltoall.localRecvToken;
    alltoallSpec.remoteSendAddr = alltoall.remoteSendAddr;
    alltoallSpec.remoteSendToken = alltoall.remoteSendToken;
    alltoallSpec.remoteRecvAddr = alltoall.remoteRecvAddr;
    alltoallSpec.remoteRecvToken = alltoall.remoteRecvToken;
    alltoallSpec.bytes = alltoall.bytes;
    alltoallSpec.memorySliceBytes = alltoall.memorySliceBytes;
    alltoallSpec.memSlicePerBlock = alltoall.memSlicePerBlock;
    alltoallSpec.localGsa = attempt->plan.kernelLocalGsa.startId;
    alltoallSpec.remoteGsa = static_cast<uint16_t>(attempt->plan.kernelLocalGsa.startId + 1U);
    alltoallSpec.localXn = attempt->plan.kernelLocalXn.startId;
    alltoallSpec.remoteXn = static_cast<uint16_t>(attempt->plan.kernelLocalXn.startId + 1U);
    alltoallSpec.lengthXn = static_cast<uint16_t>(attempt->plan.kernelLocalXn.startId + 2U);
    alltoallSpec.preSyncLocalAddrXn =
        preSyncOnCopyRoute ? copyResource.localXn : preResource.localXn;
    alltoallSpec.preSyncLocalTokenXn = postResource.localXn;
    alltoallSpec.preSyncLocalMarkerXn = copyResource.localXn;
    alltoallSpec.preSyncRemoteMarkerXn = copyResource.remoteXn;
    alltoallSpec.preSyncMarkerArgIndex = 0;
    alltoallSpec.preSyncMarkerEnabled = true;
    alltoallSpec.channelId = copyResource.channelId;
    alltoallSpec.preSyncChannelId =
        preSyncOnCopyRoute ? copyResource.channelId : preResource.channelId;
    alltoallSpec.preSyncMarkerChannelId = alltoallSpec.preSyncChannelId;
    alltoallSpec.preSyncTokenChannelId = preResource.channelId;
    alltoallSpec.copyChannelId = copyResource.channelId;
    alltoallSpec.postSyncChannelId = postResource.channelId;
    alltoallSpec.preSyncRemoteAddrXn =
        preSyncOnCopyRoute ? copyResource.localXn : preSyncRemoteAddrXn;
    alltoallSpec.preSyncRemoteTokenXn =
        preSyncPeerLocalXn ? postResource.localXn : postResource.remoteXn;
    alltoallSpec.preSyncRemoteNotifyCke =
        preSyncOnCopyRoute ? attempt->allocation.remoteNotifyCke.startId : preResource.notifyCke;
    alltoallSpec.preSyncLocalWaitCke =
        preSyncOnCopyRoute
            ? (copyResource.localWaitCke == 0 ? copyResource.notifyCke : copyResource.localWaitCke)
            : (preResource.localWaitCke == 0 ? preResource.notifyCke : preResource.localWaitCke);
    alltoallSpec.preSyncRemoteTokenNotifyCke = preResource.notifyCke;
    alltoallSpec.preSyncTokenLocalWaitCke =
        preResource.localWaitCke == 0 ? preResource.notifyCke : preResource.localWaitCke;
    alltoallSpec.copyCompletionCke =
        copyResource.localWaitCke == 0 ? copyResource.notifyCke : copyResource.localWaitCke;
    alltoallSpec.postSyncRemoteNotifyCke = postResource.notifyCke;
    alltoallSpec.postSyncLocalWaitCke =
        postResource.localWaitCke == 0 ? postResource.notifyCke : postResource.localWaitCke;
    alltoallSpec.sourceCke = preResource.sourceCke;
    alltoallSpec.ckeMask = preResource.remoteNotifyMask == 0 ? 1U : preResource.remoteNotifyMask;
    alltoallSpec.preSyncNotify = std::getenv("TILEXR_CCU_DIRECT_ALLTOALL_SKIP_PRE_SYNC") == nullptr;
    alltoallSpec.preSyncWait = std::getenv("TILEXR_CCU_DIRECT_ALLTOALL_SKIP_PRE_SYNC_WAIT") == nullptr;
    alltoallSpec.postSyncNotify = false;
    alltoallSpec.postSyncWait = false;
    alltoallSpec.emitFinish = false;

    if (DirectTraceEnabled()) {
        std::cerr << "TileXRDirectCcuTrace alltoallSpec"
                  << " direction=LocalToRemote"
                  << " localRank=" << alltoallSpec.localRank
                  << " localGsa=" << alltoallSpec.localGsa
                  << " localXn=" << alltoallSpec.localXn
                  << " remoteGsa=" << alltoallSpec.remoteGsa
                  << " remoteXn=" << alltoallSpec.remoteXn
                  << " lengthXn=" << alltoallSpec.lengthXn
                  << " preLocalAddrXn=" << alltoallSpec.preSyncLocalAddrXn
                  << " preLocalTokenXn=" << alltoallSpec.preSyncLocalTokenXn
                  << " preChannelId=" << alltoallSpec.preSyncChannelId
                  << " preTokenChannelId=" << alltoallSpec.preSyncTokenChannelId
                  << " copyChannelId=" << alltoallSpec.copyChannelId
                  << " postChannelId=" << alltoallSpec.postSyncChannelId
                  << " preNotifyCke=" << alltoallSpec.preSyncRemoteNotifyCke
                  << " preTokenNotifyCke=" << alltoallSpec.preSyncRemoteTokenNotifyCke
                  << " preTokenWaitCke=" << alltoallSpec.preSyncTokenLocalWaitCke
                  << " preRemoteAddrXn=" << alltoallSpec.preSyncRemoteAddrXn
                  << " preRemoteTokenXn=" << alltoallSpec.preSyncRemoteTokenXn
                  << " copyCompletionCke=" << alltoallSpec.copyCompletionCke
                  << " postNotifyCke=" << alltoallSpec.postSyncRemoteNotifyCke
                  << " preSyncNotify=" << (alltoallSpec.preSyncNotify ? 1 : 0)
                  << " preSyncWait=" << (alltoallSpec.preSyncWait ? 1 : 0)
                  << " preSyncOnCopyRoute=" << (preSyncOnCopyRoute ? 1 : 0)
                  << " preSyncPeerLocalXn=" << (preSyncPeerLocalXn ? 1 : 0)
                  << " postSyncNotify=" << (alltoallSpec.postSyncNotify ? 1 : 0)
                  << " postSyncWait=" << (alltoallSpec.postSyncWait ? 1 : 0)
                  << " emitFinish=" << (alltoallSpec.emitFinish ? 1 : 0)
                  << " localSendAddr=0x" << std::hex << alltoallSpec.localSendAddr
                  << " localRecvAddr=0x" << alltoallSpec.localRecvAddr
                  << " remoteSendAddr=0x" << alltoallSpec.remoteSendAddr
                  << " remoteRecvAddr=0x" << alltoallSpec.remoteRecvAddr
                  << " bytes=0x" << alltoallSpec.bytes
                  << std::dec << std::endl;
    }

    TileXRCcuProgram program;
    TileXRCcuAllToAllProgramReport alltoallReport;
    if (TileXRCcuBuildAllToAll2RankProgram(alltoallSpec, &program.sync, &alltoallReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = alltoallReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (program.sync.empty() || program.sync.size() > std::numeric_limits<uint16_t>::max()) {
        if (report != nullptr) {
            report->message = "invalid direct CCU alltoall instruction count";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    attempt->plan.taskWindows[0].instCnt = static_cast<uint16_t>(program.sync.size());

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

int ValidateDirectAllToAllMeshRouteResources(
    const TileXRCcuAllToAllMeshProgramSpec& mesh,
    const TileXRCcuProducerPlan& plan,
    TileXRCcuDirectInstallReport* report)
{
    if (mesh.peers.size() != mesh.rankSize - 1U || plan.syncResources.size() != mesh.peers.size()) {
        if (report != nullptr) {
            report->message = "alltoall mesh route binding validation has an invalid shape";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (size_t ordinal = 0; ordinal < mesh.peers.size(); ++ordinal) {
        const auto& resource = plan.syncResources[ordinal];
        const auto& route = mesh.peers[ordinal].route;
        if (route.preSyncMarkerEnabled ||
            route.preSyncChannelId != resource.channelId ||
            route.preSyncTokenChannelId != resource.channelId ||
            route.copyChannelId != resource.channelId ||
            route.postSyncChannelId != resource.channelId ||
            route.preSyncLocalWaitCke != resource.localWaitCke ||
            route.preSyncTokenLocalWaitCke != resource.localWaitCke ||
            route.postSyncLocalWaitCke != resource.localWaitCke ||
            route.preSyncRemoteNotifyCke != resource.notifyCke ||
            route.preSyncRemoteTokenNotifyCke != resource.notifyCke ||
            route.postSyncRemoteNotifyCke != resource.notifyCke ||
            route.copyCompletionCke !=
                mesh.remoteCompletionCkes[ordinal / TILEXR_CCU_DIRECT_ALLTOALL_CKE_MASK_BITS]) {
            if (report != nullptr) {
                std::ostringstream stream;
                stream << "alltoall mesh route binding mismatch peerRank=" << mesh.peers[ordinal].peerRank
                       << " ordinal=" << ordinal;
                report->message = stream.str();
            }
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

int BuildDirectAllToAllMeshLaunchPackage(
    const TileXRCcuDirectAllToAllMeshSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr ||
        attempt->plan.syncResources.size() != DirectAllToAllMeshPeerCount(alltoall.rankSize) ||
        attempt->plan.kernelLocalGsa.num < TILEXR_CCU_DIRECT_MEMORY_COPY_LOCAL_GSA_COUNT) {
        if (report != nullptr) {
            report->message = "missing direct CCU alltoall mesh producer resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    auto peers = alltoall.peers;
    std::sort(peers.begin(), peers.end(), [](const TileXRCcuDirectAllToAllMeshPeerSpec& lhs,
                                             const TileXRCcuDirectAllToAllMeshPeerSpec& rhs) {
        return lhs.peerRank < rhs.peerRank;
    });
    TileXRCcuAllToAllMeshProgramSpec mesh;
    mesh.rankSize = alltoall.rankSize;
    mesh.localRank = alltoall.localRank;
    mesh.localSendAddr = alltoall.localSendAddr;
    mesh.localSendToken = alltoall.localSendToken;
    mesh.localRecvAddr = alltoall.localRecvAddr;
    mesh.localRecvToken = alltoall.localRecvToken;
    mesh.chunkBytes = alltoall.chunkBytes;
    mesh.selfSourceGsa = attempt->plan.kernelLocalGsa.startId;
    mesh.selfDestinationGsa = static_cast<uint16_t>(attempt->plan.kernelLocalGsa.startId + 1U);

    const uint16_t localXnStart = attempt->allocation.localXn.startId;
    const uint16_t remoteXnStart = attempt->allocation.remoteXn.startId;
    const uint32_t completionCkeCount = DirectAllToAllMeshCompletionCkeCount(alltoall.rankSize);
    for (uint32_t group = 0; group < completionCkeCount; ++group) {
        mesh.remoteCompletionCkes.push_back(
            static_cast<uint16_t>(attempt->allocation.sourceCke.startId + 1U + group));
    }
    for (uint32_t ordinal = 0; ordinal < peers.size(); ++ordinal) {
        const TileXRCcuSyncResource& resource = attempt->plan.syncResources[ordinal];
        TileXRCcuAllToAllMeshPeerSpec peer;
        peer.peerRank = peers[ordinal].peerRank;
        auto& route = peer.route;
        route.localRank = alltoall.localRank;
        route.localSendAddr = alltoall.localSendAddr;
        route.localSendToken = alltoall.localSendToken;
        route.localRecvAddr = alltoall.localRecvAddr;
        route.localRecvToken = alltoall.localRecvToken;
        route.remoteRecvAddr = peers[ordinal].remoteRecvAddr;
        route.remoteRecvToken = peers[ordinal].remoteRecvToken;
        route.bytes = alltoall.chunkBytes;
        route.localGsa = attempt->plan.kernelLocalGsa.startId;
        route.remoteGsa = static_cast<uint16_t>(attempt->plan.kernelLocalGsa.startId + 1U);
        route.localXn = localXnStart;
        route.remoteXn = static_cast<uint16_t>(remoteXnStart + 2U);
        route.lengthXn = static_cast<uint16_t>(localXnStart + 2U);
        route.preSyncLocalAddrXn = localXnStart;
        route.preSyncLocalTokenXn = static_cast<uint16_t>(localXnStart + 1U);
        route.preSyncRemoteAddrXn = remoteXnStart;
        route.preSyncRemoteTokenXn = static_cast<uint16_t>(remoteXnStart + 1U);
        route.preSyncMarkerEnabled = false;
        route.preSyncChannelId = resource.channelId;
        route.preSyncTokenChannelId = resource.channelId;
        route.copyChannelId = resource.channelId;
        route.postSyncChannelId = resource.channelId;
        route.copyCompletionCke =
            mesh.remoteCompletionCkes[ordinal / TILEXR_CCU_DIRECT_ALLTOALL_CKE_MASK_BITS];
        route.preSyncLocalWaitCke = resource.localWaitCke;
        route.preSyncRemoteNotifyCke = resource.notifyCke;
        route.preSyncTokenLocalWaitCke = resource.localWaitCke;
        route.preSyncRemoteTokenNotifyCke = resource.notifyCke;
        route.postSyncLocalWaitCke = resource.localWaitCke;
        route.postSyncRemoteNotifyCke = resource.notifyCke;
        route.sourceCke = attempt->allocation.sourceCke.startId;
        route.ckeMask = static_cast<uint16_t>(1U << TILEXR_CCU_ALLTOALL_POST_SYNC_ID);
        route.preSyncNotify = true;
        route.preSyncWait = true;
        route.postSyncNotify = true;
        route.postSyncWait = true;
        route.emitFinish = false;
        mesh.peers.push_back(peer);
    }
    mesh.selfSourceXn = localXnStart;
    mesh.selfDestinationXn = static_cast<uint16_t>(localXnStart + 1U);
    mesh.selfLengthXn = static_cast<uint16_t>(localXnStart + 2U);
    mesh.selfChannelId = 0;
    mesh.selfCompletionCke = attempt->plan.syncResources[0].localWaitCke;

    if (ValidateDirectAllToAllMeshRouteResources(mesh, attempt->plan, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuProgram program;
    TileXRCcuAllToAllProgramReport alltoallReport;
    if (TileXRCcuBuildAllToAllMeshProgram(mesh, &program.sync, &alltoallReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = alltoallReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (program.sync.empty() || program.sync.size() > std::numeric_limits<uint16_t>::max()) {
        if (report != nullptr) {
            report->message = "invalid direct CCU alltoall mesh instruction count";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    attempt->plan.taskWindows[0].instCnt = static_cast<uint16_t>(program.sync.size());

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

uint32_t SignalWaitInstructionCount(TileXRCcuSignalWaitProgramRole role)
{
    if (role == TileXRCcuSignalWaitProgramRole::Wait) {
        return TILEXR_CCU_DIRECT_WAIT_INSTRUCTION_COUNT;
    }
    if (role == TileXRCcuSignalWaitProgramRole::Signal) {
        return TILEXR_CCU_DIRECT_SIGNAL_INSTRUCTION_COUNT;
    }
    return TILEXR_CCU_DIRECT_SIGNAL_WAIT_INSTRUCTION_COUNT;
}

TileXRCcuBarrierMode SignalWaitBarrierMode(TileXRCcuSignalWaitProgramRole role)
{
    if (role == TileXRCcuSignalWaitProgramRole::Wait) {
        return TileXRCcuBarrierMode::SyncCkePostOnly;
    }
    return role == TileXRCcuSignalWaitProgramRole::Signal ?
        TileXRCcuBarrierMode::SyncCkePostOnly :
        TileXRCcuBarrierMode::SyncCke;
}

TileXRCcuBarrierMode EffectiveSignalWaitBarrierMode(const TileXRCcuDirectSignalWaitSpec& signalWait)
{
    return signalWait.overrideBarrierMode ? signalWait.barrierMode : SignalWaitBarrierMode(signalWait.role);
}

uint32_t BarrierInstructionCount(TileXRCcuBarrierMode mode)
{
    switch (mode) {
        case TileXRCcuBarrierMode::SyncCke:
        case TileXRCcuBarrierMode::SyncCkeSetWait:
            return 3U;
        case TileXRCcuBarrierMode::SyncXnLoadPostOnly:
            return 2U;
        case TileXRCcuBarrierMode::SyncXn:
        case TileXRCcuBarrierMode::LocalCke:
            return 2U;
        case TileXRCcuBarrierMode::SyncXnPostOnly:
        case TileXRCcuBarrierMode::SyncCkePostOnly:
        case TileXRCcuBarrierMode::LocalCkePostOnly:
            return 1U;
        default:
            return 2U;
    }
}

uint32_t SignalWaitInstructionCount(const TileXRCcuDirectSignalWaitSpec& signalWait)
{
    if (signalWait.overrideBarrierMode) {
        return BarrierInstructionCount(signalWait.barrierMode);
    }
    return SignalWaitInstructionCount(signalWait.role);
}

int BuildDirectSignalWaitLaunchPackage(
    const TileXRCcuDirectSignalWaitSpec& signalWait,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr || attempt->plan.syncResources.empty()) {
        if (report != nullptr) {
            report->message = "missing direct CCU signal/wait producer resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const TileXRCcuSyncResource& resource = attempt->plan.syncResources[0];

    TileXRCcuSignalWaitProgramSpec spec;
    spec.role = signalWait.role;
    spec.channelId = resource.channelId;
    spec.remoteXn = resource.remoteXn;
    spec.localXn = resource.localXn;
    spec.localGsa = attempt->plan.kernelLocalGsa.num == 0 ? 0 : attempt->plan.kernelLocalGsa.startId;
    spec.remoteNotifyCke = resource.notifyCke;
    spec.remoteNotifyMask = resource.remoteNotifyMask == 0 ? 1U : resource.remoteNotifyMask;
    spec.localWaitCke = resource.localWaitCke == 0 ? resource.notifyCke : resource.localWaitCke;
    spec.localWaitMask = resource.localWaitMask == 0 ? 1U : resource.localWaitMask;
    spec.sourceCke = resource.sourceCke;
    spec.sourceCkeMask = spec.remoteNotifyMask;

    TileXRCcuProgram program;
    TileXRCcuBarrierProgramReport signalWaitReport;
    if (signalWait.overrideBarrierMode) {
        TileXRCcuBarrierSyncSpec barrier;
        barrier.channelId = spec.channelId;
        barrier.remoteXn = spec.remoteXn;
        barrier.localXn = spec.localXn;
        barrier.remoteNotifyCke = spec.remoteNotifyCke;
        barrier.remoteNotifyMask = spec.remoteNotifyMask;
        barrier.localWaitCke = spec.localWaitCke;
        barrier.localWaitMask = spec.localWaitMask;
        barrier.sourceCke = spec.sourceCke;
        barrier.sourceCkeMask = spec.sourceCkeMask;
        barrier.clearLocalWait = spec.clearLocalWait;
        std::vector<TileXRCcuBarrierSyncSpec> barriers;
        barriers.push_back(barrier);
        if (TileXRCcuBuildBarrierProgram(
                barriers,
                &program.sync,
                &signalWaitReport,
                signalWait.barrierMode) != TILEXR_SUCCESS) {
            if (report != nullptr) {
                report->message = signalWaitReport.message;
            }
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    } else if (TileXRCcuBuildSignalWaitProgram(spec, &program.sync, &signalWaitReport) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = signalWaitReport.message;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (program.sync.empty() || program.sync.size() > std::numeric_limits<uint16_t>::max()) {
        if (report != nullptr) {
            report->message = "invalid direct CCU signal/wait instruction count";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    attempt->plan.taskWindows[0].instCnt = static_cast<uint16_t>(program.sync.size());

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

int BuildDirectSyncXnPingLaunchPackage(
    const TileXRCcuDirectSyncXnPingSpec& syncXnPing,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (attempt == nullptr || attempt->plan.syncResources.empty() || attempt->plan.taskWindows.size() != 1) {
        if (report != nullptr) {
            report->message = "missing direct CCU SyncXn ping producer resources";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (syncXnPing.localRank > 3U || syncXnPing.peerRank > 3U || syncXnPing.localRank == syncXnPing.peerRank) {
        if (report != nullptr) {
            report->message = "direct CCU SyncXn ping requires distinct rank ids in the range [0, 3]";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const TileXRCcuSyncResource& resource = attempt->plan.syncResources[0];
    const uint16_t defaultRemoteNotifyMask = static_cast<uint16_t>(1U << syncXnPing.localRank);
    const uint16_t remoteNotifyMask =
        syncXnPing.remoteNotifyMask == 0 ? defaultRemoteNotifyMask : syncXnPing.remoteNotifyMask;
    if (resource.localXn == 0 || resource.remoteXn == 0 || resource.channelId == 0 ||
        resource.notifyCke == 0) {
        if (report != nullptr) {
            report->message = "missing direct CCU SyncXn ping XN/CKE/channel resource";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuProgram program;
    program.sync.reserve(TILEXR_CCU_DIRECT_SYNC_XN_PING_INSTRUCTION_COUNT);
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(resource.localXn, syncXnPing.payload, 0, &instr) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = "failed to encode direct CCU SyncXn ping payload load";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    program.sync.push_back(instr);

    TileXRCcuSyncXnSpec notify;
    notify.remoteXn = resource.remoteXn;
    notify.localXn = resource.localXn;
    notify.channelId = resource.channelId;
    notify.notifyCke = resource.notifyCke;
    notify.notifyMask = remoteNotifyMask;
    notify.clearWait = true;
    if (TileXRCcuEncodeSyncXn(notify, &instr) != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = "failed to encode direct CCU SyncXn ping notify";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    program.sync.push_back(instr);
    attempt->plan.taskWindows[0].instCnt = static_cast<uint16_t>(program.sync.size());

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

void ApplyTaskTimeoutOverride(uint16_t taskTimeout, TileXRCcuDirectInstallAttempt* attempt)
{
    if (taskTimeout == 0 || attempt == nullptr) {
        return;
    }
    for (auto& task : attempt->package.tasks) {
        task.timeout = taskTimeout;
    }
    for (auto& task : attempt->submitTasks) {
        task.timeout = taskTimeout;
    }
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
    const TileXRCcuDirectAllToAll2RankSpec* alltoall,
    const TileXRCcuDirectAllToAllMeshSpec* alltoallMesh,
    const TileXRCcuDirectSignalWaitSpec* signalWait,
    const TileXRCcuDirectSyncXnPingSpec* syncXnPing,
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
    if (alltoallMesh != nullptr) {
        std::string capacityMessage;
        if (!DirectAllToAllMeshCapacityFits(
                attempt->resourceSpec,
                alltoallMesh->rankSize,
                DirectAllToAllMeshInstructionCount(alltoallMesh->rankSize, alltoallMesh->chunkBytes),
                &capacityMessage)) {
            return Fail(attempt, report, capacityMessage);
        }
    }

    const bool customProgram = memoryCopy != nullptr || alltoall != nullptr || alltoallMesh != nullptr ||
        signalWait != nullptr || syncXnPing != nullptr;
    attempt->resourceRequest.sqeArgCount = customProgram ? 0U : options.sqeArgCount;
    attempt->resourceRequest.syncResourceCount =
        alltoallMesh != nullptr ? DirectAllToAllMeshPeerCount(alltoallMesh->rankSize) :
        alltoall != nullptr ? TILEXR_CCU_DIRECT_ALLTOALL_SYNC_RESOURCE_COUNT :
        syncXnPing != nullptr ? options.syncResourceCount :
        customProgram ? 1U : options.syncResourceCount;
    attempt->resourceRequest.syncInstructionCount =
        memoryCopy != nullptr ?
        std::max<uint32_t>(options.syncInstructionCount, TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT) :
        alltoall != nullptr ?
        std::max<uint32_t>(
            options.syncInstructionCount,
            DirectAllToAll2RankInstructionCapacity(alltoall->bytes)) :
        alltoallMesh != nullptr ?
        std::max<uint32_t>(options.syncInstructionCount,
            DirectAllToAllMeshInstructionCount(alltoallMesh->rankSize, alltoallMesh->chunkBytes)) :
        signalWait != nullptr ?
        std::max<uint32_t>(options.syncInstructionCount, SignalWaitInstructionCount(*signalWait)) :
        syncXnPing != nullptr ?
        std::max<uint32_t>(
            options.syncInstructionCount,
            SyncXnPingAllocationInstructionCount(options.syncResourceCount)) :
        options.syncInstructionCount;
    attempt->resourceRequest.bindingsPerSyncResource = alltoallMesh != nullptr ?
        1U : options.bindingsPerSyncResource;
    attempt->resourceRequest.minimumLocalXnCount =
        alltoallMesh != nullptr ?
            TILEXR_CCU_DIRECT_ALLTOALL_XN_BINDINGS_PER_CHANNEL : 0U;
    attempt->resourceRequest.minimumRemoteXnCount =
        alltoallMesh != nullptr ?
            TILEXR_CCU_DIRECT_ALLTOALL_XN_BINDINGS_PER_CHANNEL : 0U;
    attempt->resourceRequest.sourceCkeCount = alltoallMesh != nullptr ?
        1U + DirectAllToAllMeshCompletionCkeCount(alltoallMesh->rankSize) : 1U;
    attempt->resourceRequest.barrierMode =
        alltoallMesh != nullptr ? TileXRCcuBarrierMode::SyncCke :
        alltoall != nullptr ? TileXRCcuBarrierMode::SyncXn :
        syncXnPing != nullptr ? TileXRCcuBarrierMode::SyncXn :
        signalWait == nullptr ? options.barrierMode : EffectiveSignalWaitBarrierMode(*signalWait);

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
    } else if (alltoall != nullptr) {
        ret = ConfigureDirectAllToAll2RankResources(options, *alltoall, attempt, report);
        if (ret != TILEXR_SUCCESS) {
            return Fail(
                attempt,
                report,
                report == nullptr || report->message.empty() ?
                    "failed to configure direct CCU alltoall resources" :
                    report->message);
        }
    } else if (alltoallMesh != nullptr) {
        ret = ConfigureDirectAllToAllMeshResources(options, *alltoallMesh, attempt, report);
        if (ret != TILEXR_SUCCESS) {
            return Fail(
                attempt,
                report,
                report == nullptr || report->message.empty() ?
                    "failed to configure direct CCU alltoall mesh resources" :
                    report->message);
        }
    }
    attempt->plan.barrierMode =
        alltoallMesh != nullptr ? TileXRCcuBarrierMode::SyncCke :
        alltoall != nullptr ? TileXRCcuBarrierMode::SyncXn :
        syncXnPing != nullptr ? TileXRCcuBarrierMode::SyncXn :
        signalWait == nullptr ? attempt->plan.barrierMode : EffectiveSignalWaitBarrierMode(*signalWait);

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
    if (!customProgram) {
        ret = PopulateHcommStyleSqeTaskArgs(attempt, report);
        if (ret != TILEXR_SUCCESS) {
            return Fail(attempt, report, "failed to populate direct CCU SQE task arguments");
        }
    }

    TileXRCcuLaunchPackageReport packageReport;
    ret = memoryCopy != nullptr ?
        BuildDirectMemoryCopyLaunchPackage(*memoryCopy, attempt, report) :
        alltoallMesh != nullptr ?
        BuildDirectAllToAllMeshLaunchPackage(*alltoallMesh, attempt, report) :
        alltoall != nullptr ?
        BuildDirectAllToAll2RankLaunchPackage(*alltoall, attempt, report) :
        signalWait != nullptr ?
        BuildDirectSignalWaitLaunchPackage(*signalWait, attempt, report) :
        syncXnPing != nullptr ?
        BuildDirectSyncXnPingLaunchPackage(*syncXnPing, attempt, report) :
        TileXRCcuBuildLaunchPackage(attempt->plan, &attempt->package, &packageReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(
            attempt,
            report,
            !customProgram ? packageReport.message :
                (report == nullptr || report->message.empty() ?
                    (memoryCopy != nullptr ?
                        "failed to build direct CCU memory copy launch package" :
                        alltoallMesh != nullptr ?
                        "failed to build direct CCU alltoall mesh launch package" :
                        alltoall != nullptr ?
                        "failed to build direct CCU alltoall launch package" :
                        syncXnPing != nullptr ?
                        "failed to build direct CCU SyncXn ping launch package" :
                        "failed to build direct CCU signal/wait launch package") :
                    report->message));
    }

    ApplyTaskTimeoutOverride(options.taskTimeout, attempt);
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
    ApplyTaskTimeoutOverride(options.taskTimeout, attempt);
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
    return RunDirectInstallAttemptImpl(options, nullptr, nullptr, nullptr, nullptr, nullptr, attempt, report);
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
    return RunDirectInstallAttemptImpl(options, &memoryCopy, nullptr, nullptr, nullptr, nullptr, attempt, report);
}

int TileXRCcuRunDirectAllToAll2RankInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectAllToAll2RankSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    if (alltoall.localRank > 1U ||
        alltoall.localSendAddr == 0 || alltoall.localSendToken == 0 ||
        alltoall.localRecvAddr == 0 || alltoall.localRecvToken == 0 ||
        alltoall.remoteSendAddr == 0 || alltoall.remoteSendToken == 0 ||
        alltoall.remoteRecvAddr == 0 || alltoall.remoteRecvToken == 0 ||
        alltoall.bytes == 0 || alltoall.memorySliceBytes != TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES ||
        alltoall.memSlicePerBlock == 0 ||
        alltoall.memSlicePerBlock > TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK) {
        ResetReport(report);
        ClearAttempt(attempt);
        if (report != nullptr) {
            report->message = "invalid direct CCU alltoall address/token/slice inputs";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return RunDirectInstallAttemptImpl(options, nullptr, &alltoall, nullptr, nullptr, nullptr, attempt, report);
}

int TileXRCcuRunDirectAllToAllMeshInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectAllToAllMeshSpec& alltoall,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    bool valid = alltoall.rankSize >= 2U && alltoall.rankSize <= TILEXR_CCU_DIRECT_ALLTOALL_MAX_RANK_SIZE &&
        alltoall.localRank < alltoall.rankSize &&
        alltoall.localSendAddr != 0 && alltoall.localSendToken != 0 &&
        alltoall.localRecvAddr != 0 && alltoall.localRecvToken != 0 &&
        DirectAllToAllMeshInstructionCount(alltoall.rankSize, alltoall.chunkBytes) != 0 &&
        alltoall.peers.size() == alltoall.rankSize - 1U;
    if (valid) {
        std::vector<bool> peerRanks(alltoall.rankSize, false);
        for (const auto& peer : alltoall.peers) {
            if (peer.peerRank >= alltoall.rankSize || peer.peerRank == alltoall.localRank ||
                peerRanks[peer.peerRank] || peer.remoteRecvAddr == 0 || peer.remoteRecvToken == 0) {
                valid = false;
                break;
            }
            peerRanks[peer.peerRank] = true;
        }
    }
    if (!valid) {
        ResetReport(report);
        ClearAttempt(attempt);
        if (report != nullptr) {
            report->message = "invalid direct CCU alltoall mesh address/token/rank inputs";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return RunDirectInstallAttemptImpl(options, nullptr, nullptr, &alltoall, nullptr, nullptr, attempt, report);
}

int TileXRCcuRunDirectSignalWaitInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectSignalWaitSpec& signalWait,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    return RunDirectInstallAttemptImpl(options, nullptr, nullptr, nullptr, &signalWait, nullptr, attempt, report);
}

int TileXRCcuRunDirectSyncXnPingInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectSyncXnPingSpec& syncXnPing,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report)
{
    return RunDirectInstallAttemptImpl(options, nullptr, nullptr, nullptr, nullptr, &syncXnPing, attempt, report);
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
