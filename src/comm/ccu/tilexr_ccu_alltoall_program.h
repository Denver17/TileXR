/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_ALLTOALL_PROGRAM_H
#define TILEXR_CCU_ALLTOALL_PROGRAM_H

#include "ccu/tilexr_ccu_memory_program.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

constexpr uint32_t TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES = 4096U;
constexpr uint32_t TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK = 8U;
constexpr uint32_t TILEXR_CCU_ALLTOALL_BLOCK_BYTES =
    TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES * TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK;
constexpr uint16_t TILEXR_CCU_ALLTOALL_OUTPUT_XN_ID = 1U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_TOKEN_XN_ID = 2U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_LOOP_MARKER_MASK = 1U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_POST_SYNC_ID = 3U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_POST_SYNC_MASK = 0x8U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_SIGNAL_MASK = 1U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_RANK0_SIGNAL_MASK = 1U;
constexpr uint16_t TILEXR_CCU_ALLTOALL_RANK1_SIGNAL_MASK = 2U;

struct TileXRCcuAllToAll2RankProgramSpec {
    uint32_t localRank = 0;
    uint64_t localSendAddr = 0;
    uint64_t localSendToken = 0;
    uint64_t localRecvAddr = 0;
    uint64_t localRecvToken = 0;
    uint64_t remoteSendAddr = 0;
    uint64_t remoteSendToken = 0;
    uint64_t remoteRecvAddr = 0;
    uint64_t remoteRecvToken = 0;
    uint64_t bytes = 0;
    uint32_t memorySliceBytes = TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES;
    uint32_t memSlicePerBlock = TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK;
    uint16_t localGsa = 0;
    uint16_t remoteGsa = 0;
    uint16_t localXn = 0;
    uint16_t remoteXn = 0;
    uint16_t lengthXn = 0;
    uint16_t preSyncLocalAddrXn = 0;
    uint16_t preSyncLocalTokenXn = 0;
    uint16_t preSyncLocalMarkerXn = 0;
    uint16_t preSyncRemoteMarkerXn = 0;
    uint16_t preSyncMarkerArgIndex = 0;
    uint16_t channelId = 0;
    uint16_t preSyncMarkerChannelId = 0;
    uint16_t preSyncChannelId = 0;
    uint16_t preSyncTokenChannelId = 0;
    uint16_t copyChannelId = 0;
    uint16_t postSyncChannelId = 0;
    uint16_t copyCompletionCke = 0;
    uint16_t preSyncRemoteAddrXn = 0;
    uint16_t preSyncRemoteTokenXn = 0;
    uint16_t preSyncLocalWaitCke = 0;
    uint16_t preSyncRemoteNotifyCke = 0;
    uint16_t preSyncTokenLocalWaitCke = 0;
    uint16_t preSyncRemoteTokenNotifyCke = 0;
    uint16_t postSyncLocalWaitCke = 0;
    uint16_t postSyncRemoteNotifyCke = 0;
    uint16_t sourceCke = 0;
    uint16_t ckeMask = 1;
    bool preSyncNotify = true;
    bool preSyncWait = true;
    bool preSyncMarkerEnabled = false;
    bool postSyncNotify = true;
    bool postSyncWait = true;
    bool emitFinish = true;
};

struct TileXRCcuAllToAllMeshPeerSpec {
    uint32_t peerRank = 0;
    TileXRCcuAllToAll2RankProgramSpec route;
};

struct TileXRCcuAllToAllMeshProgramSpec {
    uint32_t rankSize = 2;
    uint32_t localRank = 0;
    uint64_t localSendAddr = 0;
    uint64_t localSendToken = 0;
    uint64_t localRecvAddr = 0;
    uint64_t localRecvToken = 0;
    uint64_t chunkBytes = 0;
    uint16_t selfSourceGsa = 0;
    uint16_t selfDestinationGsa = 0;
    uint16_t selfSourceXn = 0;
    uint16_t selfDestinationXn = 0;
    uint16_t selfLengthXn = 0;
    uint16_t selfChannelId = 0;
    uint16_t selfCompletionCke = 0;
    std::vector<uint16_t> remoteCompletionCkes;
    std::vector<TileXRCcuAllToAllMeshPeerSpec> peers;
};

struct TileXRCcuAllToAllProgramReport {
    uint32_t preSyncInstructionCount = 0;
    uint32_t blockCount = 0;
    uint32_t bytesPerBlock = 0;
    uint32_t copyInstructionCount = 0;
    uint32_t postSyncInstructionCount = 0;
    uint32_t finishInstructionCount = 0;
    uint32_t totalInstructionCount = 0;
    uint32_t peerCount = 0;
    uint32_t syncResourceCount = 0;
    uint32_t remoteBlockCount = 0;
    uint32_t selfBlockCount = 0;
    std::string message;
};

int TileXRCcuBuildAllToAll2RankProgram(
    const TileXRCcuAllToAll2RankProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report);

int TileXRCcuBuildAllToAllMeshProgram(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuAllToAllProgramReport* report);

int TileXRCcuValidateAllToAllMeshProgramBindings(
    const TileXRCcuAllToAllMeshProgramSpec& spec,
    const std::vector<TileXRCcuInstr>& program,
    TileXRCcuAllToAllProgramReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_ALLTOALL_PROGRAM_H
