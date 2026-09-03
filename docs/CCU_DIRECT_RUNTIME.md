# TileXR Direct CCU 技术说明

本文记录 `direct-ccu-rebased` 分支中 TileXR 自研 Direct CCU 路径的技术原理、运行流程、执行方法和测试方法。它面向继续开发和问题定位，不作为外部稳定 API 文档。

## 1. 当前边界

Direct CCU 当前属于 `src/comm/ccu` 下的内部后端能力，由 `TileXRComm` 按需持有 `TileXRCcuBackend`。它的目标是在不链接 hcomm/HCCL 私有 CCU producer 的前提下，由 TileXR host 侧完成 CCU repository、mission、lower-layer 资源和 submit task 的准备，并最终通过 CANN runtime 的 `rtCCULaunch` 下发任务。

当前需要明确的边界如下：

- 生产代码不得链接或包含 hcomm/HCCL 私有 CCU producer 接口。
- 对外安装头文件当前不暴露 `TileXRDirectCcu*`、`PrepareDirectCcu`、`SubmitPrepared` 等 Direct CCU C API。
- `TileXRCcuCollectivePlanner::Supports()` 目前仍返回 `false`，通用 collective/alltoall 尚未接入为正式 TileXR collective 后端。
- 已维护的数据面验证路径是 direct CCU P2P copy probe，基于 CCU memory-copy microcode，不走 alltoall。
- 默认非 P2P barrier/smoke 只能作为安装和提交链路诊断，不能替代 P2P copy 的数据正确性证明。

## 2. 总体架构

核心对象关系：

```text
TileXRComm
  |
  +-- TileXRCcuBackend
        |
        +-- TileXRCcuRuntimeSession
        |     |
        |     +-- TileXRCcuDirectRuntime
        |
        +-- TileXRCcuCollectivePlanner
        |
        +-- TileXRCcuExecutor
```

主要模块职责：

- `tilexr_ccu_backend.*`：Direct CCU 后端门面，封装 runtime session、planner 和 executor。
- `tilexr_ccu_runtime_session.*`：管理 rank、rankSize、device、socket/thread allgather 和 direct runtime 可用状态。
- `tilexr_ccu_direct_runtime.*`：动态加载 HCCP/RA/runtime 符号，初始化底层 RA/HDC/CCU TLV，注册 resource window，导出本端和远端 transport 信息。
- `tilexr_ccu_resource_allocator.*`：按 driver basic info 解出的资源窗口分配 mission、repository instruction、local/remote XN、GSA、CKE、channel 等资源。
- `tilexr_ccu_collective_planner.*`：把 runtime session、资源分配、lower-layer plan、repository 安装和 prepared submit task 串起来。
- `tilexr_ccu_direct_orchestrator.*`：执行完整 direct install attempt，包括资源规格解码、资源分配、lower-layer plan、launch package、manifest、hardware install 和 submit task 生成。
- `tilexr_ccu_install_provider.*`：定义硬件安装需求、证据和校验，执行 repository/lower-layer/mission 等安装步骤。
- `tilexr_ccu_repository.*`：构造 CCU instruction repository image，并负责把 repository image 安装到设备侧。
- `tilexr_ccu_barrier_program.*`、`tilexr_ccu_memory_program.*`、`tilexr_ccu_microcode.*`：生成 CCU barrier 和 memory-copy 指令。
- `tilexr_ccu_runtime.*`：把 `TileXRCcuTask` 映射为 runtime task，并调用 `rtCCULaunch`。

## 3. 初始化与生命周期

`TileXRComm::Init()` 和 `TileXRComm::InitThread()` 结束前会调用 `InitCcuBackendIfEnabled()`。是否启用由环境变量控制：

```bash
export TILEXR_ENABLE_CCU_BACKEND=1
```

启用后流程如下：

1. `TileXRComm::InitCcuBackend()` 创建 `TileXRCcuBackend`。
2. `TileXRCcuBackend::Init()` 重置 planner，并调用 `TileXRCcuRuntimeSession::Init()`。
3. `TileXRCcuRuntimeSession::Init()` 对单 rank communicator 直接跳过 direct runtime。
4. 多 rank 时创建 `TileXRCcuDirectRuntime`，传入 rank、rankSize、device 和 allgather 回调。
5. `TileXRCcuDirectRuntime::Init()` 动态加载底层符号，解析逻辑 device 到物理 device，初始化 RA/HDC 和 CCU TLV。
6. runtime 初始化成功后刷新 direct CCU basic info，缓存 die、resource address、mission/resource 范围等基础信息。

失败策略是降级而不是让 communicator 初始化失败：

- runtime 初始化失败会记录 warning，并将 direct CCU 后端关闭。
- 失败状态按 `devId` 记录，避免一个 device 初始化失败污染同进程其他 device。
- 后续同 device 再初始化会直接跳过，并保留明确的 unavailable message。

`TileXRComm::Destroy()` 会关闭 CCU backend。`TileXRCcuBackend::Shutdown()` 会先重置 planner，再关闭 runtime session；runtime session 会释放 direct runtime、basic info 缓存和 allgather 轮次状态。

## 4. Direct Runtime 原理

`TileXRCcuDirectRuntime` 是 TileXR 和底层 driver/RA/HCCP/runtime 交互的边界，承担四类工作。

### 4.1 符号与设备初始化

runtime 动态解析所需符号，不在 `tile-comm` 链接期引入 hcomm/HCCL 私有依赖。初始化阶段会：

- 加载 HCCP/RA 相关入口。
- 选择 direct CCU HDC 类型。
- 解析 `logicDevId -> devicePhyId`。
- 初始化 RA/HDC。
- 初始化 CCU TLV。
- 创建 driver adapter，用于 basic info 查询和后续 install。

如果任一关键步骤失败，runtime 返回不可用状态，并由 session 记录 device-scoped failure。

### 4.2 Resource Window 注册

CCU lower-layer/repository 安装需要一块可被 peer 识别的 resource window。当前推荐使用 RA ctx 模式：

```bash
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE=ra_ctx
```

RA ctx 模式的关键步骤：

1. 通过 `RaGetDevEidInfoNum` / `RaGetDevEidInfoList` 获取可用 EID。
2. 通过 `TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX` 或 rank 级变量选择 EID。
3. 调用 `RaCtxInit` 创建 RA ctx。
4. 通过 `RaCtxTokenIdAlloc` 分配 token id。
5. 对 CCU resource address 做页对齐后调用 `RaCtxLmemRegister`。
6. 导出 `addr/bytes/tokenId/rawTokenId/tokenValue/eid/raCtxHandle`。

常用 EID 选择变量：

```bash
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX=3
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_RANK0=3
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_RANK1=3
```

### 4.3 Peer 信息交换

runtime session 为 direct runtime 提供 allgather：

- 有 socket exchange 时使用 `TileXRSockExchange::AllGather()`。
- thread mode 或测试模式下没有 socket exchange 时，使用进程内 thread allgather。

allgather 用于交换：

- 本端 resource window token 和 EID。
- endpoint route 信息。
- remote XN/CKE/channel 绑定所需的 peer 资源窗口。
- P2P copy 端点中的 source/destination address 和 process token。

thread allgather 带超时和失败中止逻辑，避免单 rank runtime init 失败后其他 rank 永久等待。

### 4.4 Endpoint Route 与 Remote Buffer

`ExportRemoteCcuRmaBuffers()` 会把本端 resource window 信息 allgather 到所有 rank，并为每个 peer 生成 remote CCU buffer 信息。可用 RA ctx endpoint route 时，会进一步：

- 查询本端到 peer EID 的 TP handle。
- 交换 TP handle。
- 导入 peer QP。
- 使用导入得到的 TPN 和本端 doorbell token 构造 channel route。

这里存在一个重要细节：channel 中使用的 remote EID 采用 hcomm 兼容语义，导入后会使用反向 EID 表示。

## 5. 资源模型与所有权

Direct CCU 执行前需要把硬件资源划分为明确的窗口。资源来自 `TileXRCcuBasicInfo`，再由 `TileXRCcuDecodeBasicInfo()` 和 `TileXRCcuBuildResourceSpec()` 转换为 `TileXRCcuResourceSpec`。

主要资源：

- `mission`：mission id 和 mission key。
- `repository`：CCU instruction repository 中的指令槽。
- `localXn`：本端 CCU XN 资源。
- `localGsa`：本端 GSA 资源，P2P memory copy 用于装载地址、token 和长度。
- `remoteXn`：绑定 peer 侧 XN 的本地表示。
- `notifyCke`、`localWaitCke`、`remoteNotifyCke`：同步/完成通知使用的 CKE。
- `channels`：指向 peer resource window/endpoint route 的 channel 绑定。

`TileXRCcuResourceAllocator` 使用 receipt 记录每次分配。当前释放策略是严格 LIFO，用来保证 cursor 回退不会和仍在使用的资源重叠。这个设计的目的不是做复杂资源池，而是在多次 prepare/launch 场景中先保证资源窗口所有权清晰、可诊断、可回收。

## 6. Lower-layer Install 原理

lower-layer install 负责把 CCU 执行需要的资源上下文安装到 driver 可识别的位置。主要输入来自：

- basic info 解码出的资源窗口。
- allocator 分配出的 mission/XN/CKE/channel range。
- 本端 resource window token。
- peer resource window token。
- verified endpoint route。

准备流程：

1. `PrepareDirectCcuLowerLayerTemplateFromAllocation()` 先注册本端 resource window。
2. 导出本端 token 后，如果已经有 verified endpoint route 就配置到 runtime；否则尝试自动采集。
3. `ExportRemoteCcuRmaBuffers()` 交换 peer token 和 endpoint 信息。
4. `ExchangeDirectCcuRemoteNotifyCke()` 交换 peer 本地 XN/CKE/channel 分配结果。
5. `TileXRCcuBuildLowerLayerTransportTemplate()` 构造 transport snapshot。
6. `TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot()` 生成安装计划。

安装计划覆盖的 surface 包括 local XN、remote XN binding、notify CKE、channel binding、repository 和 mission/key。`tilexr_ccu_install_provider` 会为这些 surface 建立 manifest 和 install evidence。只有 install evidence 与 launch package fingerprint、device、rank、provider 和资源范围匹配时，后续 prepared submit task 才被认为可提交。

## 7. Repository、Mission 与 Launch Package

Direct CCU 的 host 下发不是直接下发一段普通 kernel，而是下发 CCU mission 和 instruction repository。

完整 install attempt 的关键步骤在 `RunDirectInstallAttemptImpl()` 中：

1. 校验 basic info、provider 和 repository install 输入。
2. 解码 basic info，生成 resource spec。
3. 分配 mission、repository、XN、GSA、CKE、channel 等资源。
4. 如是 P2P memory copy，调整 local XN/GSA/remote XN 资源布局。
5. 准备 lower-layer install plan。
6. 将 producer plan 与 lower-layer proof 对齐，确保 sync resource 使用真实 peer 资源。
7. 构造 CCU program 和 repository image。
8. 绑定 launch package 的 device/rank/provider scope。
9. 构造 install manifest。
10. 执行 `TileXRCcuInstallHardware()`，安装 repository/lower-layer/mission。
11. 根据 install evidence 生成 `submitTasks`。

`TileXRCcuTask` 是最终提交给 runtime 的任务描述，关键字段包括：

- `dieId`
- `missionId`
- `key`
- `instStartId`
- `instCnt`
- `timeout`
- `argSize`
- `args[]`

提交时 `TileXRCcuSubmitPreparedTasks()` 会逐个任务调用 `TileXRCcuSubmitTaskWithReport()`，后者在 `tilexr_ccu_runtime.cpp` 中映射到 `rtCCULaunch()`。如果中途某个 task 失败，submit report 会记录已提交数量和失败 task 的 mission/key/instruction/args 诊断信息。

## 8. P2P CCU Copy 执行原理

当前硬件数据面验证使用 direct CCU P2P copy，不走 alltoall。它验证的是 CCU 指令真实从 peer device memory 读写数据，而不是只验证 host marker 或 IPC 同步。

### 8.1 端点准备

每个 rank 分配两块 device buffer：

- `source`：写入 rank 相关的数据 pattern。
- `destination`：初始化为固定填充值。

然后通过 `rtUbDevQueryInfo(QUERY_PROCESS_TOKEN, ...)` 查询 source/destination 的 process token，打包为 CCU memory token。所有 rank 通过 session allgather 交换端点：

```text
rank -> {sourceAddr, sourceToken, destinationAddr, destinationToken, bytes}
```

### 8.2 方向语义

`TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_DIRECTION` 支持两种方向：

- `remote_to_local`：active rank 从 peer source 拷贝到本 rank destination，active rank 校验本地 destination。
- `local_to_remote`：active rank 从本 rank source 拷贝到 peer destination，inactive rank 等待 done gate 后校验本地 destination。

只有 active rank 真正 submit direct CCU task；inactive rank 会打印 `p2pCcuCopy skipped`，但仍通过 done gate 参与同步，必要时校验自己的 destination。

### 8.3 Memory-copy Microcode

P2P copy 会构造一个 memory copy program，典型使用 7 条 CCU 指令：

- 把 local/remote address、token、length 装入 GSA/XN。
- 根据方向生成 `TransRmtMemToLocMem` 或 `TransLocMemToRmtMem`。
- 使用 CKE 完成通知，host 侧随后 `aclrtSynchronizeStream()` 等待 stream 完成。

通过 `TILEXR_CCU_DIRECT_TRACE=1` 可在日志中看到解码后的 CCU 指令，例如：

```text
decoded=TransRmtMemToLocMem
decoded=TransLocMemToRmtMem
```

## 9. 执行方法

### 9.1 构建

在 NPU 服务器上执行：

```bash
cd /home/tileXR
source scripts/common_env.sh
cmake --build build_ccu_direct --target tile-comm -j4
```

如果 build 目录不同，需要同步修改 `TILEXR_TILE_COMM_LIB`。

### 9.2 两卡 P2P Copy Smoke

以下命令是当前推荐的 direct CCU 数据面 smoke。设备号按实际健康设备调整。

```bash
cd /home/tileXR
source scripts/common_env.sh

export TILEXR_TILE_COMM_LIB=/home/tileXR/build_ccu_direct/src/comm/libtile-comm.so
export TILEXR_CCU_SMOKE_DEVICES=3,2
export TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY=1
export TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY=1
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES=64
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_ACTIVE_RANK=0
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_RANK0=3
export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_RANK1=3
export TILEXR_CCU_DIRECT_SMOKE_READY_TIMEOUT_MS=180000
export TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1
export TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE=0
export TILEXR_CCU_SMOKE_TIMEOUT=180
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_DIRECTION=remote_to_local

timeout 420s bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
```

反方向验证：

```bash
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_DIRECTION=local_to_remote
timeout 420s bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
```

预期关键日志：

```text
tilexr_ccu_direct_smoke prepare ret=0 ... installSucceeded=1 ... submitReady=1
tilexr_ccu_direct_smoke submit ret=0 ... submitted=1
tilexr_ccu_direct_smoke p2pCcuCopy ... mismatches=0 ... passed=1
tilexr_ccu_direct_smoke_runner success
```

### 9.3 覆盖 `TileXRComm` 自动初始化路径

默认 P2P smoke 可以只用 direct CCU internal init。若要覆盖 `TileXRComm::Init()` 中的 backend auto-init 路径，增加：

```bash
export TILEXR_ENABLE_CCU_BACKEND=1
export TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=0
timeout 420s bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
```

### 9.4 Dry-run 参数检查

不触碰 ACL/NPU，只检查 runner 推导出的 repository/task 参数：

```bash
export TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1
export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY=1
export TILEXR_CCU_DIRECT_SMOKE_DRY_RUN=1
bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
```

## 10. 测试矩阵

### 10.1 本地单元测试

推荐先跑 CCU 相关 Python 测试：

```bash
python3 -m unittest \
  tests.ccu.test_tilexr_ccu_resource_allocator \
  tests.ccu.test_tilexr_ccu_lower_layer_plan_builder \
  tests.ccu.test_tilexr_ccu_backend_boundary \
  tests.ccu.test_tilexr_ccu_direct_orchestrator \
  tests.ccu.test_tilexr_ccu_direct_smoke_probe \
  tests.ccu.test_tilexr_ccu_direct_smoke_runner \
  tests.ccu.test_tilexr_ccu_public_comm_api
```

覆盖更完整的 CCU suite：

```bash
python3 -m unittest discover tests/ccu
```

### 10.2 依赖边界检查

每次修改 `src/comm/ccu` 后应确认 `tile-comm` 没有引入 hcomm/HCCL 私有 CCU 依赖：

```bash
source scripts/common_env.sh
bash tests/ccu/check_tile_comm_no_hcomm_deps.sh build/src/comm/libtile-comm.so
```

### 10.3 硬件 Smoke

硬件 smoke 由 `tests/ccu/run_tilexr_ccu_direct_smoke.sh` 启动两个 rank 进程。runner 默认是安全的，必须设置：

```bash
export TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1
```

runner 会：

- 编译 `tests/ccu/ccu_tilexr_direct_smoke_probe.cpp`。
- 检查 `npu-smi info` 是否可完成。
- 默认拒绝 busy/unhealthy 设备。
- 分别启动 rank0/rank1。
- 为 prepare、install、submit、p2p result 做日志断言。
- 用 `timeout` 包住 rank 进程，避免测试卡死。

如确认设备健康但 `npu-smi` health 字段不是 OK，可显式放开 unhealthy 检查：

```bash
export TILEXR_CCU_SMOKE_ALLOW_UNHEALTHY_NPU=1
```

不要在没有明确授权时设置：

```bash
export TILEXR_CCU_SMOKE_ALLOW_BUSY_NPU=1
```

### 10.4 前序验证记录

在 950 验证服务器的前序复测中，当前分支曾通过以下检查：

- `cmake --build build_ccu_direct --target tile-comm -j4`
- 143 个 CCU 单元测试通过，1 个 skip。
- 两卡 P2P copy smoke 在 `remote_to_local` 和 `local_to_remote` 两个方向通过。
- `TILEXR_ENABLE_CCU_BACKEND=1` 且 `TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=0` 的自动初始化路径通过 P2P copy smoke。

注意：默认非 P2P smoke 曾暴露 repository install 旧参数问题，因此不能把默认非 P2P smoke 作为当前推荐验证路径。

## 11. 常见问题定位

### 11.1 runtime 初始化失败

看日志中的：

```text
TileXR direct CCU runtime init failed
logicDevId
devicePhyId
hdcType
raInitialized
ccuTlvInitialized
message
```

如果同一 device 后续被跳过，说明 device-scoped unavailable 状态已经记录。需要先解决首次失败原因，或换健康 device 重试。

### 11.2 prepare 卡住或 allgather 超时

重点检查：

- rank0/rank1 是否都启动。
- `TILEXR_COMM_ID`、端口和 rankSize 是否一致。
- 某个 rank 是否先因 runtime unavailable 退出。
- thread mode 下 uid 是否一致。
- `TILEXR_CCU_DIRECT_SMOKE_READY_TIMEOUT_MS` 是否过短。

### 11.3 resource window 注册失败

重点检查：

- 是否设置 `TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE=ra_ctx`。
- EID index 是否在 `RaGetDevEidInfoList` 返回列表内。
- 是否选择了错误 device 或 busy device。
- resource address 是否来自当前 device 的 CCU basic info。

可打开：

```bash
export TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE=1
```

查看 EID、TPN、doorbell token、QP import 等信息。

### 11.4 repository 或 lower-layer install 失败

重点看 prepare report：

- `installAttempted`
- `installSucceeded`
- `requiredInstallSurfaceCount`
- `publicVerifiedInstallSurfaceCount`
- `missingInstallSurfaceCount`
- `message`

如果缺失 surface，通常说明 lower-layer plan 没有拿到完整 peer route/token，或 resource range 与 manifest 不匹配。

### 11.5 submit 失败

submit 失败时 report 会打印 task 关键字段：

```text
missionId
key
instStartId
instCnt
argSize
args[]
rtRet
```

定位顺序：

1. 确认 prepare 阶段 `submitReady=1`。
2. 确认 repository mission window 覆盖 task 的 `instStartId/instCnt`。
3. 确认 mission id/key 与 install manifest 匹配。
4. 确认 stream 非空。
5. 打开 `TILEXR_CCU_DIRECT_TRACE=1` 查看最终 task 和 microcode 解码。

### 11.6 P2P copy 不匹配

重点区分方向：

- `remote_to_local`：active rank 校验本地 destination。
- `local_to_remote`：inactive rank 等待 active rank done 后校验本地 destination。

常见原因：

- active rank 设置错误。
- direction 与期望校验 rank 不一致。
- process token 查询失败。
- endpoint route 不完整导致 channel 指向错误 peer。
- submit 成功但 stream synchronize 失败。

## 12. 后续扩展建议

当前代码已经具备 direct runtime、resource allocator、lower-layer install、repository install、mission/task submit 和 P2P copy 数据面验证基础。后续要做泛化 alltoall/collective，建议按以下顺序推进：

1. 先把 collective request 到 producer plan 的映射补齐，而不是绕过 `TileXRCcuCollectivePlanner::Supports()`。
2. 复用现有 resource allocator 和 lower-layer evidence，不新增无证据的 env override。
3. 用多 task submit 的单元测试覆盖 launch package 批量下发。
4. 在两卡 P2P copy 稳定后，再扩展到 4 卡 N-to-N 数据流。
5. alltoall 数据正确性必须以 device buffer 内容校验为准，不能用 host marker 或单纯 submit 成功替代。
