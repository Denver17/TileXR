# Server Dedup 接收优化方案

状态：**B 预取已扩至全部可用槽位（最多 8，见第 10 节）；Probe 分段打点及第 9 节 metadata Scalar 压缩已实现；A/C 尚未实现**。原深度 2 实现基于 `7ec8ab1`，metadata 压缩基于 `0a99364`。
固定考虑 A5 / Ascend 950、CANN 9.1、64 核，沿用 48 个发送核和 16 个接收核。第 1 节保留优化前的证据，第 2/4 节为后续方案。

当前已实施「最多 8 槽的 payload 预取」、Probe 分段打点及 metadata 重复读取/计算压缩；保留逐包 ReduceSum 与 Scalar ready 比较，批量 Sum + Compares 和 probe 双缓冲留作后续优化。
目标是减少逐 Token 的归约调用、Scalar 判定和立即等待，让不同包的 MTE2、Scalar 和 MTE3 工作重叠，同时保持现有包格式、输出身份和完成条件。

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
一次 probe 最多检查 `M=16` 个包，过程为：

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

## 2. 后续增量 A：二维 Sum 和向量 ready 位图（未实现）

### 2.1 按 Token 行归约

使用 CANN 公共高级 API `AscendC::Sum`，把每一行的全部 Flag 聚合成一个结果：

```text
flags[count][B*8]  --Sum(最后一维)-->  sums[count]
```

参数为 `outter=count`、`inner=B*8`、`n=B*8`。`inner*sizeof(float)=B*32` 已满足行对齐。
输出改为紧凑的 `sums[i]`，后续不能继续使用旧的 `sums[i*8]`。

必须传入显式的 `LocalTensor<uint8_t>` scratch。不使用自动申请临时空间的重载，因为参考实现会调用 `PopStackBuffer`，不符合控制隐式 local memory/栈资源的目标。
开发和编译验证阶段使用已安装 CANN 9.1 的 `GetSumMaxMinTmpSize`，核对支持边界内的 B 及 `count=1..16`，结合实际 A5 实现的物理访问范围，确定 kernel 内显式 scratch 容量规则。
该查询不是 device API，也不接入运行时 Host 调用路径。scratch 容量计算、分配和复用由 kernel 负责，不为此新增 Host 计算逻辑或由 Host 传入的 `sumTmpBytes`/tiling 容量字段；不能把当前单包 `B*32` 的 workBuf 当作批量 scratch 容量。

参考 A5 实现的 `SumForOneRepeatTime` / `ReduceSumNextN` 在 SIMD 函数内部仍按行、向量块进行加载和归约。
因此这是**一次标准二维归约 API 调用，不是用一条硬件指令归约任意大小的 16 个包**。
预期减少外层 API 调用和逐 Token scratch 复用屏障，实际指令数、寄存器和耗时由编译与实测确认。

### 2.2 用 Compares 直接产生 ready mask

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
首版在 Flag 矩阵末尾额外预留并初始化 256B 保护空间，保持行步长仍为 `B*8`，不改变有效归约范围；确认实际 CANN 实现的加载范围后可收紧该余量。
scratch 除了查询 API 容量，还要核对实际 A5 分支的最后一次向量访问，不能从旧 ReduceSum 的临时空间大小推断。

后续 A 增量的每核预算继续为 190KiB，所有项目按实际 TPipe 分配的对齐量计入；下表的 Sum/位图/尾部保护空间尚未引入本次 B：

| 项目 | 首版容量 |
| --- | --- |
| prefix | `align32(experts * 4)` |
| tokenPos + pendingIdx | `2 * align32(tasks.Count() * 4)` |
| Flag 矩阵及尾部保护 | `16 * B * 32 + 256` B |
| 紧凑 Sum 结果存储 | 保留 512B；只有前 count 个 float 有效 |
| Sum scratch | kernel 按开发阶段核对的容量规则计算，覆盖 count=1..16，按 32B 对齐并包含物理访问余量 |
| readyBits | 32B |
| probe snapshot | `16 * 4 = 64` B |
| 每个数据 lane | `tokenStride + 32 + align32(4 + 8*(K-1))` B，依次为 wire、meta、group |

令前七项合计为 `fixedBytes`，每 lane 大小为 `laneBytes`；预取调度复用 snapshot，不另占描述符 UB：

```text
N = min(8, floor((190*1024 - fixedBytes) / laneBytes))
D = N
```

kernel 的资源计算须保证 `fixedBytes < 190*1024` 且至少容纳一个 lane，再做无符号减法和除法，不能依赖无符号下溢后的结果。
若新预算使边界内的 case 无一路可用，必须在 kernel 验证中判为实现失败并修复；不能以 `N=0` 继续执行、静默跳过任务，或交给 Host 拦截该 case。
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

## 4. 第三阶段（可选）：probe 与包处理重叠

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
| A：批量判定 | 接收 Probe、kernel 显式 UB/scratch 容量计算与分配；开发阶段核对标准 API 容量 | 移除逐 Token ReduceSum 调用及 Scalar 比较；Flag 拷贝方式不变 |
| B：最多 8 槽预取 | 接收调度与 ReceiveOne 拆分、每 lane 加载事件、复用现有 snapshot 的双游标 | 为输入加载与其他包 metadata/输出重叠提供调度条件 |
| C：可选 probe 重叠 | 两批快照调度、probe 缓冲和事件归属 | 隐藏部分 Flag 搬运/归约，代价是额外 UB 和扫描复杂度 |

首版不重写包解析、输出和转发算法，不调整任务分核，也不把 AtomicAdd 分配改成批量号段；后者需要独立论证 group 索引和输出布局契约。
当前接收元数据包含角色分支、包尾间接索引、group/relay 构造，不宜为「全部向量化」盲目增加转置、临时数组和寄存器。

profiling 保留每 core 的耗时区间，以及每 rank 独立 JSON、只保留最后一次执行的输出约定。
保留完整 `probe` 区间，本次按每个 probe 批次增加两个嵌套子区间：

- `Probe_load_flag`：索引快照、整批 Flag 拷贝下发，以及现有 MTE2_V 依赖指令下发。
- `Probe_cal_sum`：逐包 ReduceSum、共享 scratch 的 PIPE_V、V_S 等待，以及读取每个 sum、与预期值比较、形成 ready 位图的完整逻辑。它覆盖哪些/多少个包达到预期 sum 的判定，不额外增加 popcount 或记录 ready 数量。

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
