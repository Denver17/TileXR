#ifndef TILEXR_CHECKER_COLLECTIVE_TRACE_SHIM_H
#define TILEXR_CHECKER_COLLECTIVE_TRACE_SHIM_H

#include <cstdint>
#include <type_traits>

#define TILEXR_CHECKER_CUSTOM_BLOCK_HOOK
#define TILEXR_CHECKER_ENABLE_PIPE_TRACE_HOOK
#include "tilexr/checker/trace_runtime.h"

namespace AscendC {
namespace checker_hook {
inline int GetBlockIdx()
{
    auto *runtime = tilexr::checker::TraceRuntime::Current();
    return runtime == nullptr ? 0 : runtime->block_idx();
}

inline int GetBlockNum()
{
    auto *runtime = tilexr::checker::TraceRuntime::Current();
    return runtime == nullptr ? 1 : runtime->block_num();
}
}  // namespace checker_hook
}  // namespace AscendC

#include "comm_args.h"
#include "kernel_operator.h"

#ifndef LCCL_COLLECTIVES_H
#define LCCL_COLLECTIVES_H
#endif
#ifndef LCCL_SYNC_H
#define LCCL_SYNC_H
#endif
#ifndef LCCL_IPC_QUEUE_H
#define LCCL_IPC_QUEUE_H
#endif
#ifndef LCCL_ALLREDUCE_QUANT_H
#define LCCL_ALLREDUCE_QUANT_H
#endif
#ifndef TILEXR_CHECKER_TRACE_SOURCE_FILE
#define TILEXR_CHECKER_TRACE_SOURCE_FILE "src/collectives/kernels/allreduce_big_data.h"
#endif

using namespace AscendC;
using namespace TileXR;

#define KERNELS_ARGS_FUN() \
GM_ADDR input, GM_ADDR output, GM_ADDR commArgs, int64_t len, int64_t magic, int op, int root, int cycleCount, \
GM_ADDR scale, int64_t scaleCount, GM_ADDR offset

#define KERNELS_ARGS_CALL() \
input, output, commArgs, len, magic, op, root, cycleCount, scale, scaleCount, offset

namespace tilexr {
namespace checker {

inline SourceLocation AllReduceBigDataLoc(int line) {
    return SourceLocation{"src/collectives/kernels/allreduce_big_data.h", line};
}

static inline SourceLocation TraceSourceLoc(int line) {
    return SourceLocation{TILEXR_CHECKER_TRACE_SOURCE_FILE, line};
}

static inline SourceLocation AllReduceQuantLoc(int line) {
    return SourceLocation{"src/collectives/kernels/allreduce_quant.h", line};
}

static inline SourceLocation CollectivesLoc(int line) {
    return SourceLocation{"src/collectives/kernels/collectives.h", line};
}

inline uint64_t MergeSyncMagic(int32_t magic, int32_t value) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(magic)) << 32) |
           static_cast<uint32_t>(value);
}

inline int64_t CompletedSyncValue() {
    auto *runtime = TraceRuntime::Current();
    const uint64_t magic = runtime == nullptr ? 0 : static_cast<uint64_t>(runtime->test_case().magic);
    return static_cast<int64_t>((magic << 32) | 0xFFFFFFFFULL);
}

struct TraceAdapterHookObservation {
    const char *symbol;
    const int *lines;
    int line_count;
};

struct TraceAdapterMetadata {
    const char *adapter_name;
    const char *source_file;
    const char *target_header;
    const char *include_guard;
    const TraceAdapterHookObservation *known_hooks;
    int known_hook_count;
    const TraceAdapterHookObservation *manual_review_candidates;
    int manual_review_candidate_count;
};

inline bool AuditHookObservationArray(const TraceAdapterHookObservation *hooks,
                                      int hook_count, const char **reason)
{
    if (hook_count < 0) {
        if (reason != nullptr) {
            *reason = "negative hook count";
        }
        return false;
    }
    if (hook_count > 0 && hooks == nullptr) {
        if (reason != nullptr) {
            *reason = "missing hook observation array";
        }
        return false;
    }
    for (int i = 0; i < hook_count; ++i) {
        if (hooks[i].symbol == nullptr || hooks[i].symbol[0] == '\0') {
            if (reason != nullptr) {
                *reason = "missing hook symbol";
            }
            return false;
        }
        if (hooks[i].line_count <= 0 || hooks[i].lines == nullptr) {
            if (reason != nullptr) {
                *reason = "missing hook line list";
            }
            return false;
        }
        for (int line_index = 0; line_index < hooks[i].line_count; ++line_index) {
            if (hooks[i].lines[line_index] <= 0) {
                if (reason != nullptr) {
                    *reason = "invalid hook line";
                }
                return false;
            }
        }
    }
    return true;
}

inline bool AuditTraceAdapterMetadata(const TraceAdapterMetadata &metadata,
                                      const char **reason)
{
    if (reason != nullptr) {
        *reason = nullptr;
    }
    if (metadata.adapter_name == nullptr || metadata.adapter_name[0] == '\0' ||
        metadata.source_file == nullptr || metadata.source_file[0] == '\0' ||
        metadata.target_header == nullptr || metadata.target_header[0] == '\0' ||
        metadata.include_guard == nullptr || metadata.include_guard[0] == '\0') {
        if (reason != nullptr) {
            *reason = "missing adapter metadata string";
        }
        return false;
    }
    if (!AuditHookObservationArray(metadata.known_hooks, metadata.known_hook_count, reason)) {
        return false;
    }
    return AuditHookObservationArray(metadata.manual_review_candidates,
                                     metadata.manual_review_candidate_count, reason);
}

}  // namespace checker
}  // namespace tilexr

class SyncCollectives {
public:
    FORCE_INLINE_AICORE SyncCollectives() {}
    FORCE_INLINE_AICORE void Init(int rank, int rankSize, GM_ADDR *shareAddrs)
    {
        (void)rank;
        (void)rankSize;
        (void)shareAddrs;
    }
    FORCE_INLINE_AICORE int32_t CalEventIdByMulBlockNum(int32_t blockMultiplier, int32_t targetCoreId)
    {
        return blockMultiplier * GetBlockNum() + targetCoreId;
    }
    FORCE_INLINE_AICORE void SetSyncFlag(int32_t magic, int32_t value, int32_t eventID, int32_t rank)
    {
        SetSyncFlagTrace(magic, value, eventID, rank, tilexr::checker::TraceSourceLoc(0));
    }
    FORCE_INLINE_AICORE void SetSyncFlagTrace(int32_t magic, int32_t value, int32_t eventID,
                                              int32_t rank, tilexr::checker::SourceLocation loc)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->StoreFlag(rank, -1, eventID,
                               tilexr::checker::MergeSyncMagic(magic, value),
                               loc, "trace SetSyncFlag");
        }
    }
    FORCE_INLINE_AICORE void WaitSyncFlag(int32_t magic, int32_t value, int32_t eventID, int32_t rank,
        bool breakCycle = true)
    {
        (void)breakCycle;
        WaitSyncFlagTrace(magic, value, eventID, rank, tilexr::checker::TraceSourceLoc(0));
    }
    FORCE_INLINE_AICORE void WaitSyncFlagTrace(int32_t magic, int32_t value, int32_t eventID,
                                               int32_t rank, tilexr::checker::SourceLocation loc)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->WaitFlag(runtime->rank(), rank, eventID,
                              tilexr::checker::MergeSyncMagic(magic, value),
                              loc, "trace WaitSyncFlag");
        }
    }
    FORCE_INLINE_AICORE void SetInnerFlag(int32_t magic, int32_t eventID)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->StoreFlag(runtime->rank(), -1, runtime->block_idx(),
                               tilexr::checker::MergeSyncMagic(magic, eventID),
                               tilexr::checker::TraceSourceLoc(130),
                               "trace SetInnerFlag");
        }
    }
    FORCE_INLINE_AICORE void WaitInnerFlag(int32_t magic, int32_t eventID, int64_t waitRank, int64_t waitBlock)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->WaitFlag(runtime->rank(), static_cast<int>(waitRank), static_cast<int>(waitBlock),
                              tilexr::checker::MergeSyncMagic(magic, eventID),
                              tilexr::checker::TraceSourceLoc(144),
                              "trace WaitInnerFlag");
        }
    }
    FORCE_INLINE_AICORE void SetOuterFlag(int32_t magic, int32_t eventID)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->StoreFlag(runtime->rank(), -1, runtime->block_idx(),
                               tilexr::checker::MergeSyncMagic(magic, eventID),
                               tilexr::checker::TraceSourceLoc(157),
                               "trace SetOuterFlag");
        }
    }
    FORCE_INLINE_AICORE void WaitOneRankPartOuterFlag(int32_t magic, int32_t eventID, int64_t waitRank,
        int64_t startBlock, int64_t flagNum)
    {
        (void)startBlock;
        (void)flagNum;
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            for (int64_t block = 0; block < flagNum; ++block) {
                runtime->WaitFlag(runtime->rank(), static_cast<int>(waitRank),
                                  static_cast<int>(startBlock + block),
                                  tilexr::checker::MergeSyncMagic(magic, eventID),
                                  tilexr::checker::TraceSourceLoc(161),
                                  "trace WaitOneRankPartOuterFlag");
            }
        }
    }
    FORCE_INLINE_AICORE int64_t GetInnerFlag(int64_t waitRank, int64_t waitBlock)
    {
        (void)waitRank;
        (void)waitBlock;
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            return tilexr::checker::CompletedSyncValue();
        }
        return 0;
    }
    FORCE_INLINE_AICORE int64_t GetOuterFlag(int64_t waitRank, int64_t waitBlock)
    {
        (void)waitRank;
        (void)waitBlock;
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            return tilexr::checker::CompletedSyncValue();
        }
        return 0;
    }

    FORCE_INLINE_AICORE void SetInnerFlag(int32_t magic, int32_t eventID, int64_t setRank, int64_t setBlock)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->StoreFlag(runtime->rank(), static_cast<int>(setRank), static_cast<int>(setBlock),
                               tilexr::checker::MergeSyncMagic(magic, eventID),
                               tilexr::checker::TraceSourceLoc(130),
                               "trace SetInnerFlag");
        }
    }

    FORCE_INLINE_AICORE void SetOuterFlag(int32_t magic, int32_t eventID, int64_t setRank, int64_t setBlock)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->StoreFlag(runtime->rank(), static_cast<int>(setRank), static_cast<int>(setBlock),
                               tilexr::checker::MergeSyncMagic(magic, eventID),
                               tilexr::checker::TraceSourceLoc(161),
                               "trace SetOuterFlag");
        }
    }
};

template <typename T>
class IpcQueue {
public:
    FORCE_INLINE_AICORE IpcQueue() {}
    FORCE_INLINE_AICORE void Init(SyncCollectives *sync, int64_t magic, GM_ADDR workSpace,
                                  uint64_t bufferNum, uint64_t blockNum)
    {
        (void)sync;
        (void)magic;
        (void)bufferNum;
        block_num_ = blockNum == 0 ? 1 : blockNum;
        buff_.SetGlobalBuffer(reinterpret_cast<__gm__ T *>(workSpace), bufferNum);
    }
    FORCE_INLINE_AICORE void DeQue(int *, int *, int) {}
    FORCE_INLINE_AICORE void DeQue(int, int = -1) {}
    FORCE_INLINE_AICORE GlobalTensor<T> EnQue()
    {
        GlobalTensor<T> tensor = buff_[static_cast<int64_t>(rear_ * block_num_)];
        ++rear_;
        return tensor;
    }
    FORCE_INLINE_AICORE GlobalTensor<T> ReadFront()
    {
        GlobalTensor<T> tensor = buff_[static_cast<int64_t>(front_ * block_num_)];
        ++front_;
        return tensor;
    }

private:
    uint64_t block_num_ = 1;
    uint64_t front_ = 0;
    uint64_t rear_ = 0;
    GlobalTensor<T> buff_;
};

class Collectives {
public:
    FORCE_INLINE_AICORE Collectives(int rank, int rankSize, uint32_t extraFlag)
        : rank(rank), rankSize(rankSize), extraFlag(extraFlag) {}

    FORCE_INLINE_AICORE void Init(KERNELS_ARGS_FUN())
    {
        this->len = len;
        this->magic = magic;
        this->root = root;
        this->blockIdx = GetBlockIdx();
        this->blockNum = GetBlockNum();
        __gm__ CommArgs *localArgs = reinterpret_cast<__gm__ CommArgs *>(commArgs);
        if (localArgs != nullptr) {
            for (int i = 0; i < rankSize; ++i) {
                shareAddrs[i] = localArgs->peerMems[i];
            }
        }
        sync.Init(rank, rankSize, shareAddrs);
    }

    template <typename T1, typename T2>
    FORCE_INLINE_AICORE T1 CeilDiv(T1 a, T2 b)
    {
        if (b == 0) {
            return 0;
        }
        return (a + b - 1) / b;
    }

    template <typename T, typename U = T>
    FORCE_INLINE_AICORE void CpGM2GMPingPong(int64_t dataSizeRemain,
                                            const GlobalTensor<U>& inputGT,
                                            const GlobalTensor<T>& outputGT,
                                            int op)
    {
        CpGM2GMPingPongTrace(dataSizeRemain, inputGT, outputGT, op,
                             tilexr::checker::TraceSourceLoc(129));
    }

    template <typename T, typename U = T>
    FORCE_INLINE_AICORE void CpGM2GMPingPongTrace(int64_t dataSizeRemain,
                                                 const GlobalTensor<U>& inputGT,
                                                 const GlobalTensor<T>& outputGT,
                                                 int op,
                                                 tilexr::checker::SourceLocation loc)
    {
        RecordCollectivesPingPongPipeTrace(dataSizeRemain, op);
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime != nullptr) {
            runtime->RecordCopy(outputGT.GetPhyAddr(), inputGT.GetPhyAddr(),
                                static_cast<size_t>(dataSizeRemain), op,
                                loc,
                                "trace CpGM2GMPingPong");
        }
    }

    FORCE_INLINE_AICORE void DumpLcclLogInfo(LogId, Op) {}

protected:
    FORCE_INLINE_AICORE void RecordCollectivesPingPongPipeTrace(int64_t dataSizeRemain, int op)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime == nullptr || dataSizeRemain <= 0) {
            return;
        }
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_ALL, -1,
                                 tilexr::checker::CollectivesLoc(442),
                                 "trace Collectives::SetAtomic entry barrier");
        if (op != -1) {
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_ALL, -1,
                                     tilexr::checker::CollectivesLoc(448),
                                     "trace Collectives::SetAtomic exit barrier");
        } else {
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_ALL, -1,
                                     tilexr::checker::CollectivesLoc(448),
                                     "trace Collectives::SetAtomic exit barrier");
        }
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID0,
                                 tilexr::checker::CollectivesLoc(268),
                                 "trace Collectives::CpGM2GMPingPong init event0");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID1,
                                 tilexr::checker::CollectivesLoc(269),
                                 "trace Collectives::CpGM2GMPingPong init event1");

        constexpr int64_t kAbstractChunkBytes = 1024 * 1024;
        for (int64_t i = 0, remain = dataSizeRemain; remain > 0; ++i) {
            const int event_id = (i & 1) ? EVENT_ID0 : EVENT_ID1;
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, event_id,
                                     tilexr::checker::CollectivesLoc(273),
                                     "trace Collectives::CpGM2GMPingPong wait writeback pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, event_id,
                                     tilexr::checker::CollectivesLoc(281),
                                     "trace Collectives::CpGM2GMPingPong set copy pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, event_id,
                                     tilexr::checker::CollectivesLoc(282),
                                     "trace Collectives::CpGM2GMPingPong wait copy pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, event_id,
                                     tilexr::checker::CollectivesLoc(284),
                                     "trace Collectives::CpGM2GMPingPong set writeback pipe");
            remain -= kAbstractChunkBytes;
        }
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID0,
                                 tilexr::checker::CollectivesLoc(289),
                                 "trace Collectives::CpGM2GMPingPong final wait event0");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID1,
                                 tilexr::checker::CollectivesLoc(290),
                                 "trace Collectives::CpGM2GMPingPong final wait event1");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID3,
                                 tilexr::checker::CollectivesLoc(292),
                                 "trace Collectives::CpGM2GMPingPong set scalar pipe");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID3,
                                 tilexr::checker::CollectivesLoc(293),
                                 "trace Collectives::CpGM2GMPingPong wait scalar pipe");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_ALL, -1,
                                 tilexr::checker::CollectivesLoc(456),
                                 "trace Collectives::UnsetAtomic barrier");
    }

    int rank;
    int rankSize;
    uint32_t extraFlag;
    int root = 0;
    int64_t len = 0;
    int64_t magic = 0;
    int64_t blockIdx = 0;
    int64_t blockNum = 1;
    GM_ADDR shareAddrs[TILEXR_MAX_RANK_SIZE] = {};
    SyncCollectives sync;
};

FORCE_INLINE_AICORE int64_t GetDataCount(const int64_t dataLen, const int64_t useBlockNum)
{
    return useBlockNum == 0 ? 0 : dataLen / useBlockNum;
}

class AllReduceQuant : protected Collectives {
public:
    FORCE_INLINE_AICORE AllReduceQuant(int rank, int rankSize, uint32_t extraFlag)
        : Collectives(rank, rankSize, extraFlag) {}

protected:
    FORCE_INLINE_AICORE void RecordSmallScalePipeTrace(int64_t dataSizeRemain)
    {
        auto *runtime = tilexr::checker::TraceRuntime::Current();
        if (runtime == nullptr) {
            return;
        }
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID0,
                                 tilexr::checker::AllReduceQuantLoc(189),
                                 "trace AllReduceQuant small-scale init event0");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID1,
                                 tilexr::checker::AllReduceQuantLoc(190),
                                 "trace AllReduceQuant small-scale init event1");
        constexpr int64_t kAbstractChunkBytes = 1024 * 1024;
        int64_t remain = dataSizeRemain <= 0 ? 1 : dataSizeRemain;
        for (int64_t i = 0; remain > 0; ++i) {
            const int event_id = (i & 1) ? EVENT_ID0 : EVENT_ID1;
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, event_id,
                                     tilexr::checker::AllReduceQuantLoc(195),
                                     "trace AllReduceQuant small-scale wait writeback pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_V, event_id,
                                     tilexr::checker::AllReduceQuantLoc(197),
                                     "trace AllReduceQuant small-scale set vector pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_V, event_id,
                                     tilexr::checker::AllReduceQuantLoc(197),
                                     "trace AllReduceQuant small-scale wait vector pipe");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_V, -1,
                                     tilexr::checker::AllReduceQuantLoc(199),
                                     "trace AllReduceQuant small-scale vector barrier");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeBarrier, PIPE_V, -1,
                                     tilexr::checker::AllReduceQuantLoc(201),
                                     "trace AllReduceQuant small-scale vector barrier");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_V, event_id,
                                     tilexr::checker::AllReduceQuantLoc(203),
                                     "trace AllReduceQuant small-scale set vector writeback");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_V, event_id,
                                     tilexr::checker::AllReduceQuantLoc(203),
                                     "trace AllReduceQuant small-scale wait vector writeback");
            runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, event_id,
                                     tilexr::checker::AllReduceQuantLoc(205),
                                     "trace AllReduceQuant small-scale set writeback pipe");
            remain -= kAbstractChunkBytes;
        }
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID0,
                                 tilexr::checker::AllReduceQuantLoc(209),
                                 "trace AllReduceQuant small-scale final wait event0");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID1,
                                 tilexr::checker::AllReduceQuantLoc(210),
                                 "trace AllReduceQuant small-scale final wait event1");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeSet, PIPE_ALL, EVENT_ID3,
                                 tilexr::checker::AllReduceQuantLoc(211),
                                 "trace AllReduceQuant small-scale set scalar pipe");
        runtime->RecordPipeEvent(tilexr::checker::EventKind::kPipeWait, PIPE_ALL, EVENT_ID3,
                                 tilexr::checker::AllReduceQuantLoc(211),
                                 "trace AllReduceQuant small-scale wait scalar pipe");
    }

    template <typename T, typename U>
    FORCE_INLINE_AICORE void CpGM2GMPingPong(int64_t dataSizeRemain,
                                            const GlobalTensor<U>& inputGT,
                                            const GlobalTensor<T>& outputGT,
                                            int op, T, T)
    {
        RecordSmallScalePipeTrace(dataSizeRemain);
        Collectives::CpGM2GMPingPong(dataSizeRemain, inputGT, outputGT, op);
    }

    template <typename T, typename U>
    FORCE_INLINE_AICORE void CpGM2GMPingPong(int64_t dataSizeRemain,
                                            const GlobalTensor<U>& inputGT,
                                            const GlobalTensor<T>& outputGT,
                                            int op, const GlobalTensor<T>&,
                                            int64_t, T)
    {
        RecordSmallScalePipeTrace(dataSizeRemain);
        Collectives::CpGM2GMPingPong(dataSizeRemain, inputGT, outputGT, op);
    }
};

#endif  // TILEXR_CHECKER_COLLECTIVE_TRACE_SHIM_H
