/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：定义 Host/AIV 共享的 view、queue、state、SGE 布局和状态码，不依赖 CANN kernel 头文件。
 * 调用关系：Host provider 填写这些结构，device/wire/write 层读取并使用其中的地址和 key。
 * 资源与生命周期：结构只描述资源；复制 view 不会复制或接管底层 MR、QP 或 state。
 *
 * Purpose: Define Host/AIV view, queue, state, SGE layouts and status codes without CANN kernel headers.
 * Calls: Host providers fill these structures; device/wire/write code consumes their addresses and keys.
 * Lifetime: These are resource descriptors; copying a view neither copies nor takes ownership of the MR, QP or state.
 */
#ifndef TILEXR_RDMA_DEVICE_TYPES_H
#define TILEXR_RDMA_DEVICE_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Host/device ABI: fixed-width, pointer-free PODs. All addresses are device VAs.
 * The provider must validate actual NDA resources and this queue ABI before
 * exporting a view. These structures do not enable a capability or select a Host fallback. */
#define TILEXR_RDMA_DEVICE_VERSION 2U
#define TILEXR_RDMA_QUEUE_ABI_UBNIC_V1 1U
#define TILEXR_RDMA_QUEUE_ABI_UBNIC_V2 2U
#define TILEXR_RDMA_DEVICE_MAX_SGE 16U
#define TILEXR_RDMA_MAX_QPS 16U
#define TILEXR_RDMA_DB_UNKNOWN 0U
#define TILEXR_RDMA_DB_SOFT 1U
#define TILEXR_RDMA_DB_HARD 2U
#define TILEXR_RDMA_DEVICE_SCRATCH_BYTES 512U
#define TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT 32U
#define TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT 4096U

enum TileXRRDMADeviceStatus {
    TILEXR_RDMA_DEVICE_SUCCESS = 0,
    TILEXR_RDMA_DEVICE_INVALID = 1,
    TILEXR_RDMA_DEVICE_UNSUPPORTED = 2,
    TILEXR_RDMA_DEVICE_BUSY = 3,
    TILEXR_RDMA_DEVICE_TIMEOUT = 4,
    TILEXR_RDMA_DEVICE_CQE_ERROR = 5,
    TILEXR_RDMA_DEVICE_CQE_MISMATCH = 6,
    TILEXR_RDMA_DEVICE_POISONED = 7
};

/* One 64-byte-aligned, transport-owned HBM allocation per QP, initially zero.
 * Shared by ALL memory registrations using that QP; never reset at registration.
 * Only one AIV may use a QP, including its state, at a time. A posted timeout or
 * error poisons the QP until teardown: it does not cancel NIC memory accesses. */
typedef struct TileXRRDMADeviceState {
    uint64_t producer;       /* Next absolute SQ WQEBB sequence. */
    uint64_t completed;      /* First uncompleted SQ WQEBB (not a WR count). */
    uint64_t cqConsumer;     /* Next absolute CQ slot; legacy all-signaled WR ordinal. */
    uint32_t error;          /* Sticky TileXRRDMADeviceStatus; zero is healthy. */
    uint32_t syndrome;       /* Raw CQE syndrome on failure. */
    uint64_t reserved[4];    /* Opt-in pipeline: [0] mode tag, [1] next WR ordinal. */
} TileXRRDMADeviceState;

typedef struct TileXRRDMADeviceQueue {
    uint64_t sq;             /* sq_info.qbuf.base, HBM. */
    uint64_t cq;             /* cq_info.qbuf.base, HBM. */
    uint64_t sqDbr;          /* sq_info.dbr_pi_va; required only for SOFT. */
    uint64_t cqDbr;          /* cq_info.dbr_ci_va, NOT cq_info.db_hw_va. */
    uint64_t doorbell;       /* HARD: UAR base; SOFT: complete mapped address. */
    uint64_t state;          /* 64-byte-aligned TileXRRDMADeviceState device VA. */
    uint64_t sqDbValue;      /* Native-order template; PI bits 32..39 must be 0. */
    uint64_t doorbellBytes;  /* Accessible bytes from doorbell; HARD needs 2048. */
    uint32_t qpn;
    uint32_t cqn;            /* Diagnostic identity; not part of CQ CI record. */
    uint32_t sqDepth;        /* Power of two, >=2; batches reserve one entry. */
    uint32_t sqEntryBytes;   /* Exactly 64 for this ABI. */
    uint32_t cqDepth;        /* Power of two, >=2. */
    uint32_t cqEntryBytes;   /* Exactly 64; the defined CQE header is 32 bytes. */
    uint32_t dbMode;         /* Explicit TILEXR_RDMA_DB_SOFT/HARD; 0 is rejected. */
    uint32_t queueAbi;       /* TILEXR_RDMA_QUEUE_ABI_UBNIC_V2. */
    uint32_t maxSendSge;     /* Negotiated QP limit, capped at DEVICE_MAX_SGE. */
    uint32_t maxSendWr;      /* Admitted outstanding WRs, independent of BBs. */
} TileXRRDMADeviceQueue;

/* Sources within deviceView.localAddress/localBytes, all using its localKey.
 * The NIC concatenates these segments into ONE contiguous remote WRITE. */
typedef struct TileXRRDMADeviceSge {
    uint64_t localOffset;
    uint32_t bytes;
    uint32_t reserved;       /* Must be zero. */
} TileXRRDMADeviceSge;

/* A single peer's view of a registered region. Caller keeps the registration,
 * QP, mappings and state alive until all users and remote consumers quiesce.
 * Producing/consuming streams need application synchronization; WRITE completion
 * alone does not notify or synchronize a remote consumer. */
typedef struct TileXRRDMADeviceView {
    uint32_t version;        /* TILEXR_RDMA_DEVICE_VERSION. */
    uint32_t structBytes;    /* sizeof(TileXRRDMADeviceView), currently 152. */
    uint64_t localAddress;
    uint64_t localBytes;
    uint64_t remoteAddress;
    uint64_t remoteBytes;
    uint32_t localKey;
    uint32_t remoteKey;
    TileXRRDMADeviceQueue queue;
} TileXRRDMADeviceView;

#ifdef __cplusplus
static_assert(sizeof(TileXRRDMADeviceState) == 64, "RDMA state ABI");
static_assert(sizeof(TileXRRDMADeviceSge) == 16, "RDMA SGE ABI");
static_assert(sizeof(TileXRRDMADeviceQueue) == 104, "RDMA queue ABI");
static_assert(sizeof(TileXRRDMADeviceView) == 152, "RDMA view ABI");
static_assert(offsetof(TileXRRDMADeviceQueue, state) == 40, "RDMA state address ABI");
static_assert(offsetof(TileXRRDMADeviceQueue, qpn) == 64, "RDMA queue fields ABI");
static_assert(offsetof(TileXRRDMADeviceQueue, dbMode) == 88, "RDMA mode ABI");
static_assert(offsetof(TileXRRDMADeviceView, queue) == 48, "RDMA queue view ABI");
#endif

#endif
