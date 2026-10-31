#ifndef TILEXR_STRAGGLER_DEVICE_H
#define TILEXR_STRAGGLER_DEVICE_H

#include <cstdint>

#include "comm_args.h"
#include "tilexr_straggler_layout.h"

namespace TileXR {

struct TileXRStragglerTimingToken {
    uint64_t startCycle = 0;
    uint64_t readyCycle = 0;
    uint64_t launchId = 0;
};

#if TILEXR_ASCENDC_AICORE_COMPILE

constexpr int64_t TILEXR_STRAGGLER_UB_OFFSET = 64;

FORCE_INLINE_AICORE void TileXRStragglerWriteGm(
    GM_ADDR destination, __ubuf__ uint8_t *source, uint32_t bytes)
{
    AscendC::LocalTensor<uint8_t> sourceTensor;
    AscendC::GlobalTensor<uint8_t> destinationTensor;
    AscendC::DataCopyExtParams params(1, bytes, 0, 0, 0);
    sourceTensor.address_.logicPos = static_cast<uint8_t>(AscendC::TPosition::VECIN);
    sourceTensor.address_.bufferAddr = reinterpret_cast<uint64_t>(source);
    destinationTensor.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(destination));
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::DataCopyPad(destinationTensor, sourceTensor, params);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

FORCE_INLINE_AICORE TileXRStragglerTimingToken TileXRStragglerTimingBegin(
    GM_ADDR recordAddress)
{
    TileXRStragglerTimingToken token {};
    if (recordAddress == nullptr) {
        return token;
    }
    __gm__ TileXRStragglerDeviceControl *control =
        reinterpret_cast<__gm__ TileXRStragglerDeviceControl *>(
            recordAddress - sizeof(TileXRStragglerDeviceControl));
    token.launchId = control->launchId;
    if (token.launchId == 0) {
        return token;
    }
    const uint64_t delayCycles = control->delayCycles;
    token.startCycle = static_cast<uint64_t>(AscendC::GetSystemCycle());
    if (delayCycles != 0) {
        while (static_cast<uint64_t>(AscendC::GetSystemCycle()) - token.startCycle < delayCycles) {
        }
    }
    token.readyCycle = static_cast<uint64_t>(AscendC::GetSystemCycle());
    return token;
}

FORCE_INLINE_AICORE void TileXRStragglerTimingEnd(
    TileXRStragglerTimingToken token, GM_ADDR recordAddress)
{
    if (AscendC::GetBlockIdx() != 0 || recordAddress == nullptr ||
        token.launchId == 0 || token.startCycle == 0) {
        return;
    }
    __gm__ TileXRStragglerDeviceControl *control =
        reinterpret_cast<__gm__ TileXRStragglerDeviceControl *>(
            recordAddress - sizeof(TileXRStragglerDeviceControl));
    if (control->launchId != token.launchId) {
        return;
    }
    AscendC::PipeBarrier<PIPE_ALL>();
    __ubuf__ TileXRStragglerTimingRecord *record =
        reinterpret_cast<__ubuf__ TileXRStragglerTimingRecord *>(TILEXR_STRAGGLER_UB_OFFSET);
    record->launchId = token.launchId;
    record->operationId = control->operationId;
    record->messageBytes = control->messageBytes;
    record->startCycle = token.startCycle;
    record->readyCycle = token.readyCycle;
    record->endCycle = static_cast<uint64_t>(AscendC::GetSystemCycle());
    record->generation = control->generation;
    record->componentKind = control->componentKind;
    record->operationKind = control->operationKind;
    record->stageId = control->stageId;
    record->rank = control->rank;
    record->status = TILEXR_TIMING_RECORD_STATUS_COMPLETE;
    record->blockCount = static_cast<uint32_t>(AscendC::GetBlockNum());
    record->reserved = 0;
    TileXRStragglerWriteGm(recordAddress, reinterpret_cast<__ubuf__ uint8_t *>(record), sizeof(*record));
}

#endif

} // namespace TileXR

#endif // TILEXR_STRAGGLER_DEVICE_H
