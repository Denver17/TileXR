/*
 * 功能：导出 NDA 4.2 测试 ABI，模拟 queue 创建、能力协商和故障，并记录存活的 QP/CQ。
 * 调用关系：test_rdma_nda.cpp 通过真实 RdmaNdaResources 动态加载本 DSO；底层 verbs 使用测试替身。
 * 资源与生命周期：通过传入的 callback 分配/释放 queue buffer 和映射，不访问真实 NPU/NIC。
 * Purpose: Export a fake NDA 4.2 ABI with queue creation, capability negotiation, fault injection and live QP/CQ tracking.
 * Calls: test_rdma_nda.cpp loads this DSO through real RdmaNdaResources with fake underlying verbs.
 * Lifetime: Supplied callbacks allocate/free queue buffers and mappings; no real NPU/NIC is accessed.
 */
#include <infiniband/verbs.h>
extern "C" {
#include <ibv_extend.h>
}
#include <cassert>
#include <cstring>
#include <cerrno>
#include <map>

extern "C" int tilexr_fake_nda_mrs(ibv_pd*);

namespace {
uint32_t capabilities = IBV_EXTEND_DEV_NDA;
int destroyFailures = 0;
int queryFailure = 0;
int cqFailure = 0;
int qpFailure = 0;
bool invalidExport = false;
bool ubDoorbell = false;
alignas(65536) unsigned char doorbell[65536] {};
struct Cq { ibv_extend_ops ops; void* buffer; ibv_pd* pd; ibv_context_extend* context; };
struct Qp { ibv_extend_ops ops; void* buffer; doorbell_map_desc desc; void* mapped; ibv_context_extend* context; };
std::map<ibv_cq_extend*, Cq> cqs;
std::map<ibv_qp_extend*, Qp> qps;
uint32_t RingEntries(uint32_t minimum)
{
    uint32_t entries = 2;
    while (entries < minimum) { entries *= 2; }
    return entries;
}
}
extern "C" {
void tilexr_fake_nda_cap(uint32_t cap) { capabilities = cap; }
void tilexr_fake_nda_destroy_fail(int count) { destroyFailures = count; }
void tilexr_fake_nda_create_fail(int query, int cq, int qp)
{
    queryFailure = query; cqFailure = cq; qpFailure = qp;
}
void tilexr_fake_nda_invalid_export(int enabled) { invalidExport = enabled != 0; }
void tilexr_fake_nda_ub_doorbell(int enabled) { ubDoorbell = enabled != 0; }
int tilexr_fake_nda_queues() { return static_cast<int>(qps.size() + cqs.size()); }
const char* ibv_extend_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch)
{
    *major = 4; *minor = 2; *patch = 0;
    return "4.2.0";
}
int ibv_extend_check_version(uint32_t major, uint32_t minor, uint32_t patch)
{
    return major == 4 && minor <= 2 && patch == 0 ? 0 : -1;
}
ibv_context_extend* ibv_open_extend(ibv_context* context)
{
    auto* result = new ibv_context_extend {};
    result->context = context;
    return result;
}
int ibv_close_extend(ibv_context_extend* context)
{
    for (const auto& entry : cqs) { assert(entry.second.context != context); }
    for (const auto& entry : qps) { assert(entry.second.context != context); }
    delete context;
    return 0;
}
int ibv_query_device_extend(ibv_context_extend*, ibv_device_attr_extend* attr)
{
    if (queryFailure) { errno = EIO; return -1; }
    *attr = ibv_device_attr_extend {};
    attr->ext_cap = capabilities;
    return 0;
}
ibv_cq_extend* ibv_create_cq_extend(ibv_context_extend* context, ibv_cq_init_attr_extend* init)
{
    // UBNIC CQ setup finds the FE created by a preceding NDA MR registration.
    assert(tilexr_fake_nda_mrs(init->attr.parent_domain) > 0);
    if (cqFailure) { errno = cqFailure; return nullptr; }
    assert(init->type == QU_BUF_DMA_MODE_INDEP_UB);
    ibv_device_attr limits {};
    if (ibv_query_device(context->context, &limits) != 0 || init->attr.cqe < 2 ||
        init->attr.cqe > limits.max_cqe) { errno = EINVAL; return nullptr; }
    const uint32_t entries = RingEntries(static_cast<uint32_t>(init->attr.cqe));
    const size_t queueBytes = size_t(entries) * 64U;
    void* buffer = init->ops->alloc(queueBytes + 64U);
    if (!buffer) { return nullptr; }
    init->ops->memset_s(buffer, 0, queueBytes + 64U);
    auto* result = new ibv_cq_extend {};
    result->cq = ibv_create_cq(context->context, static_cast<int>(entries), nullptr, nullptr, 0);
    result->cq_info.qbuf.base = reinterpret_cast<uintptr_t>(buffer);
    result->cq_info.qbuf.entry_cnt = entries;
    result->cq_info.qbuf.entry_size = 64;
    result->cq_info.dbr_ci_va = {static_cast<unsigned char*>(buffer) + queueBytes, 4};
    result->resv[0] = 9;
    cqs[result] = Cq {*init->ops, buffer, init->attr.parent_domain, context};
    return result;
}
ibv_qp_extend* ibv_create_qp_extend(ibv_context_extend* context, ibv_qp_init_attr_extend* init)
{
    if (qpFailure) { errno = qpFailure; return nullptr; }
    assert(init->type == QU_BUF_DMA_MODE_INDEP_UB && init->attr.sq_sig_all == 1);
    ibv_device_attr limits {};
    if (ibv_query_device(context->context, &limits) != 0 || limits.max_qp_wr < 1 ||
        init->attr.cap.max_send_wr < 1U || init->attr.cap.max_send_sge < 1U ||
        limits.max_sge < 1 || init->attr.cap.max_send_sge > static_cast<uint32_t>(limits.max_sge) ||
        init->attr.cap.max_send_wr > static_cast<uint32_t>(limits.max_qp_wr)) {
        errno = EINVAL; return nullptr;
    }
    const uint32_t entries = RingEntries(init->attr.cap.max_send_wr + 1U);
    const size_t queueBytes = size_t(entries) * 64U;
    void* buffer = init->ops->alloc(queueBytes + 64U);
    if (!buffer) { return nullptr; }
    init->ops->memset_s(buffer, 0, queueBytes + 64U);
    doorbell_map_desc desc {};
    desc.type = DB_MAP_MODE_HOST_VA;
    desc.hva = reinterpret_cast<uintptr_t>(doorbell);
    desc.size = sizeof(doorbell);
    if (ubDoorbell) {
        desc = doorbell_map_desc {};
        desc.type = DB_MAP_MODE_UB_RES;
        desc.ub_res.guid_l = 0x1234;
        desc.ub_res.guid_h = 0x5678;
        desc.ub_res.bits.offset = 5U * 4096U + 64U;
        desc.size = 2048;
    }
    void* mapped = init->ops->db_mmap(&desc);
    if (!mapped) { init->ops->free(buffer); return nullptr; }
    auto* result = new ibv_qp_extend {};
    (void)context;
    result->qp = ibv_create_qp(init->pd, &init->attr);
    result->sq_info.qbuf.base = reinterpret_cast<uintptr_t>(buffer);
    result->sq_info.qbuf.entry_cnt = entries;
    result->sq_info.qbuf.entry_size = invalidExport ? 32 : 64;
    result->sq_info.dbr_pi_va = {static_cast<unsigned char*>(buffer) + queueBytes, 4};
    result->sq_info.db_hw_va = {mapped, 8}; // Register width, not the mapped aperture.
    result->vendor_priv_info.vendor1.db_cos = 3;
    qps[result] = Qp {*init->ops, buffer, desc, mapped, context};
    return result;
}
int ibv_destroy_qp_extend(ibv_context_extend*, ibv_qp_extend* qp)
{
    if (destroyFailures > 0) { --destroyFailures; return -1; }
    auto it = qps.find(qp);
    assert(it != qps.end());
    auto resource = it->second;
    // The test driver retires the handle even when a void callback fails.
    resource.ops.free(resource.buffer);
    resource.ops.db_unmap(resource.mapped, &resource.desc);
    qps.erase(it);
    assert(ibv_destroy_qp(qp->qp) == 0);
    delete qp;
    return 0;
}
int ibv_destroy_cq_extend(ibv_context_extend*, ibv_cq_extend* cq)
{
    auto it = cqs.find(cq);
    assert(it != cqs.end());
    assert(tilexr_fake_nda_mrs(it->second.pd) > 0); // FE registration outlives queues.
    it->second.ops.free(it->second.buffer);
    cqs.erase(it);
    assert(ibv_destroy_cq(cq->cq) == 0);
    delete cq;
    return 0;
}
}
