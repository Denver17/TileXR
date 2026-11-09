/*
 * 声明供原生 EP 和 Torch 适配层使用的 Pool、Pending、Allocation 接口，实现在
 * src/elastic_memory/pool.cpp。Submit 接收输入 stream 和 Device 启动回调，Pending::Wait
 * 返回完成后的分配；Allocation 持有内存使用权，消费者须登记 stream/event 后才能退休。
 * Close 停止接收请求并尝试回收，仍存活的 Allocation 可继续保持 segment 和 Core。
 */
#ifndef TILEXR_ELASTIC_MEMORY_H
#define TILEXR_ELASTIC_MEMORY_H
#include <acl/acl.h>
#include <cstdint>
#include <functional>
#include <memory>
#include "tilexr_elastic_memory_abi.h"
namespace TileXR { namespace Elastic {
struct Config {
    uint64_t maxCommittedBytes = 1ULL << 30;
    uint64_t maxCachedFreeBytes = 32ULL << 20;
    uint64_t segmentBytes = 2ULL << 20;
    uint32_t maxPending = 64;
};
struct Stats {
    uint64_t liveBytes=0, pendingRetireBytes=0, freeBytes=0, committedBytes=0;
    uint64_t reclaimableSegmentBytes=0, metadataBytes=0, largestFreeRange=0;
    uint64_t peakCommittedBytes=0, growthCount=0, trimCount=0, failureCount=0;
    // Subsets of freeBytes and live/pending reservations; not additional committed HBM.
    uint64_t blockedFreeBytes=0, paddingBytes=0;
};
class Core;
class Allocation {
public:
    ~Allocation();
    void* Data() const;
    uint64_t Bytes() const;
    uint64_t Rows() const;
    Handle Identity() const;
    // Register every consumer stream; it must remain valid until this lease retires.
    void RecordStream(aclrtStream stream);
    // Transfer an already-recorded event to the lease; the pool queries and destroys it.
    void RecordEvent(aclrtEvent event);
    // Native adapter hook runs on the service thread, outside the pool lock, before retirement events.
    void SetRetireHook(std::function<void()> hook);
private:
    friend class Core;
    struct Impl;
    explicit Allocation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
class Pending {
public:
    // Throws std::runtime_error on runtime/protocol failure; returns only after data completion.
    std::shared_ptr<Allocation> Wait();
private:
    friend class Core;
    struct Impl;
    explicit Pending(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};
class Pool {
public:
    // Caller must have selected the device/context. No data HBM is allocated by Create.
    static std::shared_ptr<Pool> Create(const Config& config = {});
    ~Pool();
    // launch executes on a native service thread with the captured ACL context, without Python GIL.
    // inputStream records readiness at submission. Capture input owners in launch until it completes.
    std::shared_ptr<Pending> Submit(std::function<int(DeviceView, aclrtStream)> launch,
                                    aclrtStream inputStream);
    Stats Collect();
    Stats Trim(bool wait = true);
    Stats Close();
    Stats Snapshot() const;
private:
    explicit Pool(std::shared_ptr<Core> core);
    std::shared_ptr<Core> core_;
};
} }
#endif
