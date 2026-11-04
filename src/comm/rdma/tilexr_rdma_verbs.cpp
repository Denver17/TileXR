/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：选择 RoCEv2 GID，创建并连接 RC QP，注册 MR，以及提交 Host READ/WRITE 或导出 AIV queue。
 * 调用关系：实现 transport 使用的 RdmaProvider；动态加载 verbs/HAL，AIV queue 交给 RdmaNdaResources 创建。
 * 资源与生命周期：provider 持有 context、PD 和 queue；MR 注销不释放调用方的 payload buffer，释放失败保留资源记录。
 *
 * Purpose: Select a RoCEv2 GID, connect RC QPs, register MRs and post Host READ/WRITE or export AIV queues.
 * Calls: Implements the transport's RdmaProvider using dynamic verbs/HAL calls; RdmaNdaResources creates AIV queues.
 * Lifetime: Owns context, PD and queues. Deregistration leaves caller-owned payload buffers allocated;
 *           failed releases retain resource records.
 */
#include "rdma/tilexr_rdma_transport.h"
#include "tilexr_log.h"
#include <new>

#ifndef TILEXR_HAVE_IBVERBS
#define TILEXR_HAVE_IBVERBS 0
#endif
#ifndef TILEXR_HAVE_IBV_EXTEND
#define TILEXR_HAVE_IBV_EXTEND 0
#endif

#if TILEXR_HAVE_IBVERBS
#if TILEXR_HAVE_IBV_EXTEND
#include "rdma/tilexr_rdma_nda.h"
#endif
#include <infiniband/verbs.h>
#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <thread>

namespace TileXR {
namespace {
// Resolve public verbs ABI only. Inline post/poll use the opened provider's ops.
// Do not link libibverbs: an absent runtime library must leave TileXR usable.
struct Verbs {
    void* library = nullptr;
    void* halLibrary = nullptr;
    // Public prototypes also used by perf-reference-ascend's acl_memory.h.
    using RegisterSegment = int (*)(uint32_t, uint64_t, uint64_t);
    using UnregisterSegment = int (*)(uint32_t, uint64_t);
    RegisterSegment registerSegment = nullptr;
    UnregisterSegment unregisterSegment = nullptr;
    using QueryGidEx = int (*)(ibv_context*, uint32_t, uint32_t, ibv_gid_entry*, uint32_t, size_t);
    QueryGidEx queryGidEx = nullptr;
#define TILEXR_VERBS_SYMBOLS(X) \
    X(ibv_get_device_list) X(ibv_free_device_list) X(ibv_get_device_name) \
    X(ibv_open_device) X(ibv_close_device) X(ibv_query_device) X(ibv_query_port) \
    X(ibv_query_gid) X(ibv_alloc_pd) X(ibv_dealloc_pd) X(ibv_create_cq) \
    X(ibv_destroy_cq) X(ibv_create_qp) X(ibv_modify_qp) X(ibv_destroy_qp) \
    X(ibv_reg_mr) X(ibv_dereg_mr)
#define DECLARE(name) decltype(&name) name##Fn = nullptr;
    TILEXR_VERBS_SYMBOLS(DECLARE)
#undef DECLARE
    int Load()
    {
        library = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            TILEXR_LOG(WARN) << "RDMA cannot load libibverbs.so.1: " << dlerror();
            return TILEXR_ERROR_NOT_FOUND;
        }
#define RESOLVE(name) name##Fn = reinterpret_cast<decltype(name##Fn)>(dlsym(library, #name)); \
        if (name##Fn == nullptr) { TILEXR_LOG(WARN) << "RDMA missing verbs symbol " << #name; \
            return TILEXR_ERROR_NOT_SUPPORT; }
        TILEXR_VERBS_SYMBOLS(RESOLVE)
#undef RESOLVE
        queryGidEx = reinterpret_cast<QueryGidEx>(dlsym(library, "_ibv_query_gid_ex"));
        return TILEXR_SUCCESS;
    }
    int LoadUbSegmentApi()
    {
        halLibrary = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
        if (halLibrary == nullptr) { return TILEXR_ERROR_NOT_FOUND; }
        registerSegment = reinterpret_cast<RegisterSegment>(dlsym(halLibrary, "halMemRegUbSegment"));
        unregisterSegment = reinterpret_cast<UnregisterSegment>(dlsym(halLibrary, "halMemUnRegUbSegment"));
        return registerSegment != nullptr && unregisterSegment != nullptr ?
            TILEXR_SUCCESS : TILEXR_ERROR_NOT_SUPPORT;
    }
    ~Verbs()
    {
        if (library != nullptr) { dlclose(library); }
        if (halLibrary != nullptr) { dlclose(halLibrary); }
    }
};
#undef TILEXR_VERBS_SYMBOLS

ibv_mtu MtuEnum(uint32_t bytes)
{
    switch (bytes) {
        case 256: return IBV_MTU_256;
        case 512: return IBV_MTU_512;
        case 1024: return IBV_MTU_1024;
        case 2048: return IBV_MTU_2048;
        case 4096: return IBV_MTU_4096;
        default: return static_cast<ibv_mtu>(0);
    }
}
uint32_t MtuBytes(ibv_mtu mtu)
{
    return mtu >= IBV_MTU_256 && mtu <= IBV_MTU_4096 ? 128U << mtu : 0U;
}
bool NonzeroGid(const ibv_gid& gid)
{
    for (uint8_t byte : gid.raw) { if (byte != 0) { return true; } }
    return false;
}

class VerbsProvider final : public RdmaProvider {
public:
    ~VerbsProvider() override
    {
        // Keep loaded code alive if a caller abandons live driver resources.
        // Explicit collective Shutdown is the only normal release path.
        if (context_ != nullptr) { verbs_.library = nullptr; verbs_.halLibrary = nullptr; }
    }
    int Open(const RdmaConfig& config) override;
    int Create(int peer, RdmaEndpoint& endpoint) override;
    int Connect(int peer, const RdmaEndpoint& endpoint) override;
    int Register(void* ptr, size_t bytes, RdmaMemory& memory) override;
    int Deregister(RdmaMemory& memory) override;
    int Transfer(bool read, int peer, const RdmaMemory& local,
        uint64_t localOffset, const RdmaMemory& remote, uint64_t remoteOffset,
        uint32_t bytes, uint32_t timeoutMs) override;
    int DestroyQueues() override;
    int Close() override;
    bool DeviceDirect() const override;
    int DeviceQueue(int peer, TileXRRDMADeviceQueue& queue) override;
private:
    struct Queue {
        ibv_qp* qp = nullptr;
        ibv_cq* cq = nullptr;
        uint32_t psn = 0;
        uint32_t pathMtu = 0;
        bool connected = false;
    };
    int FindGid(int count);
    Verbs verbs_;
#if TILEXR_HAVE_IBV_EXTEND
    RdmaNdaResources nda_;
#endif
    RdmaConfig config_;
    ibv_context* context_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_gid gid_ {};
    int gidIndex_ = -1;
    uint32_t mtu_ = 0;
    uint64_t nextWr_ = 1;
    size_t mrCount_ = 0;
    size_t segmentCount_ = 0;
    std::map<int, Queue> queues_;
};

int VerbsProvider::FindGid(int count)
{
    const int begin = config_.gidIndex < 0 ? 0 : config_.gidIndex;
    const int end = config_.gidIndex < 0 ? std::min(count, 256) : begin + 1;
    if (begin >= count) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    for (int index = begin; index < end; ++index) {
        if (verbs_.queryGidEx != nullptr) {
            ibv_gid_entry entry {};
            if (verbs_.queryGidEx(context_, config_.port, static_cast<uint32_t>(index),
                &entry, 0, sizeof(entry)) == 0) {
                if (entry.gid_type == IBV_GID_TYPE_ROCE_V2 && NonzeroGid(entry.gid)) {
                    gid_ = entry.gid;
                    gidIndex_ = index;
                    return TILEXR_SUCCESS;
                }
                continue;
            }
        }
        // Standard sysfs GID metadata identifies RoCEv2 without a vendor ABI.
        const std::string path = "/sys/class/infiniband/" + config_.device + "/ports/" +
            std::to_string(config_.port) + "/gid_attrs/types/" + std::to_string(index);
        std::ifstream input(path);
        std::string type;
        std::getline(input, type);
        if (type != "RoCE v2") { continue; }
        ibv_gid gid {};
        if (verbs_.ibv_query_gidFn(context_, static_cast<uint8_t>(config_.port), index, &gid) == 0 &&
            NonzeroGid(gid)) {
            gid_ = gid;
            gidIndex_ = index;
            return TILEXR_SUCCESS;
        }
    }
    TILEXR_LOG(WARN) << "RDMA NIC " << config_.device << " has no selected nonzero RoCEv2 GID";
    return TILEXR_ERROR_NOT_FOUND;
}

int VerbsProvider::Open(const RdmaConfig& config)
{
    if (context_ != nullptr || pd_ != nullptr) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (config.aivDirect && (!config.indepUb || (config.doorbellMode != TILEXR_RDMA_DB_SOFT &&
        config.doorbellMode != TILEXR_RDMA_DB_HARD))) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
#if !TILEXR_HAVE_IBV_EXTEND
    if (config.aivDirect) { return TILEXR_ERROR_NOT_SUPPORT; }
#endif
    config_ = config;
    int ret = verbs_.Load();
    if (ret != TILEXR_SUCCESS) { return ret; }
    int count = 0;
    ibv_device** devices = verbs_.ibv_get_device_listFn(&count);
    if (devices == nullptr) { return TILEXR_ERROR_NOT_FOUND; }
    for (int i = 0; i < count; ++i) {
        const char* name = verbs_.ibv_get_device_nameFn(devices[i]);
        if (name != nullptr && config.device == name) {
            context_ = verbs_.ibv_open_deviceFn(devices[i]);
            break;
        }
    }
    verbs_.ibv_free_device_listFn(devices);
    if (context_ == nullptr) {
        TILEXR_LOG(WARN) << "RDMA NIC cannot be opened: " << config.device;
        return TILEXR_ERROR_NOT_FOUND;
    }
    ibv_device_attr device {};
    ibv_port_attr port {};
    // Public legacy query_port fills the common prefix. Zero extended fields.
    if (verbs_.ibv_query_deviceFn(context_, &device) != 0 || config.port == 0 ||
        config.port > device.phys_port_cnt ||
        verbs_.ibv_query_portFn(context_, static_cast<uint8_t>(config.port),
            reinterpret_cast<_compat_ibv_port_attr*>(&port)) != 0 ||
        port.state != IBV_PORT_ACTIVE || port.link_layer != IBV_LINK_LAYER_ETHERNET ||
        device.max_qp_rd_atom < 1 || device.max_qp_init_rd_atom < 1) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    // UBNIC exports enum 6 for its 8192-byte active port MTU, beyond the
    // standard verbs enum. Negotiate only the configured standard MTU (<=4096).
    // perf-reference-ascend 66a7475f, perftest_parameters.h:84-92.
    const uint32_t activeMtu = config.device.compare(0, 5, "hrn5_") == 0 &&
        static_cast<int>(port.active_mtu) == 6 ? 8192U : MtuBytes(port.active_mtu);
    mtu_ = std::min(config.mtu, activeMtu);
    if (MtuEnum(mtu_) == 0) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    ret = FindGid(port.gid_tbl_len);
    if (ret != TILEXR_SUCCESS) { return ret; }
#if TILEXR_HAVE_IBV_EXTEND
    if (config.aivDirect) {
        ret = nda_.Open(context_, config);
        if (ret != TILEXR_SUCCESS) { return ret; }
    }
#endif
    pd_ = verbs_.ibv_alloc_pdFn(context_);
    if (pd_ == nullptr) { return TILEXR_ERROR_INTERNAL; }
    if (config.indepUb) {
        ret = verbs_.LoadUbSegmentApi();
        if (ret != TILEXR_SUCCESS) { return ret; }
    }
    TILEXR_LOG(INFO) << "RDMA selected " << config.device << " port=" << config.port
                     << " gid_index=" << gidIndex_ << " mtu=" << mtu_
                     << (config.aivDirect ? ", AIV NDA queue submission" : ", standard verbs Host submission");
    return TILEXR_SUCCESS;
}

int VerbsProvider::Create(int peer, RdmaEndpoint& endpoint)
{
    if (pd_ == nullptr || queues_.count(peer) != 0) { return TILEXR_ERROR_NOT_INITIALIZED; }
    Queue& queue = queues_[peer];
    if (config_.aivDirect) {
#if TILEXR_HAVE_IBV_EXTEND
        const int ret = nda_.Create(peer, pd_, queue.qp, queue.cq);
        if (ret != TILEXR_SUCCESS) { return ret; }
#else
        return TILEXR_ERROR_NOT_SUPPORT;
#endif
    } else {
        queue.cq = verbs_.ibv_create_cqFn(context_, 16, nullptr, nullptr, 0);
        if (queue.cq == nullptr) { return TILEXR_ERROR_INTERNAL; }
        ibv_qp_init_attr init {};
        init.send_cq = queue.cq;
        init.recv_cq = queue.cq;
        init.qp_type = IBV_QPT_RC;
        init.cap.max_send_wr = 16;
        init.cap.max_recv_wr = 1;
        init.cap.max_send_sge = 1;
        init.cap.max_recv_sge = 1;
        queue.qp = verbs_.ibv_create_qpFn(pd_, &init);
        if (queue.qp == nullptr) { return TILEXR_ERROR_INTERNAL; }
    }
    ibv_qp_attr attr {};
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = static_cast<uint8_t>(config_.port);
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (verbs_.ibv_modify_qpFn(queue.qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        return TILEXR_ERROR_INTERNAL;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    queue.psn = static_cast<uint32_t>(now ^ (static_cast<uint64_t>(getpid()) << 8) ^
        queue.qp->qp_num) & 0xffffffU;
    std::copy(gid_.raw, gid_.raw + 16, endpoint.gid.begin());
    endpoint.qpn = queue.qp->qp_num;
    endpoint.psn = queue.psn;
    endpoint.mtu = mtu_;
    queue.pathMtu = mtu_;
    return TILEXR_SUCCESS;
}

int VerbsProvider::Connect(int peer, const RdmaEndpoint& endpoint)
{
    auto it = queues_.find(peer);
    if (it == queues_.end() || it->second.qp == nullptr || endpoint.qpn == 0 ||
        endpoint.qpn > 0xffffffU || endpoint.psn > 0xffffffU || MtuEnum(endpoint.mtu) == 0 ||
        std::all_of(endpoint.gid.begin(), endpoint.gid.end(), [](uint8_t byte) { return byte == 0; })) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto& queue = it->second;
    ibv_qp_attr attr {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = MtuEnum(std::min(mtu_, endpoint.mtu));
    attr.dest_qp_num = endpoint.qpn;
    attr.rq_psn = endpoint.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = static_cast<uint8_t>(config_.port);
    std::copy(endpoint.gid.begin(), endpoint.gid.end(), attr.ah_attr.grh.dgid.raw);
    attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(gidIndex_);
    attr.ah_attr.grh.hop_limit = static_cast<uint8_t>(config_.hopLimit);
    attr.ah_attr.grh.traffic_class = static_cast<uint8_t>(config_.trafficClass);
    int ret = verbs_.ibv_modify_qpFn(queue.qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret != 0) {
        TILEXR_LOG(ERROR) << "RDMA RTR failed peer=" << peer << " status=" << ret << " errno=" << errno
            << " local_qpn=" << queue.qp->qp_num << " remote_qpn=" << endpoint.qpn
            << " gid_index=" << gidIndex_ << " mtu=" << std::min(mtu_, endpoint.mtu);
        return TILEXR_ERROR_INTERNAL;
    }
    attr = ibv_qp_attr {};
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = queue.psn;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    ret = verbs_.ibv_modify_qpFn(queue.qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    queue.connected = false;
    if (ret != 0) {
        TILEXR_LOG(ERROR) << "RDMA RTS failed peer=" << peer << " status=" << ret << " errno=" << errno
            << " local_qpn=" << queue.qp->qp_num;
        return TILEXR_ERROR_INTERNAL;
    }
    queue.pathMtu = std::min(mtu_, endpoint.mtu);
#if TILEXR_HAVE_IBV_EXTEND
    if (config_.aivDirect) {
        TileXRRDMADeviceQueue view {};
        ret = nda_.DeviceQueue(peer, static_cast<uint32_t>(gidIndex_), MtuEnum(queue.pathMtu), view);
        if (ret != TILEXR_SUCCESS) { return ret; }
    }
#endif
    queue.connected = true;
    return TILEXR_SUCCESS;
}

bool VerbsProvider::DeviceDirect() const
{
#if TILEXR_HAVE_IBV_EXTEND
    return config_.aivDirect && nda_.Active();
#else
    return false;
#endif
}

int VerbsProvider::DeviceQueue(int peer, TileXRRDMADeviceQueue& queue)
{
    queue = TileXRRDMADeviceQueue {};
    if (!DeviceDirect()) { return TILEXR_ERROR_NOT_SUPPORT; }
    const auto it = queues_.find(peer);
    if (it == queues_.end() || !it->second.connected) { return TILEXR_ERROR_NOT_INITIALIZED; }
#if TILEXR_HAVE_IBV_EXTEND
    return nda_.DeviceQueue(peer, static_cast<uint32_t>(gidIndex_), MtuEnum(it->second.pathMtu), queue);
#else
    return TILEXR_ERROR_NOT_SUPPORT;
#endif
}

int VerbsProvider::Register(void* ptr, size_t bytes, RdmaMemory& memory)
{
    memory.address = reinterpret_cast<uintptr_t>(ptr);
    memory.bytes = bytes;
    if (config_.indepUb) {
        if (verbs_.registerSegment(config_.deviceId, memory.address, bytes) != 0) {
            return TILEXR_ERROR_INTERNAL;
        }
        memory.segmentRegistered = true;
        ++segmentCount_;
    }
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (config_.aivDirect) {
        // UBNIC INDEP_UB NDA MR ABI, ibv_extend 4.2. Reference provenance:
        // perf-reference-ascend 66a7475f, perftest_resources.c:2086-2092,
        // register_mr(): flagForUBNICNda = (1 << 21). Upstream verbs does not
        // give this vendor bit a public IBV_ACCESS_* enumerator.
        constexpr int kUbnicNdaMemoryAccess = 1 << 21;
        access |= kUbnicNdaMemoryAccess;
    }
    ibv_mr* mr = verbs_.ibv_reg_mrFn(pd_, ptr, bytes, access);
    if (mr == nullptr) {
        TILEXR_LOG(ERROR) << "RDMA MR registration failed errno=" << errno
                         << "; provider must support the selected NPU allocation";
        return TILEXR_ERROR_INTERNAL;
    }
    memory.lkey = mr->lkey;
    memory.rkey = mr->rkey;
    memory.resource = mr;
    ++mrCount_;
    return TILEXR_SUCCESS;
}

int VerbsProvider::Deregister(RdmaMemory& memory)
{
    if (memory.resource != nullptr) {
        if (verbs_.ibv_dereg_mrFn(static_cast<ibv_mr*>(memory.resource)) != 0) { return TILEXR_ERROR_INTERNAL; }
        memory.resource = nullptr;
        --mrCount_;
    }
    if (memory.segmentRegistered) {
        if (verbs_.unregisterSegment(config_.deviceId, memory.address) != 0) { return TILEXR_ERROR_INTERNAL; }
        --segmentCount_;
    }
    memory = RdmaMemory {};
    return TILEXR_SUCCESS;
}

int VerbsProvider::Transfer(bool read, int peer, const RdmaMemory& local, uint64_t localOffset,
    const RdmaMemory& remote, uint64_t remoteOffset, uint32_t bytes, uint32_t timeoutMs)
{
    // AIV mode never falls back to a Host producer on its device-owned SQ.
    if (config_.aivDirect) { return TILEXR_ERROR_NOT_SUPPORT; }
    const auto it = queues_.find(peer);
    if (it == queues_.end() || !it->second.connected) { return TILEXR_ERROR_NOT_INITIALIZED; }
    const auto& queue = it->second;
    ibv_sge sge {};
    sge.addr = local.address + localOffset;
    sge.length = bytes;
    sge.lkey = local.lkey;
    ibv_send_wr wr {};
    wr.wr_id = nextWr_++;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote.address + remoteOffset;
    wr.wr.rdma.rkey = remote.rkey;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(queue.qp, &wr, &bad) != 0) { return TILEXR_ERROR_INTERNAL; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint32_t polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ibv_wc completion {};
        const int count = ibv_poll_cq(queue.cq, 1, &completion);
        if (count < 0) { return TILEXR_ERROR_INTERNAL; }
        if (count == 1) {
            if (completion.status != IBV_WC_SUCCESS || completion.wr_id != wr.wr_id ||
                completion.qp_num != queue.qp->qp_num) {
                TILEXR_LOG(ERROR) << "RDMA completion error peer=" << peer << " status="
                    << completion.status << " vendor_err=" << completion.vendor_err;
                return TILEXR_ERROR_INTERNAL;
            }
            return TILEXR_SUCCESS;
        }
        if ((++polls & 1023U) == 0) { std::this_thread::yield(); }
    }
    TILEXR_LOG(ERROR) << "RDMA completion timeout peer=" << peer << " timeout_ms=" << timeoutMs;
    return TILEXR_ERROR_TIMEOUT;
}

int VerbsProvider::DestroyQueues()
{
#if TILEXR_HAVE_IBV_EXTEND
    if (config_.aivDirect) {
        for (auto& entry : queues_) { entry.second.connected = false; }
        const int ret = nda_.DestroyQueues();
        if (ret == TILEXR_SUCCESS) { queues_.clear(); }
        return ret;
    }
#endif
    int status = TILEXR_SUCCESS;
    for (auto& entry : queues_) {
        auto& queue = entry.second;
        queue.connected = false;
        if (queue.qp != nullptr) {
            if (verbs_.ibv_destroy_qpFn(queue.qp) != 0) { status = TILEXR_ERROR_INTERNAL; continue; }
            queue.qp = nullptr;
        }
        if (queue.cq != nullptr) {
            if (verbs_.ibv_destroy_cqFn(queue.cq) != 0) { status = TILEXR_ERROR_INTERNAL; continue; }
            queue.cq = nullptr;
        }
    }
    if (status == TILEXR_SUCCESS) { queues_.clear(); }
    return status;
}

int VerbsProvider::Close()
{
    if (!queues_.empty() || mrCount_ != 0 || segmentCount_ != 0) { return TILEXR_ERROR_INTERNAL; }
#if TILEXR_HAVE_IBV_EXTEND
    const int ret = nda_.Close();
    if (ret != TILEXR_SUCCESS) { return ret; }
#endif
    if (pd_ != nullptr) {
        if (verbs_.ibv_dealloc_pdFn(pd_) != 0) { return TILEXR_ERROR_INTERNAL; }
        pd_ = nullptr;
    }
    if (context_ != nullptr) {
        if (verbs_.ibv_close_deviceFn(context_) != 0) { return TILEXR_ERROR_INTERNAL; }
        context_ = nullptr;
    }
    return TILEXR_SUCCESS;
}
} // namespace
std::unique_ptr<RdmaProvider> MakeRdmaVerbsProvider()
{
    return std::unique_ptr<RdmaProvider>(new (std::nothrow) VerbsProvider());
}
} // namespace TileXR
#else
namespace TileXR {
namespace {
class UnavailableProvider final : public RdmaProvider {
public:
    int Open(const RdmaConfig&) override { return TILEXR_ERROR_NOT_SUPPORT; }
    int Create(int, RdmaEndpoint&) override { return TILEXR_ERROR_NOT_SUPPORT; }
    int Connect(int, const RdmaEndpoint&) override { return TILEXR_ERROR_NOT_SUPPORT; }
    int Register(void*, size_t, RdmaMemory&) override { return TILEXR_ERROR_NOT_SUPPORT; }
    int Deregister(RdmaMemory&) override { return TILEXR_SUCCESS; }
    int Transfer(bool, int, const RdmaMemory&, uint64_t, const RdmaMemory&, uint64_t,
        uint32_t, uint32_t) override { return TILEXR_ERROR_NOT_SUPPORT; }
    int DestroyQueues() override { return TILEXR_SUCCESS; }
    int Close() override { return TILEXR_SUCCESS; }
};
}
std::unique_ptr<RdmaProvider> MakeRdmaVerbsProvider()
{
    return std::unique_ptr<RdmaProvider>(new (std::nothrow) UnavailableProvider());
}
} // namespace TileXR
#endif
