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
    int64_t offsetCycles;
    uint64_t minRoundTripCycles;
    uint64_t medianRoundTripCycles;
    uint64_t errorBoundCycles;
    int64_t offsetNs;
    uint64_t minRoundTripNs;
    uint64_t medianRoundTripNs;
    uint64_t errorBoundNs;
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
    uint64_t precisionCycles;
    uint64_t precisionNs;
    TileXRClockRankResult ranks[TILEXR_STRAGGLER_MAX_RANKS];
} TileXRClockSyncResult;

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
int TileXRStragglerDestroy(TileXRStraggler detector);
int TileXRClockSynchronize(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRClockSyncResult *result);
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
