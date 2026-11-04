#pragma once
#include <stddef.h>
#include <stdint.h>
struct BenchArgs {
    uint64_t view, result, sourceOffset, targetOffset, stride;
    uint32_t bytes, count, rank, direction, cyclicSlots, calibration;
    // The historical 64-byte prefix is unchanged; bytes remains per-SGE bytes
    // and stride remains the local/source stride. Rebuild Host and ELF together.
    uint64_t targetStride;
    uint32_t sgeCount, reserved;
};
struct BenchResult {
    uint64_t magic, status, start, end, producer, completed, cqConsumer, error;
};
static_assert(offsetof(BenchArgs, calibration) == 60, "matched benchmark legacy fields");
static_assert(offsetof(BenchArgs, targetStride) == 64, "matched benchmark legacy prefix");
static_assert(offsetof(BenchArgs, sgeCount) == 72, "matched benchmark SGE fields");
static_assert(sizeof(BenchArgs) == 80, "matched benchmark launch ABI");
static_assert(sizeof(BenchResult) == 64, "matched benchmark result ABI");
