/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_alltoall_program.h"

#include <algorithm>
#include <set>

namespace TileXR {
namespace {

constexpr uint16_t TILEXR_CCU_TRACE_LOAD_SQE_ARGS_TO_X_HEADER = 0x0001U;
constexpr uint16_t TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER = 0x0003U;
constexpr uint16_t TILEXR_CCU_TRACE_SET_CKE_HEADER = 0x0802U;
constexpr uint16_t TILEXR_CCU_TRACE_CLEAR_CKE_HEADER = 0x0804U;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_RMT_MEM_HEADER = 0x1009U;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MEM_HEADER = 0x100aU;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MS_HEADER = 0x1000U;
constexpr uint16_t TILEXR_CCU_TRACE_TRANS_LOC_MS_TO_LOC_MEM_HEADER = 0x1002U;
constexpr uint32_t TILEXR_CCU_ALLTOALL_MAX_RANK_SIZE = 64U;
constexpr uint32_t TILEXR_CCU_CKE_MASK_BITS = 16U;

uint32_t CompletionCkeCount(size_t peerCount)
{
    return static_cast<uint32_t>((peerCount + TILEXR_CCU_CKE_MASK_BITS - 1U) / TILEXR_CCU_CKE_MASK_BITS);
}

uint16_t CompletionMaskForGroup(size_t peerCount, uint32_t group)
{
    const size_t begin = static_cast<size_t>(group) * TILEXR_CCU_CKE_MASK_BITS;
    const size_t remaining = peerCount > begin ? peerCount - begin : 0U;
    const uint32_t bits = static_cast<uint32_t>(std::min<size_t>(remaining, TILEXR_CCU_CKE_MASK_BITS));
    return bits == TILEXR_CCU_CKE_MASK_BITS ? 0xffffU : static_cast<uint16_t>((1U << bits) - 1U);
}

size_t MeshPreSyncInstructionCount(size_t peerCount)
{
    return 3U + peerCount * 3U;
}

size_t MeshCopyInstructionCountPerBlock(size_t peerCount)
{
    return peerCount * 6U + 9U + CompletionCkeCount(peerCount);
}

constexpr uint16_t TILEXR_CCU_TRACE_SYNC_CKE_HEADER = 0x100bU;
constexpr uint16_t TILEXR_CCU_TRACE_SYNC_XN_HEADER = 0x100dU;
constexpr uint16_t TILEXR_CCU_ALLTOALL_SOURCE_CKE_INIT_MASK = 0xffffU;

uint16_t PreSyncSignalMask(const TileXRCcuAllToAll2RankProgramSpec& spec)
{
    (void)spec;
    return static_cast<uint16_t>(1U << TILEXR_CCU_ALLTOALL_OUTPUT_XN_ID);
}

uint16_t PreSyncTokenMask(const TileXRCcuAllToAll2RankProgramSpec& spec)
{
    (void)spec;
    return static_cast<uint16_t>(1U << TILEXR_CCU_ALLTOALL_TOKEN_XN_ID);
}

uint16_t PostSyncSignalMask(const TileXRCcuAllToAll2RankProgramSpec& spec)
{
    return spec.ckeMask;
}

void ResetReport(TileXRCcuAllToAllProgramReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuAllToAllProgramReport{};
    }
}

int Fail(
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report,
    const std::string& message)
{
    if (program != nullptr) {
        program->clear();
    }
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

int ValidateSpec(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    if (program == nullptr) {
        return Fail(program, report, "missing output direct CCU alltoall program");
    }
    if (spec.localRank > 1U) {
        return Fail(program, report, "direct CCU alltoall localRank must be 0 or 1");
    }
    if (spec.localSendAddr == 0 || spec.localRecvAddr == 0 || spec.remoteRecvAddr == 0) {
        return Fail(program, report, "missing direct CCU alltoall address");
    }
    if (spec.localSendToken == 0 || spec.localRecvToken == 0 || spec.remoteRecvToken == 0) {
        return Fail(program, report, "missing direct CCU alltoall token");
    }
    if (spec.bytes == 0 || spec.bytes % TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES != 0) {
        return Fail(program, report, "direct CCU alltoall bytes must be nonzero and 4KB aligned");
    }
    if (spec.memorySliceBytes != TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES) {
        return Fail(program, report, "direct CCU alltoall memorySliceBytes must be 4096");
    }
    if (spec.memSlicePerBlock == 0 || spec.memSlicePerBlock > TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK) {
        return Fail(program, report, "direct CCU alltoall memSlicePerBlock must be in [1, 8]");
    }
    const uint64_t bytesPerBlock = static_cast<uint64_t>(spec.memorySliceBytes) * spec.memSlicePerBlock;
    if (spec.bytes % bytesPerBlock != 0) {
        return Fail(program, report, "direct CCU alltoall bytes must align to memSlicePerBlock");
    }
    if (spec.localGsa == 0 || spec.remoteGsa == 0 || spec.localXn == 0 || spec.remoteXn == 0 ||
        spec.lengthXn == 0) {
        return Fail(program, report, "missing direct CCU alltoall GSA/XN resource");
    }
    const uint16_t preSyncChannelId = spec.preSyncChannelId == 0 ? spec.channelId : spec.preSyncChannelId;
    const uint16_t copyChannelId = spec.copyChannelId == 0 ? spec.channelId : spec.copyChannelId;
    const uint16_t postSyncChannelId = spec.postSyncChannelId == 0 ? spec.channelId : spec.postSyncChannelId;
    if (preSyncChannelId == 0 || copyChannelId == 0 || postSyncChannelId == 0 ||
        spec.copyCompletionCke == 0 || spec.preSyncLocalWaitCke == 0 ||
        spec.preSyncRemoteNotifyCke == 0 || spec.postSyncLocalWaitCke == 0 ||
        spec.postSyncRemoteNotifyCke == 0 ||
        (spec.postSyncNotify && spec.sourceCke == 0) || spec.ckeMask == 0) {
        return Fail(program, report, "missing direct CCU alltoall CKE/channel resource");
    }
    if (spec.preSyncMarkerEnabled &&
        (spec.preSyncLocalMarkerXn == 0 || spec.preSyncRemoteMarkerXn == 0 ||
            spec.preSyncMarkerArgIndex >= TILEXR_CCU_SQE_ARGS_LEN)) {
        return Fail(program, report, "missing direct CCU alltoall loop marker resource");
    }
    return TILEXR_SUCCESS;
}

int AppendSetSourceCke(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    uint16_t mask,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuCkeSpec set;
    set.ckeId = spec.sourceCke;
    set.mask = mask;
    set.clearWait = true;

    TileXRCcuInstr instr;
    if (TileXRCcuEncodeSetCke(set, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall source CKE set");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendNotifyWait(
    uint16_t localWaitCke,
    uint16_t mask,
    const char* phase,
    bool clearCkeWait,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuCkeSpec wait;
    wait.waitCkeId = localWaitCke;
    wait.waitMask = mask;
    wait.clearWait = true;

    TileXRCcuInstr instr;
    const int ret = clearCkeWait ?
        TileXRCcuEncodeClearCke(wait, &instr) :
        TileXRCcuEncodeSetCke(wait, &instr);
    if (ret != TILEXR_SUCCESS) {
        return Fail(program, report, std::string("failed to encode direct CCU alltoall ") + phase + " NotifyWait");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendSyncPair(
    uint16_t remoteNotifyCke,
    uint16_t localWaitCke,
    uint16_t channelId,
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    uint16_t localMask,
    uint16_t waitMask,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuSyncCkeSpec post;
    post.remoteCke = remoteNotifyCke;
    post.localCke = spec.sourceCke;
    post.localCkeMask = localMask;
    post.channelId = channelId;
    post.clearWait = true;

    TileXRCcuInstr instr;
    if (TileXRCcuEncodeSyncCke(post, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall SyncCke");
    }
    program->push_back(instr);

    return AppendNotifyWait(localWaitCke, waitMask, "PostSync", true, program, report);
}

int AppendRemoteNotify(
    uint16_t remoteNotifyCke,
    uint16_t channelId,
    uint16_t localXn,
    uint16_t remoteXn,
    uint64_t value,
    uint16_t secFlag,
    uint16_t mask,
    const char* phase,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(localXn, value, secFlag, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, std::string("failed to encode direct CCU alltoall ") + phase + " variable load");
    }
    program->push_back(instr);

    TileXRCcuSyncXnSpec notify;
    notify.remoteXn = remoteXn;
    notify.localXn = localXn;
    notify.channelId = channelId;
    notify.notifyCke = remoteNotifyCke;
    notify.notifyMask = mask;
    notify.clearWait = true;

    if (TileXRCcuEncodeSyncXn(notify, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, std::string("failed to encode direct CCU alltoall ") + phase + " SyncXn notify");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendSyncXnNotify(
    uint16_t remoteNotifyCke,
    uint16_t channelId,
    uint16_t localXn,
    uint16_t remoteXn,
    uint16_t mask,
    const char* phase,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuSyncXnSpec notify;
    notify.remoteXn = remoteXn;
    notify.localXn = localXn;
    notify.channelId = channelId;
    notify.notifyCke = remoteNotifyCke;
    notify.notifyMask = mask;
    notify.clearWait = true;

    TileXRCcuInstr instr;
    if (TileXRCcuEncodeSyncXn(notify, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, std::string("failed to encode direct CCU alltoall ") + phase +
            " SyncXn notify");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendRemoteMarkerNotify(
    uint16_t remoteNotifyCke,
    uint16_t channelId,
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadSqeArgsToX(
            spec.preSyncLocalMarkerXn,
            spec.preSyncMarkerArgIndex,
            &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall PreSync loop marker load");
    }
    program->push_back(instr);

    TileXRCcuSyncXnSpec notify;
    notify.remoteXn = spec.preSyncRemoteMarkerXn;
    notify.localXn = spec.preSyncLocalMarkerXn;
    notify.channelId = channelId;
    notify.notifyCke = remoteNotifyCke;
    notify.notifyMask = TILEXR_CCU_ALLTOALL_LOOP_MARKER_MASK;
    notify.clearWait = true;
    if (TileXRCcuEncodeSyncXn(notify, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall PreSync loop marker notify");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendPreSyncPhase(
    uint16_t remoteNotifyCke,
    uint16_t localWaitCke,
    uint16_t outputChannelId,
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    const uint16_t outputMask = PreSyncSignalMask(spec);
    const uint16_t tokenMask = PreSyncTokenMask(spec);
    const uint16_t markerMask = spec.preSyncMarkerEnabled ? TILEXR_CCU_ALLTOALL_LOOP_MARKER_MASK : 0U;
    const uint16_t waitMask = static_cast<uint16_t>(markerMask | outputMask | tokenMask);
    const uint16_t localOutputXn =
        spec.preSyncLocalAddrXn == 0 ? spec.localXn : spec.preSyncLocalAddrXn;
    const uint16_t localTokenXn =
        spec.preSyncLocalTokenXn == 0 ? spec.lengthXn : spec.preSyncLocalTokenXn;
    const uint16_t tokenChannelId =
        spec.preSyncTokenChannelId == 0 ? outputChannelId : spec.preSyncTokenChannelId;
    const uint16_t markerChannelId =
        spec.preSyncMarkerChannelId == 0 ? outputChannelId : spec.preSyncMarkerChannelId;
    const uint16_t tokenNotifyCke =
        spec.preSyncRemoteTokenNotifyCke == 0 ? remoteNotifyCke : spec.preSyncRemoteTokenNotifyCke;
    if (spec.preSyncMarkerEnabled &&
        AppendRemoteMarkerNotify(
            remoteNotifyCke,
            markerChannelId,
            spec,
            program,
            report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (AppendRemoteNotify(
            remoteNotifyCke,
            outputChannelId,
            localOutputXn,
            spec.preSyncRemoteAddrXn,
            spec.localRecvAddr,
            0,
            outputMask,
            "PreSync output",
            program,
            report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (AppendRemoteNotify(
            tokenNotifyCke,
            tokenChannelId,
            localTokenXn,
            spec.preSyncRemoteTokenXn,
            spec.localRecvToken,
            1,
            tokenMask,
            "PreSync token",
            program,
            report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (!spec.preSyncWait) {
        return TILEXR_SUCCESS;
    }

    return AppendNotifyWait(localWaitCke, waitMask, "PreSync output", false, program, report);
}

int AppendPostSyncPhase(
    uint16_t remoteNotifyCke,
    uint16_t localWaitCke,
    uint16_t channelId,
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    const uint16_t notifyMask = PostSyncSignalMask(spec);
    const uint16_t waitMask = PostSyncSignalMask(spec);
    if (AppendSetSourceCke(spec, notifyMask, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!spec.postSyncWait) {
        TileXRCcuSyncCkeSpec post;
        post.remoteCke = remoteNotifyCke;
        post.localCke = spec.sourceCke;
        post.localCkeMask = notifyMask;
        post.channelId = channelId;
        post.clearWait = true;

        TileXRCcuInstr instr;
        if (TileXRCcuEncodeSyncCke(post, &instr) != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode direct CCU alltoall PostSync notify-only SyncCke");
        }
        program->push_back(instr);
        return TILEXR_SUCCESS;
    }
    return AppendSyncPair(
        remoteNotifyCke,
        localWaitCke,
        channelId,
        spec,
        notifyMask,
        waitMask,
        program,
        report);
}

int AppendCopyBlock(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    uint64_t offset,
    uint64_t bytesPerBlock,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuMemoryCopySpec copy;
    copy.direction = TileXRCcuMemoryCopyDirection::LocalToRemote;
    copy.localGsa = spec.localGsa;
    copy.localXn = spec.localXn;
    copy.remoteGsa = spec.remoteGsa;
    copy.remoteXn = spec.remoteXn;
    copy.lengthXn = spec.lengthXn;
    copy.localAddr = spec.localSendAddr + offset;
    copy.localToken = spec.localSendToken;
    copy.remoteAddr = spec.remoteRecvAddr + offset;
    copy.remoteToken = spec.remoteRecvToken;
    copy.lengthBytes = bytesPerBlock;
    copy.channelId = spec.copyChannelId == 0 ? spec.channelId : spec.copyChannelId;
    copy.completionCke = spec.copyCompletionCke;
    copy.completionMask = spec.ckeMask;

    std::vector<TileXRCcuInstr> block;
    TileXRCcuMemoryProgramReport memoryReport;
    if (TileXRCcuBuildMemoryCopyProgram(copy, &block, &memoryReport) != TILEXR_SUCCESS) {
        return Fail(program, report, memoryReport.message);
    }
    program->insert(program->end(), block.begin(), block.end());
    return TILEXR_SUCCESS;
}

int AppendMeshRemoteCopyBlock(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    uint64_t offset,
    uint64_t bytesPerBlock,
    uint16_t completionCke,
    uint16_t completionMask,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuMemoryCopySpec copy;
    copy.direction = TileXRCcuMemoryCopyDirection::LocalToRemote;
    copy.localGsa = spec.localGsa;
    copy.localXn = spec.localXn;
    copy.remoteGsa = spec.remoteGsa;
    copy.remoteXn = spec.remoteXn;
    copy.lengthXn = spec.lengthXn;
    copy.localAddr = spec.localSendAddr + offset;
    copy.localToken = spec.localSendToken;
    copy.remoteAddr = spec.remoteRecvAddr + offset;
    copy.remoteToken = spec.remoteRecvToken;
    copy.lengthBytes = bytesPerBlock;
    copy.channelId = spec.copyChannelId == 0 ? spec.channelId : spec.copyChannelId;
    copy.completionCke = completionCke;
    copy.completionMask = completionMask;

    std::vector<TileXRCcuInstr> block;
    TileXRCcuMemoryProgramReport memoryReport;
    if (TileXRCcuBuildMemoryCopyProgram(copy, &block, &memoryReport) != TILEXR_SUCCESS || block.size() != 7U) {
        return Fail(program, report, memoryReport.message.empty() ?
            "failed to build direct CCU alltoall mesh remote copy block" : memoryReport.message);
    }
    block.pop_back();
    program->insert(program->end(), block.begin(), block.end());
    return TILEXR_SUCCESS;
}

int AppendFinish(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(spec.localXn, 0, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall finish instruction");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

void FillReport(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    const std::vector<TileXRCcuInstr>& program,
    TileXRCcuAllToAllProgramReport* report)
{
    if (report == nullptr) {
        return;
    }
    const uint32_t bytesPerBlock = spec.memorySliceBytes * spec.memSlicePerBlock;
    const uint32_t markerInstructionCount = spec.preSyncMarkerEnabled ? 2U : 0U;
    report->preSyncInstructionCount =
        spec.preSyncNotify ? (spec.preSyncWait ? 5U : 4U) + markerInstructionCount : 0U;
    report->blockCount = static_cast<uint32_t>(spec.bytes / bytesPerBlock);
    report->bytesPerBlock = bytesPerBlock;
    report->copyInstructionCount = report->blockCount * 7U;
    report->postSyncInstructionCount = !spec.postSyncNotify ? 0U : (spec.postSyncWait ? 3U : 2U);
    report->finishInstructionCount = spec.emitFinish ? 1U : 0U;
    report->totalInstructionCount = static_cast<uint32_t>(program.size());
    report->message = "ok";
}

int ValidateMeshSpec(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    if (program == nullptr) {
        return Fail(program, report, "missing output direct CCU alltoall mesh program");
    }
    if (spec.rankSize < 2U || spec.rankSize > TILEXR_CCU_ALLTOALL_MAX_RANK_SIZE ||
        spec.localRank >= spec.rankSize || spec.peers.size() != spec.rankSize - 1U) {
        return Fail(program, report, "direct CCU alltoall mesh requires 2..64 ranks and rankSize-1 peers");
    }
    if (spec.localSendAddr == 0 || spec.localRecvAddr == 0 ||
        spec.localSendToken == 0 || spec.localRecvToken == 0 || spec.chunkBytes == 0 ||
        spec.chunkBytes % TILEXR_CCU_ALLTOALL_BLOCK_BYTES != 0) {
        return Fail(program, report, "invalid direct CCU alltoall mesh local buffer");
    }
    if (spec.selfSourceGsa == 0 || spec.selfDestinationGsa == 0 || spec.selfSourceXn == 0 ||
        spec.selfDestinationXn == 0 || spec.selfLengthXn == 0 ||
        spec.selfCompletionCke == 0 ||
        spec.remoteCompletionCkes.size() != CompletionCkeCount(spec.peers.size())) {
        return Fail(program, report, "missing direct CCU alltoall mesh self-copy resource");
    }
    std::vector<bool> peerRanks(spec.rankSize, false);
    std::set<uint16_t> channelIds;
    std::set<uint16_t> completionCkes;
    const auto& sharedRoute = spec.peers.front().route;
    if (spec.selfSourceXn != sharedRoute.localXn ||
        spec.selfDestinationXn != sharedRoute.preSyncLocalTokenXn ||
        spec.selfLengthXn != sharedRoute.lengthXn) {
        return Fail(program, report, "alltoall mesh self copy must share source, destination, and length XNs");
    }
    for (uint16_t completionCke : spec.remoteCompletionCkes) {
        if (completionCke == 0 || completionCke == sharedRoute.sourceCke ||
            !completionCkes.insert(completionCke).second) {
            return Fail(program, report,
                "alltoall mesh completion CKE overlaps source CKE or duplicates another completion CKE");
        }
    }
    for (size_t ordinal = 0; ordinal < spec.peers.size(); ++ordinal) {
        const auto& peer = spec.peers[ordinal];
        if (peer.peerRank >= spec.rankSize || peer.peerRank == spec.localRank || peerRanks[peer.peerRank]) {
            return Fail(program, report, "invalid direct CCU alltoall mesh peer rank");
        }
        peerRanks[peer.peerRank] = true;
        if (peer.route.localRank != spec.localRank || peer.route.localSendAddr != spec.localSendAddr ||
            peer.route.localSendToken != spec.localSendToken || peer.route.localRecvAddr != spec.localRecvAddr ||
            peer.route.localRecvToken != spec.localRecvToken || peer.route.bytes != spec.chunkBytes ||
            peer.route.preSyncMarkerEnabled || !peer.route.preSyncNotify || !peer.route.preSyncWait ||
            !peer.route.postSyncNotify || !peer.route.postSyncWait) {
            return Fail(program, report, "invalid direct CCU alltoall mesh peer route");
        }
        if (peer.route.preSyncLocalAddrXn != sharedRoute.preSyncLocalAddrXn ||
            peer.route.preSyncLocalTokenXn != sharedRoute.preSyncLocalTokenXn ||
            peer.route.preSyncChannelId != peer.route.copyChannelId ||
            peer.route.preSyncTokenChannelId != peer.route.copyChannelId ||
            peer.route.postSyncChannelId != peer.route.copyChannelId ||
            peer.route.copyCompletionCke != spec.remoteCompletionCkes[ordinal / TILEXR_CCU_CKE_MASK_BITS] ||
            peer.route.ckeMask != TILEXR_CCU_ALLTOALL_POST_SYNC_MASK ||
            !channelIds.insert(peer.route.copyChannelId).second) {
            return Fail(program, report, "duplicate direct CCU alltoall mesh peer resource");
        }
        TileXRCcuAllToAll2RankProgramSpec validationRoute = peer.route;
        validationRoute.localRank = 0;
        std::vector<TileXRCcuInstr> ignored;
        TileXRCcuAllToAllProgramReport ignoredReport;
        if (ValidateSpec(validationRoute, &ignored, &ignoredReport) != TILEXR_SUCCESS) {
            return Fail(program, report, ignoredReport.message);
        }
    }
    return TILEXR_SUCCESS;
}

int AppendMeshPeerPosts(
    const std::vector<TileXRCcuAllToAllMeshPeerSpec>& peers,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    if (peers.empty()) {
        return Fail(program, report, "missing direct CCU alltoall mesh peers");
    }
    const auto& shared = peers.front().route;
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(shared.preSyncLocalAddrXn, shared.localRecvAddr, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh output variable");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeLoadImdToXn(shared.preSyncLocalTokenXn, shared.localRecvToken, 1, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh token variable");
    }
    program->push_back(instr);

    if (AppendSetSourceCke(shared, TILEXR_CCU_ALLTOALL_SOURCE_CKE_INIT_MASK, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    for (const auto& peer : peers) {
        const auto& route = peer.route;
        if (AppendSyncXnNotify(
                route.preSyncRemoteNotifyCke,
                route.preSyncChannelId,
                shared.preSyncLocalAddrXn,
                route.preSyncRemoteAddrXn,
                PreSyncSignalMask(route),
                "mesh PreSync output",
                program,
                report) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        if (AppendSyncXnNotify(
                route.preSyncRemoteTokenNotifyCke,
                route.preSyncTokenChannelId,
                shared.preSyncLocalTokenXn,
                route.preSyncRemoteTokenXn,
                PreSyncTokenMask(route),
                "mesh PreSync token",
                program,
                report) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

int AppendLocalCopyBlock(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    uint64_t offset,
    uint64_t bytesPerBlock,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToGsa(spec.selfSourceGsa, spec.localSendAddr + offset, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh self source address");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeLoadImdToXn(spec.selfSourceXn, spec.localSendToken, 1U, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh self source token");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeLoadImdToGsa(spec.selfDestinationGsa, spec.localRecvAddr + offset, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh self destination address");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeLoadImdToXn(spec.selfDestinationXn, spec.localRecvToken, 1U, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh self destination token");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeLoadImdToXn(spec.selfLengthXn, bytesPerBlock, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to load direct CCU alltoall mesh self length");
    }
    program->push_back(instr);

    TileXRCcuLocalMsTransferSpec transfer;
    transfer.localGsa = spec.selfSourceGsa;
    transfer.localXn = spec.selfSourceXn;
    transfer.localMs = 0;
    transfer.lengthXn = spec.selfLengthXn;
    transfer.channelId = 0;
    transfer.setCkeId = spec.selfCompletionCke;
    transfer.setCkeMask = 1U;
    if (TileXRCcuEncodeTransLocMemToLocMs(transfer, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall mesh self transfer to local MS");
    }
    program->push_back(instr);

    TileXRCcuCkeSpec wait;
    wait.waitCkeId = spec.selfCompletionCke;
    wait.waitMask = 1U;
    if (TileXRCcuEncodeClearCke(wait, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to wait direct CCU alltoall mesh self transfer");
    }
    program->push_back(instr);

    transfer.localGsa = spec.selfDestinationGsa;
    transfer.localXn = spec.selfDestinationXn;
    if (TileXRCcuEncodeTransLocMsToLocMem(transfer, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall mesh self transfer from local MS");
    }
    program->push_back(instr);
    if (TileXRCcuEncodeClearCke(wait, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to wait direct CCU alltoall mesh self transfer from local MS");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendMeshPostNotify(
    const TileXRCcuAllToAll2RankProgramSpec& route,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    TileXRCcuSyncCkeSpec post;
    post.remoteCke = route.postSyncRemoteNotifyCke;
    post.localCke = route.sourceCke;
    post.localCkeMask = route.ckeMask;
    post.channelId = route.postSyncChannelId;
    post.clearWait = true;
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeSyncCke(post, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to notify direct CCU alltoall mesh completion");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

uint16_t InstructionSlot(const TileXRCcuInstr& instr, uint32_t slot)
{
    return static_cast<uint16_t>(
        (instr.words[slot / 4U] >> ((slot % 4U) * 16U)) & 0xffffU);
}

int FailBindingValidation(TileXRCcuAllToAllProgramReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = "direct CCU alltoall encoded binding validation failed: " + message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool MatchesSyncXn(
    const TileXRCcuInstr& instr,
    uint16_t remoteXn,
    uint16_t localXn,
    uint16_t channelId,
    uint16_t notifyCke,
    uint16_t notifyMask)
{
    return InstructionSlot(instr, 0) == TILEXR_CCU_TRACE_SYNC_XN_HEADER &&
        InstructionSlot(instr, 1) == remoteXn &&
        InstructionSlot(instr, 2) == localXn &&
        InstructionSlot(instr, 4) == channelId &&
        InstructionSlot(instr, 5) == notifyCke &&
        InstructionSlot(instr, 6) == notifyMask;
}

bool MatchesWait(
    const TileXRCcuInstr& instr,
    uint16_t header,
    uint16_t waitCke,
    uint16_t waitMask)
{
    return InstructionSlot(instr, 0) == header &&
        InstructionSlot(instr, 4) == waitCke &&
        InstructionSlot(instr, 5) == waitMask;
}

bool MatchesTransfer(
    const TileXRCcuInstr& instr,
    uint16_t header,
    uint16_t remoteGsa,
    uint16_t remoteXn,
    uint16_t localGsa,
    uint16_t localXn,
    uint16_t lengthXn,
    uint16_t channelId,
    uint16_t completionCke,
    uint16_t completionMask)
{
    return InstructionSlot(instr, 0) == header &&
        InstructionSlot(instr, 1) == remoteGsa &&
        InstructionSlot(instr, 2) == remoteXn &&
        InstructionSlot(instr, 3) == localGsa &&
        InstructionSlot(instr, 4) == localXn &&
        InstructionSlot(instr, 5) == lengthXn &&
        InstructionSlot(instr, 6) == channelId &&
        InstructionSlot(instr, 12) == completionCke &&
        InstructionSlot(instr, 13) == completionMask;
}

bool MatchesLocalMsTransfer(
    const TileXRCcuInstr& instr,
    uint16_t header,
    uint16_t localGsa,
    uint16_t localXn,
    uint16_t localMs,
    uint16_t lengthXn,
    uint16_t channelId,
    uint16_t completionCke,
    uint16_t completionMask)
{
    const bool memToMs = header == TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MS_HEADER;
    return InstructionSlot(instr, 0) == header &&
        InstructionSlot(instr, memToMs ? 1U : 3U) == localMs &&
        InstructionSlot(instr, memToMs ? 2U : 1U) == localGsa &&
        InstructionSlot(instr, memToMs ? 3U : 2U) == localXn &&
        InstructionSlot(instr, 4) == lengthXn &&
        InstructionSlot(instr, 5) == channelId &&
        InstructionSlot(instr, 12) == completionCke &&
        InstructionSlot(instr, 13) == completionMask;
}

int ValidateMeshProgramBindings(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    const std::vector<TileXRCcuInstr>& program,
    TileXRCcuAllToAllProgramReport* report)
{
    auto peers = spec.peers;
    std::sort(peers.begin(), peers.end(), [](const TileXRCcuAllToAllMeshPeerSpec& lhs,
                                             const TileXRCcuAllToAllMeshPeerSpec& rhs) {
        return lhs.peerRank < rhs.peerRank;
    });
    const uint32_t blocksPerChunk = static_cast<uint32_t>(
        spec.chunkBytes / TILEXR_CCU_ALLTOALL_BLOCK_BYTES);
    const size_t preSyncInstructions = MeshPreSyncInstructionCount(peers.size());
    const size_t copyInstructionsPerBlock = MeshCopyInstructionCountPerBlock(peers.size());
    const size_t expectedSize = preSyncInstructions +
        static_cast<size_t>(blocksPerChunk) * copyInstructionsPerBlock + peers.size() * 2U + 1U;
    if (peers.size() != spec.rankSize - 1U || blocksPerChunk == 0 || program.size() != expectedSize) {
        return FailBindingValidation(report, "unexpected mesh program shape");
    }

    const auto& sharedRoute = peers.front().route;
    if (InstructionSlot(program[0], 0) != TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER ||
        InstructionSlot(program[0], 1) != sharedRoute.preSyncLocalAddrXn ||
        InstructionSlot(program[1], 0) != TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER ||
        InstructionSlot(program[1], 1) != sharedRoute.preSyncLocalTokenXn ||
        InstructionSlot(program[2], 0) != TILEXR_CCU_TRACE_SET_CKE_HEADER ||
        InstructionSlot(program[2], 2) != sharedRoute.sourceCke ||
        InstructionSlot(program[2], 3) != TILEXR_CCU_ALLTOALL_SOURCE_CKE_INIT_MASK) {
        return FailBindingValidation(report, "pre-sync variable loads do not match the shared mesh resources");
    }
    for (size_t ordinal = 0; ordinal < peers.size(); ++ordinal) {
        const auto& route = peers[ordinal].route;
        if (!MatchesSyncXn(
                program[3U + ordinal * 2U],
                route.preSyncRemoteAddrXn,
                sharedRoute.preSyncLocalAddrXn,
                route.preSyncChannelId,
                route.preSyncRemoteNotifyCke,
                PreSyncSignalMask(route))) {
            return FailBindingValidation(report, "output SyncXn does not match its peer route");
        }
        if (!MatchesSyncXn(
                program[4U + ordinal * 2U],
                route.preSyncRemoteTokenXn,
                sharedRoute.preSyncLocalTokenXn,
                route.preSyncTokenChannelId,
                route.preSyncRemoteTokenNotifyCke,
                PreSyncTokenMask(route))) {
            return FailBindingValidation(report, "token SyncXn does not match its peer route");
        }
        const size_t wait = 3U + peers.size() * 2U + ordinal;
        if (!MatchesWait(
                program[wait],
                TILEXR_CCU_TRACE_SET_CKE_HEADER,
                route.preSyncLocalWaitCke,
                static_cast<uint16_t>(PreSyncSignalMask(route) | PreSyncTokenMask(route)))) {
            return FailBindingValidation(report, "pre-sync waits do not match their peer route");
        }
    }

    size_t instruction = preSyncInstructions;
    for (uint32_t block = 0; block < blocksPerChunk; ++block) {
        for (size_t ordinal = 0; ordinal < peers.size(); ++ordinal) {
            const auto& route = peers[ordinal].route;
            if (!MatchesTransfer(
                    program[instruction + 5U],
                    TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_RMT_MEM_HEADER,
                    route.remoteGsa,
                    route.remoteXn,
                    route.localGsa,
                    route.localXn,
                    route.lengthXn,
                    route.copyChannelId,
                    route.copyCompletionCke,
                    static_cast<uint16_t>(1U << (ordinal % TILEXR_CCU_CKE_MASK_BITS)))) {
                return FailBindingValidation(report, "remote copy does not match its peer route");
            }
            instruction += 6U;
        }
        if (!MatchesLocalMsTransfer(
                program[instruction + 5U],
                TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_LOC_MS_HEADER,
                spec.selfSourceGsa,
                spec.selfSourceXn,
                0,
                spec.selfLengthXn,
                0,
                spec.selfCompletionCke,
                1U) ||
            !MatchesWait(
                program[instruction + 6U],
                TILEXR_CCU_TRACE_CLEAR_CKE_HEADER,
                spec.selfCompletionCke,
                1U) ||
            !MatchesLocalMsTransfer(
                program[instruction + 7U],
                TILEXR_CCU_TRACE_TRANS_LOC_MS_TO_LOC_MEM_HEADER,
                spec.selfDestinationGsa,
                spec.selfDestinationXn,
                0,
                spec.selfLengthXn,
                0,
                spec.selfCompletionCke,
                1U) ||
            !MatchesWait(
                program[instruction + 8U],
                TILEXR_CCU_TRACE_CLEAR_CKE_HEADER,
                spec.selfCompletionCke,
                1U)) {
            return FailBindingValidation(report, "self copy does not match its local route");
        }
        instruction += 9U;
        for (uint32_t group = 0; group < spec.remoteCompletionCkes.size(); ++group) {
            if (!MatchesWait(
                    program[instruction],
                    TILEXR_CCU_TRACE_CLEAR_CKE_HEADER,
                    spec.remoteCompletionCkes[group],
                    CompletionMaskForGroup(peers.size(), group))) {
                return FailBindingValidation(report, "grouped remote copy wait does not match the mesh completion CKE");
            }
            ++instruction;
        }
    }
    for (const auto& peer : peers) {
        const auto& route = peer.route;
        if (InstructionSlot(program[instruction], 0) != TILEXR_CCU_TRACE_SYNC_CKE_HEADER ||
            InstructionSlot(program[instruction], 1) != route.postSyncRemoteNotifyCke ||
            InstructionSlot(program[instruction], 2) != route.sourceCke ||
            InstructionSlot(program[instruction], 3) != route.ckeMask ||
            InstructionSlot(program[instruction], 4) != route.postSyncChannelId) {
            return FailBindingValidation(report, "post-sync notify does not match its peer route");
        }
        ++instruction;
    }
    for (const auto& peer : peers) {
        if (!MatchesWait(
                program[instruction],
                TILEXR_CCU_TRACE_CLEAR_CKE_HEADER,
                peer.route.postSyncLocalWaitCke,
                peer.route.ckeMask)) {
            return FailBindingValidation(report, "post-sync wait does not match its peer route");
        }
        ++instruction;
    }
    if (instruction + 1U != program.size() ||
        InstructionSlot(program[instruction], 0) != TILEXR_CCU_TRACE_LOAD_IMD_TO_XN_HEADER ||
        InstructionSlot(program[instruction], 1) != spec.selfSourceXn) {
        return FailBindingValidation(report, "finish instruction does not match the mesh program");
    }
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCcuValidateAllToAllMeshProgramBindings(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    const std::vector<TileXRCcuInstr>& program,
    TileXRCcuAllToAllProgramReport* report)
{
    return ValidateMeshProgramBindings(spec, program, report);
}

int TileXRCcuBuildAllToAll2RankProgram(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    ResetReport(report);
    if (program != nullptr) {
        program->clear();
    }
    int ret = ValidateSpec(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const uint64_t bytesPerBlock = static_cast<uint64_t>(spec.memorySliceBytes) * spec.memSlicePerBlock;
    const uint32_t blockCount = static_cast<uint32_t>(spec.bytes / bytesPerBlock);
    const uint16_t preSyncChannelId = spec.preSyncChannelId == 0 ? spec.channelId : spec.preSyncChannelId;
    const uint16_t postSyncChannelId = spec.postSyncChannelId == 0 ? spec.channelId : spec.postSyncChannelId;
    const uint32_t markerInstructionCount = spec.preSyncMarkerEnabled ? 2U : 0U;
    program->reserve(
        (spec.preSyncNotify ? (spec.preSyncWait ? 5U : 4U) + markerInstructionCount : 0U) + blockCount * 7U +
        (!spec.postSyncNotify ? 0U : (spec.postSyncWait ? 3U : 2U)) +
        (spec.emitFinish ? 1U : 0U));

    if (spec.preSyncNotify) {
        ret = AppendPreSyncPhase(
            spec.preSyncRemoteNotifyCke,
            spec.preSyncLocalWaitCke,
            preSyncChannelId,
            spec,
            program,
            report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    for (uint32_t block = 0; block < blockCount; ++block) {
        const uint64_t offset = static_cast<uint64_t>(block) * bytesPerBlock;
        ret = AppendCopyBlock(spec, offset, bytesPerBlock, program, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    if (spec.postSyncNotify) {
        ret = AppendPostSyncPhase(
            spec.postSyncRemoteNotifyCke,
            spec.postSyncLocalWaitCke,
            postSyncChannelId,
            spec,
            program,
            report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }
    if (spec.emitFinish) {
        ret = AppendFinish(spec, program, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    FillReport(spec, *program, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildAllToAllMeshProgram(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report)
{
    ResetReport(report);
    if (program != nullptr) {
        program->clear();
    }
    int ret = ValidateMeshSpec(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    auto peers = spec.peers;
    std::sort(peers.begin(), peers.end(), [](const TileXRCcuAllToAllMeshPeerSpec& lhs,
                                             const TileXRCcuAllToAllMeshPeerSpec& rhs) {
        return lhs.peerRank < rhs.peerRank;
    });
    const uint64_t bytesPerBlock = TILEXR_CCU_ALLTOALL_BLOCK_BYTES;
    const uint32_t blocksPerChunk = static_cast<uint32_t>(spec.chunkBytes / bytesPerBlock);
    const size_t preSyncInstructions = MeshPreSyncInstructionCount(peers.size());
    const size_t copyInstructionsPerBlock = MeshCopyInstructionCountPerBlock(peers.size());
    program->reserve(preSyncInstructions +
        static_cast<size_t>(blocksPerChunk) * copyInstructionsPerBlock + peers.size() * 2U + 1U);

    ret = AppendMeshPeerPosts(peers, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    for (const auto& peer : peers) {
        ret = AppendNotifyWait(
            peer.route.preSyncLocalWaitCke,
            static_cast<uint16_t>(PreSyncSignalMask(peer.route) | PreSyncTokenMask(peer.route)),
            "mesh PreSync",
            false,
            program,
            report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    const uint64_t selfBaseOffset = static_cast<uint64_t>(spec.localRank) * spec.chunkBytes;
    for (uint32_t block = 0; block < blocksPerChunk; ++block) {
        for (size_t ordinal = 0; ordinal < peers.size(); ++ordinal) {
            const auto& peer = peers[ordinal];
            TileXRCcuAllToAll2RankProgramSpec route = peer.route;
            route.localSendAddr = spec.localSendAddr + static_cast<uint64_t>(peer.peerRank) * spec.chunkBytes;
            route.remoteRecvAddr += static_cast<uint64_t>(spec.localRank) * spec.chunkBytes;
            ret = AppendMeshRemoteCopyBlock(
                route,
                static_cast<uint64_t>(block) * bytesPerBlock,
                bytesPerBlock,
                peer.route.copyCompletionCke,
                static_cast<uint16_t>(1U << (ordinal % TILEXR_CCU_CKE_MASK_BITS)),
                program,
                report);
            if (ret != TILEXR_SUCCESS) {
                return ret;
            }
        }
        ret = AppendLocalCopyBlock(
            spec,
            selfBaseOffset + static_cast<uint64_t>(block) * bytesPerBlock,
            bytesPerBlock,
            program,
            report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        for (uint32_t group = 0; group < spec.remoteCompletionCkes.size(); ++group) {
            ret = AppendNotifyWait(
                spec.remoteCompletionCkes[group],
                CompletionMaskForGroup(peers.size(), group),
                "mesh Copy",
                true,
                program,
                report);
            if (ret != TILEXR_SUCCESS) {
                return ret;
            }
        }
    }

    for (const auto& peer : peers) {
        ret = AppendMeshPostNotify(peer.route, program, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }
    for (const auto& peer : peers) {
        ret = AppendNotifyWait(
            peer.route.postSyncLocalWaitCke,
            peer.route.ckeMask,
            "mesh PostSync",
            true,
            program,
            report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    TileXRCcuInstr finish;
    if (TileXRCcuEncodeLoadImdToXn(spec.selfSourceXn, 0, 0, &finish) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode direct CCU alltoall mesh finish");
    }
    program->push_back(finish);

    if (TileXRCcuValidateAllToAllMeshProgramBindings(spec, *program, report) != TILEXR_SUCCESS) {
        program->clear();
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (report != nullptr) {
        report->preSyncInstructionCount = static_cast<uint32_t>(preSyncInstructions);
        report->blockCount = blocksPerChunk;
        report->bytesPerBlock = static_cast<uint32_t>(bytesPerBlock);
        report->copyInstructionCount = static_cast<uint32_t>(blocksPerChunk * copyInstructionsPerBlock);
        report->postSyncInstructionCount = static_cast<uint32_t>(peers.size() * 2U);
        report->finishInstructionCount = 1U;
        report->totalInstructionCount = static_cast<uint32_t>(program->size());
        report->peerCount = static_cast<uint32_t>(peers.size());
        report->syncResourceCount = static_cast<uint32_t>(peers.size());
        report->remoteBlockCount = static_cast<uint32_t>(peers.size()) * blocksPerChunk;
        report->selfBlockCount = blocksPerChunk;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
