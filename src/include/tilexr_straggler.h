/*
 * 功能：声明诊断 session、clock 校准、drift 校正、sleep_at 和观测接口及数据布局。
 * 调用关系：应用和通信算子调用这些 Host 接口，diagnostics 模块实现采样和分析。
 * 资源与生命周期：调用方持有 communicator 和 stream，在创建线程使用 session 的 opaque handle，
 *   并在销毁 communicator 前调用 session 销毁接口。
 */
#ifndef TILEXR_STRAGGLER_H
#define TILEXR_STRAGGLER_H

#include <cstdint>

#include "acl/acl_base.h"
#include "tilexr_api.h"
#include "tilexr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TILEXR_STRAGGLER_MAX_RANKS 128

typedef void *TileXRStraggler;

typedef enum TileXRStragglerClassification {
    TILEXR_STRAGGLER_UNKNOWN = 0,
    TILEXR_STRAGGLER_NORMAL = 1,
    TILEXR_STRAGGLER_SLOW = 2,
    TILEXR_STRAGGLER_FAST = 3,
} TileXRStragglerClassification;

typedef enum TileXRStragglerReason {
    TILEXR_STRAGGLER_REASON_NONE = 0,
    TILEXR_STRAGGLER_REASON_DURATION = 1u << 0,
    TILEXR_STRAGGLER_REASON_LATE_START = 1u << 1,
    TILEXR_STRAGGLER_REASON_CLOCK_UNSYNCHRONIZED = 1u << 2,
    TILEXR_STRAGGLER_REASON_INCOMPLETE = 1u << 3,
    TILEXR_STRAGGLER_REASON_LAUNCH_MISMATCH = 1u << 4,
    TILEXR_STRAGGLER_REASON_OBSERVATION_ERROR = 1u << 5,
    TILEXR_STRAGGLER_REASON_COMPLETION = 1u << 6,
    TILEXR_STRAGGLER_REASON_EARLY_START = 1u << 7,
    TILEXR_STRAGGLER_REASON_PRE_COMMUNICATION = 1u << 8,
} TileXRStragglerReason;

typedef enum TileXRClockFrequencySource {
    TILEXR_CLOCK_FREQUENCY_UNKNOWN = 0,
    TILEXR_CLOCK_FREQUENCY_CANN_PRODUCT_RULE = 1,
} TileXRClockFrequencySource;

typedef enum TileXRStragglerComponentKind {
    TILEXR_STRAGGLER_COMPONENT_UNKNOWN = 0,
    TILEXR_STRAGGLER_COMPONENT_COLLECTIVES = 1,
    TILEXR_STRAGGLER_COMPONENT_EP = 2,
    TILEXR_STRAGGLER_COMPONENT_MOONEP = 3,
    TILEXR_STRAGGLER_COMPONENT_USER = 0x10000,
} TileXRStragglerComponentKind;

typedef struct TileXRStragglerConfig {
    uint32_t clockSyncSamples;
    uint32_t clockSyncWarmupSamples;
    uint32_t recordCapacity;
    double relativeThreshold;
    double madThreshold;
    uint64_t absoluteThresholdNs;
    uint32_t timeoutMs;
    uint32_t maxCalibrationAgeMs;
    const char *outputDir;
} TileXRStragglerConfig;

typedef struct TileXRClockRankResult {
    int32_t rank;
    int32_t status;
    // Logical rank 0 minus local estimate; add to local timestamps. Unbiased only
    // under symmetric effective one-way delays (including endpoint overhead).
    int64_t offsetCycles;
    uint64_t minRoundTripCycles;
    uint64_t medianRoundTripCycles;
    // ceil(minimum valid effective RTT / 2), not a measured clock residual.
    uint64_t errorBoundCycles;
    int64_t offsetNs;
    uint64_t minRoundTripNs;
    uint64_t medianRoundTripNs;
    uint64_t errorBoundNs; // Converted model bound; path symmetry is assumed, not verified.
    uint32_t validSamples;
    uint32_t rejectedSamples;
} TileXRClockRankResult;

typedef struct TileXRClockSyncResult {
    uint32_t version;
    uint32_t rankSize;
    uint32_t sampleCount;
    uint32_t generation;
    uint32_t frequencySource;
    uint32_t warmupSampleCount;
    uint64_t cyclesPerSecond;
    // Maximum per-rank errorBound, not measured post-synchronization accuracy.
    uint64_t precisionCycles;
    uint64_t precisionNs;
    TileXRClockRankResult ranks[TILEXR_STRAGGLER_MAX_RANKS];
} TileXRClockSyncResult;

typedef enum TileXRClockDriftState {
    TILEXR_CLOCK_DRIFT_DISABLED = 0,
    TILEXR_CLOCK_DRIFT_COLLECTING = 1,
    TILEXR_CLOCK_DRIFT_READY = 2,
    TILEXR_CLOCK_DRIFT_INSUFFICIENT = 3,
    TILEXR_CLOCK_DRIFT_UNSTABLE = 4,
} TileXRClockDriftState;

// Separate extension: existing clock/config/result layouts remain unchanged.
// t_rank0 = t_local + offsetCycles + rateCorrection * (t_local-anchorCycle).
// The reference is communicator rank 0, independent of its physical device ID.
// rateCorrection is reference-minus-local cycles per local cycle, NOT ns/s.
// In opt-in tracking mode, anchor/offset/rate and trainedAtGeneration describe
// the latest accepted feedback update; sample/span/RMS/SE retain initial-fit metadata.
// This estimate still assumes symmetric effective forward/reverse delays.
typedef struct TileXRClockDriftModel {
    uint32_t version;
    uint32_t state;
    uint64_t anchorCycle;
    double offsetCycles;
    double rateCorrection;
    uint64_t spanCycles;
    uint64_t cyclesPerSecond;
    uint32_t sampleCount;
    uint32_t rejectedSamples;
    double fitRmsCycles;
    double rateStandardError;
    uint64_t requestedWindowUs;
    uint32_t trainedAtGeneration;
    int32_t rank;
} TileXRClockDriftModel;

typedef struct TileXRStragglerRankObservation {
    int32_t rank;
    int32_t status;
    uint32_t classification;
    uint32_t reasonFlags;
    uint64_t localStartCycle;
    uint64_t localReadyCycle;
    uint64_t localEndCycle;
    int64_t alignedStartNs;
    int64_t alignedReadyNs;
    int64_t alignedEndNs;
    uint64_t durationNs;
    uint64_t preCommunicationNs;
    int64_t durationDeltaNs;
    int64_t preCommunicationDeltaNs;
    int64_t startDeltaNs;
    int64_t completionDeltaNs;
    double relativeDelta;
    double robustScore;
    double startRobustScore;
    double completionRobustScore;
    double preCommunicationRobustScore;
    uint32_t blockCount;
    uint32_t reserved;
} TileXRStragglerRankObservation;

typedef struct TileXRStragglerOperation {
    uint32_t componentKind;
    uint32_t operationKind;
    uint32_t stageId;
    uint32_t reserved;
    uint64_t operationId;
    uint64_t messageBytes;
} TileXRStragglerOperation;

typedef struct TileXRStragglerObservation {
    void *detector;
    void *deviceRecord;
    uint64_t launchId;
    uint64_t messageBytes;
    uint64_t delayCycles;
    uint64_t operationId;
    uint32_t generation;
    uint32_t componentKind;
    uint32_t operationKind;
    uint32_t stageId;
    uint32_t rank;
    uint32_t reserved;
} TileXRStragglerObservation;

typedef struct TileXRStragglerReport {
    uint32_t version;
    uint32_t rankSize;
    uint32_t componentKind;
    uint32_t operationKind;
    uint32_t stageId;
    uint32_t calibrationGeneration;
    uint64_t launchId;
    uint64_t operationId;
    uint64_t messageBytes;
    uint64_t medianDurationNs;
    uint64_t madDurationNs;
    uint64_t clockPrecisionNs;
    uint64_t droppedRecords;
    uint64_t calibrationAgeNs;
    uint64_t absoluteThresholdNs;
    uint64_t medianPreCommunicationNs;
    uint64_t madPreCommunicationNs;
    double relativeThreshold;
    double madThreshold;
    uint32_t complete;
    uint32_t reserved;
    TileXRStragglerRankObservation ranks[TILEXR_STRAGGLER_MAX_RANKS];
} TileXRStragglerReport;

int TileXRStragglerConfigDefault(TileXRStragglerConfig *config);
int TileXRStragglerCreate(TileXRCommPtr comm,
                          const TileXRStragglerConfig *config,
                          TileXRStraggler *detector);
/* With TILEXR_CLOCK_TRANSPORT=rdma, synchronization and destruction are
 * collective. Quiesce other users of the communicator's RDMA QPs first. A
 * failed clock may close the entire RDMA transport during destroy, invalidating
 * its other RDMA handles/views. Failed destruction retains the session to retry. */
int TileXRStragglerDestroy(TileXRStraggler detector);
int TileXRClockSynchronize(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRClockSyncResult *result);
/* Opt in at create with TILEXR_CLOCK_DRIFT_WINDOW_US (1..60000000).
 * Repeated synchronization calls collect timestamp exchanges until that span;
 * the first exchange crossing the requested span closes training. Coefficients
 * then remain frozen for this detector. Too few/noisy samples are reported as
 * INSUFFICIENT/UNSTABLE, never silently applied. No extra NPU work is launched
 * by either function below. Existing precisionNs retains its RTT/2 meaning. */
/* TILEXR_CLOCK_DRIFT_TRACK=1 opts into 10-second initial training followed by
 * feedback on later synchronization calls. Requires outputDir and >=64 samples;
 * any explicit drift window must be 10000000 us. The caller schedules collective
 * calls every ~60 seconds on the owner thread with pending observations drained.
 * Two 16-sample groups must agree on a signed residual >1000 ns before offset
 * re-anchoring and a 0.25-gain rate adjustment. Elapsed time starts at the last
 * accepted model anchor, not the last probe. Per-generation files and
 * clock_drift_latest.json persist local samples and all ranks' active models.
 * Successful probes refresh validation age; model generation changes only on
 * accepted updates. Corrected timestamps can step at a model update boundary.
 * 单次长窗口训练后每约60秒集合短测；残差确认超阈值后更新并落盘，时间可能在更新点跳变。 */
int TileXRClockGetDriftModel(TileXRStraggler detector, int rank,
                             TileXRClockDriftModel *model);
int TileXRClockCorrectCycle(const TileXRClockDriftModel *model,
                            uint64_t localCycle, int64_t *referenceCycle);
/* Enqueue one AIV busy wait on stream until an absolute logical-rank-0 cycle.
 * referenceCycle uses the signed cycle domain of TileXRClockCorrectCycle.
 * Snapshots this rank's READY drift model at invocation; no collective,
 * calibration or Host stream synchronization is performed. The inverse is
 * rounded up, so integer rounding cannot advance the model-relative deadline.
 * A deadline already passed when the kernel starts exits immediately.
 * Returns NOT_INITIALIZED for an unready model, PARA_CHECK_FAIL for invalid
 * arguments/model or an unrepresentable local cycle, and propagates launch
 * errors. Later execution errors are reported by stream synchronization.
 * Use on the detector's creating thread with its device/context current.
 * Keep the borrowed stream alive until the next clock synchronization or
 * detector destruction; both drain pending waits before changing session state.
 * Waiting consumes one AIV and is subject to the runtime task timeout. It
 * orders later work on this stream only; model error/drift and scheduling
 * delays remain. Later model updates do not change an enqueued deadline. */
int TileXRClockSleepAt(TileXRStraggler detector, int64_t referenceCycle, aclrtStream stream);
int TileXRStragglerObservationBegin(TileXRStraggler detector,
                                    const TileXRStragglerOperation *operation,
                                    aclrtStream stream,
                                    TileXRStragglerObservation *observation);
int TileXRStragglerObservationBeginForComm(TileXRCommPtr comm,
                                           const TileXRStragglerOperation *operation,
                                           aclrtStream stream,
                                           TileXRStragglerObservation *observation);
int TileXRStragglerObservationEnd(TileXRStraggler detector,
                                  const TileXRStragglerObservation *observation,
                                  aclrtStream stream,
                                  int launchStatus);
int TileXRStragglerCollect(TileXRStraggler detector,
                            aclrtStream stream,
                            TileXRStragglerReport *report);

#ifdef __cplusplus
}
#endif

#endif // TILEXR_STRAGGLER_H
