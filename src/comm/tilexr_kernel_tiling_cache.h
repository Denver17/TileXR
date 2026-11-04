#ifndef TILEXR_COMM_KERNEL_TILING_CACHE_H
#define TILEXR_COMM_KERNEL_TILING_CACHE_H

#include <map>
#include <mutex>
#include <vector>

#include "acl/acl_rt.h"
#include "tilexr_log.h"
#include "../include/tilexr_types.h"

namespace TileXR {

// Communicator-owned immutable GM snapshots. An address remains valid until
// communicator destruction, which already requires all submitted work to finish.
class KernelTilingCache {
public:
    ~KernelTilingCache()
    {
        if (Clear() != TILEXR_SUCCESS) {
            TILEXR_LOG(ERROR) << "Kernel tiling memory cleanup failed";
        }
    }

    int Get(const void *data, uint32_t bytes, uint8_t **deviceData)
    {
        if (deviceData == nullptr) return TILEXR_ERROR_PARA_CHECK_FAIL;
        *deviceData = nullptr;
        if (data == nullptr || bytes == 0U) return TILEXR_ERROR_PARA_CHECK_FAIL;
        const auto *begin = static_cast<const uint8_t *>(data);
        std::vector<uint8_t> key(begin, begin + bytes);
        std::lock_guard<std::mutex> lock(mutex_);
        // Create the owning record before allocating GM so container allocation
        // cannot throw after allocation and lose the device pointer.
        auto &entry = entries_[key];
        if (!entry.ready) {
            if (entry.address == nullptr &&
                aclrtMalloc(&entry.address, bytes, ACL_MEM_MALLOC_NORMAL_ONLY) != ACL_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            // Synchronous initialization finishes before the address is exposed.
            // A failed copy leaves an unpublished allocation owned for retry/free.
            if (aclrtMemcpy(entry.address, bytes, data, bytes, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            entry.ready = true;
        }
        *deviceData = static_cast<uint8_t *>(entry.address);
        return TILEXR_SUCCESS;
    }

    int Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int result = TILEXR_SUCCESS;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.address != nullptr && aclrtFree(it->second.address) != ACL_SUCCESS) {
                result = TILEXR_ERROR_INTERNAL;
                ++it; // Retain ownership so a failed communicator teardown can retry.
            } else {
                it = entries_.erase(it);
            }
        }
        return result;
    }

private:
    struct Entry {
        void *address = nullptr;
        bool ready = false;
    };
    std::mutex mutex_;
    std::map<std::vector<uint8_t>, Entry> entries_;
};

} // namespace TileXR

#endif
