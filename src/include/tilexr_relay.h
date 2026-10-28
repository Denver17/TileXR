/* Copyright (c) 2026 TileXR Project. Licensed under Apache-2.0. */
/* Operator-independent C ABI for immutable Relay plans and prepared executions.
 * Descriptors declare concrete memory/ABI requirements; handles own Host state only.
 * Device allocations, communicators, contexts and streams remain caller-owned.
 * Execution is synchronous; kernel generation is outside this runtime API. */
#ifndef TILEXR_RELAY_H
#define TILEXR_RELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TILEXR_RELAY_VERSION 1u
#define TILEXR_RELAY_SLOT_COUNT 16u
#define TILEXR_RELAY_FLAG_PLANE_BYTES 1048576u
#define TILEXR_RELAY_FLAG_SLOT_BYTES 64u

typedef struct TileXRRelayPlanImpl *TileXRRelayPlan;
typedef struct TileXRRelayExecutionImpl *TileXRRelayExecution;

typedef enum TileXRRelaySource {
    TILEXR_RELAY_UNUSED = 0,
    TILEXR_RELAY_USER = 1,
    TILEXR_RELAY_WORKSPACE = 2,
    TILEXR_RELAY_OWNER_PAYLOAD = 3,
    TILEXR_RELAY_PEER_PAYLOAD = 4,
    TILEXR_RELAY_OWNER_FLAGS = 5,
    TILEXR_RELAY_PEER_FLAGS = 6
} TileXRRelaySource;

typedef enum TileXRRelayRegionSpace {
    TILEXR_RELAY_PRIVATE = 0,
    TILEXR_RELAY_PAYLOAD = 1
} TileXRRelayRegionSpace;

typedef enum TileXRRelayInitialization {
    TILEXR_RELAY_UNINITIALIZED = 0,
    TILEXR_RELAY_ZERO_ONCE = 1
} TileXRRelayInitialization;

/* Tags are caller-defined stable integers; zero is a valid byte-buffer tag. */
typedef struct TileXRRelayBindingSpec {
    uint64_t bytes;
    uint32_t alignment;
    uint32_t dtypeTag;
    uint32_t layoutTag;
    uint32_t reserved;
} TileXRRelayBindingSpec;

/* All declared regions are live for the launch and must be disjoint per space.
 * ZERO_ONCE is an adapter obligation, never a per-launch Host memset. */
typedef struct TileXRRelayRegion {
    uint64_t offset;
    uint64_t bytes;
    uint32_t alignment;
    uint32_t space;
    uint32_t initialization;
    uint32_t reserved;
} TileXRRelayRegion;

/* The same disjoint slot interval is reserved in publication and ACK planes.
 * V1 supports two participants and one outstanding credit per channel. */
typedef struct TileXRRelayChannel {
    uint32_t firstSlot;
    uint32_t slotCount;
    uint32_t participants;
    uint32_t maxCredits;
} TileXRRelayChannel;

/* USER index addresses a binding; WORKSPACE/PAYLOAD index addresses a region.
 * Flag slots use index=0 and offsets from the beginning of the two fixed planes.
 * UNUSED must have every other field zero. */
typedef struct TileXRRelaySlot {
    uint32_t source;
    uint32_t index;
    uint64_t offset;
    uint64_t bytes;
    uint32_t alignment;
    uint32_t reserved;
} TileXRRelaySlot;

typedef struct TileXRRelayPlanDesc {
    uint32_t structSize;
    uint32_t version;
    uint32_t abiVersion;
    uint32_t protocolVersion;
    /* Build-time content identity: tilexr_relay_<lowercase SHA256>.o.
     * Installed binaries must remain immutable; runtime does not rehash them. */
    const char *fileName;
    const char *sha256;
    const char *entry;
    /* Supported target family is "Ascend910B"; exact revisions match a prefix. */
    const char *target;
    uint32_t blocks;
    uint32_t rankCount;
    uint64_t workspaceBytes;
    uint64_t peerPayloadBytes;
    uint32_t bindingCount;
    uint32_t regionCount;
    uint32_t channelCount;
    uint32_t reserved;
    const TileXRRelayBindingSpec *bindings;
    const TileXRRelayRegion *regions;
    const TileXRRelayChannel *channels;
    TileXRRelaySlot slots[TILEXR_RELAY_SLOT_COUNT];
} TileXRRelayPlanDesc;

typedef struct TileXRRelayBinding {
    void *data;
    uint64_t bytes;
    uint32_t dtypeTag;
    uint32_t layoutTag;
} TileXRRelayBinding;

/* Creation copies all borrowed descriptor data, without allocating device memory.
 * Descriptors are immutable. Handle use/destruction must not race other calls. */
int TileXRRelayPlanCreate(const TileXRRelayPlanDesc *description, TileXRRelayPlan *plan);
int TileXRRelayPlanGetWorkspaceSize(TileXRRelayPlan plan, uint64_t *bytes);
/* Caller owns truthful allocation sizes/current device, matching rank plan/order,
 * zero-once initialization and exclusive communicator use. Launch is synchronous.
 * After an attempted launch fails, the plan is poisoned: stop the process before
 * freeing/reusing device storage. Destroy only releases Host descriptor ownership,
 * never borrowed buffers or registered process-lifetime code. */
int TileXRRelayLaunch(TileXRRelayPlan plan, void *comm,
    const TileXRRelayBinding *bindings, uint32_t bindingCount,
    void *workspace, uint64_t workspaceBytes, void *stream);
int TileXRRelayPlanDestroy(TileXRRelayPlan plan);

/* Freeze validated buffer addresses, communicator mappings, current context/device
 * and registered code. Creation neither launches nor consumes a generation.
 * Caller-owned memory, communicator and context must remain alive and address-
 * stable through execution destruction; contents may change between launches.
 * Pointer checks cannot detect freed/reused allocations or communicators. Context
 * reset/destruction is unsupported while executions are alive. New addresses
 * require a new execution; launch never silently rebinds or falls back. */
int TileXRRelayExecutionCreate(TileXRRelayPlan plan, void *comm,
    const TileXRRelayBinding *bindings, uint32_t bindingCount,
    void *workspace, uint64_t workspaceBytes, TileXRRelayExecution *execution);
/* Synchronous, with a fresh epoch and FFTS state every time. Executions and the
 * original launch API share their parent plan's serialization and poison state.
 * Plan destruction invalidates child launches, but child destruction remains safe.
 * No use/destruction of the same public handle may race; separate plans do not
 * isolate concurrent use of a communicator. */
int TileXRRelayExecutionLaunch(TileXRRelayExecution execution, void *stream);
/* Releases Host state only, never borrowed memory or registered code. */
int TileXRRelayExecutionDestroy(TileXRRelayExecution execution);

#ifdef __cplusplus
}
#endif
#endif
