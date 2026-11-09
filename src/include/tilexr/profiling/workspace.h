/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
// 功能：提供借用 workspace 的池与 lease；AclCapture 同步完成后归还，分配内存仍归调用者。
// Purpose: Provide borrowed workspace pools and leases; AclCapture returns leases after synchronization while callers retain allocation ownership.
#ifndef TILEXR_PROFILING_WORKSPACE_H
#define TILEXR_PROFILING_WORKSPACE_H
#include "host.h"
#include <memory>
#include <mutex>

namespace TileXR { namespace Profiling {
namespace Detail {
struct WorkspaceSlot {
    void *address = nullptr;
    uint64_t bytes = 0, generation = 0;
    uintptr_t context = 0;
    bool busy = false;
};
struct WorkspaceState {
    std::mutex mutex;
    std::vector<WorkspaceSlot> slots;
};
}
class AclCapture;
// Pool owns reservation metadata only. The allocator must keep every registered
// device allocation and its context alive until BusyCount()==0.
class WorkspaceLease {
public:
    WorkspaceLease() = default;
    WorkspaceLease(const WorkspaceLease &) = delete;
    WorkspaceLease &operator=(const WorkspaceLease &) = delete;
    WorkspaceLease(WorkspaceLease &&other) noexcept { Swap(other); }
    WorkspaceLease &operator=(WorkspaceLease &&other) noexcept
    { if (this != &other) { Reset(); Swap(other); } return *this; }
    ~WorkspaceLease() noexcept { Reset(); }
    explicit operator bool() const { return bool(state_); }
    void *Address() const { return address_; }
    uint64_t Bytes() const { return bytes_; }
    uint64_t Generation() const { return generation_; }
    uintptr_t Context() const { return context_; }
private:
    friend class WorkspacePool;
    friend class AclCapture;
    void Swap(WorkspaceLease &other) noexcept
    {
        state_.swap(other.state_); std::swap(slot_,other.slot_);
        std::swap(address_,other.address_); std::swap(bytes_,other.bytes_);
        std::swap(generation_,other.generation_); std::swap(context_,other.context_);
        std::swap(exposed_,other.exposed_);
    }
    void Reset() noexcept
    {
        if (!state_) return;
        // An exposed lease is quarantined if its capture cannot synchronize.
        // Never silently return storage which may still be used by a kernel.
        if (!exposed_) {
            try {
                std::lock_guard<std::mutex> lock(state_->mutex);
                auto &slot = state_->slots[slot_];
                if (slot.busy && slot.generation == generation_) slot.busy = false;
            } catch (...) {}
        }
        state_.reset(); address_ = nullptr;
    }
    std::shared_ptr<Detail::WorkspaceState> state_;
    size_t slot_ = 0;
    void *address_ = nullptr;
    uint64_t bytes_ = 0, generation_ = 0;
    uintptr_t context_ = 0;
    bool exposed_ = false;
};
class WorkspacePool {
public:
    WorkspacePool() : state_(new Detail::WorkspaceState) {}
    WorkspacePool(const WorkspacePool &) = delete;
    WorkspacePool &operator=(const WorkspacePool &) = delete;
    Status Add(void *address, uint64_t bytes, uintptr_t context)
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        if (!start || start % CACHE_LINE_BYTES || bytes < 128 || !context ||
            bytes > UINTPTR_MAX || start > UINTPTR_MAX - bytes)
            return {StatusCode::InvalidArgument,"invalid caller-owned workspace range/context"};
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto &slot : state_->slots) {
            const uintptr_t other = reinterpret_cast<uintptr_t>(slot.address);
            if (start < other + slot.bytes && other < start + bytes)
                return {StatusCode::InvalidArgument,"workspace allocations must not overlap"};
        }
        Detail::WorkspaceSlot slot; slot.address=address; slot.bytes=bytes; slot.context=context;
        state_->slots.push_back(slot); return {};
    }
    Status Acquire(uint64_t requiredBytes, uintptr_t context, WorkspaceLease *out)
    {
        if (!out || *out || !requiredBytes || !context)
            return {StatusCode::InvalidArgument,"lease output must be empty and request valid"};
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (size_t i=0; i<state_->slots.size(); ++i) {
            auto &slot=state_->slots[i];
            if (slot.busy || slot.context!=context || slot.bytes<requiredBytes || slot.generation==UINT64_MAX) continue;
            slot.busy=true; ++slot.generation;
            out->state_=state_; out->slot_=i; out->address_=slot.address;
            out->bytes_=slot.bytes; out->context_=slot.context; out->generation_=slot.generation;
            return {};
        }
        return {StatusCode::InvalidState,"no free workspace for context/size"};
    }
    size_t BusyCount() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        size_t count=0; for (const auto &slot:state_->slots) if(slot.busy) ++count;
        return count;
    }
private:
    std::shared_ptr<Detail::WorkspaceState> state_;
};
}}
#endif
