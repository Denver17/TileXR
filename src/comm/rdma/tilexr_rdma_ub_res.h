/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：定义 HAL doorbell 资源映射所用的 ABI 结构和请求字段。
 * 调用关系：tilexr_rdma_nda.cpp 用这些结构调用 halResAddrMapV2/halResAddrUnmapV2。
 *
 * Purpose: Define the HAL ABI structures and request fields for doorbell resource mappings.
 * Calls: tilexr_rdma_nda.cpp uses these structures with halResAddrMapV2/halResAddrUnmapV2.
 */
#ifndef TILEXR_RDMA_UB_RES_H
#define TILEXR_RDMA_UB_RES_H
#include <cstdint>
#include <cstddef>

namespace TileXR {
namespace RdmaUbRes {
// Private dynamic HAL ABI, matching perftest 66a7475f acl_memory.h and the
// Ascend 25.9 driver res_map_info_{in,out}. Never treat a UB GUID as a Host VA.
constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kProcessCp1 = 0;
constexpr uint32_t kNdaDoorbell = 12;
struct Doorbell {
    uint64_t guidLow;
    uint64_t guidHigh;
    uint32_t index;
    uint32_t count;
    uint32_t reserved[8];
};
struct MapIn {
    uint32_t id;
    uint32_t process;
    uint32_t type;
    uint32_t resource;
    uint32_t flags;
    uint32_t privateBytes;
    void* privateData;
    uint32_t reserved[8];
};
struct MapOut { uint64_t address; uint32_t bytes; uint32_t reserved[8]; };
static_assert(sizeof(Doorbell) == 56 && sizeof(MapIn) == 64 &&
    offsetof(MapIn, privateData) == 24 && sizeof(MapOut) == 48, "HAL NDA resource ABI");
inline MapIn Request(Doorbell& db, uint32_t resource)
{
    MapIn in {};
    in.process = kProcessCp1;
    in.type = kNdaDoorbell;
    in.resource = resource;
    in.privateBytes = sizeof(db);
    in.privateData = &db;
    return in;
}
} // namespace RdmaUbRes
} // namespace TileXR
#endif
