/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_A5_CLEANUP_H
#define TILEXR_SDMA_A5_CLEANUP_H

#include <cstddef>
#include <vector>

namespace TileXR {
namespace detail {

using A5CleanupFn = int (*)(void*, void*);
using A5ConstCleanupFn = int (*)(void*, const void*);

struct A5QueryCleanupOps {
    void* opaque = nullptr;
    A5CleanupFn setCurrentContext = nullptr;
    A5CleanupFn destroyStream = nullptr;
    A5CleanupFn destroyContext = nullptr;
    A5CleanupFn freeDevice = nullptr;
    A5ConstCleanupFn destroyTensor = nullptr;
};

struct A5PendingQueryCleanup {
    void* ownerContext = nullptr;
    void* isolatedContext = nullptr;
    void* queryStream = nullptr;
    void* healthStream = nullptr;
    std::vector<void*> ownerBuffers;
    std::vector<void*> isolatedBuffers;
    std::vector<const void*> tensors;
    void* restoreContext = nullptr;
    bool restorePending = false;

    bool Empty() const
    {
        return isolatedContext == nullptr && queryStream == nullptr &&
            healthStream == nullptr && ownerBuffers.empty() &&
            isolatedBuffers.empty() && tensors.empty() && !restorePending;
    }
};

inline bool A5SetCleanupContext(const A5QueryCleanupOps& ops, void* context)
{
    return ops.setCurrentContext != nullptr &&
        ops.setCurrentContext(ops.opaque, context) == 0;
}

inline bool A5ReleaseCleanupHandle(void*& handle, A5CleanupFn cleanup,
                                   const A5QueryCleanupOps& ops)
{
    if (handle == nullptr) {
        return true;
    }
    if (cleanup != nullptr && cleanup(ops.opaque, handle) == 0) {
        handle = nullptr;
        return true;
    }
    return false;
}

inline bool A5ReleaseCleanupHandles(std::vector<void*>& handles,
                                    A5CleanupFn cleanup,
                                    const A5QueryCleanupOps& ops)
{
    bool released = true;
    size_t reverse = handles.size();
    while (reverse > 0U) {
        --reverse;
        if (handles[reverse] != nullptr && cleanup != nullptr &&
            cleanup(ops.opaque, handles[reverse]) == 0) {
            handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(reverse));
        } else {
            released = false;
        }
    }
    return released;
}

inline bool A5ReleaseCleanupTensors(std::vector<const void*>& tensors,
                                    const A5QueryCleanupOps& ops)
{
    bool released = true;
    size_t reverse = tensors.size();
    while (reverse > 0U) {
        --reverse;
        if (tensors[reverse] != nullptr && ops.destroyTensor != nullptr &&
            ops.destroyTensor(ops.opaque, tensors[reverse]) == 0) {
            tensors.erase(tensors.begin() + static_cast<std::ptrdiff_t>(reverse));
        } else {
            released = false;
        }
    }
    return released;
}

inline bool CleanupA5QueryResources(A5PendingQueryCleanup& state,
                                    const A5QueryCleanupOps& ops,
                                    void* callerContext)
{
    bool released = true;
    if (state.restorePending) {
        if (!A5SetCleanupContext(ops, state.restoreContext)) {
            return false;
        }
        state.restoreContext = nullptr;
        state.restorePending = false;
    }

    if (state.healthStream != nullptr) {
        if (state.ownerContext == nullptr ||
            !A5SetCleanupContext(ops, state.ownerContext)) {
            released = false;
        } else {
            released = A5ReleaseCleanupHandle(
                state.healthStream, ops.destroyStream, ops) && released;
        }
    }

    const bool hasIsolatedResources = state.queryStream != nullptr ||
        !state.isolatedBuffers.empty() || !state.tensors.empty();
    if (state.isolatedContext != nullptr) {
        if (!A5SetCleanupContext(ops, state.isolatedContext)) {
            released = false;
        } else {
            released = A5ReleaseCleanupHandle(
                state.queryStream, ops.destroyStream, ops) && released;
            if (state.queryStream == nullptr) {
                released = A5ReleaseCleanupHandles(
                    state.isolatedBuffers, ops.freeDevice, ops) && released;
                released = A5ReleaseCleanupTensors(state.tensors, ops) && released;
            }
            if (state.queryStream == nullptr && state.tensors.empty() &&
                state.isolatedBuffers.empty()) {
                released = A5ReleaseCleanupHandle(
                    state.isolatedContext, ops.destroyContext, ops) && released;
            }
        }
    } else if (hasIsolatedResources) {
        released = false;
    }

    const bool isolatedReleased = state.isolatedContext == nullptr &&
        state.queryStream == nullptr && state.isolatedBuffers.empty() &&
        state.tensors.empty();
    if (!state.ownerBuffers.empty() && state.healthStream == nullptr &&
        isolatedReleased) {
        if (state.ownerContext == nullptr ||
            !A5SetCleanupContext(ops, state.ownerContext)) {
            released = false;
        } else {
            released = A5ReleaseCleanupHandles(
                state.ownerBuffers, ops.freeDevice, ops) && released;
        }
    } else if (!state.ownerBuffers.empty()) {
        released = false;
    }

    if (!A5SetCleanupContext(ops, callerContext)) {
        state.restoreContext = callerContext;
        state.restorePending = true;
        released = false;
    }
    return released && state.Empty();
}

} // namespace detail
} // namespace TileXR

#endif // TILEXR_SDMA_A5_CLEANUP_H
