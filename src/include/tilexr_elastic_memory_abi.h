/*
 * 定义 pool.cpp 与 tilexr_elastic_memory_device.h 共用的环状态、分配 Handle 和 mailbox。
 * Host 在调用前发布只读 PoolState，Device 按实际 bytes 分配，将选中段的 RingState 写入
 * committed 槽后发布 Output；扩容请求和响应通过独立槽传递，以请求序号匹配本次调用。
 * Handle 的 generation 区分分配身份，完整 lease 和连续退休前缀由 Host 管理。
 */
#ifndef TILEXR_ELASTIC_MEMORY_ABI_H
#define TILEXR_ELASTIC_MEMORY_ABI_H
#include <stdint.h>
namespace TileXR { namespace Elastic {
constexpr uint64_t kAbiVersion = 3;
constexpr uint32_t kMaxSegments = 32;
constexpr uint32_t kMaxAllocations = 128;
enum Error : uint64_t { Success=0, Invalid=1, Capacity=2, MetadataFull=3, StaleHandle=4,
    RuntimeFailure=5, Closed=6, ProtocolFailure=7 };
struct Handle { uint64_t segmentId, offset, bytes, generation; };
struct Segment { uint64_t id, address, bytes; };
// Host advances reclaim after a safe contiguous prefix; Device applies it to head.
// tailOffset avoids Device division; alignment lets Host validate the completed result.
struct RingState {
    Segment segment;
    uint64_t head, tail, reclaim, tailOffset, alignment;
};
struct alignas(64) PoolState {
    uint64_t version, requestId, segmentCount, reservationCount, reserved[4];
    RingState segments[kMaxSegments]; // previous current segment first
};
struct alignas(64) GrowRequest { uint64_t sequence, bytes, alignment, reserved[5]; };
struct alignas(64) GrowReply { uint64_t sequence, segmentId, address, bytes, error, reserved[3]; };
struct alignas(64) Output {
    uint64_t sequence, error;
    Handle handle;
    uint64_t address, rows;
};
// Output is immutable after publication; late execution failures use this separate slot.
struct alignas(64) FinalStatus { uint64_t sequence, error, reserved[6]; };
struct alignas(64) Mailbox {
    GrowRequest request;
    GrowReply reply;
    Output output;
    uint64_t version, requestId, poolId, reserved[5];
    RingState committed; // Device writes before output.sequence publication
    FinalStatus finalStatus; // optional late error, checked after producer completion
};
struct DeviceView { uint64_t state, mailbox; };
static_assert(sizeof(GrowRequest)==64 && sizeof(GrowReply)==64 && sizeof(Output)==64,"mailbox lines");
static_assert(sizeof(RingState)==64 && sizeof(PoolState)==64*(kMaxSegments+1),"ring snapshot layout");
static_assert(sizeof(FinalStatus)==64 && sizeof(Mailbox)==384,"ring mailbox layout");
} }
#endif
