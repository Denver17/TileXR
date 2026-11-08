/*
 * 功能：定义 Host/kernel 共用的 launch 参数、队列结果和 multi-QP 计时布局，以及 warmup 与单 WQE 模式常量。
 * 调用关系：bench、multiqp 及其 kernel 共用本文件；Host 打包参数并分配结果区，kernel 按相同布局读写。
 * 资源与生命周期：结构中的地址只引用 Host 准备的 GM buffer，不转移所有权；Host 程序和 ELF 必须使用匹配的 ABI。
 *
 * Purpose: Define shared Host/kernel launch, queue-result and multi-QP timing layouts plus warmup/single-WQE constants.
 * Calls: bench, multiqp and their kernels share these definitions; Hosts pack arguments/allocate results and kernels use the same layout.
 * Lifetime: Address fields reference Host-prepared GM buffers without taking ownership; Host executables and ELFs need matching ABIs.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef TILEXR_RDMA_SINGLE_WQE_CALLS
#define TILEXR_RDMA_SINGLE_WQE_CALLS 0
#endif
constexpr bool kMultiQpSingleWqeCalls = TILEXR_RDMA_SINGLE_WQE_CALLS != 0;
constexpr uint32_t kMultiQpPostBatch = kMultiQpSingleWqeCalls ? 1U : 64U;
constexpr uint32_t kMultiQpWarmupCalls = 32U;
struct BenchArgs {
    uint64_t view, result, sourceOffset, targetOffset, stride;
    uint32_t bytes, count, rank, direction, cyclicSlots, calibration;
    // The historical 64-byte prefix is unchanged; bytes remains per-SGE bytes
    // and stride remains the local/source stride. Rebuild Host and ELF together.
    uint64_t targetStride;
    uint32_t sgeCount, reserved;
    uint64_t profiling;
};
struct BenchResult {
    uint64_t magic, status, start, end, producer, completed, cqConsumer, error;
};
// multiqp-only suffix array follows all legacy results; bench's ABI stays unchanged.
struct MultiQpTiming {
    uint64_t issueEnd, warmupCalls, reserved[2];
};
static_assert(offsetof(BenchArgs, calibration) == 60, "matched benchmark legacy fields");
static_assert(offsetof(BenchArgs, targetStride) == 64, "matched benchmark legacy prefix");
static_assert(offsetof(BenchArgs, sgeCount) == 72, "matched benchmark SGE fields");
static_assert(offsetof(BenchArgs, profiling) == 80, "profiling launch suffix");
static_assert(sizeof(BenchArgs) == 88, "matched benchmark launch ABI; rebuild Host and ELF together");
static_assert(sizeof(BenchResult) == 64, "matched benchmark result ABI");
static_assert(sizeof(MultiQpTiming) == 32, "aligned multiqp timing suffix");
