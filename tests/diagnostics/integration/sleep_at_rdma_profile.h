/*
 * 功能：定义 64-WQE 测试的 payload、guard 和 marker 常量，声明采集入口。
 * 调用关系：Host helper 和 AIV kernel 共用布局，clock E2E 调用采集入口；stream 由 E2E 持有。
 */
#ifndef TILEXR_TEST_SLEEP_AT_RDMA_PROFILE_H
#define TILEXR_TEST_SLEEP_AT_RDMA_PROFILE_H
#include <cstdint>

namespace SleepAtRdmaProfile {
constexpr uint32_t kWqes = 64;
constexpr uint32_t kBytes = 4096;
constexpr uint64_t kGuard = 64;
constexpr uint64_t kPayload = uint64_t(kWqes) * kBytes;
constexpr uint64_t kSource = kGuard;
constexpr uint64_t kTarget = kSource + kPayload + kGuard;
constexpr uint64_t kRegionBytes = kTarget + kPayload + kGuard;
constexpr uint64_t kMagic = 0x534C5052444D4131ULL;
constexpr uint32_t kEntry = 1;
constexpr uint32_t kExit = 2;
}

#if !defined(__CCE__)
#include <string>
#include "tilexr_straggler.h"
bool RunSleepAtRdmaProfile(TileXRCommPtr comm, TileXRStraggler detector,
    aclrtStream stream, int rank, int device, const std::string &outputDir);
#endif
#endif
