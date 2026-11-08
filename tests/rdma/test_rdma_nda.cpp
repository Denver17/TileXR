/*
 * 功能：验证 NDA queue 容量、pending FIFO、device queue 导出、mapping 共享，以及创建和清理失败后的重试行为。
 * 调用关系：调用实际 RdmaNdaResources；fake verbs、ibv_extend、ACL/HAL DSO 提供资源计数和失败注入。
 * 资源与生命周期：测试显式调用 DestroyQueues/Close 并检查资源计数；不创建真实 NPU/NIC 资源。
 *
 * Purpose: Test NDA queue capacity, pending FIFOs, queue exports, shared mappings and retry behavior after setup/cleanup failures.
 * Calls: Exercises real RdmaNdaResources with resource counters and failure injection from fake verbs, ibv_extend and ACL/HAL DSOs.
 * Lifetime: Explicitly calls DestroyQueues/Close and checks resource counts; creates no real NPU/NIC resources.
 */
#include "rdma/tilexr_rdma_nda.h"
#include "rdma/tilexr_rdma_transport.h"
#include <infiniband/verbs.h>
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include <cerrno>
#include <utility>

using namespace TileXR;
template <typename T> T Symbol(void* library, const char* name)
{
    auto result = reinterpret_cast<T>(dlsym(library, name));
    assert(result);
    return result;
}
int main()
{
    void* extension = dlopen("libibv_extend.so", RTLD_NOW | RTLD_LOCAL);
    void* acl = dlopen("libascendcl.so", RTLD_NOW | RTLD_LOCAL);
    void* verbs = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(extension && acl && verbs);
    auto cap = Symbol<void (*)(uint32_t)>(extension, "tilexr_fake_nda_cap");
    auto failDestroy = Symbol<void (*)(int)>(extension, "tilexr_fake_nda_destroy_fail");
    auto failCreate = Symbol<void (*)(int, int, int)>(extension, "tilexr_fake_nda_create_fail");
    auto invalidExport = Symbol<void (*)(int)>(extension, "tilexr_fake_nda_invalid_export");
    auto queues = Symbol<int (*)()>(extension, "tilexr_fake_nda_queues");
    auto allocated = Symbol<int (*)()>(acl, "tilexr_fake_acl_allocations");
    auto mappings = Symbol<int (*)()>(acl, "tilexr_fake_acl_mappings");
    auto failAcl = Symbol<void (*)(const char*, int)>(acl, "tilexr_fake_acl_fail");
    auto segments = Symbol<int (*)()>(verbs, "tilexr_fake_segments");
    auto mrs = Symbol<int (*)(ibv_pd*)>(verbs, "tilexr_fake_nda_mrs");
    auto failVerbs = Symbol<void (*)(const char*, int)>(verbs, "tilexr_fake_fail");
    auto queueLimits = Symbol<void (*)(int, int)>(verbs, "tilexr_fake_queue_limits");
    void* hal = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    assert(hal);
    auto ubMode = Symbol<void (*)(int)>(extension, "tilexr_fake_nda_ub_doorbell");
    auto ubMappings = Symbol<int (*)()>(hal, "tilexr_fake_ub_mappings");
    auto failUb = Symbol<void (*)(int, int)>(hal, "tilexr_fake_ub_fail");
    auto sgeLimit = Symbol<void (*)(int)>(verbs, "tilexr_fake_sge_limit");
    ibv_context context {};
    ibv_pd pd {};
    pd.context = &context;
    RdmaConfig config;
    config.device = "hrn5_mock";
    config.aivDirect = true;
    config.indepUb = true;
    config.doorbellMode = TILEXR_RDMA_DB_HARD;
    // Exercise provider admission rather than accepting any requested capacity.
    // The 4095-WR limit reproduces the real device's rejection of 8192 WRs.
    struct QueueLimits { int sendWr; int cqe; uint32_t sqDepth; uint32_t cqDepth; };
    for (const auto& limits : {QueueLimits{4095, 8387583, 4096, 8192},
        {16383, 8387583, 8192, 8192}, {255, 1024, 256, 1024}}) {
        queueLimits(limits.sendWr, limits.cqe);
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(resource.Create(1, &pd, qp, cq) == 0 && qp && cq);
        TileXRRDMADeviceQueue view {};
        assert(resource.DeviceQueue(1, 1, IBV_MTU_1024, view) == 0);
        assert(view.sqDepth == limits.sqDepth && view.cqDepth == limits.cqDepth);
        assert(view.pendingBatches && (view.pendingBatches & 63U) == 0 && view.pendingReserved == 0);
        const uint32_t sqSlots = view.sqDepth - 1U, cqSlots = view.cqDepth - 1U;
        const uint32_t ringSlots = sqSlots < cqSlots ? sqSlots : cqSlots;
        assert(view.pendingCapacity == (ringSlots < view.maxSendWr ? ringSlots : view.maxSendWr));
        assert(resource.DestroyQueues() == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    for (const auto& limits : {std::pair<int, int>{0, 8192}, {-1, 8192}, {4095, 1}, {4095, -1}}) {
        queueLimits(limits.first, limits.second);
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == TILEXR_ERROR_NOT_SUPPORT);
        assert(!resource.Active() && resource.DestroyQueues() == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mrs(&pd) == 0);
    }
    queueLimits(4095, 8387583);
    for (int limit : {1, 2, 16, 32, 0, -1}) {
        sgeLimit(limit);
        RdmaNdaResources resource;
        const int opened = resource.Open(&context, config);
        if (limit < 1) {
            assert(opened == TILEXR_ERROR_NOT_SUPPORT);
        } else {
            assert(opened == 0);
            ibv_qp* qp = nullptr; ibv_cq* cq = nullptr;
            assert(resource.Create(1, &pd, qp, cq) == 0);
            TileXRRDMADeviceQueue view {};
            assert(resource.DeviceQueue(1, 1, IBV_MTU_1024, view) == 0);
            assert(view.maxSendSge == static_cast<uint32_t>(limit < 16 ? limit : 16));
            assert(view.maxSendWr == 4095 && view.queueAbi == TILEXR_RDMA_QUEUE_ABI_UBNIC_V2);
        }
        assert(resource.DestroyQueues() == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    sgeLimit(16);
    failVerbs("query_device", 1);
    {
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == TILEXR_ERROR_NOT_SUPPORT);
        assert(!resource.Active() && resource.DestroyQueues() == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mrs(&pd) == 0);
    }
    cap(0);
    {
        RdmaNdaResources unadvertised;
        assert(unadvertised.Open(&context, config) == 0 && unadvertised.Active());
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(unadvertised.Create(1, &pd, qp, cq) == 0 && qp && cq);
        assert(mrs(&pd) == 1); // Setup provides an FE before the application registers payload memory.
        TileXRRDMADeviceQueue view {};
        assert(unadvertised.DeviceQueue(1, 1, IBV_MTU_1024, view) == 0 && view.state);
        assert(unadvertised.DestroyQueues() == 0 && unadvertised.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0 && mrs(&pd) == 0);
    }
    failCreate(1, 0, 0);
    {
        RdmaNdaResources unavailable;
        assert(unavailable.Open(&context, config) == TILEXR_ERROR_NOT_SUPPORT);
        assert(!unavailable.Active() && unavailable.DestroyQueues() == 0 && unavailable.Close() == 0);
    }
    for (bool cqFails : {true, false}) {
        failCreate(0, cqFails ? EINVAL : 0, cqFails ? 0 : EPERM);
        RdmaNdaResources rejected;
        assert(rejected.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(rejected.Create(1, &pd, qp, cq) == TILEXR_ERROR_INTERNAL && !qp);
        assert(mrs(&pd) == 1);
        assert(queues() == (cqFails ? 0 : 1)); // A rejected QP retains its CQ for explicit teardown.
        TileXRRDMADeviceQueue view {};
        assert(rejected.DeviceQueue(1, 1, IBV_MTU_1024, view) != 0 && view.state == 0);
        assert(rejected.DestroyQueues() == 0 && rejected.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    failCreate(0, 0, 0);
    for (bool registrationFails : {true, false}) {
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        failVerbs(registrationFails ? "register" : "deregister", 1);
        const int created = resource.Create(1, &pd, qp, cq);
        if (registrationFails) {
            assert(created != 0 && !qp && !cq && queues() == 0 && mrs(&pd) == 0);
        } else {
            assert(created == 0 && mrs(&pd) == 1);
            assert(resource.DestroyQueues() != 0 && queues() == 0 && mrs(&pd) == 1);
            assert(allocated() >= 1 && segments() == 1); // Failed deregistration retains pinned backing.
            assert(resource.Close() != 0);
        }
        assert(resource.DestroyQueues() == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0 && mrs(&pd) == 0);
    }
    invalidExport(1);
    {
        RdmaNdaResources invalid;
        assert(invalid.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(invalid.Create(1, &pd, qp, cq) == 0);
        TileXRRDMADeviceQueue view {};
        assert(invalid.DeviceQueue(1, 1, IBV_MTU_1024, view) == TILEXR_ERROR_NOT_SUPPORT);
        assert(view.state == 0 && view.sq == 0);
        assert(invalid.DestroyQueues() == 0 && invalid.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    invalidExport(0);
    cap(2);
    for (uint32_t mode : {TILEXR_RDMA_DB_SOFT, TILEXR_RDMA_DB_HARD}) {
        config.doorbellMode = mode;
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == 0 && resource.Active());
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(resource.Create(1, &pd, qp, cq) == 0 && qp && cq);
        TileXRRDMADeviceQueue first {}, second {};
        assert(resource.DeviceQueue(1, 1, IBV_MTU_1024, first) == 0);
        assert(first.state && first.dbMode == mode && first.sqDepth == 4096 && first.cqn == 9);
        assert(first.cqDepth == 8192);
        assert(first.doorbellBytes == (mode == TILEXR_RDMA_DB_HARD ? 2048U : 8U));
        assert(first.sqDbValue == (uint64_t(qp->qp_num) | (1ULL << 20) | (3ULL << 24) |
            (21ULL << 27) | (2ULL << 50) | (1ULL << 53)));
        assert(resource.DeviceQueue(1, 1, IBV_MTU_1024, second) == 0 && second.state == first.state);
        assert(second.pendingBatches == first.pendingBatches && second.pendingCapacity == first.pendingCapacity);
        assert(resource.Create(2, &pd, qp, cq) == 0);
        assert(mrs(&pd) == 1); // One FE registration is shared across this owner's queues.
        assert(mappings() == 1 && queues() == 4); // Same doorbell page, independent QPs.
        assert(resource.DeviceQueue(2, 1, IBV_MTU_1024, second) == 0 && second.state != first.state);
        assert(second.pendingBatches != first.pendingBatches);
        assert(resource.Close() != 0); // Cannot unload live callbacks/memory.
        failDestroy(1);
        assert(resource.DestroyQueues() != 0 && queues() == 2);
        assert(mrs(&pd) == 1);
        assert(resource.DestroyQueues() == 0);
        assert(resource.Close() == 0 && queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    for (const char* failure : {"alloc", "memset", "map", "free", "unmap"}) {
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        const bool release = std::string(failure) == "free" || std::string(failure) == "unmap";
        if (!release) { failAcl(failure, 1); }
        const int created = resource.Create(1, &pd, qp, cq);
        assert(release ? created == 0 : created != 0);
        if (release) { failAcl(failure, 2); }
        int destroyed = resource.DestroyQueues();
        if (destroyed != 0) { destroyed = resource.DestroyQueues(); }
        assert(destroyed == 0 && resource.Close() == 0);
        assert(queues() == 0 && allocated() == 0 && mappings() == 0 && segments() == 0);
    }
    ubMode(1);
    {
        RdmaNdaResources first, second;
        assert(first.Open(&context, config) == 0 && second.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        assert(first.Create(1, &pd, qp, cq) == 0);
        assert(first.Create(2, &pd, qp, cq) == 0 && ubMappings() == 1);
        assert(second.Create(1, &pd, qp, cq) == 0 && ubMappings() == 2);
        TileXRRDMADeviceQueue view {};
        assert(first.DeviceQueue(1, 1, IBV_MTU_1024, view) == 0 && view.doorbellBytes == 2048);
        assert(first.DestroyQueues() == 0 && first.Close() == 0 && ubMappings() == 1);
        assert(second.DeviceQueue(1, 1, IBV_MTU_1024, view) == 0);
        assert(second.DestroyQueues() == 0 && second.Close() == 0 && ubMappings() == 0);
    }
    for (bool mapFails : {true, false}) {
        RdmaNdaResources resource;
        assert(resource.Open(&context, config) == 0);
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        if (mapFails) { failUb(1, 0); }
        const int created = resource.Create(1, &pd, qp, cq);
        if (mapFails) { assert(created != 0 && ubMappings() == 0); }
        else {
            assert(created == 0 && ubMappings() == 1);
            failUb(0, 2); // Extension callback and first ledger retry both fail.
            assert(resource.DestroyQueues() != 0 && ubMappings() == 1);
            assert(resource.Close() != 0);
        }
        assert(resource.DestroyQueues() == 0 && resource.Close() == 0 && ubMappings() == 0);
        assert(allocated() == 0 && mappings() == 0 && segments() == 0 && queues() == 0);
    }
    dlclose(hal);
    dlclose(verbs);
    dlclose(acl);
    dlclose(extension);
    std::cout << "NDA capability, queue export, mapping sharing and cleanup retry tests passed\n";
}
