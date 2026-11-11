/*
 * 实现 tilexr_elastic_memory.h 的 Pool、Pending 和 Allocation。Core 在 service stream
 * 按 FIFO 启动调用，发布只读环快照，并通过 mapped mailbox 向驻留 kernel 回复 HBM 扩容。
 * WaitMetadata 只等待不可变地址和 shape；Wait/Synchronize 继续报告完整调用的最终结果。
 * 消费者 stream 等待保留的 producer event；FIFO 下一请求和回收仍在完整完成后推进。
 * Allocation 保持 Core 存活，最后一个引用释放后，服务线程
 * 检查登记的 stream/event、推进连续可回收前缀并尝试归还空 segment。无法确认设备访问结束时
 * 保留资源和输入 owner，拒绝后续请求，避免提前释放。
 */
#include "tilexr_elastic_memory.h"
#include "allocator.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace TileXR { namespace Elastic {
namespace {
void Check(aclError error, const char* operation) {
    if (error != ACL_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(error));
}
uint64_t Load(const uint64_t* value) { return __atomic_load_n(value, __ATOMIC_ACQUIRE); }
void Store(uint64_t* destination, uint64_t value) { __atomic_store_n(destination, value, __ATOMIC_RELEASE); }
bool Same(const Handle& a, const Handle& b) {
    return a.segmentId == b.segmentId && a.offset == b.offset &&
           a.bytes == b.bytes && a.generation == b.generation;
}
bool Complete(aclrtEvent event) {
    aclrtEventRecordedStatus status;
    Check(aclrtQueryEventStatus(event, &status), "aclrtQueryEventStatus");
    return status == ACL_EVENT_RECORDED_STATUS_COMPLETE;
}
constexpr size_t kMailboxBytes = (sizeof(Mailbox) + 4095) / 4096 * 4096;
constexpr size_t kMaxConsumerStreams = 128;
std::atomic<uint64_t> nextPoolId{1};
}

// No Allocation/Pending back-reference: completion can outlive either without a cycle.
struct ProducerState {
    std::mutex mutex;
    std::condition_variable changed;
    aclrtEvent event = nullptr;
    std::vector<aclrtEvent> waitBoundaries;
    std::vector<aclrtStream> waitStreams;
    bool recorded = false, done = false;
    std::string error;
};
struct Pending::Impl {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::string error;
    std::shared_ptr<ProducerState> producer = std::make_shared<ProducerState>();
    std::shared_ptr<Allocation> allocation;
};
struct Allocation::Impl {
    std::shared_ptr<Core> core;
    std::shared_ptr<ProducerState> producer;
    Handle handle{};
    void* address = nullptr;
    uint64_t rows = 0;
};

class Core : public std::enable_shared_from_this<Core> {
    struct Reservation {
        Handle handle;
        uint64_t end, bytes;
        bool retired = false;
    };
    struct SegmentRecord {
        void* base = nullptr;
        uint64_t bytes = 0;
        bool draining = false;
        RingState ring{};
        std::deque<Reservation> reservations;
    };
    struct LeaseRecord {
        Handle handle;
        bool retiring = false;
        bool eventsRecorded = false;
        std::vector<aclrtStream> streams;
        std::vector<aclrtEvent> events;
        std::function<void()> beforeEvents;
    };
    struct Work {
        std::function<int(DeviceView, aclrtStream)> launch;
        aclrtEvent input = nullptr;
        std::shared_ptr<Pending::Impl> pending;
        std::shared_ptr<std::promise<Stats>> control;
        bool wait = false;
        bool inputsSafe = false;
    };
public:
    explicit Core(const Config& config) : config_(config), poolId_(nextPoolId.fetch_add(1)) {
        if (!config.maxCommittedBytes || !config.segmentBytes || !config.maxPending)
            throw std::invalid_argument("ElasticMemory budgets, segmentBytes and maxPending must be positive");
        Check(aclrtGetCurrentContext(&context_), "aclrtGetCurrentContext");
        if (!context_) throw std::runtime_error("ElasticMemory requires a current ACL context");
        try {
            Check(aclrtCreateStream(&stream_), "aclrtCreateStream");
            void* memory = nullptr;
            if (posix_memalign(&memory, 4096, kMailboxBytes)) throw std::bad_alloc();
            mailbox_ = static_cast<Mailbox*>(memory);
            std::memset(mailbox_, 0, kMailboxBytes);
            Check(aclrtHostRegister(mailbox_, kMailboxBytes, ACL_HOST_REGISTER_MAPPED, &mapped_),
                  "aclrtHostRegister");
            Check(aclrtMalloc(&state_, sizeof(PoolState), ACL_MEM_MALLOC_NORMAL_ONLY), "aclrtMalloc metadata");
            PoolState initial{};
            initial.version = kAbiVersion;
            Check(aclrtMemcpy(state_, sizeof(initial), &initial, sizeof(initial), ACL_MEMCPY_HOST_TO_DEVICE),
                  "aclrtMemcpy initial PoolState");
            stats_.metadataBytes = kMailboxBytes + sizeof(PoolState);
        } catch (...) { Cleanup(); throw; }
    }
    ~Core() { if (!workerStarted_) Cleanup(); }
    void Start() {
        // The worker's reference is released only after Close and the last lease retirement.
        // No joinable thread is owned by Core, so last-reference destruction cannot self-join.
        auto self = shared_from_this();
        try {
            std::thread([self] { self->Run(); }).detach();
            workerStarted_ = true;
        }
        catch (...) { Cleanup(); stopped_ = true; throw; }
    }
    std::shared_ptr<Pending> Submit(std::function<int(DeviceView, aclrtStream)> launch,
                                    aclrtStream inputStream) {
        if (!launch) throw std::invalid_argument("ElasticMemory launch closure is empty");
        aclrtContext current = nullptr;
        Check(aclrtGetCurrentContext(&current), "aclrtGetCurrentContext submit");
        if (current != context_) throw std::runtime_error("ElasticMemory Submit requires the pool ACL context");
        // Serialize admission and Close independently of the nonblocking statistics lock.
        std::lock_guard<std::mutex> admission(admissionMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) throw std::runtime_error("ElasticMemory pool is closed");
            if (!failure_.empty()) throw std::runtime_error(failure_);
            if (producers_.size() + pendingCount_ >= size_t(config_.maxPending) + kMaxAllocations)
                throw std::runtime_error("ElasticMemory retained producer event metadata is full");
            if (pendingCount_ >= config_.maxPending) throw std::runtime_error("ElasticMemory pending queue is full");
        }
        Work work;
        work.pending = std::make_shared<Pending::Impl>();
        auto pending = std::shared_ptr<Pending>(new Pending(work.pending));
        Check(aclrtCreateEvent(&work.input), "aclrtCreateEvent input");
        const aclError status = aclrtRecordEvent(work.input, inputStream);
        if (status != ACL_SUCCESS) { aclrtDestroyEvent(work.input); Check(status, "aclrtRecordEvent input"); }
        work.launch = std::move(launch);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(work));
            ++pendingCount_;
            changed_.notify_all();
        }
        return pending;
    }
    void Dispose() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (std::this_thread::get_id() == workerId_) {
                closed_ = true;
                changed_.notify_all();
                return;
            }
        }
        try { Trim(true, true); } catch (...) {}
    }
    Stats Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return StatisticsLocked();
    }
    Stats Collect() {
        changed_.notify_all();
        return Snapshot();
    }
    Stats Trim(bool wait, bool close = false) {
        std::unique_lock<std::mutex> admission(admissionMutex_, std::defer_lock);
        if (close) admission.lock();
        auto completion = std::make_shared<std::promise<Stats>>();
        auto result = completion->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (std::this_thread::get_id() == workerId_)
                throw std::runtime_error("ElasticMemory cannot drain from its launch callback");
            if (close) closed_ = true;
            if (stopping_) changed_.wait(lock, [this] { return stopped_; });
            if (stopped_) return StatisticsLocked();
            if (queue_.size() >= static_cast<size_t>(config_.maxPending) + 8)
                throw std::runtime_error("ElasticMemory control queue is full");
            Work work;
            work.control = completion;
            work.wait = wait;
            queue_.push_back(std::move(work));
            changed_.notify_all();
        }
        if (admission.owns_lock()) admission.unlock();
        const Stats stats = result.get();
        if (close) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (leases_.empty() && producers_.empty()) changed_.wait(lock, [this] { return stopped_; });
            return StatisticsLocked();
        }
        return stats;
    }
    void Record(const Handle& handle, aclrtStream stream) {
        if (!handle.bytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = leases_.find(handle.generation);
        if (found == leases_.end() || !Same(found->second.handle, handle) || found->second.retiring)
            throw std::runtime_error("ElasticMemory stale allocation lease");
        auto& streams = found->second.streams;
        if (std::find(streams.begin(), streams.end(), stream) != streams.end()) return;
        if (streams.size() == kMaxConsumerStreams)
            throw std::runtime_error("ElasticMemory consumer stream metadata is full");
        streams.push_back(stream);
    }
    void RecordEvent(const Handle& handle, aclrtEvent event) {
        if (!event) throw std::invalid_argument("ElasticMemory consumer event is null");
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = leases_.find(handle.generation);
        if (!handle.bytes || found == leases_.end() || !Same(found->second.handle, handle) || found->second.retiring)
            throw std::runtime_error("ElasticMemory event requires a live nonempty allocation lease");
        auto& events = found->second.events;
        if (std::find(events.begin(), events.end(), event) != events.end()) return;
        if (events.size() >= kMaxConsumerStreams)
            throw std::runtime_error("ElasticMemory consumer event metadata is full");
        events.push_back(event);
    }
    void SetRetireHook(const Handle& handle, std::function<void()> hook) {
        if (!handle.bytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = leases_.find(handle.generation);
        if (found == leases_.end() || !Same(found->second.handle, handle) || found->second.retiring)
            throw std::runtime_error("ElasticMemory retirement hook requires a live lease");
        if (found->second.beforeEvents) throw std::runtime_error("ElasticMemory retirement hook is already set");
        found->second.beforeEvents = std::move(hook);
    }
    void WaitOnProducer(const Handle& handle, const std::shared_ptr<ProducerState>& producer, aclrtStream stream) {
        aclrtContext current = nullptr;
        Check(aclrtGetCurrentContext(&current), "aclrtGetCurrentContext consumer wait");
        if (current != context_) throw std::runtime_error("ElasticMemory consumer wait requires the pool ACL context");
        Record(handle, stream);
        std::lock_guard<std::mutex> lock(producer->mutex);
        if (!producer->error.empty()) throw std::runtime_error(producer->error);
        if (producer->done) return;
        if (!producer->recorded) throw std::runtime_error("ElasticMemory producer event is not recorded");
        if (std::find(producer->waitStreams.begin(), producer->waitStreams.end(), stream) !=
            producer->waitStreams.end()) return;
        if (producer->waitBoundaries.size() >= kMaxConsumerStreams)
            throw std::runtime_error("ElasticMemory producer wait metadata is full");
        // Retain the producer event until every queued wait has actually passed, including
        // zero-byte output whose separately-owned count tensors still need this dependency.
        aclrtEvent boundary = nullptr;
        Check(aclrtCreateEvent(&boundary), "aclrtCreateEvent consumer wait boundary");
        try {
            producer->waitStreams.push_back(stream);
            try { producer->waitBoundaries.push_back(boundary); }
            catch (...) { producer->waitStreams.pop_back(); throw; }
        } catch (...) { aclrtDestroyEvent(boundary); throw; }
        try {
            Check(aclrtStreamWaitEvent(stream, producer->event), "aclrtStreamWaitEvent producer");
            Check(aclrtRecordEvent(boundary, stream), "aclrtRecordEvent consumer wait boundary");
        } catch (...) {
            // A submitted wait may still refer to the producer event.
            unsafe_ = true;
            producer->error = "ElasticMemory consumer wait submission failed; resources quarantined";
            Poison(producer->error);
            throw;
        }
    }
    void Retire(const Handle& handle) noexcept {
        if (!handle.bytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = leases_.find(handle.generation);
        if (found != leases_.end() && Same(found->second.handle, handle)) found->second.retiring = true;
        changed_.notify_all();
    }
private:
    uint64_t ReservationCountLocked() const {
        uint64_t count = 0;
        for (const auto& segment : segments_) count += segment.second.reservations.size();
        return count;
    }
    Stats StatisticsLocked() const {
        Stats result = stats_;
        result.liveBytes = result.pendingRetireBytes = result.reclaimableSegmentBytes = 0;
        result.largestFreeRange = result.blockedFreeBytes = result.paddingBytes = 0;
        for (const auto& segment : segments_) {
            for (const auto& reservation : segment.second.reservations) {
                if (reservation.retired) {
                    result.blockedFreeBytes += reservation.bytes;
                    continue;
                }
                const auto lease = leases_.find(reservation.handle.generation);
                if (lease != leases_.end() && lease->second.retiring)
                    result.pendingRetireBytes += reservation.bytes;
                else result.liveBytes += reservation.bytes;
                result.paddingBytes += reservation.bytes - reservation.handle.bytes;
            }
        }
        if (producerActive_ && !producerKnown_) {
            // Until the Device commits its selected ring, every unassigned byte may be in use.
            result.liveBytes = result.committedBytes - result.pendingRetireBytes;
            result.freeBytes = result.blockedFreeBytes = 0;
            return result;
        }
        result.freeBytes = result.committedBytes - result.liveBytes - result.pendingRetireBytes;
        for (const auto& segment : segments_) {
            const RingState& ring = segment.second.ring;
            const uint64_t used = ring.tail - ring.reclaim;
            const uint64_t available = segment.second.bytes - used;
            uint64_t largest = available;
            if (used) {
                const uint64_t suffix = segment.second.bytes - ring.tailOffset;
                largest = std::min(available, suffix);
                if (available > suffix) largest = std::max(largest, available - suffix);
            }
            if (!segment.second.draining) result.largestFreeRange = std::max(result.largestFreeRange, largest);
            if (!producerActive_ && !segment.second.draining && segment.second.reservations.empty())
                result.reclaimableSegmentBytes += segment.second.bytes;
        }
        return result;
    }
    void ClearProducerLocked() {
        producerActive_ = producerKnown_ = false;
    }
    bool ValidateCommittedLocked(const Output& output, const RingState& committed,
                                 uint64_t requestId, uint64_t* reserved) const {
        const auto segment = segments_.find(output.handle.segmentId);
        if (segment == segments_.end() || output.handle.generation != requestId ||
            output.handle.offset > segment->second.bytes ||
            output.handle.bytes > segment->second.bytes - output.handle.offset ||
            output.address != reinterpret_cast<uint64_t>(segment->second.base) + output.handle.offset)
            return false;
        RingState expected = segment->second.ring;
        Handle handle{};
        uint64_t address = 0;
        if (TryAllocateRing(expected, output.handle.bytes, committed.alignment, requestId, &handle, &address) != Success ||
            !Same(handle, output.handle) || address != output.address ||
            std::memcmp(&expected, &committed, sizeof(expected)) != 0) return false;
        const RingState& before = segment->second.ring;
        *reserved = committed.tail - (before.reclaim == before.tail ? 0 : before.tail);
        return *reserved >= output.handle.bytes;
    }
    void Finish(const std::shared_ptr<Pending::Impl>& pending, const std::string& error) {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->error = error;
        {
            std::lock_guard<std::mutex> producerLock(pending->producer->mutex);
            if (pending->error.empty()) pending->error = pending->producer->error;
            pending->producer->error = pending->error;
            pending->producer->done = true;
            pending->producer->changed.notify_all();
        }
        pending->done = true;
        pending->changed.notify_all();
    }
    void Poison(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_.empty()) { failure_ = reason; ++stats_.failureCount; }
    }
    void Grow(uint64_t sequence, const std::string& forcedError) {
        uint64_t error = Success, address = 0, id = 0, bytes = 0;
        const uint64_t requested = Load(&mailbox_->request.bytes);
        const uint64_t alignment = Load(&mailbox_->request.alignment);
        uint64_t required = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!forcedError.empty()) error = RuntimeFailure;
            else if (!requested || !alignment || (alignment & (alignment - 1)) || alignment > (1ULL << 30)) error = Invalid;
            else if (segments_.size() == kMaxSegments || !nextSegmentId_) error = MetadataFull;
            else if (requested > std::numeric_limits<uint64_t>::max() - (alignment - 1)) error = Capacity;
            else {
                required = requested + alignment - 1;
                const uint64_t remaining = config_.maxCommittedBytes - stats_.committedBytes;
                if (required > remaining) error = Capacity;
                else bytes = std::min(std::max(config_.segmentBytes, required), remaining);
            }
        }
        void* unpublished = nullptr;
        if (error == Success) {
            void* base = nullptr;
            // Only the service thread commits data segments. Snapshot/Collect never wait on malloc.
            if (aclrtMalloc(&base, bytes, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS) error = RuntimeFailure;
            else {
                try {
                    std::lock_guard<std::mutex> lock(mutex_);
                    id = nextSegmentId_++;
                    SegmentRecord record;
                    record.base = base;
                    record.bytes = bytes;
                    record.ring.segment = Segment{id, reinterpret_cast<uint64_t>(base), bytes};
                    segments_.emplace(id, std::move(record));
                    address = reinterpret_cast<uint64_t>(base);
                    stats_.committedBytes += bytes;
                    ++stats_.growthCount;
                    stats_.peakCommittedBytes = std::max(stats_.peakCommittedBytes, stats_.committedBytes);
                } catch (...) {
                    // Insertion has the strong exception guarantee. No Device address was
                    // published and no accounting changed; the failed allocation stays private.
                    unpublished = base;
                    id = address = bytes = 0;
                    error = RuntimeFailure;
                }
            }
        }
        Store(&mailbox_->reply.segmentId, id);
        Store(&mailbox_->reply.address, address);
        Store(&mailbox_->reply.bytes, bytes);
        Store(&mailbox_->reply.error, error);
        Store(&mailbox_->reply.sequence, sequence);
        // Unblock the resident kernel before calling Runtime free: even an allocator
        // cleanup that waits for Device work must not prevent the failure response.
        if (unpublished) Check(aclrtFree(unpublished), "aclrtFree rejected growth");
    }
    std::shared_ptr<Allocation> Export(uint64_t requestId, const std::shared_ptr<ProducerState>& producer) {
        if (Load(&mailbox_->output.sequence) != requestId)
            throw std::runtime_error("ElasticMemory kernel did not publish matching output");
        const Output output = mailbox_->output;
        if (output.error != Success)
            throw std::runtime_error("ElasticMemory Device call failed: " + std::to_string(output.error));
        // Output and committed are immutable after output.sequence, even while the kernel
        // writes payload. Final errors use finalStatus; no data completion is implied here.
        const RingState committed = mailbox_->committed;
        std::unique_ptr<Allocation::Impl> impl(new Allocation::Impl);
        impl->core = shared_from_this();
        impl->producer = producer;
        impl->address = reinterpret_cast<void*>(output.address);
        impl->rows = output.rows;
        auto allocation = std::shared_ptr<Allocation>(new Allocation(std::move(impl)));
        std::lock_guard<std::mutex> lock(mutex_);
        if (output.handle.bytes) {
            auto segment = segments_.find(output.handle.segmentId);
            uint64_t reserved = 0;
            if (!ValidateCommittedLocked(output, committed, requestId, &reserved) ||
                leases_.count(output.handle.generation) || ReservationCountLocked() >= kMaxAllocations)
                throw std::runtime_error("ElasticMemory invalid committed ring or output allocation");
            for (const auto& entry : leases_) {
                const Handle& old = entry.second.handle;
                if (old.segmentId == output.handle.segmentId && old.offset < output.handle.offset + output.handle.bytes &&
                    output.handle.offset < old.offset + old.bytes)
                    throw std::runtime_error("ElasticMemory output overlaps an existing lease");
            }
            LeaseRecord record;
            record.handle = output.handle;
            // Keep the reservation/lease insertion atomic with respect to allocation failures.
            segment->second.reservations.push_back(Reservation{output.handle, committed.tail, reserved, false});
            try { leases_.emplace(output.handle.generation, std::move(record)); }
            catch (...) { segment->second.reservations.pop_back(); throw; }
            segment->second.ring = committed;
            currentSegmentId_ = output.handle.segmentId;
        } else if (output.address || output.handle.segmentId || output.handle.offset || output.handle.generation) {
            throw std::runtime_error("ElasticMemory invalid zero-size output");
        }
        allocation->impl_->handle = output.handle;
        // Reservation now belongs to the lease, so do not count it a second time. Keep
        // producerActive until completion: the segment cannot yet be reclaimed or trimmed.
        producerKnown_ = true;
        return allocation;
    }
    void Execute(Work& work) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failure_.empty() && !nextRequestId_) {
                failure_ = "ElasticMemory request generation exhausted";
                ++stats_.failureCount;
            }
            error = failure_;
        }
        if (!error.empty()) {
            // Never-launched input owners remain retained until their recorded producer finishes.
            Check(aclrtSynchronizeEvent(work.input), "aclrtSynchronizeEvent rejected input");
            work.inputsSafe = true;
            Check(aclrtDestroyEvent(work.input), "aclrtDestroyEvent rejected input");
            work.input = nullptr;
            Finish(work.pending, error);
            return;
        }
        const uint64_t requestId = nextRequestId_++;
        std::memset(mailbox_, 0, sizeof(Mailbox));
        mailbox_->version = kAbiVersion;
        mailbox_->poolId = poolId_;
        mailbox_->requestId = requestId;
        PoolState snapshot{};
        snapshot.version = kAbiVersion;
        snapshot.requestId = requestId;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reservationCount = ReservationCountLocked();
            auto current = segments_.find(currentSegmentId_);
            if (current != segments_.end()) snapshot.segments[snapshot.segmentCount++] = current->second.ring;
            for (const auto& segment : segments_)
                if (segment.first != currentSegmentId_ && !segment.second.draining)
                    snapshot.segments[snapshot.segmentCount++] = segment.second.ring;
        }
        // Only the service thread stages snapshots, strictly between allocator kernels.
        Check(aclrtMemcpy(state_, sizeof(snapshot), &snapshot, sizeof(snapshot), ACL_MEMCPY_HOST_TO_DEVICE),
              "aclrtMemcpy immutable ring snapshot");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            producerActive_ = true;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        Check(aclrtStreamWaitEvent(stream_, work.input), "aclrtStreamWaitEvent input");
        const auto producer = work.pending->producer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            producers_.push_back(producer);
        }
        Check(aclrtCreateEvent(&producer->event), "aclrtCreateEvent completion");
        const aclrtEvent done = producer->event;
        try {
            const int status = work.launch(DeviceView{reinterpret_cast<uint64_t>(state_),
                                                      reinterpret_cast<uint64_t>(mapped_)}, stream_);
            if (status) error = "ElasticMemory launch failed: " + std::to_string(status);
        } catch (const std::exception& exception) { error = exception.what(); }
        catch (...) { error = "ElasticMemory launch closure threw"; }
        const aclError recorded = aclrtRecordEvent(done, stream_);
        if (recorded != ACL_SUCCESS) {
            // A failed event record gives no proof that previously submitted accesses stopped.
            unsafe_ = true;
            Check(recorded, "aclrtRecordEvent completion");
        }
        {
            std::lock_guard<std::mutex> lock(producer->mutex);
            producer->recorded = true;
        }
        bool exported = false;
        auto publishMetadata = [&] {
            if (exported || !error.empty() || Load(&mailbox_->output.sequence) != requestId ||
                mailbox_->output.error != Success) return;
            auto allocation = Export(requestId, producer);
            std::lock_guard<std::mutex> lock(work.pending->mutex);
            work.pending->allocation = std::move(allocation);
            exported = true;
            work.pending->changed.notify_all();
        };
        uint64_t serviced = 0;
        try {
            while (!Complete(done)) {
                publishMetadata();
                const uint64_t request = Load(&mailbox_->request.sequence);
                if (request && request != serviced) {
                    if (serviced) error = "ElasticMemory kernel requested more than one growth";
                    if (request != requestId) error = "ElasticMemory growth request ID mismatch";
                    Grow(request, error);
                    serviced = request;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
        } catch (...) { unsafe_ = true; throw; }
        work.inputsSafe = true;
        Check(aclrtDestroyEvent(work.input), "aclrtDestroyEvent input");
        work.input = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error.empty()) error = failure_;
        }
        if (!error.empty()) throw std::runtime_error(error);
        if (Load(&mailbox_->output.sequence) != requestId)
            throw std::runtime_error("ElasticMemory kernel did not publish matching output");
        if (mailbox_->output.error != Success)
            throw std::runtime_error("ElasticMemory Device call failed: " + std::to_string(mailbox_->output.error));
        const uint64_t finalSequence = Load(&mailbox_->finalStatus.sequence);
        if (finalSequence != 0 && finalSequence != requestId)
            throw std::runtime_error("ElasticMemory final status request ID mismatch");
        if (finalSequence == requestId && mailbox_->finalStatus.error != Success)
            throw std::runtime_error("ElasticMemory Device final failure: " + std::to_string(mailbox_->finalStatus.error));
        publishMetadata();
        Finish(work.pending, {});
    }
    void ReapProducers(bool wait = false) {
        if (unsafe_) return;
        for (size_t index = 0; ; ) {
            std::shared_ptr<ProducerState> producer;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (index == producers_.size()) break;
                if (producers_[index].use_count() != 1) { ++index; continue; }
                producer = producers_[index];
            }
            // Sole external holder is this worker; no new stream waits can be submitted.
            bool ready = producer->done;
            for (aclrtEvent event : producer->waitBoundaries) {
                if (wait) Check(aclrtSynchronizeEvent(event), "aclrtSynchronizeEvent consumer wait boundary");
                else if (!Complete(event)) ready = false;
            }
            if (!ready) { ++index; continue; }
            for (aclrtEvent& event : producer->waitBoundaries) {
                Check(aclrtDestroyEvent(event), "aclrtDestroyEvent consumer wait boundary");
                event = nullptr;
            }
            if (producer->event) Check(aclrtDestroyEvent(producer->event), "aclrtDestroyEvent producer");
            producer->event = nullptr;
            std::lock_guard<std::mutex> lock(mutex_);
            producers_.erase(producers_.begin() + index);
        }
    }
    void Reclaim(bool wait, bool trim) {
        if (unsafe_) return;
        std::vector<LeaseRecord*> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& entry : leases_) if (entry.second.retiring) retired.push_back(&entry.second);
        }
        // The service is the only eraser. Once retiring, no caller can mutate stream/event/hook
        // fields, so stable map nodes can be serviced without holding the public snapshot lock.
        for (LeaseRecord* pointer : retired) {
            LeaseRecord& lease = *pointer;
            if (lease.beforeEvents) {
                auto hook = std::move(lease.beforeEvents);
                hook(); // May flush a framework queue whose deleters call back into Retire().
            }
            if (!lease.eventsRecorded) {
                for (aclrtStream stream : lease.streams) {
                    aclrtEvent event = nullptr;
                    Check(aclrtCreateEvent(&event), "aclrtCreateEvent retirement");
                    lease.events.push_back(event);
                    Check(aclrtRecordEvent(event, stream), "aclrtRecordEvent retirement");
                }
                lease.eventsRecorded = true;
            }
            bool ready = true;
            for (aclrtEvent event : lease.events) {
                if (wait) Check(aclrtSynchronizeEvent(event), "aclrtSynchronizeEvent retirement");
                else if (!Complete(event)) ready = false;
            }
            if (!ready) continue;
            for (aclrtEvent event : lease.events) Check(aclrtDestroyEvent(event), "aclrtDestroyEvent retirement");
            const Handle handle = lease.handle;
            std::lock_guard<std::mutex> lock(mutex_);
            auto segment = segments_.find(handle.segmentId);
            if (segment == segments_.end()) throw std::runtime_error("ElasticMemory retired segment is missing");
            auto& reservations = segment->second.reservations;
            auto reservation = std::find_if(reservations.begin(), reservations.end(), [&](const Reservation& item) {
                return Same(item.handle, handle);
            });
            if (reservation == reservations.end()) throw std::runtime_error("ElasticMemory retired reservation is missing");
            reservation->retired = true;
            leases_.erase(handle.generation);
            while (!reservations.empty() && reservations.front().retired) {
                segment->second.ring.reclaim = reservations.front().end;
                reservations.pop_front();
            }
        }
        std::vector<uint64_t> candidates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            uint64_t cached = StatisticsLocked().reclaimableSegmentBytes;
            for (auto& segment : segments_) {
                if (producerActive_ || !segment.second.reservations.empty() ||
                    (!trim && !closed_ && cached <= config_.maxCachedFreeBytes)) continue;
                segment.second.draining = true;
                cached -= segment.second.bytes;
                candidates.push_back(segment.first);
            }
        }
        for (uint64_t id : candidates) {
            void* base = nullptr;
            uint64_t bytes = 0;
            { std::lock_guard<std::mutex> lock(mutex_); base = segments_.at(id).base; bytes = segments_.at(id).bytes; }
            Check(aclrtFree(base), "aclrtFree segment");
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.committedBytes -= bytes;
            ++stats_.trimCount;
            segments_.erase(id);
            if (currentSegmentId_ == id) currentSegmentId_ = segments_.empty() ? 0 : segments_.begin()->first;
        }
    }
    void Run() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerId_ = std::this_thread::get_id();
        }
        try { Check(aclrtSetCurrentContext(context_), "aclrtSetCurrentContext worker"); }
        catch (const std::exception& exception) { Poison(exception.what()); unsafe_ = true; }
        for (;;) {
            Work work;
            bool hasWork = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait_for(lock, std::chrono::milliseconds(1), [this] { return !queue_.empty(); });
                if (!queue_.empty()) { work = std::move(queue_.front()); queue_.pop_front(); hasWork = true; }
            }
            if (hasWork) {
                try {
                    if (work.control) {
                        if (!unsafe_) {
                            try { Reclaim(work.wait, true); ReapProducers(work.wait); }
                            catch (...) { unsafe_ = true; throw; }
                        }
                        work.control->set_value(Snapshot());
                    } else Execute(work);
                } catch (const std::exception& exception) {
                    if (work.pending && !work.inputsSafe) unsafe_ = true;
                    Poison(exception.what());
                    if (work.pending) Finish(work.pending, exception.what());
                    if (work.control) work.control->set_exception(std::current_exception());
                } catch (...) {
                    if (work.pending && !work.inputsSafe) unsafe_ = true;
                    Poison("ElasticMemory unknown native service failure");
                    if (work.pending) Finish(work.pending, "ElasticMemory unknown native service failure");
                    if (work.control) work.control->set_exception(std::current_exception());
                }
                if (work.pending) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    --pendingCount_;
                    if (work.inputsSafe) ClearProducerLocked();
                }
                if (unsafe_ && work.launch && !work.inputsSafe) quarantined_.push_back(std::move(work));
            }
            try { Reclaim(false, false); ReapProducers(); }
            catch (const std::exception& exception) { unsafe_ = true; Poison(exception.what()); }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_ && queue_.empty() && ((leases_.empty() && producers_.empty()) || unsafe_)) { stopping_ = true; break; }
            }
        }
        if (unsafe_) {
            // Deliberate fail-closed quarantine: retain context-bound resources AND input owners.
            // Resetting the device or freeing memory after an unconfirmed stop is not safe.
            static std::mutex quarantineMutex;
            static auto* quarantine = new std::vector<std::shared_ptr<Core>>;
            std::lock_guard<std::mutex> lock(quarantineMutex);
            quarantine->push_back(shared_from_this());
        }
        if (!unsafe_) Cleanup();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            changed_.notify_all();
        }
    }
    void Cleanup() noexcept {
        for (const auto& segment : segments_) aclrtFree(segment.second.base);
        { std::lock_guard<std::mutex> lock(mutex_); segments_.clear(); }
        if (state_) { aclrtFree(state_); state_ = nullptr; }
        if (mapped_) { aclrtHostUnregister(mailbox_); mapped_ = nullptr; }
        if (mailbox_) { std::free(mailbox_); mailbox_ = nullptr; }
        if (stream_) { aclrtDestroyStream(stream_); stream_ = nullptr; }
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.metadataBytes = 0;
    }
    Config config_;
    uint64_t poolId_;
    aclrtContext context_ = nullptr;
    aclrtStream stream_ = nullptr;
    Mailbox* mailbox_ = nullptr;
    void* mapped_ = nullptr;
    void* state_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex admissionMutex_;
    std::condition_variable changed_;
    std::thread::id workerId_;
    std::deque<Work> queue_;
    std::map<uint64_t, SegmentRecord> segments_;
    std::map<uint64_t, LeaseRecord> leases_;
    std::vector<Work> quarantined_;
    std::vector<std::shared_ptr<ProducerState>> producers_;
    Stats stats_;
    std::string failure_;
    uint64_t nextRequestId_ = 1, nextSegmentId_ = 1, currentSegmentId_ = 0;
    uint32_t pendingCount_ = 0;
    bool producerActive_ = false, producerKnown_ = false;
    bool workerStarted_ = false;
    bool closed_ = false, stopping_ = false, stopped_ = false;
    std::atomic<bool> unsafe_{false};
};

Allocation::Allocation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Allocation::~Allocation() { impl_->core->Retire(impl_->handle); }
void* Allocation::Data() const { return impl_->address; }
uint64_t Allocation::Bytes() const { return impl_->handle.bytes; }
uint64_t Allocation::Rows() const { return impl_->rows; }
Handle Allocation::Identity() const { return impl_->handle; }
void Allocation::WaitOnStream(aclrtStream stream) {
    impl_->core->WaitOnProducer(impl_->handle, impl_->producer, stream);
}
void Allocation::Synchronize() {
    std::unique_lock<std::mutex> lock(impl_->producer->mutex);
    impl_->producer->changed.wait(lock, [this] { return impl_->producer->done; });
    if (!impl_->producer->error.empty()) throw std::runtime_error(impl_->producer->error);
}
void Allocation::RecordStream(aclrtStream stream) { impl_->core->Record(impl_->handle, stream); }
void Allocation::RecordEvent(aclrtEvent event) { impl_->core->RecordEvent(impl_->handle, event); }
void Allocation::SetRetireHook(std::function<void()> hook) { impl_->core->SetRetireHook(impl_->handle, std::move(hook)); }
Pending::Pending(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::shared_ptr<Allocation> Pending::Wait() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait(lock, [this] { return impl_->done; });
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
    return impl_->allocation;
}
std::shared_ptr<Allocation> Pending::WaitMetadata() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait(lock, [this] { return impl_->allocation || impl_->done; });
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
    return impl_->allocation;
}
Pool::Pool(std::shared_ptr<Core> core) : core_(std::move(core)) {}
std::shared_ptr<Pool> Pool::Create(const Config& config) {
    auto core = std::make_shared<Core>(config);
    auto pool = std::shared_ptr<Pool>(new Pool(core));
    core->Start();
    return pool;
}
Pool::~Pool() { core_->Dispose(); }
std::shared_ptr<Pending> Pool::Submit(std::function<int(DeviceView, aclrtStream)> launch, aclrtStream inputStream) {
    return core_->Submit(std::move(launch), inputStream);
}
Stats Pool::Collect() { return core_->Collect(); }
Stats Pool::Trim(bool wait) { return core_->Trim(wait); }
Stats Pool::Close() { return core_->Trim(true, true); }
Stats Pool::Snapshot() const { return core_->Snapshot(); }
} }
