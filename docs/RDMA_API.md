# RDMA WRITE 使用指南 / RDMA WRITE User Guide

TileXR 支持在 Ascend C AIV kernel 中直接提交 RDMA WRITE：Host 建立连接并管理 QP/MR，AIV 通过四个 `RDMAWrite*` 接口发送连续数据或多 SGE 数据。本文按接入顺序说明配置、调用和清理。

TileXR lets Ascend C AIV kernels post RDMA WRITE directly. The Host owns connections, QPs and MRs; four `RDMAWrite*` adapters submit contiguous or gathered data. Follow this guide for setup, calls and cleanup.

环境配置、低层队列契约、历史硬件证据与实现来源见 [RDMA 集成说明 / Integration and validation](RDMA_INTEGRATION.md)。可运行的 benchmark 及其纯 kernel 构建方式见 [rdma_compare](../tools/rdma_compare/README.md)。

## 1. 选择接口 / Choose an interface

| 本地数据 / Local data | 单 QP / Single QP | 多 QP / Multiple QPs |
| --- | --- | --- |
| 一个连续段 / One contiguous span | `TileXR::RDMAWrite` | `TileXR::RDMAWriteMultiQp` |
| 多个离散段 / Multiple scattered spans | `TileXR::RDMAWriteSge` | `TileXR::RDMAWriteSgeMultiQp` |

四个接口均同步等待本次调用的全部 WR 完成。`count` 是所选 QP 的 WR 数；多 QP 接口只选择 `qpIndex`，应用负责启动 AIV、分区和同步。每个 QP 同时只能有一个 AIV owner，包括跨 region、kernel 和 stream 的使用。

All four calls wait for every WR in that call to complete. `count` is the WR count on the selected QP. Multi-QP calls select `qpIndex`; the application launches AIVs, partitions buffers and synchronizes them. Each QP has one exclusive AIV owner across regions, kernels and streams.

多 SGE 按**列表顺序** gather 到一个远端连续区域。例如本地 `[A:64B, B:128B, C:32B]` 写到远端 `R`，结果为 `[R,R+64)=A`、`[R+64,R+192)=B`、`[R+192,R+224)=C`。不需要先拼接 payload；一个 WQE 只有一个远端起点和 rkey。

Multiple SGEs are gathered in **list order** into one contiguous remote span. The example above produces `[A][B][C]` at `R`, without a payload staging copy. One WQE has one remote starting address and rkey; arbitrary remote scatter needs separate WRs.

Host 下发仍由 `TileXRRDMAPut/Get` 提供，使用 `TILEXR_RDMA_MODE=host`。本文的 AIV 接口要求 `aiv` 模式；AIV 提交失败时不会改用 Host 提交。

Host posting remains available through `TileXRRDMAPut/Get` in `host` mode. The AIV interfaces in this guide require `aiv` mode and never fall back to Host posting.

## 2. 构建和环境 / Build and environment

项目构建目标为 C++14/CANN 9.1；当前 AIV 适配器要求 Ascend950 的 `__NPU_ARCH__ == 3510` 与受支持的 UBNIC 队列 ABI。需要匹配驱动的 `ibv_extend.h` 4.2 或更高兼容版本、`libibv_extend`、`libibverbs.so.1`、ACL/HAL，以及可注册的本机原始 HBM。无需 SHMEM/Hcomm；普通 RC 连接成功并不证明 AIV 队列可用。历史 CANN 9.3 结果不证明 CANN 9.1 实机兼容性。

The project targets C++14/CANN 9.1. The AIV adapter requires Ascend950 architecture `3510`, a supported UBNIC queue ABI, matching driver extension headers/libraries, verbs, ACL/HAL and registerable original local HBM. SHMEM/Hcomm is not required. An RC handshake alone does not qualify AIV queues; historical CANN 9.3 results do not qualify CANN 9.1 device execution.

按 [构建验证](BUILD_VERIFICATION.md) 初始化子模块、加载环境并构建安装。`TILEXR_BUILD_RDMA` 默认开启；缺少依赖时查询到的 RDMA 能力可能不可用，必须检查 readiness。

Initialize submodules, source the environment, then build/install as described in [build verification](BUILD_VERIFICATION.md). `TILEXR_BUILD_RDMA` defaults to ON; missing dependencies may leave RDMA unavailable, so always query readiness.

每个 rank 在初始化 communicator 前设置以下变量；NIC/GID 只是示例，必须匹配该 rank 的实际 NPU/NIC。

Set these variables on each rank before communicator initialization. NIC/GID values below are examples and must match that rank's actual NPU/NIC.

```bash
export TILEXR_ENABLE_RDMA=1
export TILEXR_RDMA_MODE=aiv
export TILEXR_RDMA_DEVICE=hrn5_0
export TILEXR_RDMA_DMA_MODE=indep_ub
export TILEXR_RDMA_DB_MODE=hard
export TILEXR_RDMA_GID_INDEX=1
export TILEXR_COMM_ID='<bootstrap-ip>:10067'
# 仅在需要 RDMA-only communicator 时设置 / Set only for an RDMA-only communicator:
export TILEXR_ENABLE_IPC=0
export TILEXR_ENABLE_CREDIT_IPC=0
export TILEXR_ENABLE_UDMA=0
```

门铃模式必须与驱动/固件匹配，不能猜测 `hard`/`soft`。所有 rank 的 enable、提交模式及请求的 QP 数必须一致。TCP bootstrap 可达不证明 RoCE payload 可达，也不证明经过 L3 路由器。`InitThread`、allocation-only `Init/InitAll`、memory-only domain 和单 rank communicator 不初始化 RDMA。

Select `hard` or `soft` only from the installed driver/firmware contract. All ranks must agree on enablement, submission mode and requested QP count. TCP connectivity does not prove RoCE reachability or routed L3 forwarding. `InitThread`, allocation-only `Init/InitAll`, memory-only domains and single-rank communicators do not initialize RDMA.

## 3. Host 资源准备 / Prepare Host resources

| 顺序 / Step | 调用方职责 / Caller responsibility |
| --- | --- |
| 1 | 初始化 ACL、选择设备和 stream、建立多 rank communicator。 / Initialize ACL, select the device/stream and create a multirank communicator. |
| 2 | `TileXRRDMAQuery`：检查返回码、`connected && deviceDirect && !failed`。 / Check the return code and AIV readiness. |
| 3 | 所有 rank collective 调用 `TileXRRDMAConfigureQps`，然后分配 HBM。 / Collectively configure QPs, then allocate HBM. |
| 4 | 按相同顺序 collective `Register` 各 rank 的原始 allocation。 / Collectively register original local allocations in matching order. |
| 5 | `GetDeviceViews` 导出 Host POD 数组，复制到 GM。 / Export Host POD views and copy them to GM. |
| 6 | 发布 source/view，确认接收端就绪，启动发送 kernel。 / Publish source/views, confirm receiver readiness and launch the sender. |
| 7 | 检查 stream 及每个 AIV 状态；通知接收端完成消费。 / Check the stream and every AIV status, then coordinate receiver consumption. |
| 8 | 等待所有使用者退出，collective `Unregister`/`Shutdown`，成功后释放。 / Quiesce all users, collectively unregister/shut down, then free on success. |

`ConfigureQps` 接受每个 peer 的 1..16 个 QP，默认 1，只能保持或增长，并且必须在任何 MR 注册前调用。即使数量不变，它仍是 collective。部分扩容失败后停止使用 transport，按第 6 节清理。

`ConfigureQps` accepts 1..16 QPs per peer, defaults to one, permits only unchanged/increasing counts and must precede all MR registrations. It remains collective even if the count is unchanged. A partial expansion failure requires teardown as described in section 6.

`GetDeviceViews` 输出数组的数量必须**恰好等于已配置 QP 数**。同一 handle 的 views 共享 local/remote MR 和 key，各 QP 有独立队列与 state。不同 handle 指向相同 QP 的 views 仍共享该 QP 的所有权和进度。`TileXRRDMAGetDeviceViewV2` 及其源代码别名 `TileXRRDMAGetDeviceView` 始终选择 QP0。

`GetDeviceViews` requires an array count **exactly equal to the configured QP count**. Views for one handle share MRs/keys but have independent per-QP queues/state. Views from different handles targeting one QP still share its ownership and progress. The single-view V2 API and its source alias always select QP0.

导出失败后不要使用旧 GM view。支持数量范围内的失败会清空 Host 输出，但不会替应用清空 GM 副本。仅在本次导出和复制成功后启动 kernel。

Do not reuse an old GM view after export failure. Supported-count failures clear Host output, not the application's existing GM copy. Launch only after successful export and copy.

### Host 正常时序示例 / Host success-path example

下面示例使用两 rank、4 QP、32MiB MR，仅 rank0 发送。`DemoApp` 是应用需要实现的控制通道与纯 AIV launcher，不是 TileXR API。`CheckAll` 必须让所有 rank 在失败时离开正常流程；`PreparePayload` 完成 source 发布；`LaunchRegistered` 打包四参数 kernel ABI；`ConsumeAndQuiesce` 负责接收端校验及消费者退出。

This example uses two ranks, four QPs and a 32MiB MR, with rank0 sending. Implement `DemoApp` in the application: `CheckAll` coordinates failure across ranks; `PreparePayload` publishes source data; `LaunchRegistered` launches the four-argument pure-AIV kernel; `ConsumeAndQuiesce` validates and quiesces consumers. These hooks are not TileXR APIs or a complete error-recovery implementation.

```cpp
#include <acl/acl.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include "tilexr_api.h"

constexpr uint32_t kDemoQps = 4;
constexpr size_t kDemoMrBytes = 32ULL * 1024 * 1024;

struct DemoSession {
    TileXRCommPtr comm = nullptr;
    TileXRRDMAMemHandle handle = 0;
    aclrtStream stream = nullptr;
    void* payload = nullptr;
    void* viewsGM = nullptr;
    void* statusGM = nullptr;
    std::array<TileXRRDMADeviceView, kDemoQps> hostViews{};
    // 此对象不自动 free；错误路径必须保留资源供集体 Shutdown。
};

struct DemoApp {
    virtual void CheckAll(int localStatus, const char* phase) = 0;
    virtual int PreparePayload(void* payload, size_t bytes, aclrtStream stream) = 0;
    virtual int LaunchRegistered(const char* symbol, uint32_t blocks,
        aclrtStream stream, void* viewsGM, void* statusGM,
        uint32_t qpCount, uint32_t count) = 0;
    virtual int ConsumeAndQuiesce(int rank, void* payload, size_t bytes,
        aclrtStream stream) = 0;
    virtual ~DemoApp() = default;
};

// 应用在进程入口初始化 ACL；不要在已使用 ACL 的库调用中重复 aclInit。
void PrepareDemo(DemoSession& s, DemoApp& app, int rank, int physicalDevice)
{
    app.CheckAll(aclrtSetDevice(physicalDevice), "select physical device");
    app.CheckAll(aclrtCreateStream(&s.stream), "create stream");
    app.CheckAll(TileXRCommInitRankWithDomain(1, 2, rank, &s.comm), "comm init");
    TileXRRDMAInfo info{};
    app.CheckAll(TileXRRDMAQuery(s.comm, &info), "query");
    app.CheckAll(info.connected && info.deviceDirect && !info.hostDriven && !info.failed
        ? 0 : -1, "require AIV capability");
    app.CheckAll(TileXRRDMAConfigureQps(s.comm, kDemoQps), "configure before MR");
    app.CheckAll(aclrtMalloc(&s.payload, kDemoMrBytes, ACL_MEM_MALLOC_HUGE_FIRST), "payload");
    app.CheckAll(app.PreparePayload(s.payload, kDemoMrBytes, s.stream), "publish source");
    app.CheckAll(TileXRRDMARegister(s.comm, s.payload, kDemoMrBytes, &s.handle), "register");
    app.CheckAll(TileXRRDMAGetDeviceViews(s.comm, s.handle, 1-rank,
        s.hostViews.data(), kDemoQps), "export exact view count");
    app.CheckAll(aclrtMalloc(&s.viewsGM, sizeof(s.hostViews), ACL_MEM_MALLOC_HUGE_FIRST), "views GM");
    app.CheckAll(aclrtMalloc(&s.statusGM, kDemoQps*32, ACL_MEM_MALLOC_HUGE_FIRST), "status GM");
    app.CheckAll(aclrtMemcpy(s.viewsGM, sizeof(s.hostViews), s.hostViews.data(),
        sizeof(s.hostViews), ACL_MEMCPY_HOST_TO_DEVICE), "copy POD views");
    app.CheckAll(aclrtMemset(s.statusGM, kDemoQps*32, 0xff, kDemoQps*32), "status sentinel");
    app.CheckAll(aclrtSynchronizeStream(s.stream), "ready on all ranks");
}

// symbol 为下文四个示例之一；owners=单QP时1，多QP时kDemoQps。
// count=1 是单次；count=512 是每个 owner 的512次批量。
void RunAndCloseDemo(DemoSession& s, DemoApp& app, int rank,
    const char* symbol, uint32_t owners, uint32_t count)
{
    app.CheckAll((owners == 1 || owners == kDemoQps) && (count == 1 || count == 512)
        ? 0 : -1, "demo launch geometry");
    int localStatus = 0;
    if (rank == 0) {
        localStatus = app.LaunchRegistered(symbol, owners, s.stream,
            s.viewsGM, s.statusGM, kDemoQps, count);
        if (localStatus == 0) { localStatus = aclrtSynchronizeStream(s.stream); }
        std::array<uint32_t, kDemoQps*8> status{};
        if (localStatus == 0) {
            localStatus = aclrtMemcpy(status.data(), sizeof(status), s.statusGM,
                sizeof(status), ACL_MEMCPY_DEVICE_TO_HOST);
        }
        for (uint32_t q = 0; localStatus == 0 && q < owners; ++q) {
            localStatus = static_cast<int>(status[q*8]);
        }
    }
    app.CheckAll(localStatus, "sender complete; notify receiver explicitly");
    app.CheckAll(app.ConsumeAndQuiesce(rank, s.payload, kDemoMrBytes, s.stream), "consumers done");
    app.CheckAll(aclrtSynchronizeStream(s.stream), "all local device users stopped");
    app.CheckAll(TileXRRDMAUnregister(s.comm, s.handle), "unregister collectively");
    s.handle = 0; // 只有成功后才能清 handle；此时所有旧 view 都失效。
    app.CheckAll(TileXRRDMAShutdown(s.comm), "close QPs collectively");
    app.CheckAll(aclrtFree(s.payload), "free unregistered payload"); s.payload = nullptr;
    app.CheckAll(aclrtFree(s.viewsGM), "free view copies"); s.viewsGM = nullptr;
    app.CheckAll(aclrtFree(s.statusGM), "free result slots"); s.statusGM = nullptr;
    app.CheckAll(TileXRCommDestroy(s.comm), "destroy communicator"); s.comm = nullptr;
    app.CheckAll(aclrtDestroyStream(s.stream), "destroy stream"); s.stream = nullptr;
}
```

所有 rank 采用相同阶段调用 `aclInit`、`PrepareDemo`、`RunAndCloseDemo`；只有整个应用不再使用 ACL 时才 reset/finalize。单 QP 示例传 `owners=1`，多 QP 示例传 `owners=kDemoQps`，`count` 可选 1 或 512。例如 `RunAndCloseDemo(s, app, rank, "demo_write_sge_multiqp", kDemoQps, 512)` 表示每个 QP 各发送 512 条 WR。

All ranks call `aclInit`, `PrepareDemo` and `RunAndCloseDemo` in matching phases; reset/finalize ACL only when the whole application is done. Use one owner for single-QP examples or `kDemoQps` owners for multi-QP examples. The call above sends 512 WRs **per QP**. Use a fresh example session when switching demonstrations; production reuse must respect cumulative capacity.

## 4. 四种 AIV 调用示例 / Four AIV examples

以下代码放在同一个纯 AIV 文件中，均接收 `(viewsGM, statusGM, qpCount, count)`。每个 owner 有独立 scratch 和 32B 结果槽；每个 QP 的 source/destination 分区为 2MiB。批量 stride 为 2048B，示例只接受 `count=1/512`；Host 已完成 source/view 发布。

Place these functions in one pure-AIV source file. Each takes `(viewsGM, statusGM, qpCount, count)` and uses private scratch and a 32B result slot per owner. Each QP has 2MiB source/destination partitions. The sample uses a 2048B batch stride and count 1/512, with source/views already published by the Host.

```cpp
#include "kernel_operator.h"
#include "tilexr_rdma_write.h"

namespace RdmaDemo {
constexpr uint64_t kSource = 4096;
constexpr uint64_t kDestination = 16ULL * 1024 * 1024;
constexpr uint64_t kQpSpan = 2ULL * 1024 * 1024;

template <AscendC::HardEvent event>
__aicore__ inline void Sync(AscendC::TPipe& pipe)
{
    const int32_t id = static_cast<int32_t>(pipe.FetchEventID(event));
    AscendC::SetFlag<event>(id);
    AscendC::WaitFlag<event>(id);
}

__aicore__ inline TileXR::RDMAWriteOptions Options(uint32_t count)
{
    TileXR::RDMAWriteOptions options;
    options.count = count;
    if (count > 1) { options.localStride = options.remoteStride = 2048; }
    return options;
}

struct Buffers {
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> scratchBuffer, resultBuffer;
    __aicore__ inline void Init(uint32_t numSge)
    {
        pipe.InitBuffer(scratchBuffer, TileXR::RDMAWriteScratchBytes(numSge));
        pipe.InitBuffer(resultBuffer, 32);
    }
    __aicore__ inline void Finish(GM_ADDR statusGM, uint32_t index, uint32_t status)
    {
        auto result = resultBuffer.Get<uint32_t>();
        for (uint32_t i = 0; i < 8; ++i) { result.SetValue(i, 0); }
        result.SetValue(0, status);
        AscendC::GlobalTensor<uint32_t> output;
        output.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t*>(statusGM) + index*8, 8);
        Sync<AscendC::HardEvent::S_MTE3>(pipe);
        AscendC::DataCopy(output, result, 8); // 元素数，不是8字节
        Sync<AscendC::HardEvent::MTE3_S>(pipe);
    }
};
} // namespace RdmaDemo

// 1. 单QP、连续512B。
extern "C" __global__ __aicore__ void demo_write(
    GM_ADDR viewsGM, GM_ADDR statusGM, uint32_t qpCount, uint32_t count)
{
    (void)qpCount;
    if (AscendC::GetBlockIdx() != 0) { return; }
    RdmaDemo::Buffers b; b.Init(1);
    auto scratch = b.scratchBuffer.Get<uint8_t>();
    auto options = RdmaDemo::Options(count);
    auto view = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    const uint32_t status = TileXR::RDMAWrite(view, RdmaDemo::kSource,
        RdmaDemo::kDestination, 512, options, scratch);
    b.Finish(statusGM, 0, status);
}

// 2. 单QP、两个512B本地段；逆序读取，远端得到连续1024B。
extern "C" __global__ __aicore__ void demo_write_sge(
    GM_ADDR viewsGM, GM_ADDR statusGM, uint32_t qpCount, uint32_t count)
{
    (void)qpCount;
    if (AscendC::GetBlockIdx() != 0) { return; }
    RdmaDemo::Buffers b; b.Init(2);
    auto scratch = b.scratchBuffer.Get<uint8_t>();
    auto options = RdmaDemo::Options(count);
    auto view = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    TileXRRDMADeviceSge sges[2] = {
        {RdmaDemo::kSource+640, 512, 0}, {RdmaDemo::kSource, 512, 0}};
    const uint32_t status = TileXR::RDMAWriteSge(view, sges, 2,
        RdmaDemo::kDestination, options, scratch);
    b.Finish(statusGM, 0, status);
}

// 3. 多QP、连续段；每个AIV只调用自己index对应的QP。
extern "C" __global__ __aicore__ void demo_write_multiqp(
    GM_ADDR viewsGM, GM_ADDR statusGM, uint32_t qpCount, uint32_t count)
{
    const uint32_t index = AscendC::GetBlockIdx();
    if (index >= qpCount) { return; }
    RdmaDemo::Buffers b; b.Init(1);
    auto scratch = b.scratchBuffer.Get<uint8_t>();
    auto options = RdmaDemo::Options(count);
    auto views = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    const uint64_t delta = uint64_t(index) * RdmaDemo::kQpSpan;
    const uint32_t status = TileXR::RDMAWriteMultiQp(views, qpCount, index,
        RdmaDemo::kSource+delta, RdmaDemo::kDestination+delta, 512, options, scratch);
    b.Finish(statusGM, index, status);
}

// 4. 多QP、多SGE；共享MR但各QP写不同远端分区。
extern "C" __global__ __aicore__ void demo_write_sge_multiqp(
    GM_ADDR viewsGM, GM_ADDR statusGM, uint32_t qpCount, uint32_t count)
{
    const uint32_t index = AscendC::GetBlockIdx();
    if (index >= qpCount) { return; }
    RdmaDemo::Buffers b; b.Init(2);
    auto scratch = b.scratchBuffer.Get<uint8_t>();
    auto options = RdmaDemo::Options(count);
    auto views = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    const uint64_t delta = uint64_t(index) * RdmaDemo::kQpSpan;
    TileXRRDMADeviceSge sges[2] = {
        {RdmaDemo::kSource+delta+640, 512, 0}, {RdmaDemo::kSource+delta, 512, 0}};
    const uint32_t status = TileXR::RDMAWriteSgeMultiQp(views, qpCount, index,
        sges, 2, RdmaDemo::kDestination+delta, options, scratch);
    b.Finish(statusGM, index, status);
}
```

| kernel | AIV 数 / AIVs | 每 QP 每 WR payload / Payload per QP per WR |
| --- | ---: | --- |
| `demo_write` | 1 | 连续 512B / Contiguous 512B |
| `demo_write_sge` | 1 | 两段 512B，远端连续 1024B / Two 512B spans gathered into 1024B |
| `demo_write_multiqp` | `qpCount` | 连续 512B / Contiguous 512B |
| `demo_write_sge_multiqp` | `qpCount` | 两段 512B，远端连续 1024B / Two 512B spans gathered into 1024B |

多 QP 包装不自动分区、跨核 barrier 或建立跨 QP 顺序。同 kernel 的另一个 AIV 生产 source 时，本 AIV 的 `PipeBarrier` 不能发布对方的数据；可先完成 producer kernel 及 stream 依赖，再启动 WRITE kernel。

Multi-QP wrappers do not partition payloads, synchronize AIVs or order different QPs. A local `PipeBarrier` cannot publish another AIV's data. Complete a separate producer kernel and its stream dependencies before WRITE when that is the application's publication protocol.

## 5. 参数与容量 / Options and capacity

直接使用 `TileXR::RDMAWriteOptions`；所有 offset/stride 均为字节。 / Use `TileXR::RDMAWriteOptions` directly; all offsets/strides are bytes.

| 字段 / Field | 默认 / Default | 契约 / Contract |
| --- | ---: | --- |
| `count` | 1 | 所选 QP 的 WR 数，必须大于 0。 / Positive WR count on the selected QP. |
| `localStride` | 0 | 每个 WR 对每个本地段的位移；0 复用 source。 / Per-WR displacement of every local segment; zero reuses it. |
| `remoteStride` | 0 | 每个远端连续结果的位移；0 重复覆盖。 / Per-WR remote displacement; zero overwrites the same span. |
| `postBatch` | 64 | 1..64；每批最后一个 WR 请求 CQE，包括不足一批的尾部。 / Last WR of every batch, including a partial tail, requests a CQE. |
| `maxInFlight` | 128 | 必须容纳 `min(count, 2*postBatch)` 个 WR；实际 QP 的 `maxSendWr` 也必须容纳该峰值。 / Both this limit and actual `maxSendWr` must admit that peak. |
| `cyclicSlots` | 0 | 0 为线性 stride；非 0 按持久 WR ordinal 循环 payload。 / Zero uses linear strides; nonzero cycles payload slots from the persistent WR ordinal. |
| `maxPolls` | 1000000 | 每个预期 batch CQE 的最大快照数，大于 0，非时间单位。 / Positive snapshot limit per expected batch CQE, not a time duration. |

每个 SGE 为 `{localOffset, bytes, reserved}`；`reserved=0`，长度大于 0，SGE 数为 `1..min(16, view.queue.maxSendSge)`。每段及总长度均小于 `2^31`，本地全部段与远端完整结果必须处于对应 MR 内；检查完整 count/stride 跨度及溢出。SGE 列表为 kernel-local 数组，在调用期间稳定，所有段共用该 view 的 local MR/lkey。

Each SGE is `{localOffset, bytes, reserved}` with zero reserved and positive length. Count is `1..min(16, view.queue.maxSendSge)`. Every segment and the total are below `2^31`; validate complete local/remote spans across count/strides and overflow. Supply a stable kernel-local list using one local MR/lkey, not a Host pointer or a cast GM array.

使用 `RDMAWriteScratchBytes(numSge)` 分配 32B 对齐的 UB：当前 1 SGE 为 4608B，2..16 SGE 为 20992B，非法数量返回 0。不要使用低层单 WR 的 512B scratch 代替。每个并发 owner 独占 scratch，且与 SGE 数组不重叠。

Allocate 32B-aligned UB with `RDMAWriteScratchBytes(numSge)`: currently 4608B for one SGE, 20992B for 2..16, and zero for invalid counts. The low-level 512B single-WR scratch is insufficient here. Concurrent owners need separate scratch, disjoint from their SGE arrays.

| 每 WR 的 SGE 数 / SGEs per WR | SQ 64B 块 / SQ blocks |
| --- | ---: |
| 1 | 1 |
| 2..5 | 2 |
| 6..9 | 3 |
| 10..13 | 4 |
| 14..16 | 5 |

本接口不复用物理 SQ/CQ 尾部，发布前必须满足以下累计容量条件。 / These interfaces reject physical SQ/CQ reuse and check cumulative capacity before posting:

```text
producer + count * WqeBlocks(numSge) < sqDepth
cqConsumer + ceil(count / postBatch) < cqDepth
min(count, 2 * postBatch) <= maxInFlight
min(count, 2 * postBatch) <= maxSendWr
```

`producer/completed` 数 SQ 块，`cqConsumer` 数 CQ 槽，`count/maxInFlight/maxSendWr` 数 WR；不能互换。预热和先前调用也消耗空间，注册新 MR 不重置 QP。耗尽时停止并重建 communicator/QP，不能清 state 或把 producer 取模。`cyclicSlots` 只循环 payload，不允许队列 wrap，也不表示远端已消费。

SQ progress counts blocks; CQ progress counts completion slots; count/in-flight limits count WRs. Warmups and earlier calls consume capacity, and a new MR does not reset a QP. Stop and recreate exhausted QPs instead of clearing state or applying modulo. Cyclic payload slots neither enable queue wrap nor acknowledge remote consumption.

`RDMAWrite*` 包括 `count=1` 都进入 pipeline 模式；健康且完全 drain 的 all-signaled QP 可以进入 pipeline，之后不能返回旧 all-signaled API。

Every `RDMAWrite*` call enters pipeline mode, including count one. A healthy drained all-signaled QP may enter it, but cannot switch back to the old all-signaled APIs.

流量为 `QP 数 * count * 各 SGE 长度之和`；例如 512 条 SGE8 WR 占 1536 个 SQ 块，默认 batch64 产生 8 个 CQE。

Payload volume is `QP count * count * sum(SGE lengths)`. For example, 512 eight-SGE WRs use 1536 SQ blocks and eight CQEs at batch64.

## 6. 完成、错误与释放 / Completion, errors and release

成功返回表示本次 WR 已完成，但不会通知接收端或建立其 stream/cache 可见性。应用先发布 source，再提交 WRITE；发送完成后通过控制消息/轮次 flag 通知远端，并按目标设备规则处理缓存和消费者依赖。不同 QP 覆盖同一区域没有全局顺序。

Success completes the call's WRs but does not notify the receiver or establish its stream/cache visibility. Publish source data before WRITE; coordinate remote completion with control messages/round flags and the target device's cache/consumer rules. Different QPs have no global ordering for overlapping destinations.

| 资源 / Resource | 释放条件 / Lifetime boundary |
| --- | --- |
| HBM、MR、key | 所有使用者退出且 collective Unregister/Shutdown 成功。 / All users quiescent and collective unregister/shutdown successful. |
| GM view、QP、state、mapping | 所有相关 kernel 退出；注销/关闭后旧 view 失效。 / Live through every using kernel; old views expire at unregister/shutdown. |
| UB scratch、SGE list | 同步调用及相关 MTE 完成。 / Live through the synchronous call and its MTE operations. |
| source | NIC 不再引用；timeout 不能证明已取消。 / Until no NIC references remain; timeout does not cancel them. |
| destination | 接收端按应用协议消费结束。 / Until receiver consumption completes under the application protocol. |

设备返回码为 `TileXRRDMADeviceStatus`：0 success、1 invalid、2 unsupported、3 busy、4 timeout、5 CQE error、6 CQE mismatch、7 poisoned。Host API 使用独立的 int 错误码；`Query.failed` 不能替代 kernel 返回码和共享 state 检查。

Device calls return the status enum above; Host APIs use their separate int error codes. Inspect every owner's kernel result and shared state, not only `Query.failed`.

错误后的处理顺序 / Error-path order:

1. 通知所有 rank 停止新发送/消费，等待所有相关 kernel/stream 退出。 / Stop new work on every rank and quiesce all related kernels/streams.
2. 保留已发布 WR 的 source 与 sticky error；不要在原 QP 重试、清计数或按成功前缀判整批通过。 / Retain source ownership and sticky errors; do not retry/reset the QP or treat a partial prefix as full success.
3. 所有 rank collective `TileXRRDMAShutdown(comm)`，成功后才释放受影响 allocation，包括注册回滚失败且没有 handle 的内存。 / Collectively shut down; free affected allocations only after success, including failed registration rollbacks without a handle.
4. 清理失败保留 handle、指针和 communicator 供重试；bootstrap 损坏时只能停止并由进程退出回收，不能继续释放可能被 NIC 引用的 MR。 / Preserve failed cleanup ownership for retry. Broken bootstrap cannot prove global teardown; stop and rely on process-exit reclamation instead of freeing NIC-referenced memory.

正常 `Unregister` 同样是 collective，并要求远端消费者先退出。不要用无条件 `aclrtFree` 的析构掩盖失败。

Normal unregister is also collective and requires remote consumer quiescence. Unconditional destructor frees cannot implement this error protocol.

## 7. 文件分工与依赖 / File responsibilities and dependencies

| 文件 / File | 职责 / Responsibility | 直接依赖 / Direct dependencies |
| --- | --- | --- |
| [tilexr_rdma.h](../src/include/tilexr_rdma.h) | Host 资源及 Put/Get API / Host resources and posting APIs | `device_types`；由 `rdma_api.cpp` 实现 / implemented by `rdma_api.cpp` |
| [tilexr_rdma_device_types.h](../src/include/tilexr_rdma_device_types.h) | 共享 POD ABI / Shared POD ABI | 固定宽度标准类型 / Fixed-width standard types |
| [tilexr_rdma_write_options.h](../src/include/tilexr_rdma_write_options.h) | 参数及 scratch 大小 / Options and scratch size | `pipeline`，Host 可包含 / Host-safe |
| [tilexr_rdma_write.h](../src/include/tilexr_rdma_write.h) | 四个业务 WRITE 入口 / Four application WRITE adapters | `device`、`write_options` |
| [tilexr_rdma_device.h](../src/include/tilexr_rdma_device.h) | AIV cache/MTE/MMIO 操作 / AIV cache/MTE/MMIO adapter | `kernel_operator.h`、`wire`、`pipeline` |
| [tilexr_rdma_pipeline.h](../src/include/tilexr_rdma_pipeline.h) | 批量调度及完成回收 / Batch scheduling and retirement | `wire`、device 提供的 IO / device-supplied IO |
| [tilexr_rdma_wire.h](../src/include/tilexr_rdma_wire.h) | 队列编码及基础算法 / Queue encoding and base algorithms | `device_types`、模板 IO / templated IO |

Host 通常包含 `tilexr_api.h`/`tilexr_rdma.h`，AIV kernel 包含 `tilexr_rdma_write.h`。`wire/pipeline` 的 IO 模板也供 CPU 模型使用，这不代表 CPU 执行了设备 DMA。文件顶部的中英文说明给出每个实现文件的功能和相邻依赖。

Host code normally includes `tilexr_api.h`/`tilexr_rdma.h`; AIV kernels include `tilexr_rdma_write.h`. Portable wire/pipeline IO templates also support CPU models, without executing device DMA. Bilingual source-file overviews describe each implementation's role and adjacent dependencies.

安装头文件位于 `${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}`，Host 库为 `${CMAKE_INSTALL_LIBDIR}/libtile-comm.so`。纯 AICore binary 独立安装在 `${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`，相对所属 Host 库加载，经 `rtDevBinaryRegister`/`rtFunctionRegister` 注册，以 `rtKernelLaunchWithFlagV2` 启动。生产 Host 库不嵌入 kernel 对象，也不使用 `kernel<<<...>>>`。

Headers install under the configured include directory and the Host library under the configured library directory. Install pure AICore binaries separately under `tilexr/kernels`, load relative to the owning Host library, register them and launch with the runtime APIs above. Production Host libraries neither embed kernel objects nor use triple-chevron launches.

V2 布局为 view 152B、queue 104B、state 64B、SGE 16B；版本及 `structBytes` 必须匹配。库、应用、kernel 与安装头文件一起重建并重新创建 QP/view，不能混用 V1/V2。

V2 uses 152B views, 104B queues, 64B state and 16B SGEs. Version and `structBytes` must match. Rebuild library, application, kernels and installed headers together and recreate QPs/views; do not mix V1/V2 artifacts.

## 8. 验证与已知限制 / Validation and known limitations

不使用 NPU 的检查 / Checks without NPU launches:

```bash
cmake -S tests/rdma -B /tmp/tilexr-rdma-build
cmake --build /tmp/tilexr-rdma-build -j4
ctest --test-dir /tmp/tilexr-rdma-build --output-on-failure
```

测试数量随 headers 改变；具备 verbs、匹配的 `ibv_extend.h` 和 ACL headers 时才启用全部 NDA mock 用例。CPU 测试不证明 HBM 注册、MTE3/`st_dev`、硬件 CQ 或真实网络可见性。

Test count depends on available headers; NDA mock cases require matching verbs/extension/ACL headers. CPU tests do not prove HBM registration, MTE3/`st_dev`, hardware CQ behavior or network visibility.

已保留的硬件记录包含 16QP/SGE1/512WR/2KiB 的 CQ timeout，根因未确定；1KiB 成功及旧工具复现不能把 2KiB 归为通过。历史回归矩阵、原始证据位置、CANN 版本以及未验证的跨机/L3/SOFT/wrap 边界统一见 [集成说明](RDMA_INTEGRATION.md#hardware-validation-boundary)。文档改写不构成新的硬件验证。

Retained hardware records include an unresolved CQ timeout at 16 QPs, SGE1, 512 WRs and 2KiB. A passing 1KiB case or reproduction with the earlier tool does not make 2KiB pass. See the linked integration record for matrices, evidence locations, versions and untested cross-host/L3/SOFT/wrap boundaries. This documentation revision establishes no new hardware result.
