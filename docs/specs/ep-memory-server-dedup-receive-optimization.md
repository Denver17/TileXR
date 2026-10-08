# Server Dedup 接收优化记录

## 2026-10-08：已确认的接收布局标记方案

接收布局标记独立于 `EpServerDedupTokenType`，只决定 CopyIn 是否保留 Flag。
每个 packet 仅第一个 32B Flag 块的第一个 float 携带标记：`1.0f` 表示压紧读取，
`1.5f` 表示完整读取；其他 Flag 字段为 `1.0f`，未就绪/清理后的字段为 `0.0f`。
不把标记解释为 Direct、FirstHop、Terminal，不从标记生成 TokenType 或 DestinationIndex。

Probe 保持原来的 Flag DMA 和一次 Sum。令 `N=blockCount*8`，和值恰为 N 时 ready 且
keepFlags=false，恰为 N+0.5 时 ready 且 keepFlags=true，其余结果均不 ready。
在每字段从 0 发布为有效值的原协议下，任意普通字段未到达时和值最多为 N-0.5，
标记字段未到达时和值最多为 N-1，因此不能误判完整到达。当前包长范围内整数及 0.5 均可被 FP32 精确表示。
返回 `readyMask` 和 `keepFlagsMask`，只使用 ready 项的布局位，不增加 header DMA、Probe 同步或 UB 分配。

两个位图绑定本批不可变的 probeIdx snapshot。预取和消费继续按同一倒序遍历，
各自向 IssueLoad/ConsumeLoaded 传入同一 keepFlags；pending 的末项替换删除不得改变关联。
keepFlags=true 使用完整 DataCopy；false 使用 DataCopyPad 每块取 480B、源 gap 32B，UB 连续。
消费按布局选择 wire 或 logical header/read-index 偏移及输出方式；去重元数据仍由 header 类型决定，
特别是 Terminal 的 DestinationIndex 仍为 `{slot, UINT32_MAX, UINT32_MAX}`。

Dispatch 是此技术的一个调用方：源端仅给需要转发的 packet 设置 1.5，每次复用发送 lane 均覆盖该字段，
避免上一包标记泄漏；标记更新放在已有 Vector→MTE3 发布依赖内。
FirstHop 中转在本地元数据快照后将 header 改为 Terminal，并独立把 UB 布局标记改为 1.0，
两次 Scalar 写均受已有 Scalar→MTE3 依赖保护，随后转发完整 packet。
下一跳由 Flag 标记选择压紧读取。去重选择、GM 布局尺寸、Flag 清理、lane 预算和核末完成屏障保持原契约。
该 Flag 语义要求全部参与 rank 同版本部署，不能混用旧内核；不改变 Combine 的协议。

实施顺序：独立布局标记定义与解码 → 源发送标记 → Probe 位图传递 → 布局感知消费和中转降为 1.0 → 回归与真实编译。
验证覆盖每个 Flag 字段缺失、两种布局与三种类型独立组合、同批混合布局、发送 lane 复用、
多轮 ping-pong、量化/非量化、auto 切换、转发 packet 和输出元数据。CPU 检查不代替 NPU 时序或性能结论。

本次实现验证（2026-10-08）：发送、接收 CPU 回归分别通过 `dedup=0/1/auto × profiling=0/1` 六组，
覆盖缺失 Flag 不提前 ready、混合布局的 snapshot/lane 对应、发送 lane 重用及中转标记复位。
B03 的 CANN 9.2 / A5 Dispatch 内核在 auto 策略、profiling OFF/ON 下均编译通过。
目标 CANN 9.1 的 `blue` 主机无法解析，未完成该版本编译；CPU 模拟与 A5 编译不能证明 NPU 运行时序或性能收益。
本次未执行 NPU 测试、安装或八机部署；构建日志及源码/产物 SHA256 保留于本机
`build/receive-layout-20261008` 和 B03 首机 `/home/hzy/TileXRTest/build/receive-layout-check`。

状态（2026-09-18）：**Dispatch 优化设计阶段收口，当前实现以[主规格](ep-memory-server-dedup.md)为准。**
本文保留优化过程和未实施候选，不再作为待执行计划。第 15 节的剩余 ≤5 双 Probe 设计暂缓，
不能用它覆盖当前 pending/退出/UB 契约。接下来的设计工作转入 [Combine](ep-memory-server-dedup-combine-open-topics.md)。

当前实现摘要：

- 默认编译期 32/32 分核，Count 阈值 512；旧章节的 48/16、阈值 48/128 属于当时构建。
- Probe 每批至多 16 包，一次二维 Sum，Scalar 拼 ready 位图；只有一个 Probe 批，消费完后再扫描。
- ready 不立即移出 pending；完成该 token 全部本地/转发下发后末项替换删除，tokenPos 保持稳定。
- MTE2/MTE3 共用按剩余 UB 计算的 1..8 槽 wire/assist/group 环，预取深度用满已分配 lane。
- 第一跳用 t=row，无 AtomicAdd、counter 清零和核末 SyncAll；核末 lane 回收、Flag 清理和本核 PIPE_ALL 保留。
- 打点为合并空扫描的 probe 与逐 token 的 recv_token；load/metadata/output/relay 子区间已停止生成。
  recv_token 包围 ConsumeLoaded，不含提前预取下发；Publish 不再执行显式 cache clean/invalidate。
- DIRECT 接收（2026-10-07）：非去重路径统一用 DataCopyPad 将 GM 的每块 480B payload 连续搬入 UB，
  源步长跳过 32B Flag；适用于所有受支持输入类型及量化模式，包括 auto 实际关闭去重时。
  压紧后 hidden、可选 scale 和三字段元数据必须使用逻辑 payload 偏移，不能再用 wire 偏移或带 Flag 的输出 helper。
  GM 协议、去重转发、lane 预算/事件、Probe 和 Flag 清理保持原契约；本项只影响 Dispatch。
  Host 模拟回归验证字节内容、跳过 Flag、输出边界和 lane 生命周期，实际性能仍须 A5 多 rank 测量。
  注意 DataCopyPad 的 GM→UB 源 stride 单位为字节（此处 32），不同于 DataCopy 的 32B 块单位。
  本次验证：dedup=0/1/auto × profiling OFF/ON 六组 CPU 接收回归通过，旧接收代码被新增跳过 Flag 检查拒绝；
  CANN 9.2 / A5 编译通过（dedup=0 OFF、auto OFF/ON）。blue 无法解析，未完成 CANN 9.1 编译或 NPU 运行验证。
- Vector ready bitmap、双 Probe、ready 后认领删除、metadata 地址缓存均未实现；不新增整包输入/输出双池。

以下第 1～14 节是历史方案、实验和交付证据，第 15 节是暂缓候选；其中“当前/下一步”均指章节日期，
不代表 2026-09-18 的默认行为。保留历史 CANN/Host 记录，但不据此宣称当前版本的设备性能已验收。

## 1. 现状和证据边界

代码入口：

- [接收调度、Probe、IssueLoad 和 ConsumeLoaded](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)。
- [Flag 读取、包尾解析、输出和转发 helper](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)。
- [tokenPos / pendingIdx 任务表](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)。
- [数据布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)与[核数、UB 预算](../../src/ep/common/ep_memory_server_dedup_count_layout.h)。
- [Server Dedup 主规格](ep-memory-server-dedup.md)与[验证指南](../EP_VALIDATION_GUIDE.md)。

`recv_tasks` 根据完整前缀和划分本核输出行，并构造稳定的 `tokenPos[j]` 和可压缩的 `pendingIdx[p]`。
当前源码已用二分查找定位本核起始前缀记录，不再把这项作为待实施优化。

设每包有 `B=blockCount` 个 512B 块，每块由 480B 数据和 32B Flag 构成，Flag 是 8 个 FP32 值；当前协议以全 1 表示到达。
优化前一次 probe 最多检查 `M=16` 个包，过程为：

1. 保存每个 pending 项对应的稳定任务 ID `j`。
2. 每包下发一次 `DataCopyParams{B, 1, 15, 0}`：从 `slot+480` 开始，每次读 32B、跳过 480B，读满 B 块；目的 UB 连续排列。这些参数的长度、间隔单位均为 32B。
3. 最多 16 次 Flag 拷贝下发后，以一次 `MTE2_V` 保证整批 Flag 对 Vector 可见。
4. 每包调用一次 `ReduceSum`，读取该包 `B*8` 个 Flag，结果写到 `sums[i*8]`；每次调用后有 `PIPE_V`，保护复用的归约 scratch。
5. `V_S` 后，Scalar 逐个 `GetValue(i*8)`，与 `float(B*8)` 比较并拼 ready 位图。

UB 已经是 `[count, B*8]` 的二维行布局，无需转置。16 个包的 GM 地址由任务表决定，通常不连续；本方案保留最多 16 次分包 Flag 拷贝，不假设一次 DataCopy 就能 gather 任意地址。

用户提供的 `fastertrace.json` 为 rank 6 的最后一次采集，接收核 48～63，共 576 个接收包、191 次 probe，记录完整且没有丢点。
全部 probe 中位数约 0.538us、最大约 2.401us；按当前扫描逻辑和后续接收事件反推，满 16 项批次的 55 个样本中位数约 2.203us，单项批次约 0.305us。
这里的批次大小是事件序列推断值，并非 trace 直接记录。约 2us 包含索引、拷贝下发、MTE2 等待、归约、同步和 Scalar 判定，不能全部归因于 ReduceSum。
该文件缺少完整 shape、实际二进制版本和运行时 `lanes_`，不据此断言当次确实使用了 8 个槽位或某个 DMA 并发度。

现有每包阶段为：

| 阶段 | 实际工作 |
| --- | --- |
| `acquire_load` | 在 MTE2 上等待槽位可复用，搬入完整带 Flag 包，立即 `MTE2_S` 等待后才继续 Scalar |
| `metadata` | 读取包尾；必要时 AtomicAdd 分配 group 序号；构造 assist/group，并修改转发包尾；发布 Scalar 到 MTE3 的依赖 |
| `output_issue` | 下发跳过 Flag 的 payload/scale 拷贝以及 assist/group 输出 |
| `relay_issue` | 必要时向多个目的 rank 下发完整包转发，最后发布槽位复用事件 |

同一包必须按数据依赖顺序处理；当前不足是下一包的 copy-in 在本包 metadata 和输出下发之后才开始。
已有异步 MTE3 输出可能与下一包 MTE2 重叠，不能说全部物理搬运完全串行；但最多 8 个槽位尚未形成主动的多包输入预取。

## 2. 增量 A：二维 Sum（已实现）和向量 ready 位图（后续）

### 2.1 按 Token 行归约

使用 CANN 公共高级 API `AscendC::Sum`，把每一行的全部 Flag 聚合成一个结果：

```text
flags[count][B*8]  --Sum(最后一维)-->  sums[count]
```

参数为 `outter=count`、`inner=B*8`、`n=B*8`。`inner*sizeof(float)=B*32` 已满足行对齐。
输出改为紧凑的 `sums[i]`，后续不能继续使用旧的 `sums[i*8]`。

必须传入显式的 `LocalTensor<uint8_t>` scratch。不使用自动申请临时空间的重载，因为参考实现会调用 `PopStackBuffer`，不符合控制隐式 local memory/栈资源的目标。
开发阶段使用已安装 CANN 9.1.0 的 `GetSumMaxMinTmpSize(n=B*8, typeSize=4, isReuseSource=false)`，核对 B=1..255；它不接收 outter，只返回单行容量，min=max 为 `align32(ceil(B/8)*4)` 字节。
实际 A5 `SumCompute` 的 scratch 行步长为 `align32(ceil(B/8))` **个 float**，与查询的字节对齐粒度不同。kernel 分配 `16 * align32(ceil(B/8)) * 4 + 256` 字节，覆盖整批所有行及最后一次整向量读取；B≤256 时为 2304B。不得只乘单行查询结果就认定安全。
当前调用 `Sum<float>(sums, flags, work, SumParams{count, B*8, B*8})` 后做一次 V_S，再读取 `sums[i]` 形成 ready 位图；不再下发逐包 ReduceSum 和 scratch 复用 PIPE_V。空任务直接返回，不调用 Sum。
该查询不是 device API，也不接入运行时 Host 调用路径。scratch 容量计算、分配和复用由 kernel 负责，不为此新增 Host 计算逻辑或由 Host 传入的 `sumTmpBytes`/tiling 容量字段；不能把旧单包 `B*32` 的 workBuf 当作批量 scratch 容量。

参考 A5 实现的 `SumForOneRepeatTime` / `ReduceSumNextN` 在 SIMD 函数内部仍按行、向量块进行加载和归约。
因此这是**一次标准二维归约 API 调用，不是用一条硬件指令归约任意大小的 16 个包**。
预期减少外层 API 调用和逐 Token scratch 复用屏障，实际指令数、寄存器和耗时由编译与实测确认。

### 2.2 用 Compares 直接产生 ready mask（尚未实现）

可以使用 `Compares(sums, float(B*8), EQ)`。目标是压缩位图，不是每 Token 一个字节：结果 bit i 对应 `sums[i]`，小端存储。
例如 16 项中第 0、3、15 项到达，得到 `0x8009`；Scalar 同步后读一次整数即可获得整批 ready 状态。

**不能直接调用 `Compares(..., uint32_t(count))` 且令 count 为 1～16。**
CANN 9.1 的 count 重载要求参与元素的总字节数按 256B 对齐，16 个 float 只有 64B。
本方案针对 A5 使用有效的「连续 mask + 单 repeat」重载，mask 为有效 float 元素个数；不将其外推到 mask 参数保留无效的其他芯片。

以下是接口与依赖示意，不是已经通过编译的补丁：

```cpp
// 前置：1 <= count <= 16；整批 Flag 的 MTE2_V 已完成。
// flags / sums / sumTmp / readyBits 为互不重叠的显式 UB。
AscendC::SumParams params;
params.outter = count;
params.inner = blocks * 8U;
params.n = blocks * 8U;
AscendC::Sum<float>(sums, flags, sumTmp, params);

AscendC::PipeBarrier<PIPE_V>();  // Sum 结果发布给后续比较。
AscendC::Compares<float, uint8_t>(
    readyBits, sums, float(blocks * 8U), AscendC::CMPMODE::EQ,
    uint64_t(count), uint8_t(1), AscendC::UnaryRepeatParams{1, 1, 8, 8});
ServerDedupReceiveSync<HardEvent::V_S>();

const uint32_t validBits = (uint32_t(1) << count) - 1U;
const uint32_t ready =
    readyBits.ReinterpretCast<uint32_t>().GetValue(0) & validBits;
```

实施约束：

- `count==0` 直接返回 0，不下发 Sum / Compares；本方案上限为 16，因此上述移位安全。
- 首版保留现有 512B sumBuf，足够覆盖单次 256B 向量读取；确认实际接口访问范围后才考虑减至 256B。
- readyBits 独立分配 32B、地址 32B 对齐；初始化时清零该块，并建立初始化到后续使用的 Vector 依赖。不能只分配逻辑上需要的 2B。
- 每次比较必须完整覆盖 count 个有效位；读取后始终与 validBits 相与，尾批不能使用上批残留的高位。
- Flag 区、归约结果、scratch、位图区不得重叠；V_S 必须位于比较之后，不能沿用归约后提前读取的顺序。
- 保持原来的全部 `B*8` 个 Flag 检查。只检查一个块、只比较首个 Flag 或仅统计 ready 数量，均不能替代现有协议。
- `popcount(ready)` 可以得到到达数量，但调度仍需 bit i 到 snapshot[i] 的映射，以处理稀疏就绪项。

Compares 在参考实现中执行向量加载、`Reg::CompareScalar`、压缩位图存储，仍需要 V_S 才能由 Scalar 读取。
它消除逐 Token Scalar 浮点读取和比较，不意味着结果直接免费出现在 Scalar 寄存器中，也不移除消费 ready 项所需的循环。

### 2.3 UB 容量和尾部访问

**Host / kernel 职责边界（已确认）：** Host 只按已确认的算子支持范围校验 shape、dtype、rank 等入参边界，超出边界的入参由 Host 拦截。Host 不负责按 shape 或最坏接收分布计算 UB 预算、scratch 大小、任务表容量或可用 lane 数，也不得因预计 UB 不足而拒绝边界内的合法 case。

支持边界内的 case 出现 UB 容量不足、分配错误或越界，属于必须在 kernel 验证中暴露并修复的实现 bug。修复应落实到 kernel 的容量计算、布局、分块或资源复用，不能通过新增 Host UB 校验或缩小入参边界来掩盖。后文的 UB 公式和检查均属于 kernel 实现与验证职责，不是 Host 准入条件。

按逻辑元素数计算容量不足以保证新 API 的物理访问安全：参考 Sum 实现含整向量 `Reg::LoadAlign`，最后一行不足一个向量的尾块也可能执行完整加载。
当前在 Flag 矩阵末尾额外预留 256B，每轮接收准备时将整个 Flag UB（含未使用行及保护区）清零，并以 V_MTE2 发布初始化完成。行步长仍为 `B*8`，有效归约范围不变。后续批次通过 V_S 完成归约后才复用 Flag/scratch，尾批只归约 count 行。
scratch 除了查询 API 容量，还要核对实际 A5 分支的最后一次向量访问，不能从旧 ReduceSum 的临时空间大小推断。

当前每核预算继续为 190KiB，所有项目按实际 TPipe 分配的对齐量计入；向量比较的 readyBits 留作后续：

| 项目 | 首版容量 |
| --- | --- |
| prefix | `align32(experts * 4)` |
| tokenPos + pendingIdx | `2 * align32(tasks.Count() * 4)` |
| Flag 矩阵及尾部保护 | `16 * B * 32 + 256` B |
| 紧凑 Sum 结果存储 | 保留 512B；只有前 count 个 float 有效 |
| Sum scratch | `16 * align32(ceil(B/8)) * 4 + 256` B；align32 此处按 float 元素数对齐 |
| readyBits | 当前不分配；后续 Compares 增加 32B |
| probe snapshot | `16 * 4 = 64` B |
| 每个数据 lane | `tokenStride + 32 + align32(4 + 8*(K-1))` B，依次为 wire、meta、group |

令前七项合计为 `fixedBytes`，每 lane 大小为 `laneBytes`；预取调度复用 snapshot，不另占描述符 UB：

```text
N = min(8, floor((190*1024 - fixedBytes) / laneBytes))
D = N
```

2026-09-17 用户明确：输入保证不超限，扣除固定开销后至少容纳一个 lane；kernel 直接按剩余 UB 计算 N，不新增容量校验、拒绝分支或兜底。N=1 是正常支持的配置。新增 Probe 存储仍须准确计入 fixedBytes，开发阶段核对布局与物理访问边界不等于新增运行时校验。
不增加按 Token 总数增长的队列，不使用大栈数组，也不为每 lane 复制 Sum scratch。GM 容量充足无法代替 Vector 必需的 UB 工作空间。

## 3. 增量 B：最多 8 槽的跨包输入预取

### 3.1 调度方式

将 `ReceiveOne` 拆成两个私有职责，复用现有包解析、输出和转发 helper：

- `IssueLoad(j, lane)`：申请 lane 的 MTE2 使用权，下发整包 GM→UB，发布加载完成事件；此处不立即等待 Scalar。
- `ConsumeLoaded(j, lane)`：临到解析时等待加载，处理 metadata，下发本地输出和全部转发，最后发布 lane 可复用事件。

每个 probe 批次仍按原 pending 位置从高到低选择 ready 项。先下发至多 D 个不同 lane 的加载，再按同一 FIFO 顺序消费。
消费一个包并下发完所有输出后，先补充下一包的预取，再消费已经预取的下一个包。
实现复用现有 16 项 snapshot 和 ready mask，以两个降序游标分别加载、消费；以 issued/consumed 序号关联 lane，不新增描述符数组或 UB 队列。没有 ready 项时继续原有扫描，不等待不存在的数据槽任务。

以 N=D=2 为例，Scalar 下发顺序为：

```text
IssueLoad(A,0) → IssueLoad(B,1)
ConsumeLoaded(A,0) → IssueLoad(C,0)
ConsumeLoaded(B,1) → IssueLoad(D,1)
ConsumeLoaded(C,0) → ...
```

实际 MTE2 可在 A 的 Scalar metadata 期间加载 B，MTE3 输出 A 可与后续独立槽位上的工作重叠。
C 的 MTE2 加载必须等 A 的所有 MTE3 读取结束；这种回压是正确的槽位保护，不应为追求重叠而删除。
预取深度等于物理槽数 N，最多可有 8 个已下发但尚未消费的包。消费并下发完一个包的全部输出后才补位；新加载仍通过该槽的 MTE3_MTE2 事件等待上一包完成写出。
当 N=1 时 D=1，自然退化为单槽顺序处理。

### 3.2 事件与缓冲区所有权

| 依赖 | 发布位置 | 消费位置 | 作用 |
| --- | --- | --- | --- |
| `MTE3_MTE2(lane)` | 初始化一次；每包全部本地输出和 relay 下发后 | 下一次 IssueLoad 该 lane；最后一次在 drain | 防止 MTE2 覆盖 MTE3 正在读取的 wire |
| `MTE2_S(lane)` | 整包加载之后 | ConsumeLoaded 解析之前 | 保证 Scalar 读取包尾时加载已完成 |
| `MTE2_MTE3(lane)` | 整包加载之后 | 该包首次输出之前 | 保证 MTE3 读取已加载的 wire |
| `S_MTE3(0)` | assist/group 和转发包尾构造完成后 | 紧邻发布立即 wait，随后下发该包输出 | 保证 MTE3 看到 Scalar 修改；没有跨包未消费事件 |

当前 `ServerDedupReceiveSync<MTE2_S>()` 用事件 0 立即 set/wait，在串行路径中可复用；拆成多包预取后，**不能把多次未消费的 MTE2_S 都 set 到事件 0**。
必须为同时未完成的加载使用不同事件，此方案按 lane 分配 MTE2_S。S_MTE3 仍是立即成对的 set/wait，沿用共享事件 0 即可。
同一个数值 ID 在不同 HardEvent 类型中不表示同一个事件；仍需核对目标 CANN/A5 的各类 ID 容量、预留项及本阶段其他 helper 的使用情况。
若同类事件有并存占用，必须调整映射或并发上限，不能直接假设 0～7 都空闲。初始化 prefix 的即时 MTE2_S 已成对结束后，才进入流水的事件生命周期。

每次 set 必须有且只有一次对应 wait，lane 再次发布同类事件之前，上次事件已经消费。MTE3_MTE2 的初始 credit 也只能消费一次。
预取补位只能复用已完成 ConsumeLoaded 下发职责的 lane，不能把等待尚未下发输出的 lane 放到 MTE2 队首，形成环形等待。

wire、meta、group 三段存储绑定同一个 lane。包尾只在加载完成后、输出开始前修改；输出开始后，直到该 lane 最后的 MTE3 读取完成，不得再改写这三段内容。
Scalar 改写 meta/group 前也必须确定前一占用者已经结束；由该 lane 的 `MTE3_MTE2 → 新加载 → MTE2_S` 依赖链保证，不能绕过这条链提前准备 metadata。

### 3.3 任务身份和完成语义

`j` 是稳定任务 ID，`row=tasks.Begin()+j` 是输出身份，`p` 是会变化的 pending 位置；二者不能混用。
`RemovePending(p)` 用末项覆盖 p，因此下发和消费均从同一 snapshot 读取稳定 j，以 `scan+index` 定位原 p，并保持批内原 p 严格降序下发、消费、删除。
高位置删除不会改变尚未消费的低位置身份；不要按 DMA 先完成者乱序删除，也不要预取时提前删除。

只有该包本地输出和所有 relay 均已下发，才删除 pending。此时表示本核已经接管其输出职责，**不表示 MTE3 已经完成，也不表示 lane 可立即写入**。
当前批次的 snapshot 在全部预取项完成消费前不得覆盖；首版不跨 probe 批次保留未消费的加载描述符。
保持 `ready==0` 时推进扫描、发生删除时重扫原位置的规则，确保末尾换入的任务不会漏掉。

末尾须消费所有已预取包，确认 issued 与 consumed 相等，再按每 lane 的事件完成现有 drain 和清理职责。
事件 wait 的下发结束不一定是 Scalar 意义的全部 DMA 完成；保留必要的最终完成屏障，不新增逐 Token `PIPE_ALL`、`SyncAll` 或全量输出 drain。
通信窗口地址、跨轮生命周期和 Flag 清理由集成时的主线协议决定；本方案不固定单窗口地址，也不重新引入旧的跨 rank 轮末握手。

### 3.4 并发度的含义

16 是每批监听包数；N≤8 是物理 UB 数据槽数；当前 D=N 是已下发但尚未消费的 payload 加载数上限（最初版本为 D=min(2,N)）。
三者独立，64 核也不表示单个接收核有 64 路 DMA。16 个接收核分别运行自己的局部流水。
MTE2 和 MTE3 是共享搬运流水，不应把 N 或 D 解释为同类指令可以同时满带宽执行的独立引擎数。
metadata 仍由该核 Scalar 顺序执行，AtomicAdd 竞争和多目的 relay 的 MTE3 压力仍可能成为限制。

用户反馈移除 AtomicAdd 的实验中 load_wait 仍明显暴露，因此将 D 从 min(2,N) 提升到 N。复用已分配槽位，不增加 UB、描述符队列或核角色。性能对比同时观察 load_wait 和整个接收阶段耗时；槽位回压或 MTE2 带宽限制仍需实机区分。

## 4. 历史候选：冻结相邻双批的 probe 重叠

本节保留早期 H/L 成对冻结方案作为取舍依据。2026-09-17 的目标改为第 15.4 节的低水位触发与 ready 认领；不实施本节的紧邻双批扫描，也不将其位置不变证明套用于任意滚动扫描。

前两阶段完成后，若 probe 仍占主要开销，再考虑两组 Flag、snapshot、sums、mask 及独立事件。
两组 scratch 只有在可能同时在用时才需要复制；复用必须证明前次 Vector 访问结束。全部资源重新计入 fixedBytes，避免减少 N 后反而退化。
probe 复用 MTE2，必须优先保证已就绪 payload 的预取，不能让投机 Flag 读取挤占关键数据搬运。

不能简单地「提前 probe 下一批、删除当前批、按旧 p 消费下一批」：swap-remove 可能已经改变下一批 p 对应的任务。
一种范围明确的安全方案是成对冻结两个相邻批次：

1. 从同一 pending 状态冻结低位置 L、高位置 H 两批，保存各自 j 和原 p；每批最多 16 项。
2. 先取得 H 的 ready，开始消费 H；在独立缓冲区中预先读取、计算 L 的 Flag。
3. H 内仍降序删除。H 的删除不会更改 L 的原位置，因此 L 的快照保持有效。
4. H 的所有预取项已完成输出下发、删除后，再按原 p 降序消费 L。
5. 两批中只要发生删除，就从原组起点重扫；都没有删除才越过两批。新换入的项必须重新 probe。

两批共享同一冻结边界，不能在其消费完成前任意滑动成第三批。Flag 缓冲区须等 Vector 完成读取才可复用，sums/mask/snapshot 须等对应消费者用完才可复用。
探测为未就绪后才到达的包留待下一次扫描，不把旧 mask 当作实时状态。
这一阶段改变扫描顺序，必须单独 review 和验证；不作为批内预取的前置条件，也不增加非阻塞完成轮询或无界队列。

## 5. 改动划分和 profiling 口径

| 增量 | 修改范围 | 预期效果 |
| --- | --- | --- |
| A：批量判定 | 接收 Probe、kernel 显式 UB/scratch 容量计算与分配；开发阶段核对标准 API 容量 | 已移除逐 Token ReduceSum 调用；Scalar 比较待第 2.2 节，Flag 拷贝方式不变 |
| B：最多 8 槽预取 | 接收调度与 ReceiveOne 拆分、每 lane 加载事件、复用现有 snapshot 的双游标 | 为输入加载与其他包 metadata/输出重叠提供调度条件 |
| C：可选 probe 重叠 | 两批快照调度、probe 缓冲和事件归属 | 隐藏部分 Flag 搬运/归约，代价是额外 UB 和扫描复杂度 |

首版不重写包解析、输出和转发算法，不调整任务分核，也不把 AtomicAdd 分配改成批量号段；后者需要独立论证 group 索引和输出布局契约。
当前接收元数据包含角色分支、包尾间接索引、group/relay 构造，不宜为「全部向量化」盲目增加转置、临时数组和寄存器。

profiling 保留每 core 的耗时区间，以及每 rank 独立 JSON、只保留最后一次执行的输出约定。
以下为第 14 节之前的历史打点布局；新采集已合并空扫描并移除两个子区间，旧 ID 仍可解析。
当时保留完整 `probe` 区间，按每个 probe 批次增加两个嵌套子区间：

- `Probe_load_flag`：索引快照、整批 Flag 拷贝下发，以及现有 MTE2_V 依赖指令下发。
- `Probe_cal_sum`：一次二维 Sum、V_S 等待，以及读取每个紧凑 sum、与预期值比较、形成 ready 位图的完整逻辑。它覆盖哪些/多少个包达到预期 sum 的判定，不额外增加 popcount 或记录 ready 数量。

这里记录的是 Scalar 观察区间。MTE2_V 约束 Vector 而非 Scalar，`Probe_load_flag` 结束不证明 Flag DMA 已完成；剩余加载等待可能在 V_S 处体现到 `Probe_cal_sum`。不为分摊物理流水耗时新增 MTE2_S 或其他同步。
B 阶段需要调整 `acquire_load` 的定义，不能拆开函数后仍把非连续工作拼成一个连续耗时段：

- `load_issue`：IssueLoad 的指令下发区间，包含槽位复用依赖指令的下发；不能等同于设备实际搬运时长。
- `load_wait`：ConsumeLoaded 等待 MTE2_S 的 Scalar 区间；反映预取未隐藏的剩余加载等待，也可能包含槽位回压。
- `metadata`、`output_issue`、`relay_issue`：继续表示现有的 Scalar 处理和下发区间；`atomic` 是 metadata 的嵌套子区间，不重复相加。

按每包/批次成对开始/结束记录，保持旧事件数字 ID，新增 LoadIssue/LoadWait/ProbeLoadFlag/ProbeCalSum 操作 ID 7/8/9/10；旧 `acquire_load` 映射保留，新 kernel 改为发出两个 load 区间。同步更新 trace 映射，不增加逐 Flag 记录或 device 端变长参数。`probe` 与其子区间为包含关系，不重复相加。
常规记录不强制加入 lane；确需分析时以有界诊断方式增加 lane 标识。Scalar 时间戳不能证明 DMA 引擎实际重叠或完成，不能据此画出虚构的 DMA 持续时间。

## 6. 后续实现的最小验证边界

下列为各增量的验证边界；本次实际执行范围和结果见第 8 节。每个增量独立构建和比较，避免把多个变化的收益或错误混在一起。

1. **真实编译**：按 [EP 开发约束](../../src/ep/AGENTS.md)，在配置的 blue 环境编译 CANN 9.1 A5 目标 `tilexr_ep_dispatch_memory_server_dedup_kernel`；不因 scratch 容量计算新增 Host/tiling 适配。若其他实际改动涉及 Host，则连同所属 Host 库编译。调整 profiling 时分别编译 OFF/ON，记录源码、编译器、配置和日志，不以 Host 模拟代替。
2. **判定正确性**：覆盖 count=1/2/15/16、全未就绪/全就绪/稀疏就绪、bit 0/7/8/15；一个包只有一个块或一个 Flag 未到达时不得 ready；连续复用缓冲及短尾批不得读入旧有效位。核对多种 B 下整向量尾部访问和 scratch 容量。
3. **流水正确性**：覆盖 N=1/2/3/8、包数超过 N、混合 ready 状态、零任务、多目的转发；验证稳定 j 与输出行映射、pending swap-remove、事件一对一消费，以及 wire/meta/group 在最后一次 MTE3 完成前不被覆盖。若实施 C，额外验证双批冻结与换入任务重扫。
4. **边界与已有检查复用**：Host 测试只依据已确认的入参边界检查接受/拒绝；边界内的 UB 容量与访问安全由 kernel 相关验证覆盖，发现越界即作为 kernel bug 修复，不通过修改 Host 拒绝规则使测试通过。优先运行相关包处理、任务表、接收和 profile 解析的有意义测试；缺失部分补行为测试，不用源文本正则匹配守卫实现。Host 结果不证明 NPU 搬运、事件或 DataAsFlag 协议正确。
5. **硬件 A/B**：同输入、shape、rank、拓扑、通信模式、CANN/编译器及 profiler/barrier 配置下比较；记录二进制身份。保留框架要求的 NPU profiler 和阶段 barrier，kernel 诊断只在相同诊断条件下做 A/B；正式性能基线关闭非必要 kernel 诊断。
6. **结果口径**：至少报告整个 recvData、probe 和剩余 load_wait，而不只比较一个变短的区间；同时检查 Dispatch 总耗时和正确性。性能报告包含每阶段 backend/native 状态、kernel 版本、数据量、可读延迟及适用的带宽，跨 rank 报 median/min/max。多节点日志和 trace 继续由各 rank 所在服务器整理，不集中保留所有轮次。

## 7. API 依据与可复用经验

本次核对 CANN 9.1 的 `Sum`、`Compares` 文档与 A5 实现：公共头分别为 `adv_api/reduce/sum.h` 和 `basic_api/kernel_operator_vec_cmpsel_intf.h`。
版本化上游为 [asc-devkit 9.1.0](https://gitcode.com/cann/asc-devkit/tree/9.1.0)，对比快照位于本工作区忽略目录 `reference/asc-devkit-inspect9_1/`：
`Sum.md`、`Compares.md`、`sum.h`、`sum_sum_3510_impl.h`、`kernel_operator_vec_cmpsel_intf_impl.h`、`dav_3510_kernel_operator_vec_cmp_impl.h`。
reference 仅作审查证据，生产构建不得 include/link 其中源码。接口重载、scratch 查询和支持范围以实际编译的 CANN 9.1 头文件再次确认。

本方案需要保留的经验是：小批次向量比较不能把逻辑 count 与 API 的 256B 计算对齐混为一谈；批量归约不能复用未经重新计算的单行 scratch；延迟等待后，原本安全的共享即时事件会变成多个未完成操作的事件冲突；任务从 pending 删除只表示输出已下发，不能替代 UB 的 DMA 完成所有权。
这些结论来自当前调用路径和参考 API 审查，性能收益及实际资源用量仍受真实编译与 A5 验证约束。

## 8. B 增量实现与交付记录

本次执行顺序与范围：

1. 接收 header 将加载发布与 Scalar 消费拆开，在原 16 项冻结 snapshot 内建立 `D=min(2,N)` 的 FIFO 预取；批次内全部消费后才覆盖 snapshot，删除仍在输出/relay 全部下发后。只增加固定个数 Scalar 调度变量，原 UB 分配不变，Host 无改动。
2. 在公共事件目录末尾追加四个操作 ID，mapping 保留旧数据兼容；Probe_load_flag / Probe_cal_sum 嵌套于 probe，load_issue / load_wait 独立配对，不跨预取生命周期拼接一个区间。
3. 接收替身检查 N=1/2/3/8、24 项跨批与 swap-remove、先预取后消费、FIFO 身份、在途 packet/meta/group 保护、事件回收；profile OFF/ON 共用检查。JSON 解析检查新阶段的正时长/嵌套及旧 acquire_load 兼容。完成后简化并独立 review 同步和所有权。
4. 按 `src/ep/AGENTS.md` 尝试 blue 的真实 CANN 9.1 A5 OFF/ON 编译；仅在连接及工具链可用时执行目标编译，不启动 NPU。

维护要点：延迟加载等待时，MTE2_S 必须在对应 DataCopy 后、后续加载之前发布，消费端仅 wait；若把整段 set/wait 一起移动到消费端，会把后续加载也纳入当前等待，失去预取效果。补位必须发生在前一个消费者已经下发所有输出之后，避免等待尚未下发的 lane 归还事件。预取深度与物理 lane 数分开，不能提前复用正在输出的 UB。

验证结果：本地 receive_packet（OFF）、receive_profile（ON）、profile 三个目标编译及 CTest 3/3 通过；两项新旧 JSON 区间解析检查通过；完整 Dispatch 和接收模板的 GM 地址空间 Clang 检查（OFF/ON）通过。主审及独立 review 未发现本次 diff 引入的可达功能缺陷。blue 编译前 SSH 探测返回 255：`banner exchange: Connection to UNKNOWN port -1: Connection refused`，未进入真实 CANN 9.1 A5 OFF/ON 编译。源码基线、SHA-256、连接日志及本地 CTest 日志保存在忽略目录 `build/server-dedup-prefetch-verification/`。未运行 NPU，不能据此宣称真实 DMA 重叠、设备事件或性能收益已验证。

## 9. Metadata Scalar 重复工作压缩

用户回传 `0a99364` 执行成功且有一定优化效果；未提供新 trace 或完整二进制/shape 身份，保留为用户运行反馈，不外推量化收益。本次在该版本上继续减少 metadata 的重复读取和计算，不实施 A/C，不改变预取、同步、AtomicAdd 分配和打点边界。

实现范围为接收调度 header 和 packet helper，依次完成：

1. 包尾对象先把原点定位到包头所在的 payload 块，保留块内偏移，`Header()` 不再重复除余。`Entry(i)` 每次访问只换算一次地址，多个字段复用同一个 16B 视图，避免逐 word 重做除法和偏移。`WireOffset` 使用等价的 `logical + floor(logical/480)*32`。视图引用原 UB，不复制尾表。
2. 消费端已读取的 role 传给 assist helper，循环前读取 entry 数量，group 的 `relayIndex*topK` 移到循环外。terminal 才读取 `tasks.Position(j)`、计算本核专家 ID；DIRECT/first-hop 不再承担这部分 UB 读取和除余计算。
3. terminal 先选中 entry，再生成 assist 的 k/weight；first-hop 直接写最终 relay 引用，不先复制随后覆盖。assist 的八个字段各写一次。group 与 relay 仍分阶段访问 entry，不缓存整张表或增加 lane 元数据。
4. relay 下发前读取一次 source rank 和 rank 数，循环内每个 entry 复用地址读取 expert/q。新变量限制在使用范围内，不新增持久缓存字段、栈数组、UB 分配、Host 校验或额外同步。

边界与维护要点：tailOffset 由数据布局保证 32B 对齐，480 同时是 32 和 16 的倍数，因此包头和单 entry 均不会跨 Flag；完整尾表可能跨一个或多个 Flag，**不能把整个 tail 当作物理连续数组**。必须逐 entry 跳过 Flag。复用 role 时保留 first-hop 修改前的正值，转发包头改为 -1 后不能反过来覆盖本地 assist 的原始角色。

验证范围：复用 receive_packet/profile OFF/ON 的真实 helper/接收替身，检查 DIRECT/first-hop/terminal、精确 weight bits、首末 entry、跨 Flag 长尾、量化/非量化、两窗地址和在途 lane 所有权；补充 H209/K32 长尾场景。做 GM 地址空间检查、blue CANN 9.1 A5 编译尝试及限定 diff review。测试不证明 CANN 优化后的 Scalar 指令数、栈/寄存器占用或实际性能；这些仍需真实编译与同配置实测。

本次本地 receive_packet（OFF）、receive_profile（ON）目标编译与 CTest 2/2 通过；最终 GM 地址空间 Clang 检查 OFF/ON 通过。主审和独立 review 未发现本次修改引入的可达功能缺陷。blue 编译前探测返回 255：`banner exchange: Connection to UNKNOWN port -1: Connection refused`，未进入真实 CANN 9.1 A5 编译。源码 SHA-256、基线、连接与本地检查记录保存在忽略目录 `build/server-dedup-metadata-verification/`。未运行 NPU。

## 10. 预取使用全部可用槽位

本次基于 `2138024`，将接收调度的深度上限从 `min(2, lanes_)` 改为 `lanes_`。当前常用 H7168 配置已经分配 8 个槽，现可先下发 8 个 ready 包，再按 FIFO 消费、逐包补位；实际可用槽少于 8 时按实际数量运行。复用原 UB、16 项 snapshot、pending 身份及事件 ID，不添加栈数组或 Host UB 校验。

提高深度后必须继续遵守 `D<=N`：补位只发生在对应旧包完成 ConsumeLoaded 的全部下发之后，MTE3_MTE2 再保护真实写出完成，不能为了提前加载而删除该事件。测试须检查预取实际达到 N、每 lane 的加载/消费身份一致、在途 wire/meta/group 不被覆盖，以及跨批删除和末尾事件回收；仅确认存在 8 份 UB 分配无法证明预取已加深。

Probe 与消费仍按批切换，metadata 仍由 Scalar 逐包处理。load_wait 可以同时包含输入搬运剩余时间和上一包 MTE3 占槽造成的回压；本次提高提前量不等于已有性能提升证据，需保持相同 shape、量化和打点配置对比接收总耗时。

本次 receive_packet（profiling OFF）和 receive_profile（ON）本地目标编译及 CTest 2/2 通过，独立 review 无确定性缺陷。blue 编译前连接返回 255：`banner exchange: Connection to UNKNOWN port -1: Connection refused`，未进入真实 CANN 9.1/A5 编译。源码摘要、编译/测试及连接日志保存在忽略目录 `build/server-dedup-eight-prefetch-verification/`；Host 替身不证明设备异步时序或性能收益，未运行 NPU。

## 11. 固定 64 核下调整发送与接收配比

配置集中在 `src/ep/common/ep_memory_server_dedup_count_layout.h`。修改头文件中的两个独立默认值后，重新编译、安装 Host 和 kernel，并向所有 rank 同步同一版本：

- `TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM`：默认 48，只允许 32、40、48；对应 `kEpServerDedupSendCoreNum`，recv 自动取 `64-send`。不调整 block_dim，仍为 64。
- `TILEXR_EP_SERVER_DEDUP_ALL_CORE_COUNT_BS_THRESHOLD`：默认 48，对应 `kEpServerDedupAllCoreCountBsThreshold`。`BS < 阈值` 时全部 64 核参与 Count，否则仅 recv 核参与；取 0 可关闭全核 Count。此阈值不随 send 核数变化。

这些是共享头文件的编译期配置，不是 run.sh 或 CMake cache 参数。宏覆盖用于定向编译验证时，所有相关翻译单元也必须保持一致；只改 Host 或只改 kernel 会使 Count 布局不一致。trace 按实际 send_data_end/recv_data_end 事件判断角色，可以读取三种配比及历史 48/16 数据。

Count/cumsum 保留已有按 recv 核数计算的 GM 布局：两张 `recv×recv×32B` 矩阵，每个 recv 核一份完整 expert prefix，之后按 1KiB 对齐放独立 counter。最大 1024 专家时，各配置占用如下（含前导预留和 counter）：

| send / recv | 单张矩阵 | 全部 prefix | 完整 Count 控制区 |
| --- | --- | --- | --- |
| 48 / 16 | 8 KiB | 64 KiB | 126 KiB |
| 40 / 24 | 18 KiB | 96 KiB | 178 KiB |
| 32 / 32 | 32 KiB | 128 KiB | 238 KiB |

三种配置均在每窗原有 2 MiB 控制区内。recv 是 8 的倍数，`recv×32B` UB 缓冲恰好覆盖 Duplicate 的整次 256B repeat，无需额外尾部预留。没有新增 Host UB 容量拦截。

维护要点：增加 recv 核数后，16 专家等合法小输入会产生没有 Count 记录的接收核。这些核不能执行零长度分配、搬运或归约，也不能退出整个 cumsum 协议；必须发布值为零的贡献、参与 prefix 完成通知并获取完整 prefix，否则其他核可能永久等待。本次将贡献发布抽为共同路径，对空分段跳过记录访问与输出拷贝。

本次基于 `006c529`，使用本地 CPU API 替身分别编译 48/16、40/24、32/32，最后一组将 BS 阈值独立设为 64；layout、counts、source_send/profile、receive_packet/profile 每组 6 项，共 18 项通过。覆盖空 Count 核、阈值两侧的实际 Count IO、全部 prefix 与矩阵清理、两窗保护、收发分区和 8 槽流水；3 项 trace 检查通过。编译配置、日志及源码摘要保留于 `build/server-dedup-core-split-verification/`。blue 连接报 `banner exchange: Connection to UNKNOWN port -1: Connection refused`，未进入真实 CANN 9.1 A5 Host/kernel OFF/ON 编译；未运行 NPU，替身结果不证明设备异步时序或性能收益。

## 12. Token 级 prefix 替代 AtomicAdd（2026-09-16）

用户已确认以第一跳的零基输出 row 作为 forwardTokenIdx，允许稀疏 group 表。本增量执行
[主设计最新编号契约](ep-memory-server-dedup.md#2026-09-16dispatch-使用-token-级-prefix-编号当前契约)，
替代前文保留 AtomicAdd 的范围约束；不改变 probe、预取、lane 或 cnt/cumsum 算法。
删除原子取号和 counter 清零；保留本核完成及全核 SyncAll。Atomic/CounterEnd 事件 ID 不复用，
新 trace 不生成这些样本，历史 trace 仍可读取。新增 prefix_indices_validated 验收字段并更新
构建清单的编号契约，避免旧任意 t 的 consumer 被当作新协议验证。Combine 实现及内存优化另行推进。

## 13. Probe 二维归约验证（2026-09-16）

本次仅实施第 2.1 节的批量 Sum 和对应 UB 布局。CANN 9.1.0 安装头 `adv_api/reduce/sum.h` 与 A5 `sum_3510_impl.h` 为实现依据；[官方 Sum 接口说明（9.1 beta3）](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910beta3/API/ascendcopapi/atlasascendc_api_07_0826.html) 仅作接口参考，最终重载和物理访问以实际 9.1.0 编译包为准。

维护要点：二维归约同时改变输出步长和整批临时空间。保留旧 `sums[i*8]` 会读到错行；只按单行查询结果分配会漏算 A5 partial 行步长及 masked load 的完整 256B 访问。当前显式区分 Flag、512B sums、uint8 scratch，并将保护空间计入 lane 预算。切换 CANN 版本时须重新核对 Sum 实现和 scratch 查询，不能把本次公式视为跨版本 API 保证。

本地基于 `d49a56b` 加本次补丁，receive_packet/profile、counts、source_send/profile 共 5 项 CPU 替身回归通过。接收用例覆盖 1..16 项尾批、B=1/8/9/31 的行边界、量化和非量化、一个 Flag 缺失、整批未就绪后重扫、1/2/3/8 lanes，以及空任务和跨轮清理；替身检查完整向量源/临时区访问范围。隔离副本分别恢复旧输出步长、单行 scratch 或移除 Flag guard，三项负向检查均失败，正常实现通过。

blue 上真实 CANN 9.1.0、Bisheng/Clang 15.0.5（2026-05-18，clang-5c68a1cb1231）编译 `tilexr_ep_dispatch_memory_server_dedup_kernel`，目标 `dav-c310-vec` / `CATLASS_ARCH=3510`，profiling OFF/ON 均编译链接成功，退出码均为 0。源码基线为 `d49a56b` 加本次补丁；源码摘要、编译器版本、配置、完整编译日志及产物摘要保留在 `build/probe-reduce-verification/`，blue 工作目录为 `~/hzy/probe-reduce-20260916.BOu4Fn/`。

blue 仅作编译，未运行 NPU；本地替身不模拟异步 Vector/MTE 流水，不能证明设备正确性或加速幅度。A5 对比应使用相同 shape、收发核划分、量化模式及 profiling 配置，同时观察完整 probe、接收阶段和 kernel 耗时；Probe_cal_sum 仅在第 14 节之前的历史分段 trace 中存在；不能把一次 API 调用等同于一条硬件指令。

## 14. 合并 Probe 空扫描打点（2026-09-16）

基线 `fda2d5c` 已实现每批最多 16 个 Token 的一次二维 `Sum` 调用，每个 Token
仍有独立归约结果和 Scalar ready 判断，不是把所有 Token 合成一个 ready 标量，
也不等于一条硬件 ReduceSum 指令。本次保持归约、Flag 同步、pending 删除及预取协议不变。

原实现每次扫描（包括 ready=0）写入 Probe、Probe_load_flag、Probe_cal_sum 三对事件。
标准 writer 每条事件读取时钟，并向 GM 写入 8B 时间戳和 4B ID；发布时刷新已写区间。
因此 P 次扫描产生 6P 条 Probe 相关记录，长时间空轮询会增加 Scalar/GM 工作并可能耗尽
每核 65536 条的记录容量。用户观察的 breakdown 330us / performance 45us 尚未在本次复测，
不能仅凭源码将全部差值归因于这些事件；逐包阶段打点和发布也仍有开销。

现在在开始扫描前记录一次 Probe begin；ready=0 时继续推进 scan，扫描到表尾回绕，
不写任何中间事件。直到某批 ready!=0 才记录 Probe end，然后执行原有加载、消费和删除。
下一轮消费结束后重新开始 Probe。所有空扫描、同步等待和最终成功扫描都计入 Probe；
不把计时起点移到发现 token 之后，也不把消费时间并入 Probe。空任务不产生 Probe。
Probe 相关记录变为 2S 条，S 为成功扫描批数，S 不超过本核任务数，与空轮询次数无关。

新采集不再生成 Probe_load_flag / Probe_cal_sum。保留其枚举、事件 ID 和映射，
历史 trace 仍可解析，不复用 ID。新旧 trace 的 probe 样本粒度不同，不能直接比较单样本均值；
应比较每核 Probe 总时间及接收/kernel 总耗时，并保持 shape、收发核划分和二进制来源一致。

维护经验：轮询热点中按尝试打点会改变被测等待本身；优先用成功边界包围整个等待，
同时验证空任务、跨批扫描、回绕和时间区间完整性。减少记录不等于隐藏等待时间或证明性能提升。

本次验证基于 `d64fc25` 加本补丁：blue 的 receive_packet（OFF）和 receive_profile（ON）
两个 CPU 替身用例通过，覆盖 128 次空扫描后到达仍只生成一对 Probe、首末扫描时间被包围、
跨批 pending 压缩、量化/非量化和 1/2/3/8 槽预取；本地 18 项 trace 测试通过，
包含新合并区间和历史细分事件的解析。替身时钟只验证边界顺序，不模拟设备耗时。
blue 使用真实 CANN 9.1.0、Bisheng/Clang 15.0.5（2026-05-18，clang-5c68a1cb1231），
分别编译 `tilexr_ep_dispatch_memory_server_dedup_kernel` 的 profiling OFF/ON，
目标 `dav-c310-vec` / `CATLASS_ARCH=3510`，编译和链接退出码均为 0。
源码摘要、配置、编译器及日志保留于 `build/probe-profile-verification/`；
blue 隔离目录为 `/home/d00520898/tilexr-probe-profile-20260916.wpCBu2/`。
未运行 NPU，未验证异步设备时序或 330us/45us case 的优化幅度。

## 15. 非 atomic 接收流水的三阶段候选（2026-09-17；2026-09-18 暂缓）

### 15.1 基线与本次澄清

基于 `codex/SilentEmotion` 的 `47563bd`，仅修改设计，没有修改 kernel。用户进一步明确：lane 数由剩余 UB 动态计算，输入保证至少一个 lane，不需要 kernel 内新增容量检查。

当前 [接收端](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h) 已实现一批最多 16 项的一次二维 Sum、非 atomic 的 `t=row`、最多 N 包预取，以及 **MTE2 和 MTE3 共用同一组 N 个物理槽的环形协议**。当前主要批间串接是“取得 ready → 完成本批全部下发 → 再 Probe”，不能将其描述为尚无多槽搬运流水。

上一版提出两套独立输入/输出池、中间增加 Vector UB→UB 复制。这是另一种架构，会增加完整包副本和 Vector 工作；用户描述的 MTE2/MTE3 轮转并不需要它。本版撤下该默认方案，也撤下固定 I/O 配比、强制至少两个 lane 及大包分块/兼容路径的待定项。阶段二保留现有环，聚焦 Scalar metadata 和已到达包的下发组织；若没有新的瓶颈证据，不为这一阶段强行增加数据搬运。

| 槽位协议 | 发送端输出包环 | 当前接收端 payload 环 |
| --- | --- | --- |
| 生产者 | Vector 封包写入 | MTE2 搬入完整 wire |
| 消费者 | MTE3 发送 | MTE3 本地输出及全部 relay |
| 数据可读 | `V_MTE3` | `MTE2_MTE3`，另有 Scalar metadata 的 `S_MTE3` |
| 下轮可覆盖 | `MTE3_V` | `MTE3_MTE2` |
| 槽号 | `packets % N` | 加载 `issued % N`，消费 `consumed % N`，相同 FIFO 顺序 |

参考 [SendPacket](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)。两者在环形槽所有权上确实可以对应为“Vector 换成 MTE2”；接收端还必须保留 Scalar 读取包尾、生成 assist/group、修改转发 header 的依赖，不能只替换事件名称就省略这部分工作。

### 15.2 阶段一：Probe reduction/ready 判定与非 atomic metadata

**Probe：保留已落地的二维 Sum，压缩 Scalar ready 判定。** 沿用第 2.1 节完整 Flag 归约和显式 scratch；按第 2.2 节候选方案增加 32B readyBits，使用向量比较、一次 Scalar 位图读取替换最多 16 次 Scalar sum 比较。保留 Sum→比较依赖及有效位屏蔽。本阶段仍同步返回 ready，不同时引入双 Probe；向量比较是否更快需要实机 A/B。

**Metadata：先减少重复 header 读取、地址换算和分支，不新增地址数组。**

- 保留 `t=row=tasks.Begin()+j` 和稀疏 group 表。复用本包的原始 role、entry 数、source/relay 引用等少量 Scalar 值；DIRECT 不承担 terminal 的专家查找，terminal 才计算本地 expert、搜索 route。
- group 的 `t*K` 循环外计算；每次访问一个 entry 时复用其视图。tail 整体可能跨 Flag，仍逐 entry 做逻辑→wire 地址换算。保持原始正 role 与转发 header 的 -1 区别，以及 weight 的精确位模式。
- 当前已有上述部分优化，需按现有 helper 核对尚余重复工作，不能把旧版已完成事项再次当作新收益。

**上一版“metadata 地址缓存”的具体含义与取舍：**

当前 wire 的 tail 已保存 route 信息：每个 entry 是 16B，含 expert、q、原始 k 和 weight。group helper 遍历 entry 读取 k/weight；后续 relay 循环再次取得 entry，读取 expert/q，计算目标 rank 和完整 GM 目的地址，随即下发 DataCopy。地址是临时 Scalar 计算结果，**没有独立的目的地址数组；每个 relay 的最终 GM 地址本来就只计算一次**。

上一版建议把 relay 地址计算提前到 group 遍历中，保存最多 `K-1` 个 64 位 GM 目的地址，稍后下发时读取缓存。它主要减少第二次 entry 地址定位/循环，不是消除原本不存在的多次 GM 地址计算。额外 UB 用来保存这些派生地址，而不是 token payload 或已有 assist/group；仅地址本身占 `8*(K-1)` 字节，再按实际分配对齐。上一版的 `D=align32(32+8*(K-1))` 还包含未定稿的 32B 控制描述符，K=6 时 D=96B/槽。

本版将其降为可选实验，默认不增加 D、不建立 per-lane 地址表，继续在 relay 下发时从原 tail 计算目的地址。只有实测表明二次遍历确为瓶颈且缓存收益超过 UB 读写/减少 lane 的代价，再单独讨论。少量 Scalar 公共值复用不等于新增持久 UB 数组；编译后的寄存器/溢出情况仍需核对。

### 15.3 阶段二：沿用动态 N 槽的 MTE2/MTE3 流水

**当前就是用户描述的同一物理环。** 每个槽含 wire、32B assist staging 和 G 字节 group staging，槽大小 `L=T+32+G`，T 为 tokenStride。加载和消费分别按 `0,1,...,N-1,0,...` 轮转：

```text
槽 i 的一轮：
等上轮 MTE3 释放 → MTE2 搬入 wire → Scalar 解析/修改 metadata
              → MTE3 本地输出 + 全部 relay → 发布槽 i 释放事件
```

`IssueLoad` 在同一 lane 的 `MTE3_MTE2` 上等待，再提交输入 DataCopy，并发布 `MTE2_MTE3` 和 `MTE2_S`。`ConsumeLoaded` 先等 `MTE2_S`，完成 metadata 后建立 `S_MTE3`；MTE3 等 `MTE2_MTE3`，依次读取 wire/meta/group 完成本地输出与 relay。所有下发结束后提交 `SetFlag<MTE3_MTE2>(lane)`，由 MTE3 按序执行到这里才释放槽，不能把 Scalar 已提交 SetFlag 理解为搬运已经结束。

初始化为每槽提供一次可写 credit；绕回槽 0 时只等待槽 0 的上轮 MTE3，不需要全环 drain。同一槽尚未读完时不覆盖，不同槽允许 MTE2 加载后续包与 MTE3 输出前包重叠。多目的 relay 必须全部被释放事件覆盖，不能在本地输出完成后提前释放。

| 依赖 | 作用 |
| --- | --- |
| `MTE3_MTE2(lane)` | 保护 wire/meta/group 整槽生命周期，MTE2 再写前消费 |
| `MTE2_S(lane)` | Scalar 读 tail 前确认本次输入完成；也使新 metadata 写入晚于上轮槽释放 |
| `MTE2_MTE3(lane)` | MTE3 读取本次 wire 前确认输入完成 |
| `S_MTE3` | MTE3 读取 Scalar 修改的 header/assist/group 前建立可见性 |

当前预取深度已经等于 N。跨 Probe 批次的 issued/consumed 序号连续，不在每批从槽 0 重新初始化。issued-consumed 计数表示已提交加载但尚未完成消费下发的包数；实际 MTE3 是否完成由事件保护。

本阶段保留这些语义，结合阶段一压缩 metadata；仅在 trace/编译证据支持时调整加载补位与消费的 Scalar 调度。不能把正常槽复用等待当作应删除的同步，也不能先给尚未提交输出的槽排入 MTE2 等待，制造依赖环。N=1 正常顺序运行，N>1 利用多个槽；不强制固定槽数，不复制完整 payload 到第二套池。

### 15.4 阶段三：剩余 ≤5 时启动下一次 Probe

**两个 bank 交替，不做两次紧邻预扫。** 每个 Probe bank 独立持有 flags、sums、scratch、readyBits、snapshot 和完成事件。首版连 scratch 也分开，先使所有权明确；共享 scratch 的省空间方案以后再证明。两个 bank 仍共享该核的 MTE2/Vector，不能解释为两个探测引擎。

拆成两个逻辑接口：

- `StartProbe(bank, scan, count)`：保存稳定 j 快照，提交 Flag 的 MTE2 读取、MTE2→Vector 依赖、Sum、ready 比较和 `V_S` 发布；**不在这里等待 Scalar 结果**。
- `FinishProbe(bank)`：在需要该批时才消费 `V_S`，读有效 readyBits，并将 ready 项认领为待处理任务。输入/输出搬运期间不做“检查 Probe 是否完成”的 Scalar 忙轮询。

`R` 定义为**当前 ready 批次中，本地输出和全部 relay 尚未完成指令下发的 Token 数**。已经预取的 Token 仍计入 R；R 不是 Probe 检查的 count、全核 pending 数或 MTE3 未完成数。一个 first-hop 的最后一个 relay 提交后才令 R 减一。

触发条件为 `0<R<=5 && nextBank 空闲 && pending 非空`。每个当前批次至多提前启动一次；R 从 6 降到 5 的边界立即检查，位于下一次 payload 补位之前。初始 R 为 1～5 时，在该批 payload 预取前检查一次；R 为 0 时没有搬运可掩盖，直接进入扫描/获取下一批的路径。

```text
时间向右，示意 Scalar 调度及可异步执行的区间（非实测比例）：

P0: Start → Finish/认领 16 项
搬运 P0: [下发前 11 项] | [继续下发最后 5 项] | R=0
P1:                     Start -------------- Finish/认领
搬运 P1:                                         [处理到 R<=5] ...
P0:                                                           Start ...
```

启动 P1 不等待 P0 的 MTE3 drain；P0 的 payload 槽可以继续由 MTE3 写出。P1 的 Flag DMA/归约是否能在最后五包期间执行，取决于已经排队的 MTE2、Vector 工作以及输出回压。调度只能先于**后续**补位插入 Probe，不能越过此前已提交的等待指令；尤其 MTE2 若已阻塞在某槽的 `MTE3_MTE2`，P1 的 Flag 读取也会排在其后。阈值 5 是首版策略，不是保证完全掩盖的时间预算。

**pending / ready 所有权调整——推荐方案，实施前需确认。**

现有 `RemovePending(p)` 要求所有输出已经下发，且依靠同一快照内降序删除。任意提前扫描下一批后继续按旧规则删除，会使其 p 快照失效；第 4 节的固定 H/L 双批方案只能在冻结范围内解决此问题。

建议把 pending 改成“尚未被 ready 队列认领”的集合：

1. `FinishProbe` 先取得 ready mask，将 ready 的稳定 j 保留在该 bank 的 snapshot/ready 队列；随后按原 p 降序从 pending 移除这些 ready 项。未 ready 项仍在 pending。
2. 认领后只用稳定 j 做 payload 加载、`row=Begin()+j`、metadata 和输出。处理当前批时不再改 pending，也不再按旧 p 删除。
3. 在 R≤5 处从剩余 pending 启动下一 Probe。当前批的 j 已不在 pending，不会重复探测/加载；下一 Probe 从 Start 到 Finish 期间 pending 保持不变，位置快照有效。
4. 当前批 R=0 后才 Finish 下一 Probe；新 ready 再按降序认领。发生认领后重扫原 scan 位置，检查 swap-remove 换入的任务；ready=0 才前进 count，尾部回绕。

例如 pending 原为 `[A,B,C,D,E,F]`，Probe 发现 B、D 到达。先在当前 bank 中保存这两个任务的稳定 ID，再按位置降序删除 D、B：

```text
pending: [A,B,C,D,E,F] → [A,B,C,F,E] → [A,E,C,F]
当前 ready: D、B（沿用原位置降序处理）
```

此时 D、B 只是从“以后需要继续扫描的列表”转到“已经确认到达、等待搬运的列表”，其 GM 数据、tokenPos 和输出 row 均未删除/改变。下一 Probe 只扫描 `[A,E,C,F]`；处理 D、B 不再修改这张表，因此下一 Probe 的位置快照稳定。“移出 pending”绝不表示已经搬完，也不释放对应 payload lane。ready 集合可以直接用 bank 的 snapshot 加 mask/游标表示，不必另建队列数组。

此方案只需两份最多 16 项的有界快照/位图，不需要按任务数增加 inverse-position 表或在每次删除时修复另一批快照。`RemovePending` 的接口注释和测试必须同步改成认领语义，宜使用明确的 `ClaimReady` 职责名，不能只在原消费循环外提前调用旧函数。

关键不变量为：`任务总数 = pending数 + 已认领但未完成下发数 + 已完成全部下发数`，每个稳定 j 恰好属于一类；in-flight MTE3 是最后一类任务的物理状态，不能重复计入。`tokenPos` 的清理所有权仍覆盖最初 `Count()` 的所有槽，不能按缩短后的 pending 清理。

**边界和收尾。**

| 场景 | 行为 |
| --- | --- |
| 当前批 ready 为 16/6 | 分别完成 11/1 个 Token 的全部下发后触发一次下一 Probe |
| 当前批 ready 为 5/1 | 认领后即满足条件，可提前启动一次；可掩盖的工作自然较少 |
| 下一次 Probe 返回 0 | 消费完当前批后获取该空结果，推进 scan；此时没有 payload 可处理，恢复扫描直到出现 ready，不忙扫同一位置 |
| pending 已空但 R>0 | 继续完成当前批，不启动零长度 Probe，不提前退出 |
| R=0 且下一 Probe 已在途 | Finish 一次，不能重新 Start 或覆盖其 bank |
| 最后一批全 ready | 即便认领令 pending=0，也必须消费全部 ready、完成payload 环 drain，随后按原协议清理 Flag |
| 未就绪项在扫描之后到达 | 保留 pending 并在后续扫描覆盖，禁止以旧的 not-ready 结果永久跳过 |

总循环只有在 pending 空、没有已认领未下发 Token、没有未 Finish 的 Probe 时才退出调度；再完成payload 槽事件回收与最终同步。bank 只有在其 Vector 完成、mask 已读取且该批 j/ready 游标用完后才能重新使用。

### 15.5 动态 lane 数与资源计账

用户已明确输入保证至少容纳一个 lane；**沿用这一前提，不添加 kernel 内容量校验、Host 拒绝或兜底路径**。N 由实际剩余 UB 计算、当前最多 8 个，N=1 是正常配置。开发时准确计入新增 Probe 存储与对齐量，不把剩余空间误算成仍有旧版数量的槽。

令 `a32(x)` 表示按 32 对齐：

```text
F = a32(E*4) + 2*a32(tasks.Count()*4)
P = (16*B*32+256) + 512 + (16*a32(ceil(B/8))*4+256) + 64 + 32
L = T + 32 + G
fixedBytes = F + banks*P + S
N = min(8, floor((190*1024 - fixedBytes)/L))
```

P 包括一份 Flag 矩阵、Sum 结果、scratch、snapshot 和新增候选 readyBits；scratch 的 a32 按 float 元素数对齐，其余按字节。当前源码尚无 readyBits，因此当前单 Probe 使用 `P-32`；阶段一/二如使用 Compares 则 banks=1、每 bank 使用完整 P，阶段三 banks=2。S 为其他实际新增固定存储；默认没有 metadata 地址缓存 D，也没有独立 I/O 双池。ready 队列复用 bank 快照。

以 P64/E384/BS96/H7168/K6、48/16 核、本核 36 个任务为算例（非生产运行时 lane 采样），F=1856B、G=64B：

| 包类型 | B / T | 当前 fixedBytes | 当前 N | 双 Probe fixedBytes（S=0） | 双 Probe N |
| --- | --- | --- | --- | --- | --- |
| MXFP8 | 16 / 8192B | 13184B | 8 | 24576B | 8 |
| 非量化 BF16 | 31 / 15872B | 20864B | 8 | 39936B | 8 |

这说明原文 4 输入+8 输出超预算的问题来自新增的双池方案，不能用来说明当前共享环缺少容量。实际 N 仍取决于每核任务表与具体 shape，不能把算例的 8 当作所有输入的常数。

事件按类型和生命周期分配。payload 环继续使用各 lane 的 `MTE2_MTE3`、`MTE2_S`、`MTE3_MTE2`；两个 Probe 使用各自 `MTE2_V`、`V_S`，与 payload 事件类型不同。不能沿用上一版“双池输入与 Probe 共占 MTE2_V，因而 I+2 受限”的推导。初始化/清理的临时事件仍需与在途事件避让，同一类型的同一 ID 再次 Set 前先消费上轮 Wait；具体映射在目标 CANN/A5 编译实现中核对。

### 15.6 实施范围与验证顺序

| 阶段 | 修改范围 | 前置条件与产出 | 必须取得的证据 |
| --- | --- | --- | --- |
| 一 | recv 的 Probe；receive_packet 的 metadata helper；相关替身 | 维持当前同步 Probe/共享 lane；产出 ready mask 与非 atomic metadata 契约 | 稀疏 ready、count=1..16、跨 Flag 长 tail、K=1/32、精确 weight bits、两种量化格式与非量化；目标 CANN 编译及 A/B |
| 二 | recv 的 payload 环调度；packet helper 的 Scalar 工作 | 保留当前动态 N 与事件协议；阶段一结果作为基线，无收益证据不强行改动 | N=1/2/3/8、延迟 MTE3、多目的 relay、同槽覆盖保护、预取深度 N、跨批环绕及不足 N 的尾批 |
| 三 | recv 的 Start/FinishProbe；receive_tasks 的认领语义；profile/trace reader 如需新事件 | 在共享 payload 环上采用双 bank 与 R≤5；产出扫描/认领/终止状态机 | 16/6/5/1/0 ready、P1 全空/部分/全 ready、swap-remove 换入项、pending 提前为空、反复回绕及每个 j 恰好处理一次 |

将阶段三的认领/调度覆盖加入现有行为替身测试，不用源代码文本匹配验证。负向用例应能发现复用旧 p、重复认领、pending 空即退出、槽覆盖早于 tail 最后读取、槽回收早于最后一个 relay 等故障；CPU 替身只能证明逻辑/受模拟的依赖，不能代替真实引擎时序。

后续实施时按 [验证指南](../EP_VALIDATION_GUIDE.md) 初始化现有 Host 测试工程，再聚焦运行：

```bash
cmake -S tests/ep -B build/server-dedup-host
cmake --build build/server-dedup-host --target test_tilexr_ep_server_dedup_receive_packet test_tilexr_ep_server_dedup_receive_profile -j8
ctest --test-dir build/server-dedup-host --output-on-failure -R '^test_tilexr_ep_server_dedup_receive_(packet|profile)$'
```

真实 CANN 9.1/A5 分别编译 profiling OFF/ON，保留源码/二进制来源和编译日志。设备验证从单轮正确性到多轮复用，覆盖用户已暴露问题的 `BS96/H7168/K6/E384/P64/quant_mode=4/fp8_format=e4m3` 及非量化对照；用同一输入、核划分、Flag 协议、打点配置分别比较基线、一、二、三阶段。本文尚未执行这些实现验收。

性能验证同时关注 recv 总耗时、完整 Dispatch、剩余 load_wait、payload 槽回压、Probe 的暴露等待以及实际引擎重叠。模式 1 沿用最后 `min(50,N)` 个有效测量轮次：每 server 对其全部卡×保留轮次先平均，再报 server 代表值的 min/max/mean；N 不包含 warmup。breakdown 用于解释流水，不能将其带打点耗时与无同等打点的 performance 数字直接相减归因。

当前 Probe 等待打点已经按第 14 节合并：从首次扫描前 Begin，到发现非空 ready 后 End，所有空重试都位于同一区间，不产生逐次记录；当前 payload 消费在这个区间之后。这里不再安排一次“合并 Probe”等价改造。

双 Probe 后，Start→Finish 是包含 payload 处理的生命周期，不能继续称作独占 Probe 成本并与 metadata/output/relay 相加。建议分别展示 **Probe 在途区间** 与 **当前 ready 耗尽后获取下一 ready 的暴露等待**；前者允许重叠，后者合并空扫描直到成功。必要时给 trace 增加新 ID，保留历史映射，不改变旧 ID 的含义。不恢复每次空扫描多对事件；最终指标及界面名称在第 15.7 节确认。

维护要点：双缓冲改造同时改变资源生命周期和任务所有权。稳定 j 解决身份问题，但不会自动保证可变 p 快照有效；提前认领必须连同退出条件一起修改。共享环的 MTE3→MTE2 槽复用依赖本来就是正确的保护；增加 Probe 并行时应保持它，不能把提前认领与提前释放物理槽混为一谈。验证必须同时覆盖逻辑队列、字节预算、事件资源和实机异步时序。

### 15.7 已明确项与剩余讨论

已明确：基于非 atomic 的 t=row；沿用 MTE2/MTE3 共用 payload 环；N 按剩余 UB 动态计算、当前最多 8，输入保证至少 1，不新增内部容量检查；当前 Probe 空扫描打点已经合并。上一版的固定 I/O 配比、强制双池、强制新增地址缓存均不作为默认方案。

| 剩余讨论 | 建议 | 需要确认的含义 |
| --- | --- | --- |
| pending 是否改为未认领集合 | 检测 ready 后保存稳定 j，并降序移出 pending | 只是转入当前 ready 集合，不是宣告搬运完成；调整 RemovePending 契约与退出条件，例子见第 15.4 节 |
| metadata 地址表是否值得实验 | 默认不加，只压缩少量 Scalar 公共值和重复解码 | 原 tail 已保存 route，relay GM 地址本来只计算一次；若测得重复遍历有明显成本，再讨论 UB 地址表 |
| 双 Probe 后 trace 如何命名 | 生命周期与暴露等待分别显示，保留已合并空扫描 | 当前合并无需重做；新生命周期会和 payload 重叠，不能继续按串行区间相加，需确定 trace reader 的展示 |

阶段三首版每批只提前启动一次 Probe；短批（≤5）立即满足条件，空扫描恢复顺序轮询。后续若实测需要按时间、relay 数或包大小调节阈值，作为独立调优，不在本轮替换剩余 Token 计数。
