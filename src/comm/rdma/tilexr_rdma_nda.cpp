/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：创建 NDA QP/CQ 和 state，按实际队列容量分配 pending FIFO，映射 doorbell 并校验 device queue 描述。
 * 调用关系：VerbsProvider 使用 RdmaNdaResources；本文件动态加载 ibv_extend、verbs 和 ACL/HAL。
 * 资源与生命周期：记录 queue allocation、mapping 和 bootstrap MR；显式 teardown 保留失败项供重试，析构遇到存活资源时保留 Impl。
 *
 * Purpose: Create NDA QPs/CQs and state, size pending storage from queue capacity, map doorbells and validate device queue descriptors.
 * Calls: VerbsProvider uses RdmaNdaResources, which dynamically loads ibv_extend, verbs and ACL/HAL.
 * Lifetime: Tracks queue allocations, mappings and the bootstrap MR. Explicit teardown retains failed items for retry;
 *           destruction retains Impl when resources remain live.
 */
#include "rdma/tilexr_rdma_nda.h"
#include "rdma/tilexr_rdma_transport.h"
#include "rdma/tilexr_rdma_ub_res.h"
#include "tilexr_rdma_wire.h"
#include "tilexr_log.h"
#include <new>

#ifndef TILEXR_HAVE_IBV_EXTEND
#define TILEXR_HAVE_IBV_EXTEND 0
#endif
#ifndef TILEXR_HAVE_IBVERBS
#define TILEXR_HAVE_IBVERBS 0
#endif

#if TILEXR_HAVE_IBVERBS && TILEXR_HAVE_IBV_EXTEND
#include <ibv_extend.h>
#include <acl/acl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <limits>
#include <iterator>
#include <list>
#include <map>
#include <atomic>
#include <algorithm>

namespace TileXR {
namespace {
// Upper target only: requests must also obey the provider's advertised limits.
// The exported SQ reserves an additional invalid stop entry.
constexpr uint32_t kDirectQueueEntries = 8192U;
static_assert(kDirectQueueEntries > TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT,
    "direct rings must leave room beyond the default in-flight window");

bool Contains(uint64_t base, uint64_t span, uint64_t address, uint64_t bytes)
{
    return bytes != 0 && address >= base && address - base <= span &&
        bytes <= span - (address - base);
}
bool PowerOfTwo(uint32_t value) { return value >= 2 && (value & (value - 1)) == 0; }
bool DescribeUb(const doorbell_map_desc& desc, RdmaUbRes::Doorbell& db)
{
    constexpr uint64_t page = RdmaUbRes::kPageBytes;
    const uint64_t offset = desc.ub_res.bits.offset;
    if (desc.size == 0 || desc.size > 4096U * page || offset >= 4096U * page ||
        desc.size > 4096U * page - offset) { return false; }
    db = RdmaUbRes::Doorbell {};
    db.guidLow = desc.ub_res.guid_l;
    db.guidHigh = desc.ub_res.guid_h;
    db.index = static_cast<uint32_t>(offset / page);
    db.count = static_cast<uint32_t>(((offset % page) + desc.size + page - 1) / page);
    return true;
}
bool SameUb(const RdmaUbRes::Doorbell& a, const RdmaUbRes::Doorbell& b)
{
    return a.guidLow == b.guidLow && a.guidHigh == b.guidHigh &&
        a.index == b.index && a.count == b.count;
}
// HAL keys a mapping by device/process/resource ID. Communicators in the same
// process must not reuse another owner's GUID index, even on the same NPU.
std::atomic<uint64_t> nextUbMapping {0};
}

struct RdmaNdaResources::Impl {
    void* extendLibrary = nullptr;
    void* verbsLibrary = nullptr;
    void* aclLibrary = nullptr;
    void* halLibrary = nullptr;
    ibv_context_extend* context = nullptr;
    ibv_mr* bootstrapMr = nullptr;
    decltype(&ibv_reg_mr) registerMr = nullptr;
    decltype(&ibv_dereg_mr) deregisterMr = nullptr;
    decltype(&ibv_query_device) queryDevice = nullptr;
    bool active = false;
    uint32_t deviceId = 0;
    uint32_t dbMode = 0;
    uint32_t capabilities = 0;
    uint32_t requestedSendSge = 0;
    uint32_t requestedSendWr = 0;
    uint32_t requestedCqe = 0;
    uint64_t pageBytes = 0;
    int callbackStatus = TILEXR_SUCCESS;
    ibv_extend_ops ops {};

#define NDA_EXTEND_SYMBOLS(X) \
    X(ibv_extend_get_version) X(ibv_extend_check_version) \
    X(ibv_open_extend) X(ibv_close_extend) X(ibv_query_device_extend) \
    X(ibv_create_cq_extend) X(ibv_create_qp_extend) \
    X(ibv_destroy_qp_extend) X(ibv_destroy_cq_extend)
#define NDA_ACL_SYMBOLS(X) \
    X(aclrtGetDevice) X(aclrtMalloc) X(aclrtFree) X(aclrtMemset) X(aclrtMemcpy) \
    X(aclrtHostRegister) X(aclrtHostUnregister)
#define DECLARE(name) decltype(&name) name##Fn = nullptr;
    NDA_EXTEND_SYMBOLS(DECLARE)
    NDA_ACL_SYMBOLS(DECLARE)
#undef DECLARE
    using RegisterSegment = int (*)(uint32_t, uint64_t, uint64_t);
    using UnregisterSegment = int (*)(uint32_t, uint64_t);
    RegisterSegment registerSegment = nullptr;
    UnregisterSegment unregisterSegment = nullptr;

    struct Allocation {
        void* address = nullptr;
        size_t bytes = 0;
        bool registered = false;
    };
    struct Mapping {
        uint64_t host = 0;
        uint64_t bytes = 0;
        uint64_t device = 0;
        size_t references = 0;
    };
    struct MappingUse {
        uint64_t host = 0;
        uint64_t bytes = 0;
        Mapping* mapping = nullptr;
        size_t references = 0;
    };
    struct Queue {
        ibv_qp_extend* qp = nullptr;
        ibv_cq_extend* cq = nullptr;
        void* state = nullptr;
        void* pending = nullptr;
        uint32_t pendingCapacity = 0;
        uint32_t maxSendSge = 0;
        uint32_t maxSendWr = 0;
    };
    std::list<Allocation> allocations;
    std::list<Mapping> mappings;
    std::list<MappingUse> mappingUses;
    struct UbMapping {
        RdmaUbRes::Doorbell db {};
        uint32_t resource = 0;
        uint64_t device = 0;
        uint64_t bytes = 0;
        size_t references = 0;
    };
    std::list<UbMapping> ubMappings;
    using MapResource = int (*)(uint32_t, RdmaUbRes::MapIn*, RdmaUbRes::MapOut*);
    using UnmapResource = int (*)(uint32_t, RdmaUbRes::MapIn*);
    MapResource mapResource = nullptr;
    UnmapResource unmapResource = nullptr;
    std::map<int, Queue> queues;

    // ibv_extend 4.2 callbacks have no user data. Bind the instance only for a
    // synchronous extension call, never through a process-global current owner.
    // A callback on an unbound thread fails closed instead of using another NPU.
    static thread_local Impl* current;
    struct Call {
        Impl* previous;
        explicit Call(Impl& owner) : previous(current)
        {
            current = &owner;
            owner.callbackStatus = TILEXR_SUCCESS;
        }
        ~Call() { current = previous; }
    };

    ~Impl()
    {
        if (extendLibrary != nullptr) { dlclose(extendLibrary); }
        if (verbsLibrary != nullptr) { dlclose(verbsLibrary); }
        if (aclLibrary != nullptr) { dlclose(aclLibrary); }
        if (halLibrary != nullptr) { dlclose(halLibrary); }
    }
    bool Live() const
    {
        return context != nullptr || bootstrapMr != nullptr || !queues.empty() ||
            !allocations.empty() || !mappings.empty() || !ubMappings.empty();
    }
    int CheckDevice() const
    {
        int32_t selected = -1;
        return aclrtGetDeviceFn != nullptr && aclrtGetDeviceFn(&selected) == 0 &&
            selected >= 0 && static_cast<uint32_t>(selected) == deviceId ?
            TILEXR_SUCCESS : TILEXR_ERROR_NOT_INITIALIZED;
    }
    void Fail(int status = TILEXR_ERROR_INTERNAL)
    {
        if (callbackStatus == TILEXR_SUCCESS) { callbackStatus = status; }
    }
    int Load();
    int RegisterBootstrap(ibv_pd* pd);
    void* Allocate(size_t bytes, bool nicVisible = true);
    int Release(void* ptr);
    void* Map(doorbell_map_desc* desc);
    void* MapUb(doorbell_map_desc* desc);
    int UnmapUb(void* ptr, doorbell_map_desc* desc);
    int Unmap(void* ptr, doorbell_map_desc* desc);
    int Drain();
    bool DeviceMemory(uint64_t address, uint64_t bytes, bool requireSegment = true) const;
    bool MappedMemory(uint64_t address, uint64_t bytes) const;
    int View(const Queue& source, uint32_t gidIndex, uint32_t mtuEnum,
        TileXRRDMADeviceQueue& queue) const;

    static void* AllocCallback(size_t bytes) noexcept
    {
        if (current == nullptr) { return nullptr; }
        try { return current->Allocate(bytes); }
        catch (...) { current->Fail(); return nullptr; }
    }
    static void FreeCallback(void* ptr) noexcept
    {
        if (current == nullptr || ptr == nullptr) { return; }
        try { if (current->Release(ptr) != TILEXR_SUCCESS) { current->Fail(); } }
        catch (...) { current->Fail(); }
    }
    static void MemsetCallback(void* dst, int value, size_t bytes) noexcept
    {
        if (current == nullptr) { return; }
        if (current->CheckDevice() != TILEXR_SUCCESS ||
            !current->DeviceMemory(reinterpret_cast<uintptr_t>(dst), bytes) ||
            current->aclrtMemsetFn(dst, bytes, value, bytes) != 0) { current->Fail(); }
    }
    static int MemcpyCallback(void* dst, size_t capacity, void* src, size_t bytes,
        uint32_t direction) noexcept
    {
        if (current == nullptr) { return -1; }
        if (bytes > capacity || direction > MEMCPY_DIR_DEVICE_TO_DEVICE ||
            current->CheckDevice() != TILEXR_SUCCESS) { current->Fail(); return -1; }
        // Convert the extension enum explicitly: ACL need not share its values.
        const aclrtMemcpyKind kinds[] = {ACL_MEMCPY_HOST_TO_HOST, ACL_MEMCPY_HOST_TO_DEVICE,
            ACL_MEMCPY_DEVICE_TO_HOST, ACL_MEMCPY_DEVICE_TO_DEVICE};
        const bool dstDevice = direction == MEMCPY_DIR_HOST_TO_DEVICE || direction == MEMCPY_DIR_DEVICE_TO_DEVICE;
        const bool srcDevice = direction == MEMCPY_DIR_DEVICE_TO_HOST || direction == MEMCPY_DIR_DEVICE_TO_DEVICE;
        if ((dstDevice && !current->DeviceMemory(reinterpret_cast<uintptr_t>(dst), bytes)) ||
            (srcDevice && !current->DeviceMemory(reinterpret_cast<uintptr_t>(src), bytes)) ||
            current->aclrtMemcpyFn(dst, capacity, src, bytes, kinds[direction]) != 0) {
            current->Fail(); return -1;
        }
        return 0;
    }
    static void* MapCallback(doorbell_map_desc* desc) noexcept
    {
        if (current == nullptr) { return nullptr; }
        try { return current->Map(desc); }
        catch (...) { current->Fail(); return nullptr; }
    }
    static int UnmapCallback(void* ptr, doorbell_map_desc* desc) noexcept
    {
        if (current == nullptr) { return -1; }
        try {
            const int ret = current->Unmap(ptr, desc);
            if (ret != TILEXR_SUCCESS) { current->Fail(ret); }
            return ret == TILEXR_SUCCESS ? 0 : -1;
        } catch (...) { current->Fail(); return -1; }
    }
};
thread_local RdmaNdaResources::Impl* RdmaNdaResources::Impl::current = nullptr;

int RdmaNdaResources::Impl::Load()
{
    // Normal loader search only; never add the toolkit's stub devlib to RPATH.
    const char* names[] = {"libibv_extend.so", "libibv_extend.so.4", "libibv_extend.so.4.2.0"};
    for (const char* name : names) {
        extendLibrary = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (extendLibrary != nullptr) { break; }
    }
    if (extendLibrary == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
#define RESOLVE_EXTEND(name) name##Fn = reinterpret_cast<decltype(name##Fn)>(dlsym(extendLibrary, #name)); \
    if (name##Fn == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
    NDA_EXTEND_SYMBOLS(RESOLVE_EXTEND)
#undef RESOLVE_EXTEND
    uint32_t major = 0, minor = 0, patch = 0;
    ibv_extend_get_versionFn(&major, &minor, &patch);
    const uint64_t installedVersion = (uint64_t(minor) << 32) | patch;
    const uint64_t headerVersion = (uint64_t(IBV_EXTEND_VERSION_MINOR) << 32) | IBV_EXTEND_VERSION_PATCH;
    // UBNIC V1 is qualified against the installed 4.2 queue export contract.
    if (IBV_EXTEND_VERSION_MAJOR != 4 || IBV_EXTEND_VERSION_MINOR < 2 || major != 4 ||
        installedVersion < headerVersion ||
        ibv_extend_check_versionFn(IBV_EXTEND_VERSION_MAJOR,
            IBV_EXTEND_VERSION_MINOR, IBV_EXTEND_VERSION_PATCH) != 0) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    verbsLibrary = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
    if (verbsLibrary == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
    registerMr = reinterpret_cast<decltype(registerMr)>(dlsym(verbsLibrary, "ibv_reg_mr"));
    deregisterMr = reinterpret_cast<decltype(deregisterMr)>(dlsym(verbsLibrary, "ibv_dereg_mr"));
    queryDevice = reinterpret_cast<decltype(queryDevice)>(dlsym(verbsLibrary, "ibv_query_device"));
    if (registerMr == nullptr || deregisterMr == nullptr || queryDevice == nullptr) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    return TILEXR_SUCCESS;
}
#undef NDA_EXTEND_SYMBOLS

int RdmaNdaResources::Impl::RegisterBootstrap(ibv_pd* pd)
{
    if (bootstrapMr != nullptr) {
        return bootstrapMr->pd == pd ? TILEXR_SUCCESS : TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    // UBNIC CQ creation looks up the FE established by MR registration. Match
    // perftest ctx_init's MR-before-CQ order even before payload registration.
    constexpr size_t bytes = 2U * 1024U * 1024U;
    void* memory = AllocCallback(bytes);
    if (memory == nullptr) { return callbackStatus; }
    constexpr int ndaAccess = 1 << 21; // ibv_extend 4.2 / perftest_resources.c:2089.
    errno = 0;
    bootstrapMr = registerMr(pd, memory, bytes,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | ndaAccess);
    const int savedErrno = errno;
    if (bootstrapMr == nullptr) {
        TILEXR_LOG(ERROR) << "RDMA AIV bootstrap MR failed: device=" << deviceId
                          << " ext_cap=" << capabilities << " bytes=" << bytes
                          << " errno=" << savedErrno << "; no CQ or kernel submission attempted";
        return TILEXR_ERROR_INTERNAL; // DestroyQueues drains the retained UB backing.
    }
    return TILEXR_SUCCESS;
}

void* RdmaNdaResources::Impl::Allocate(size_t bytes, bool nicVisible)
{
    if (bytes == 0 || CheckDevice() != TILEXR_SUCCESS) { Fail(); return nullptr; }
    // Allocate bookkeeping before acquiring a driver resource. Keep failed frees
    // in this ledger even though the extension's free callback returns void.
    allocations.emplace_back();
    auto it = std::prev(allocations.end());
    const aclrtMemMallocPolicy policy = bytes < 2U * 1024U * 1024U ?
        ACL_MEM_MALLOC_NORMAL_ONLY : ACL_MEM_MALLOC_HUGE_ONLY;
    if (aclrtMallocFn(&it->address, bytes, policy) != 0 || it->address == nullptr) {
        allocations.erase(it); Fail(); return nullptr;
    }
    it->bytes = bytes;
    if (!nicVisible) { return it->address; }
    if (registerSegment(deviceId, reinterpret_cast<uintptr_t>(it->address), bytes) != 0) {
        void* ptr = it->address;
        Fail();
        Release(ptr);
        return nullptr;
    }
    it->registered = true;
    return it->address;
}

int RdmaNdaResources::Impl::Release(void* ptr)
{
    if (CheckDevice() != TILEXR_SUCCESS) { return TILEXR_ERROR_NOT_INITIALIZED; }
    for (auto it = allocations.begin(); it != allocations.end(); ++it) {
        if (it->address != ptr) { continue; }
        if (it->registered) {
            if (unregisterSegment(deviceId, reinterpret_cast<uintptr_t>(ptr)) != 0) {
                return TILEXR_ERROR_INTERNAL;
            }
            it->registered = false;
        }
        if (aclrtFreeFn(ptr) != 0) { return TILEXR_ERROR_INTERNAL; }
        allocations.erase(it);
        return TILEXR_SUCCESS;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void* RdmaNdaResources::Impl::MapUb(doorbell_map_desc* desc)
{
    RdmaUbRes::Doorbell db {};
    if (mapResource == nullptr || unmapResource == nullptr) { Fail(TILEXR_ERROR_NOT_SUPPORT); return nullptr; }
    if (CheckDevice() != TILEXR_SUCCESS || !DescribeUb(*desc, db)) { Fail(); return nullptr; }
    const uint64_t offset = desc->ub_res.bits.offset % RdmaUbRes::kPageBytes;
    for (auto& entry : ubMappings) {
        if (!SameUb(entry.db, db)) { continue; }
        if (entry.references == 0 || entry.references == std::numeric_limits<size_t>::max()) {
            Fail(); return nullptr;
        }
        ++entry.references;
        return reinterpret_cast<void*>(entry.device + offset);
    }
    const uint64_t identity = nextUbMapping.fetch_add(1, std::memory_order_relaxed);
    if (identity > 0xffffU) { Fail(); return nullptr; }
    ubMappings.emplace_back();
    auto entry = std::prev(ubMappings.end());
    entry->db = db;
    entry->resource = db.index | (static_cast<uint32_t>(identity) << 12);
    auto in = RdmaUbRes::Request(entry->db, entry->resource);
    RdmaUbRes::MapOut out {};
    const int ret = mapResource(deviceId, &in, &out);
    if (ret != 0) {
        TILEXR_LOG(ERROR) << "RDMA UB doorbell map failed device=" << deviceId << " status=" << ret;
        ubMappings.erase(entry); Fail(); return nullptr;
    }
    entry->device = out.address;
    entry->bytes = uint64_t(db.count) * RdmaUbRes::kPageBytes;
    // A successful but unusable mapping remains owned for explicit teardown.
    if (out.address == 0 || out.bytes < entry->bytes ||
        out.address > std::numeric_limits<uint64_t>::max() - entry->bytes) { Fail(); return nullptr; }
    entry->references = 1;
    return reinterpret_cast<void*>(entry->device + offset);
}

int RdmaNdaResources::Impl::UnmapUb(void* ptr, doorbell_map_desc* desc)
{
    RdmaUbRes::Doorbell db {};
    if (CheckDevice() != TILEXR_SUCCESS || !DescribeUb(*desc, db)) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    const uint64_t offset = desc->ub_res.bits.offset % RdmaUbRes::kPageBytes;
    for (auto it = ubMappings.begin(); it != ubMappings.end(); ++it) {
        if (!SameUb(it->db, db) || it->device + offset != reinterpret_cast<uintptr_t>(ptr) ||
            it->references == 0) { continue; }
        if (it->references > 1) { --it->references; return TILEXR_SUCCESS; }
        auto in = RdmaUbRes::Request(it->db, it->resource);
        if (unmapResource(deviceId, &in) != 0) { return TILEXR_ERROR_INTERNAL; }
        ubMappings.erase(it);
        return TILEXR_SUCCESS;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void* RdmaNdaResources::Impl::Map(doorbell_map_desc* desc)
{
    if (desc != nullptr && desc->type == DB_MAP_MODE_UB_RES) { return MapUb(desc); }
    if (desc == nullptr || desc->type != DB_MAP_MODE_HOST_VA) {
        // Unknown resource descriptors must never be reinterpreted as Host VAs.
        Fail(TILEXR_ERROR_NOT_SUPPORT); return nullptr;
    }
    if (CheckDevice() != TILEXR_SUCCESS || desc->hva == 0 || desc->size == 0 ||
        desc->size > std::numeric_limits<uint64_t>::max() - desc->hva ||
        desc->hva + desc->size > std::numeric_limits<uint64_t>::max() - (pageBytes - 1)) {
        Fail(); return nullptr;
    }
    const uint64_t base = desc->hva & ~(pageBytes - 1);
    const uint64_t end = (desc->hva + desc->size + pageBytes - 1) & ~(pageBytes - 1);
    Mapping* mapped = nullptr;
    for (auto& entry : mappings) {
        if (Contains(entry.host, entry.bytes, desc->hva, desc->size)) {
            if (entry.device == 0 || entry.device > std::numeric_limits<uint64_t>::max() - entry.bytes) {
                Fail(); return nullptr;
            }
            mapped = &entry;
            break;
        }
        // ACL cannot safely register overlapping extents independently. Reuse
        // a containing registration, or reject until the old mapping is gone.
        if (base < entry.host + entry.bytes && entry.host < end) { Fail(); return nullptr; }
    }
    for (auto& use : mappingUses) {
        if (use.host == desc->hva && use.bytes == desc->size && use.mapping == mapped) {
            if (use.references == std::numeric_limits<size_t>::max() ||
                mapped->references == std::numeric_limits<size_t>::max()) { Fail(); return nullptr; }
            ++use.references;
            ++mapped->references;
            return reinterpret_cast<void*>(mapped->device + desc->hva - mapped->host);
        }
    }
    mappingUses.emplace_back();
    auto use = std::prev(mappingUses.end());
    if (mapped == nullptr) {
        try { mappings.emplace_back(); }
        catch (...) { mappingUses.erase(use); throw; }
        auto entry = std::prev(mappings.end());
        void* device = nullptr;
        const int ret = aclrtHostRegisterFn(reinterpret_cast<void*>(base), end - base,
            ACL_HOST_REGISTER_IOMEMORY, &device);
        if (ret != 0) {
            mappings.erase(entry); mappingUses.erase(use); Fail(); return nullptr;
        }
        entry->host = base;
        entry->bytes = end - base;
        entry->device = reinterpret_cast<uintptr_t>(device);
        mapped = &*entry;
        if (device == nullptr || entry->device > std::numeric_limits<uint64_t>::max() - entry->bytes) {
            // Registration succeeded, so retain its Host extent for cleanup
            // even if the returned device alias is unusable.
            mappingUses.erase(use); Fail(); return nullptr;
        }
    }
    use->host = desc->hva;
    use->bytes = desc->size;
    use->mapping = mapped;
    use->references = 1;
    ++mapped->references;
    return reinterpret_cast<void*>(mapped->device + desc->hva - mapped->host);
}

int RdmaNdaResources::Impl::Unmap(void* ptr, doorbell_map_desc* desc)
{
    if (desc != nullptr && desc->type == DB_MAP_MODE_UB_RES) { return UnmapUb(ptr, desc); }
    if (desc == nullptr || desc->type != DB_MAP_MODE_HOST_VA || CheckDevice() != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (auto it = mappingUses.begin(); it != mappingUses.end(); ++it) {
        Mapping* mapped = it->mapping;
        if (it->host != desc->hva || it->bytes != desc->size ||
            reinterpret_cast<uintptr_t>(ptr) != mapped->device + it->host - mapped->host) { continue; }
        if (mapped->references == 1) {
            if (aclrtHostUnregisterFn(reinterpret_cast<void*>(mapped->host)) != 0) {
                return TILEXR_ERROR_INTERNAL;
            }
            for (auto entry = mappings.begin(); entry != mappings.end(); ++entry) {
                if (&*entry == mapped) { mappings.erase(entry); break; }
            }
            mappingUses.erase(it);
        } else {
            --mapped->references;
            if (--it->references == 0) { mappingUses.erase(it); }
        }
        return TILEXR_SUCCESS;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool RdmaNdaResources::Impl::DeviceMemory(uint64_t address, uint64_t bytes, bool requireSegment) const
{
    for (const auto& entry : allocations) {
        if ((!requireSegment || entry.registered) &&
            Contains(reinterpret_cast<uintptr_t>(entry.address), entry.bytes, address, bytes)) {
            return true;
        }
    }
    return false;
}
bool RdmaNdaResources::Impl::MappedMemory(uint64_t address, uint64_t bytes) const
{
    for (const auto& entry : ubMappings) {
        if (Contains(entry.device, entry.bytes, address, bytes)) { return true; }
    }
    for (const auto& entry : mappings) {
        if (Contains(entry.device, entry.bytes, address, bytes)) { return true; }
    }
    return false;
}

int RdmaNdaResources::Impl::View(const Queue& source, uint32_t gidIndex, uint32_t mtuEnum,
    TileXRRDMADeviceQueue& queue) const
{
    if (source.qp == nullptr || source.qp->qp == nullptr || source.cq == nullptr || source.cq->cq == nullptr) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    const auto& sq = source.qp->sq_info;
    const auto& cq = source.cq->cq_info;
    const uint64_t db = reinterpret_cast<uintptr_t>(sq.db_hw_va.iov_base);
    const uint64_t sqDbr = reinterpret_cast<uintptr_t>(sq.dbr_pi_va.iov_base);
    const uint64_t cqDbr = reinterpret_cast<uintptr_t>(cq.dbr_ci_va.iov_base);
    const uint64_t dbBytes = dbMode == TILEXR_RDMA_DB_HARD ? 2048 : 8;
    const uint32_t qpn = source.qp->qp->qp_num;
    const uint32_t cos = source.qp->vendor_priv_info.vendor1.db_cos;
    if (source.maxSendSge == 0 || source.maxSendWr == 0 || sq.qbuf.entry_size != 64 || cq.qbuf.entry_size != 64 ||
        !PowerOfTwo(sq.qbuf.entry_cnt) || !PowerOfTwo(cq.qbuf.entry_cnt) ||
        (sq.qbuf.base & 63U) != 0 || (cq.qbuf.base & 63U) != 0 ||
        !DeviceMemory(sq.qbuf.base, uint64_t(sq.qbuf.entry_cnt) * 64) ||
        !DeviceMemory(cq.qbuf.base, uint64_t(cq.qbuf.entry_cnt) * 64) ||
        !DeviceMemory(reinterpret_cast<uintptr_t>(source.state), sizeof(TileXRRDMADeviceState), false) ||
        source.pendingCapacity != RdmaWire::PipelineRecordCapacity(sq.qbuf.entry_cnt,
            cq.qbuf.entry_cnt, source.maxSendWr) ||
        !DeviceMemory(reinterpret_cast<uintptr_t>(source.pending), uint64_t(source.pendingCapacity) * 8U, false) ||
        (reinterpret_cast<uintptr_t>(source.pending) & 63U) != 0 ||
        (reinterpret_cast<uintptr_t>(source.state) & 63U) != 0 || qpn == 0 || qpn > 0xfffffU ||
        cos > 7 || gidIndex > 127 || mtuEnum < IBV_MTU_256 || mtuEnum > IBV_MTU_4096 ||
        // iov_len describes the 8-byte doorbell register. Hard mode selects
        // PI through an offset within its mapped aperture; validate that full
        // aperture against our mapping ledger, not against the register size.
        (db & 7U) != 0 || sq.db_hw_va.iov_len < 8 || !MappedMemory(db, dbBytes) ||
        (cqDbr & 3U) != 0 || cq.dbr_ci_va.iov_len < 4 || !DeviceMemory(cqDbr, 4) ||
        (dbMode == TILEXR_RDMA_DB_SOFT && ((sqDbr & 3U) != 0 ||
            sq.dbr_pi_va.iov_len < 4 || !DeviceMemory(sqDbr, 4))) ||
        source.cq->resv[0] > std::numeric_limits<uint32_t>::max()) {
        TILEXR_LOG(ERROR) << "RDMA NDA queue export does not match the UBNIC V2 device ABI"
            << " sq_depth=" << sq.qbuf.entry_cnt << " sq_entry=" << sq.qbuf.entry_size
            << " cq_depth=" << cq.qbuf.entry_cnt << " cq_entry=" << cq.qbuf.entry_size
            << " sq_owned=" << DeviceMemory(sq.qbuf.base, uint64_t(sq.qbuf.entry_cnt) * 64)
            << " cq_owned=" << DeviceMemory(cq.qbuf.base, uint64_t(cq.qbuf.entry_cnt) * 64)
            << " qpn=" << qpn << " cos=" << cos << " cqn=" << source.cq->resv[0]
            << " db_bytes=" << sq.db_hw_va.iov_len << " db_mapped=" << MappedMemory(db, dbBytes)
            << " sq_dbr_bytes=" << sq.dbr_pi_va.iov_len << " sq_dbr_owned=" << DeviceMemory(sqDbr, 4)
            << " cq_dbr_bytes=" << cq.dbr_ci_va.iov_len << " cq_dbr_owned=" << DeviceMemory(cqDbr, 4);
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    queue = TileXRRDMADeviceQueue {};
    queue.sq = sq.qbuf.base;
    queue.cq = cq.qbuf.base;
    queue.sqDbr = sqDbr;
    queue.cqDbr = cqDbr;
    queue.doorbell = db;
    queue.state = reinterpret_cast<uintptr_t>(source.state);
    queue.sqDbValue = RdmaWire::MakeSqDbValue(qpn, gidIndex, mtuEnum, cos);
    queue.doorbellBytes = dbBytes;
    queue.qpn = qpn;
    queue.cqn = static_cast<uint32_t>(source.cq->resv[0]);
    queue.sqDepth = sq.qbuf.entry_cnt;
    queue.sqEntryBytes = sq.qbuf.entry_size;
    queue.cqDepth = cq.qbuf.entry_cnt;
    queue.cqEntryBytes = cq.qbuf.entry_size;
    queue.dbMode = dbMode;
    queue.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
    queue.maxSendSge = source.maxSendSge;
    queue.maxSendWr = source.maxSendWr;
    queue.pendingBatches = reinterpret_cast<uintptr_t>(source.pending);
    queue.pendingCapacity = source.pendingCapacity;
    return TILEXR_SUCCESS;
}

int RdmaNdaResources::Impl::Drain()
{
    // Caller has destroyed every extension QP/CQ, so no driver object can still
    // use a callback resource retained after a void free / failed db_unmap.
    // Keep the FE registration through all queue destroys, and its backing
    // through successful MR deregistration, including retryable failures.
    if (bootstrapMr != nullptr) {
        if (deregisterMr(bootstrapMr) != 0) { return TILEXR_ERROR_INTERNAL; }
        bootstrapMr = nullptr;
    }
    int status = TILEXR_SUCCESS;
    for (auto it = ubMappings.begin(); it != ubMappings.end();) {
        auto in = RdmaUbRes::Request(it->db, it->resource);
        if (unmapResource(deviceId, &in) != 0) { status = TILEXR_ERROR_INTERNAL; ++it; }
        else { it = ubMappings.erase(it); }
    }
    for (auto it = mappings.begin(); it != mappings.end();) {
        if (aclrtHostUnregisterFn(reinterpret_cast<void*>(it->host)) != 0) {
            status = TILEXR_ERROR_INTERNAL; ++it; continue;
        }
        Mapping* mapped = &*it;
        mappingUses.remove_if([mapped](const MappingUse& use) { return use.mapping == mapped; });
        it = mappings.erase(it);
    }
    for (auto it = allocations.begin(); it != allocations.end();) {
        void* ptr = it->address;
        ++it;
        if (Release(ptr) != TILEXR_SUCCESS) { status = TILEXR_ERROR_INTERNAL; }
    }
    return status;
}

RdmaNdaResources::RdmaNdaResources() : impl_(new (std::nothrow) Impl) {}
RdmaNdaResources::~RdmaNdaResources()
{
    // Teardown is explicit and retryable. Do not free memory or unload callback
    // code if a caller abandons a live/failed collective Shutdown.
    if (impl_ != nullptr && impl_->Live()) {
        TILEXR_LOG(ERROR) << "RDMA NDA resources retained after incomplete shutdown";
        impl_.release();
    }
}
bool RdmaNdaResources::Active() const { return impl_ != nullptr && impl_->active; }

int RdmaNdaResources::Open(ibv_context* context, const RdmaConfig& config)
{
    if (impl_ == nullptr) { return TILEXR_ERROR_INTERNAL; }
    auto& self = *impl_;
    if (self.Live() || self.extendLibrary != nullptr) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (!config.aivDirect || !config.indepUb || context == nullptr ||
        (config.doorbellMode != TILEXR_RDMA_DB_SOFT && config.doorbellMode != TILEXR_RDMA_DB_HARD)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    // This device encoder implements UBNIC, not arbitrary NDA-capable vendors.
    if (config.device.compare(0, 5, "hrn5_") != 0) { return TILEXR_ERROR_NOT_SUPPORT; }
    self.deviceId = config.deviceId;
    self.dbMode = config.doorbellMode;
    int ret = self.Load();
    if (ret != TILEXR_SUCCESS) { return ret; }
    ibv_device_attr limits {};
    errno = 0;
    const int limitsStatus = self.queryDevice(context, &limits);
    const int limitsErrno = errno;
    if (limitsStatus != 0 || limits.max_qp_wr < 1 || limits.max_cqe < 2 || limits.max_sge < 1) {
        TILEXR_LOG(ERROR) << "RDMA AIV queue limits unavailable: status=" << limitsStatus
                          << " errno=" << limitsErrno << " max_qp_wr=" << limits.max_qp_wr
                          << " max_cqe=" << limits.max_cqe;
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    self.requestedSendWr = std::min(kDirectQueueEntries - 1U, static_cast<uint32_t>(limits.max_qp_wr));
    self.requestedSendSge = std::min(TILEXR_RDMA_DEVICE_MAX_SGE, static_cast<uint32_t>(limits.max_sge));
    self.requestedCqe = std::min(kDirectQueueEntries, static_cast<uint32_t>(limits.max_cqe));
    self.context = self.ibv_open_extendFn(context);
    if (self.context == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
    ibv_device_attr_extend attr {};
    errno = 0;
    const int queryStatus = self.ibv_query_device_extendFn(self.context, &attr);
    const int queryErrno = errno;
    if (queryStatus != 0) {
        TILEXR_LOG(ERROR) << "RDMA AIV query_device_extend failed: status=" << queryStatus
                          << " errno=" << queryErrno;
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    self.capabilities = attr.ext_cap;
    if ((attr.ext_cap & IBV_EXTEND_DEV_NDA) == 0) {
        // The standalone perftest path attempts the extension APIs directly.
        // The bit alone does not establish whether these calls work. Only
        // successful resource creation AND validated device exports permit use.
        TILEXR_LOG(WARN) << "RDMA AIV ext_cap=" << attr.ext_cap
                         << "; checking actual extension resources without SHMEM/Hcomm";
    }
    self.aclLibrary = dlopen("libascendcl.so", RTLD_NOW | RTLD_LOCAL);
    self.halLibrary = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    if (self.aclLibrary == nullptr || self.halLibrary == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
#define RESOLVE_ACL(name) self.name##Fn = reinterpret_cast<decltype(self.name##Fn)>(dlsym(self.aclLibrary, #name)); \
    if (self.name##Fn == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
    NDA_ACL_SYMBOLS(RESOLVE_ACL)
#undef RESOLVE_ACL
#undef NDA_ACL_SYMBOLS
    self.registerSegment = reinterpret_cast<Impl::RegisterSegment>(dlsym(self.halLibrary, "halMemRegUbSegment"));
    self.unregisterSegment = reinterpret_cast<Impl::UnregisterSegment>(dlsym(self.halLibrary, "halMemUnRegUbSegment"));
    self.mapResource = reinterpret_cast<Impl::MapResource>(dlsym(self.halLibrary, "halResAddrMapV2"));
    self.unmapResource = reinterpret_cast<Impl::UnmapResource>(dlsym(self.halLibrary, "halResAddrUnmapV2"));
    if (self.registerSegment == nullptr || self.unregisterSegment == nullptr) { return TILEXR_ERROR_NOT_SUPPORT; }
    ret = self.CheckDevice();
    if (ret != TILEXR_SUCCESS) { return ret; }
    const long pageBytes = sysconf(_SC_PAGESIZE);
    if (pageBytes <= 0 || (pageBytes & (pageBytes - 1)) != 0) { return TILEXR_ERROR_NOT_SUPPORT; }
    self.pageBytes = static_cast<uint64_t>(pageBytes);
    self.ops.alloc = Impl::AllocCallback;
    self.ops.free = Impl::FreeCallback;
    self.ops.memset_s = Impl::MemsetCallback;
    self.ops.memcpy_s = Impl::MemcpyCallback;
    self.ops.db_mmap = Impl::MapCallback;
    self.ops.db_unmap = Impl::UnmapCallback;
    self.active = true;
    return TILEXR_SUCCESS;
}

int RdmaNdaResources::Create(int peer, ibv_pd* pd, ibv_qp*& qp, ibv_cq*& cq)
{
    qp = nullptr;
    cq = nullptr;
    if (!Active() || pd == nullptr || impl_->queues.count(peer) != 0) { return TILEXR_ERROR_NOT_INITIALIZED; }
    auto& self = *impl_;
    if (self.CheckDevice() != TILEXR_SUCCESS) { return TILEXR_ERROR_NOT_INITIALIZED; }
    Impl::Call call(self);
    const int registered = self.RegisterBootstrap(pd);
    if (registered != TILEXR_SUCCESS) { return registered; }
    Impl::Queue* queue = nullptr;
    try { queue = &self.queues[peer]; }
    catch (...) { return TILEXR_ERROR_INTERNAL; }
    ibv_cq_init_attr_extend cqInit {};
    cqInit.attr.cqe = self.requestedCqe;
    cqInit.attr.parent_domain = pd;
    cqInit.type = QU_BUF_DMA_MODE_INDEP_UB;
    cqInit.ops = &self.ops;
    const int requestedCqe = cqInit.attr.cqe;
    errno = 0;
    queue->cq = self.ibv_create_cq_extendFn(self.context, &cqInit);
    const int cqErrno = errno;
    if (queue->cq == nullptr || queue->cq->cq == nullptr || self.callbackStatus != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "RDMA AIV create_cq_extend failed: peer=" << peer
                          << " device=" << self.deviceId << " ext_cap=" << self.capabilities
                          << " dma_mode=" << cqInit.type << " requested_cqe=" << requestedCqe
                          << " provider_cqe=" << cqInit.attr.cqe
                          << " errno=" << cqErrno << " callback_status=" << self.callbackStatus
                          << "; no QP or kernel submission attempted";
        return self.callbackStatus != TILEXR_SUCCESS ? self.callbackStatus : TILEXR_ERROR_INTERNAL;
    }
    cq = queue->cq->cq;
    ibv_qp_init_attr_extend qpInit {};
    qpInit.pd = pd;
    qpInit.attr.send_cq = cq;
    qpInit.attr.recv_cq = cq;
    qpInit.attr.qp_type = IBV_QPT_RC;
    qpInit.attr.cap.max_send_wr = self.requestedSendWr;
    qpInit.attr.cap.max_recv_wr = 1;
    qpInit.attr.cap.max_send_sge = self.requestedSendSge;
    qpInit.attr.cap.max_recv_sge = 1;
    qpInit.attr.sq_sig_all = 1;
    qpInit.type = QU_BUF_DMA_MODE_INDEP_UB;
    qpInit.ops = &self.ops;
    errno = 0;
    queue->qp = self.ibv_create_qp_extendFn(self.context, &qpInit);
    const int qpErrno = errno;
    if (queue->qp == nullptr || queue->qp->qp == nullptr || self.callbackStatus != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "RDMA AIV create_qp_extend failed: peer=" << peer
                          << " device=" << self.deviceId << " ext_cap=" << self.capabilities
                          << " requested_send_wr=" << self.requestedSendWr
                          << " errno=" << qpErrno << " callback_status=" << self.callbackStatus
                          << "; no kernel submission attempted";
        return self.callbackStatus != TILEXR_SUCCESS ? self.callbackStatus : TILEXR_ERROR_INTERNAL;
    }
    queue->maxSendSge = std::min(qpInit.attr.cap.max_send_sge, self.requestedSendSge);
    queue->maxSendWr = std::min(qpInit.attr.cap.max_send_wr, self.requestedSendWr);
    qp = queue->qp->qp;
    // The NIC never accesses this local AIV state. Avoid registering an
    // unqualified tiny UB segment; queue callbacks above remain NIC registered.
    try { queue->state = self.Allocate(sizeof(TileXRRDMADeviceState), false); }
    catch (...) { return TILEXR_ERROR_INTERNAL; }
    if (queue->state == nullptr) { return TILEXR_ERROR_INTERNAL; }
    if (self.aclrtMemsetFn(queue->state, sizeof(TileXRRDMADeviceState), 0,
        sizeof(TileXRRDMADeviceState)) != 0) { return TILEXR_ERROR_INTERNAL; }
    const uint32_t sqDepth = queue->qp->sq_info.qbuf.entry_cnt;
    const uint32_t cqDepth = queue->cq->cq_info.qbuf.entry_cnt;
    if (!PowerOfTwo(sqDepth) || !PowerOfTwo(cqDepth) || queue->maxSendWr == 0U) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    queue->pendingCapacity = RdmaWire::PipelineRecordCapacity(sqDepth, cqDepth, queue->maxSendWr);
    // One record per potentially outstanding WR also covers one-WR tail batches
    // and callers that change postBatch. Records are written before publication.
    try { queue->pending = self.Allocate(size_t(queue->pendingCapacity) * sizeof(uint64_t), false); }
    catch (...) { return TILEXR_ERROR_INTERNAL; }
    if (queue->pending == nullptr) { return TILEXR_ERROR_INTERNAL; }
    return self.callbackStatus;
}

int RdmaNdaResources::DeviceQueue(int peer, uint32_t gidIndex, uint32_t mtuEnum,
    TileXRRDMADeviceQueue& queue) const
{
    queue = TileXRRDMADeviceQueue {};
    if (!Active()) { return TILEXR_ERROR_NOT_SUPPORT; }
    const auto it = impl_->queues.find(peer);
    if (it == impl_->queues.end()) { return TILEXR_ERROR_NOT_INITIALIZED; }
    return impl_->View(it->second, gidIndex, mtuEnum, queue);
}

int RdmaNdaResources::DestroyQueues()
{
    if (impl_ == nullptr) { return TILEXR_SUCCESS; }
    auto& self = *impl_;
    if (self.bootstrapMr == nullptr && self.queues.empty() &&
        self.allocations.empty() && self.mappings.empty() && self.ubMappings.empty()) { return TILEXR_SUCCESS; }
    if (self.CheckDevice() != TILEXR_SUCCESS) { return TILEXR_ERROR_NOT_INITIALIZED; }
    Impl::Call call(self);
    int status = TILEXR_SUCCESS;
    bool handlesRemain = false;
    for (auto& entry : self.queues) {
        auto& queue = entry.second;
        if (queue.qp != nullptr) {
            if (self.ibv_destroy_qp_extendFn(self.context, queue.qp) != 0) {
                status = TILEXR_ERROR_INTERNAL; handlesRemain = true; continue;
            }
            queue.qp = nullptr;
        }
        if (queue.cq != nullptr) {
            if (self.ibv_destroy_cq_extendFn(self.context, queue.cq) != 0) {
                status = TILEXR_ERROR_INTERNAL; handlesRemain = true; continue;
            }
            queue.cq = nullptr;
        }
    }
    if (!handlesRemain) {
        // Successful destroys invalidate their handles even if a void callback
        // failed. Retry the remaining resources, never destroy a freed handle.
        const int drained = self.Drain();
        if (drained == TILEXR_SUCCESS) { self.queues.clear(); }
        else { status = drained; }
    }
    if (status == TILEXR_SUCCESS && self.callbackStatus != TILEXR_SUCCESS) { status = self.callbackStatus; }
    return status;
}

int RdmaNdaResources::Close()
{
    if (impl_ == nullptr) { return TILEXR_SUCCESS; }
    auto& self = *impl_;
    self.active = false;
    if (self.bootstrapMr != nullptr || !self.queues.empty() ||
        !self.allocations.empty() || !self.mappings.empty() || !self.ubMappings.empty()) {
        return TILEXR_ERROR_INTERNAL;
    }
    if (self.context != nullptr) {
        if (self.ibv_close_extendFn(self.context) != 0) { return TILEXR_ERROR_INTERNAL; }
        self.context = nullptr;
    }
    return TILEXR_SUCCESS;
}
} // namespace TileXR
#else
namespace TileXR {
struct RdmaNdaResources::Impl {};
RdmaNdaResources::RdmaNdaResources() = default;
RdmaNdaResources::~RdmaNdaResources() = default;
int RdmaNdaResources::Open(ibv_context*, const RdmaConfig&) { return TILEXR_ERROR_NOT_SUPPORT; }
bool RdmaNdaResources::Active() const { return false; }
int RdmaNdaResources::Create(int, ibv_pd*, ibv_qp*&, ibv_cq*&) { return TILEXR_ERROR_NOT_SUPPORT; }
int RdmaNdaResources::DeviceQueue(int, uint32_t, uint32_t, TileXRRDMADeviceQueue& queue) const
{
    queue = TileXRRDMADeviceQueue {};
    return TILEXR_ERROR_NOT_SUPPORT;
}
int RdmaNdaResources::DestroyQueues() { return TILEXR_SUCCESS; }
int RdmaNdaResources::Close() { return TILEXR_SUCCESS; }
} // namespace TileXR
#endif
