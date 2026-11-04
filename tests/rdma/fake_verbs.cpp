// Test provider for the actual dynamic-loader/verbs call path. No NPU is used.
#include <infiniband/verbs.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#undef ibv_query_port
#undef ibv_reg_mr
namespace {
struct QueueState { ibv_qp_state state; ibv_wc completion; bool ready; };
std::map<ibv_cq*, QueueState> queues;
std::map<void*, uint64_t> segments;
std::map<ibv_mr*, int> memoryRegions;
std::string fail;
int remaining = 0;
uint32_t nextQpn = 1;
int lastGidIndex = -1;
int lastMrFlags = 0;
int activeMtu = IBV_MTU_2048;
int maxQpWr = 4095;
int maxCqe = 8387583;
int maxSge = 16;
bool Fails(const char* operation)
{
    if (fail != operation || remaining == 0) { return false; }
    --remaining;
    return true;
}
int Post(ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**)
{
    assert(queues[qp->send_cq].state == IBV_QPS_RTS);
    if (Fails("post")) { return EIO; }
    const bool read = wr->opcode == IBV_WR_RDMA_READ;
    void* local = reinterpret_cast<void*>(wr->sg_list->addr);
    void* remote = reinterpret_cast<void*>(wr->wr.rdma.remote_addr);
    std::memcpy(read ? local : remote, read ? remote : local, wr->sg_list->length);
    auto& queue = queues[qp->send_cq];
    queue.completion = ibv_wc {};
    queue.completion.wr_id = wr->wr_id;
    queue.completion.qp_num = qp->qp_num;
    queue.completion.status = Fails("wc") ? IBV_WC_REM_ACCESS_ERR : IBV_WC_SUCCESS;
    if (Fails("wr_id")) { ++queue.completion.wr_id; }
    queue.ready = !Fails("timeout");
    return 0;
}
int Poll(ibv_cq* cq, int, ibv_wc* wc)
{
    auto& queue = queues[cq];
    if (!queue.ready) { return 0; }
    *wc = queue.completion;
    queue.ready = false;
    return 1;
}
}
extern "C" {
void tilexr_fake_fail(const char* operation, int times) { fail = operation; remaining = times; }
int tilexr_fake_segments() { return static_cast<int>(segments.size()); }
int tilexr_fake_queues() { return static_cast<int>(queues.size()); }
int tilexr_fake_gid_index() { return lastGidIndex; }
int tilexr_fake_mr_flags() { return lastMrFlags; }
int tilexr_fake_nda_mrs(ibv_pd* pd)
{
    int count = 0;
    for (const auto& entry : memoryRegions) {
        if (entry.first->pd == pd && (entry.second & (1 << 21)) != 0) { ++count; }
    }
    return count;
}
void tilexr_fake_active_mtu(int value) { activeMtu = value; }
void tilexr_fake_sge_limit(int value) { maxSge = value; }
void tilexr_fake_queue_limits(int sendWr, int cqe) { maxQpWr = sendWr; maxCqe = cqe; }
ibv_device** ibv_get_device_list(int* count)
{
    static ibv_device device {};
    auto** list = new ibv_device*[2] {&device, nullptr};
    *count = 1;
    return list;
}
void ibv_free_device_list(ibv_device** list) { delete[] list; }
const char* ibv_get_device_name(ibv_device*) { return "hrn5_mock"; }
ibv_context* ibv_open_device(ibv_device*)
{
    auto* context = new ibv_context {};
    context->ops.post_send = Post;
    context->ops.poll_cq = Poll;
    return context;
}
int ibv_close_device(ibv_context* context) { delete context; return 0; }
int ibv_query_device(ibv_context*, ibv_device_attr* attr)
{
    if (Fails("query_device")) { errno = EIO; return -1; }
    *attr = ibv_device_attr {};
    attr->phys_port_cnt = 1;
    attr->max_qp_rd_atom = 1;
    attr->max_qp_init_rd_atom = 1;
    attr->max_qp_wr = maxQpWr;
    attr->max_cqe = maxCqe;
    attr->max_sge = maxSge;
    return 0;
}
int ibv_query_port(ibv_context*, uint8_t, _compat_ibv_port_attr* output)
{
    auto* attr = reinterpret_cast<ibv_port_attr*>(output);
    attr->state = IBV_PORT_ACTIVE;
    attr->link_layer = IBV_LINK_LAYER_ETHERNET;
    attr->active_mtu = static_cast<ibv_mtu>(activeMtu);
    attr->gid_tbl_len = 4;
    return 0;
}
int ibv_query_gid(ibv_context*, uint8_t, int index, ibv_gid* gid)
{
    *gid = ibv_gid {};
    gid->raw[15] = static_cast<uint8_t>(index);
    return 0;
}
int _ibv_query_gid_ex(ibv_context*, uint32_t, uint32_t index, ibv_gid_entry* entry, uint32_t, size_t)
{
    *entry = ibv_gid_entry {};
    entry->gid_type = index < 2 ? IBV_GID_TYPE_ROCE_V1 : IBV_GID_TYPE_ROCE_V2;
    entry->gid.raw[15] = static_cast<uint8_t>(index);
    return 0;
}
ibv_pd* ibv_alloc_pd(ibv_context* context)
{
    auto* pd = new ibv_pd {};
    pd->context = context;
    return pd;
}
int ibv_dealloc_pd(ibv_pd* pd) { delete pd; return 0; }
ibv_cq* ibv_create_cq(ibv_context* context, int, void*, ibv_comp_channel*, int)
{
    auto* cq = new ibv_cq {};
    cq->context = context;
    queues[cq] = QueueState {IBV_QPS_RESET, {}, false};
    return cq;
}
int ibv_destroy_cq(ibv_cq* cq)
{
    queues.erase(cq);
    delete cq;
    return 0;
}
ibv_qp* ibv_create_qp(ibv_pd* pd, ibv_qp_init_attr* attr)
{
    assert(attr->qp_type == IBV_QPT_RC);
    assert(attr->send_cq == attr->recv_cq);
    auto* qp = new ibv_qp {};
    qp->context = pd->context;
    qp->pd = pd;
    qp->qp_num = nextQpn++;
    qp->send_cq = attr->send_cq;
    return qp;
}
int ibv_modify_qp(ibv_qp* qp, ibv_qp_attr* attr, int mask)
{
    auto& state = queues[qp->send_cq].state;
    if (attr->qp_state == IBV_QPS_INIT) {
        assert(state == IBV_QPS_RESET && (mask & IBV_QP_ACCESS_FLAGS));
    } else if (attr->qp_state == IBV_QPS_RTR) {
        assert(state == IBV_QPS_INIT && attr->ah_attr.is_global == 1);
        assert(attr->path_mtu == IBV_MTU_1024); // Remote advertises smaller MTU.
        assert(attr->dest_qp_num == 700 && attr->rq_psn == 12345);
        assert(attr->ah_attr.grh.hop_limit == 64 && attr->ah_attr.grh.traffic_class == 40);
        lastGidIndex = attr->ah_attr.grh.sgid_index;
    } else if (attr->qp_state == IBV_QPS_RTS) {
        assert(state == IBV_QPS_RTR && (mask & IBV_QP_SQ_PSN));
    } else { assert(false); }
    if (Fails("modify")) { return EIO; }
    state = attr->qp_state;
    return 0;
}
int ibv_destroy_qp(ibv_qp* qp)
{
    if (Fails("destroy")) { return EIO; }
    delete qp;
    return 0;
}
ibv_mr* ibv_reg_mr(ibv_pd* pd, void* ptr, size_t bytes, int flags)
{
    lastMrFlags = flags;
    assert(segments.count(ptr)); // UBNIC UB registration precedes MR registration.
    if (Fails("register")) { errno = EIO; return nullptr; }
    auto* mr = new ibv_mr {};
    mr->pd = pd;
    mr->addr = ptr;
    mr->length = bytes;
    mr->lkey = 44;
    mr->rkey = 55;
    memoryRegions[mr] = flags;
    return mr;
}
int ibv_dereg_mr(ibv_mr* mr)
{
    assert(segments.count(mr->addr)); // UB registration must outlive MR.
    if (Fails("deregister")) { return EIO; }
    assert(memoryRegions.erase(mr) == 1);
    delete mr;
    return 0;
}
// The fake libascend_hal shares these symbols through a dependency on this DSO.
int halMemRegUbSegment(uint32_t, uint64_t address, uint64_t bytes)
{
    void* ptr = reinterpret_cast<void*>(address);
    assert(!segments.count(ptr));
    if (Fails("pin")) { return EIO; }
    segments[ptr] = bytes;
    return 0;
}
int halMemUnRegUbSegment(uint32_t, uint64_t address)
{
    if (Fails("unpin")) { return EIO; }
    for (const auto& entry : memoryRegions) {
        assert(reinterpret_cast<uintptr_t>(entry.first->addr) != address);
    }
    assert(segments.erase(reinterpret_cast<void*>(address)) == 1);
    return 0;
}
}
