# TileXR ElasticMemory 设计

[English](elastic-memory.en.md) · [验证记录](validation/elastic-memory.md) · [Torch adapter 使用说明](../integrations/elastic_memory_torch/README.md)

## 目录

1. [这篇文档解决什么问题](#1-这篇文档解决什么问题)
2. [适用场景与前提约束](#2-适用场景与前提约束)
3. [组件与所有权划分](#3-组件与所有权划分)
4. [单 owner 分段环形分配](#4-单-owner-分段环形分配)
5. [共享 ABI 与 mailbox 协议](#5-共享-abi-与-mailbox-协议)
6. [Dispatch、扩容与 FIFO 准入](#6-dispatch扩容与-fifo-准入)
7. [Host 接口与异步语义](#7-host-接口与异步语义)
8. [Tensor、stream 与容量归还](#8-tensorstream-与容量归还)
9. [统计口径](#9-统计口径)
10. [通信集成与失败处理](#10-通信集成与失败处理)
11. [性能目标与验证要求](#11-性能目标与验证要求)
12. [当前支持边界](#12-当前支持边界)
13. [图源、代码与维护入口](#13-图源代码与维护入口)

## 1. 这篇文档解决什么问题

MoE Dispatch 这类算子的输出行数由路由结果决定，只有 Device 完成计数之后才知道真实大小。
常见做法是按最坏情况预分配一块最大输出缓冲，代价是常驻占用大量 HBM；另一种做法是先跑一遍
Host 侧 Planner 预测 shape，代价是额外一次同步和一条必须与 kernel 保持一致的旁路实现。

ElasticMemory 提供第三条路径：**让 Device 在算子内部按实际大小申请内存**。它把 HBM 组织成若干
独立 segment 构成的单 owner 分段环形池，Device 算出实际字节数后直接取得一段连续区间；只有当池内
确实没有合适的连续空间时，发起分配的 kernel 才原地驻留，等待 Host 申请一块真实 HBM 并回复地址，
然后从原分配点继续执行。最终输出以普通 NPU Tensor 的形式交付，内存何时复用、何时真正还给
Runtime，由 Storage 引用和 stream 完成情况共同决定。

本文规定架构、所有权和失败时的行为要求，是**设计规定**而非执行记录：

- 第 2–10 节描述必须成立的行为；第 11 节给出性能目标与验证要求；第 12 节划定当前已实现并验证的范围。
- 具体环境、实测数值、源码与 binary 的对应关系、复现命令统一放在[验证记录](validation/elastic-memory.md)，
  不要把本文的目标条目当作已验证能力。
- 本功能不改变现有 EP V1/V2、MoonEP 或上游 DeepEP 的对外接口，也不要求引入独立 Planner。

## 2. 适用场景与前提约束

[![图 1：ElasticMemory 用户场景](diagrams/elastic-memory/use-cases.drawio.png)](diagrams/elastic-memory/use-cases.drawio.png)

图 1 展示四个环节的关系：Device 分配、Host 扩容、Torch 侧消费、整段归还。其中 `materialize`
只等待稳定的元数据，在消费 stream 上排入 producer 完成依赖后即可返回，不等待数据写完。

**适用的工作负载。** 面向算子级 output 的一次性动态申请。生命周期相同的多个输出可以合并成一个
bundle 共用同一次 reservation；一个算子内不应反复申请释放，也不面向大量微小对象或任意碎片管理。
“通常较大”只是典型形态，不是最小尺寸限制——零尺寸、对齐边界和极端请求仍须正确处理。

**单 owner。** 每个 pool 在任一时刻只有一个 Device allocator owner，申请与释放相关的 Device
元数据只由该 core 修改。不要求跨 kernel 固定使用同一个物理 core，但不支持多核并发 malloc。

**串行准入。** 同一个 pool 的调用按 FIFO 串行准入。各 rank 必须以相同顺序提交通信调用；需要并发
流水线时使用彼此独立的 pool，不能依赖各 Host 线程的到达顺序来决定 collective 顺序。

**真实大小的定义。** Device 必须在 Dispatch 内部完成计数和布局之后再申请实际字节数。`actual_rows`
要包含 route 复制、专家 padding 和必要对齐；尚未收齐的计数不能当作最终 shape。

**地址稳定且连续。** 每个 Tensor 的有效 storage 范围连续，一个 bundle 不跨 segment、也不在环尾
拼接。活跃地址一旦交付就不再变动，不搬迁 Tensor，也不把不连续的 segment 伪装成连续存储。

**初始不占数据 HBM。** 创建 pool 只建立容量有上限的控制元数据，数据 HBM 初始为零。正常命中路径不申请 Host
HBM，也不需要等 Host 把结果包装成 Tensor 才推进数据面。

**失败即终止。** 容量不足时当前 kernel 驻留等待一次 Host 扩容响应；失败则终止整次调用及相关通信域，
不重试、不重启、不重放，也不回滚已经写出的数据。

## 3. 组件与所有权划分

| 组件 | 职责 | 所有权与访问 |
|---|---|---|
| HBM segment | 保存连续的 output 区间 | Host Runtime 创建和释放；Device 只访问数据 |
| PoolState snapshot | 本轮请求头、当前段和定长段目录 | Host 在调用间准备，运行中的 kernel 只读 |
| RingState commit | 所选段的分配与回收游标 | Device owner 提交；Host 在元数据发布后校验并保存镜像 |
| Host reservation 队列 | 分配顺序、padding、lease/event 和可回收前缀 | Host 持有，不进入 Device 热路径扫描 |
| Host-visible mailbox | 扩容请求/响应、commit、不可变输出描述符、最终状态 | 每个槽只由一方写入，按本轮 sequence 配对 |
| Native service 线程 | FIFO 准入、扩容、完成检查和释放 | 使用 pool 创建时捕获的 ACL context，不依赖 Python 回调 |
| Allocation lease | 维持生产者、通信和 Storage 的使用权 | native Runtime 与 Torch adapter 共享 |
| Kernel 执行状态 | 已完成的计数、布局和已有的内存副作用 | 等待扩容期间保持在原 kernel 内 |

两侧的边界很清楚：**大数据留在 HBM，映射到 Host 的内存只承载小型控制信息**。Host 根据
pool/device 归属、segment ID、offset、bytes 和 generation 校验 handle，再由已知的 segment 基址
还原设备地址；Host 从不解引用 Device 指针。Host 指针与其 Device 映射地址分别保存，不要求二者数值相同。

## 4. 单 owner 分段环形分配

[![图 2：单 owner 分段环形池分配与回收流程](diagrams/elastic-memory/segmented-ring-allocation.drawio.png)](diagrams/elastic-memory/segmented-ring-allocation.drawio.png)

图 2 把三条路径分开画：正常命中、容量不足的慢路径、调用之间的释放。图中标注的耗时是异步改动
之前的实测基线，不能据此推断异步收益；适用边界见第 11 节。

### 4.1 连续区间与绕回

每个 segment 维护一对逻辑游标：分配尾 `tail` 和回收头 `head`/`reclaim`。二者之差是尚不能跨过的
保留窗口。为了避免在热路径上做取模，实现额外用 `tailOffset` 直接保存物理尾位置。

分配时，owner 先应用 Host 已发布的回收水位，再按段的实际 base 地址对齐候选区间。若段尾余量不足以
容纳完整 bundle，就把这段尾部余量整体记为本次 reservation 的 padding，然后尝试从段首绕回；只有当
payload、对齐 padding 和绕回 padding 的总和不超过可用容量时才提交游标。失败的尝试不改变任何已提交
状态，地址与长度运算全程检查溢出。

正常命中只触碰紧凑的当前段，没有空闲槽搜索，也没有任意空洞整理。当前段不足时才去查定长的段目录；
只有所有段都没有合适的连续区间，才发出扩容请求。需要强调的是，**逻辑空闲字节足够并不等于存在
可立即分配的连续区间**。

两个边界情况单独处理：环为空时可以在安全边界把游标规范化回零，但不得改变活跃地址，也不得复用仍
可能被观察到的 allocation generation；零尺寸输出不占用 reservation，也不强行创建数据 segment。

### 4.2 连续可回收前缀

环形结构决定了空间只能按顺序回收。Host 依据 Storage 引用以及生产者、通信、已登记 consumer stream
的完成情况，整理出每段中已可安全回收的**连续前缀**，并在下一次调用之前发布新的回收水位；Device owner
读取并校验后应用它。Host 不会并发改写正在运行的 owner 的游标。

以同段内先后分配的 `A → B → C` 为例：只要 A 仍有 view 或在途消费者，即使 B/C 已经释放，回收游标也
不能越过 A。B/C 的空间被队头阻塞，计入逻辑空闲但不可立即分配；后续请求可以改用其他段或触发扩容，
但仍受预算约束。reservation 同时记录自己的 padding 和结束游标，连续前缀回收时一并计入。

单 core 约束只限制 Device 侧的游标修改者。生命周期检查和真正的 `aclrtFree` 始终在 Host：**Device
生产完成不代表 Torch 侧的 output 已经可以释放。**

## 5. 共享 ABI 与 mailbox 协议

### 5.1 数据结构

当前 ABI 版本为 3（`kAbiVersion = 3`），核心结构都按 64B 对齐：

- `RingState`（64B）= `Segment{id, address, bytes}` + `head/tail/reclaim/tailOffset/alignment`。
- `PoolState` = 64B 头部（version、requestId、segmentCount、reservationCount）+ 最多
  `kMaxSegments = 32` 个 `RingState`，**当前段固定排在首位**。
- `Mailbox`（384B）依次为 `GrowRequest`、`GrowReply`、`Output`、64B 标识头
  （version/requestId/poolId）、`committed` 和 `finalStatus`。

非零请求序号同时用作该次非空 allocation 的 generation，`alignment` 供 Host 在元数据发布后重算校验。

### 5.2 一次调用的数据流

Host 在 launch 之前，把上一次 Device commit 的镜像与安全回收水位组装成本轮只读 `PoolState`，整体
复制到 HBM。Device 热路径用**一次 128B 的 MTE2 读取**同时取得 64B 头部和 64B 当前段 RingState；
其余段只在容量不足时才逐个读取。Host 不预先决定本次的 output bytes 或分配 offset，而是在收到
Device commit 之后重算并校验结果——`ValidateCommittedLocked` 用同一份 `TryAllocateRing` 算法在
Host 镜像上重放一遍，逐字节比对 `committed`，并检查地址、段边界以及与现有 lease 的区间重叠。

分配成功且非空时，Device 先通过 MTE3 把所选段的 64B 状态写入独立的 `committed` 槽并等待写回完成，
然后有序发布 64B `Output`：**先写 sequence 为零的完整 payload，再带本轮 sequence 重写一遍完整描述符**。
分配失败则清空 handle/address 并发布错误；入口参数非法直接返回错误交由调用者处理。

**Output 一旦发布便不可改写。** 这是元数据提前返回的前提：Host 可以在 kernel 仍在写 payload 时就把地址和
shape 包装成 Tensor。此后出现的 peer 错误或数据校验错误，写入独立的 64B `finalStatus` 槽
（mailbox offset 320），不覆盖已交付的地址或 shape。Host 在 producer 完整完成之后检查最终状态：
sequence 为零表示没有另报状态；非零则必须匹配本次请求，匹配且 error 非零则整次调用失败。

### 5.3 可见性

请求、响应、commit、Output 和 finalStatus 各自只有一个写入方，并通过 ABI version、请求 sequence 和
allocation generation 拒绝重复或过期消息。Host 在包装之前还会检查 pool/device 归属、segment 边界
以及 shape 所需字节数。

发布顺序统一为“先写完整描述符，再以平台认可的顺序发布 sequence”，读取方观察到发布标志后才取得
一致快照。需要明确的是，**C++ `volatile` 或 CPU 原子操作本身不能证明跨设备的 release/acquire**。
当前 Device 侧的扩容等待组合使用 volatile GM reload 与 DCCI 失效，并校验本轮 sequence；请求与结果
统一经 UB 到 GM 的有序写回发布。字段宽度、缓存处理、槽复用和双向运行中可见性，必须以适用的
CANN/硬件证据逐项验证。

### 5.4 容量上限

| 上限 | 值 | 说明 |
|---|---|---|
| segment 数 | 32 | `kMaxSegments` |
| 在途 reservation | 128 | `kMaxAllocations`，含已释放但仍被活跃前缀阻塞的记录 |
| 每个 lease 的登记 stream | 128 | 超出显式失败 |
| 每个 lease 的登记 event | 128 | 与 producer wait-boundary event 共享同一份上限预算 |

控制容量耗尽必须明确失败，不能只统计 live lease 后无界增长。ABI v3 的任何变更都要求 native 库、
外置 kernel 和 Torch extension **一起重建**。

## 6. Dispatch、扩容与 FIFO 准入

### 6.1 Device 在算子内算出真实大小

Dispatch 在同一次 kernel 内完成最终接收计数、计算 hidden 与辅助输出的实际布局，再由指定 owner
调用 `AllocateBundle`。生命周期相同的输出共享同一个 bundle 和 lease，按各自 dtype/通信要求对齐；
Combine handle 保存路由及逆映射，用户无需预先执行 Planner。计数完成之前已经收到的 payload，必须
保存在容量有上限、遵守 credit 的暂存区。

当前 Memory 实现由**最终计数核**担任 owner（`aivId == aivNum - 1`）：它本身已经持有最终 prefix，
因此无需等待其他发送核即可分配。owner 分配完成后，把 `{error, address, rows, magic}` 作为本轮不可变
描述符写入自己窗口内的 compact gate；其余输出核在复制前自旋等待本地 gate 的 magic，再读取完整 body。
prefix 装载、expert-map 构建和计数输出都不依赖分配结果，可以与 owner 的分配重叠执行。

跨核发布必须使用目标平台已验证的可见性协议：先观察发布标志、再取不可变 body，不能假设一次 64B DMA
就是原子的 flag+body 快照，也不能假设任意 block 都同时驻留而用普通全局自旋屏障同步。

owner 随后轮询各 peer 的 gate 聚合错误，这一步可以与其他核的本地复制重叠。因此 Host 可以在元数据
稳定后就导出 Tensor，但 **producer event 只保证访问顺序，不表示最终成功**：晚到的 peer 错误会使该
Tensor 及基于失败数据的下游结果无效，调用者必须通过 `synchronize`、Combine 或 discard 检查最终状态。

### 6.2 原 kernel 驻留等待真实 HBM 扩容

1. 当前段及其他候选段均不足时，Device 发布一次扩容请求，内容为 sequence、bytes 和 alignment。
2. 原 kernel 留在分配点等待匹配的响应，保留此前的执行状态和副作用，不写尚未分配的地址。
3. Native service 检查预算，申请一块足以满足该请求的独立 HBM segment。实际大小取
   `min(max(segmentBytes, bytes + alignment - 1), 剩余预算)`——既不无限倍增，也不会让目标段大小
   阻止剩余预算内本可满足的小请求。如果某个 transport 需要映射或注册，必须先成功完成再发布地址。
4. Host 回复 segment ID、地址、容量或错误。Device owner 成功时**只在新段尝试一次分配**，提交后从
   原分配点继续执行。成功响应却容纳不下请求属于协议失败，不再次扩容。
5. Host 申请失败、超预算、必要注册失败或分配后记账失败，都必须让 Device 收到失败响应。已经申请但
   未发布的 HBM 按安全清理规则释放——注意要**先解除驻留 kernel 的等待，再调用 Runtime free**，
   因为某些 allocator 清理本身会等待 Device 工作。

一次调用只允许一次扩容；观察到第二次请求按协议失败处理。Host 的申请与响应不得依赖当前 kernel 完成，
也不能排在它的后续 stream 操作之后；扩容请求更不能等 kernel 结束后才通过 D2H copy 读取。若某平台
无法在驻留 kernel 期间完成这些操作，则该平台不支持此路径，**不能隐式改为退出重跑**。

### 6.3 FIFO 与通信顺序

Host 排队及在途的 allocation 请求受 `maxPending` 限制（默认 64），达到上限即拒绝新提交。尚未准入的
请求在 Host 队列等待，不占用 AICore；已准入且需要扩容的 kernel 则占用其执行资源并驻留等待。队首的
“尝试分配 → 等待扩容 → 取得区间”不可被后续请求插队，新空间优先满足队首。

下一个请求必须等到 allocator 的写阶段安全结束之后才能准入，不能仅仅观察到地址已发布就启动另一个写入方；
当前实现等待本轮完整完成后再准备下一份 snapshot。已有输出可以在登记的其他 stream 上继续消费，但这
不等于同一 pool 的分配可以并发。

FIFO 只决定本池的分配次序，**不能代替通信协议自身的地址就绪、credit 或远端完成条件**。当前 Torch
buffer 一次只允许一个活跃 routing handle，必须完成 Combine 或显式 discard 后才能提交下一次 Dispatch。

## 7. Host 接口与异步语义

[![图 3：Dispatch 异步接口时序](diagrams/elastic-memory/dispatch-sequence.drawio.png)](diagrams/elastic-memory/dispatch-sequence.drawio.png)

图 3 区分元数据就绪与数据完成两个时刻。

### 7.1 Native C++14 API

API 由 `Pool`、`Pending` 和 `Allocation` 三个类组成：

- `Pool::Create(Config)` 在调用者当前 ACL context 上创建 pool，只分配控制元数据。`Config` 包含
  `maxCommittedBytes`、`maxCachedFreeBytes`、`segmentBytes` 和 `maxPending`。
- `Pool::Submit(launch, inputStream)` 记录输入 stream 依赖，并在 native service 线程上执行
  `launch` 回调；回调持有输入 owners 直到 producer 完整结束。
- `Pending::Wait()` 保持完整完成语义：返回 allocation，或抛出最终错误。
- `Pending::WaitMetadata()` 在不可变地址/shape 校验通过、且 producer 完成事件已经 record 之后即可
  返回 allocation。使用它的消费者**必须**调用 `Allocation::WaitOnStream(stream)` 建立依赖，并用
  `Allocation::Synchronize()` 检查最终结果。
- `Allocation` 另提供 `RecordStream`/`RecordEvent` 登记额外消费者，`SetRetireHook` 供 adapter 在
  service 线程、pool 锁之外执行释放前的刷新动作。

Device 入口为单 owner 调用：

```cpp
// 当前声明以公共头为准。
template<uint32_t ScratchBytes = 128>
__aicore__ inline uint64_t AllocateBundle(DeviceView view, uint64_t bytes, uint64_t alignment,
                                          uint64_t rows, uint64_t* address,
                                          Profile::Recorder* profiler = nullptr);
```

`ScratchBytes` 至少 128B 且 32B 对齐，UB scratch 从调用者的 `TPipe` 申请，由调用者 reset 或销毁。

### 7.2 Torch 侧语义

`pending.materialize(stream=None)` 调用 `WaitMetadata`，为当前或指定 stream 排入 producer 依赖后
返回普通 Tensor，返回时刻可能早于数据完成。重复调用 materialize 得到同一结果和同一 Storage，并为新增的消费
stream 建立依赖。等待期间释放 Python GIL；native service 不需要任何 Python 回调即可推进。具体示例
见 [adapter README](../integrations/elastic_memory_torch/README.md)。

| 状态 | 语义 |
|---|---|
| METADATA_READY | 最终地址/shape 稳定，分配成功，lease 可交付，producer 完成事件已录制；**不承诺最终执行成功** |
| DATA_READY | 生产者写入及必要的通信访问已完成；最终状态仍须检查 |
| FAILED | 本次结果及基于失败数据的下游结果全部无效；停止相关访问后清理，不恢复执行 |

`pending.synchronize()` 或 `result.synchronize()` 等待完整的 producer 完成并报告晚到错误。
`combine_compact()` 和 `result.discard()` 同样先检查 producer 最终状态；Combine 等自身完成后返回，
只有成功的 Combine/discard 才释放本轮 communicator 使用权。失败会使该域不可再用，不能通过下一次提交
继续使用。

### 7.3 几条容易误解的边界

- producer event 只保证 stream 执行顺序，**不能代替错误检查**，也不保证 Host 在下游工作开始前已经
  获知所有 peer 错误。
- Runtime 会把 event 保留到引用消失、且所有已排入的 wait 都确实越过安全边界之后；零字节输出同样
  保留该依赖，因为单独持有的计数 Tensor 仍可能需要它。
- 元数据提前返回不改变同池 FIFO：service 仍然等待本轮完整结束才准入下一请求。
- 丢弃 pending 不撤销设备任务。Runtime 继续持有输入 owners 和早期 lease 直到安全完成，再按
  Storage/consumer 条件释放。**不提供执行中的强制取消。**
- Host service 不依赖持有 Python GIL 的调用者，也不依赖用户调用 `materialize` 才推进。当前 active
  poll 每轮请求 `sleep_for(5µs)`，这是对操作系统的睡眠请求，既不保证 5µs 后唤醒，也不代表端到端
  延迟；Host 轮询、排队和 CPU 占用应当单列测量。

## 8. Tensor、stream 与容量归还

### 8.1 Storage lease

adapter 使用与安装版本匹配的 `torch_npu` 外部内存工厂创建 NPU Tensor，设置 device、dtype、shape、
stride 和 ND format。当前使用 `at_npu::native::from_blob`，**不复制 output，也不替换 Torch 全局
allocator**。

自定义 Storage deleter 捕获 native lease。最后一个 Storage 引用消失时只提交释放请求，不直接归还
HBM；view、detach 和重复调用 materialize 共享同一份存储所有权。Python finalizer 不能替代 Storage 持有。训练还
需要独立的 autograd 与 backward stream 适配，当前直接拒绝 `requires_grad`。

materialize 使用的 stream 会自动登记并排入 producer event 依赖；在新 stream 上重复 materialize 保留
同一 Storage，并为该 stream 建立依赖。额外的 consumer stream 用 `result.use_on_stream(stream)` 显式
登记。Torch 的 `record_stream` 不自动证明自定义 segment 的安全性，**未登记 stream 上的消费不在
支持范围内**。

还有一个与框架相关的陷阱：Torch CPU 提交队列中的任务可能晚于 Python 引用消失。因此 native 释放 hook
在 service 线程、pool mutex 之外刷新登记队列，再记录最终的 stream-tail 事件；**不能**在可能运行于
Torch 队列 worker 的 Storage deleter 里同步刷新该队列。设备 context 和登记的 stream 必须保持有效，
直到 lease 释放完成。

### 8.2 逻辑释放与真实归还

一段区间可以释放，需要同时满足两个条件：没有 Storage/export 或尚未 materialize 的 handle 持有；生产者、通信以及
所有登记的消费者均已完成。环形池随后只推进连续的安全前缀。要区分三个不同状态：**逻辑空闲、可重新
分配、整段归还 HBM**。

空 segment 在安全边界从未来的 snapshot 中排除，确认没有在途访问后才释放；若 transport 持有注册资源，
先完成相应注销。不得释放仍有 kernel 使用的 segment 表版本，也不得移动活跃地址来腾出空间。

三个入口的语义：

- `collect()` 非阻塞地请求处理已满足条件的待释放对象，并返回当前 snapshot；返回的计数可能早于本次
  collection 生效。
- `trim(wait=True)` 等待已释放对象的相关设备工作并释放可归还的整段，不等待用户删除活跃 Tensor，
  也不强制释放它们。
- `close()` 拒绝新提交并排空（或失败处理）在途请求；仍存活的 Storage 继续保持 native Core 和
  segment，**不要把入口关闭等同于 HBM 已全部释放**。

空闲缓存预算 `maxCachedFreeBytes` 控制保留的空 segment 容量，可以设为零；但一个小 view 仍可能保留
整段。`maxCommittedBytes` 限制实际提交量，Runtime OOM 仍可能提前发生。

[![图 4：Storage 释放、池内复用与真实 HBM 归还](diagrams/elastic-memory/reclaim-sequence.drawio.png)](diagrams/elastic-memory/reclaim-sequence.drawio.png)

图 4 区分 Storage 引用消失、访问结束、连续前缀可复用和整段归还四个时刻；活跃 view 可以一直保留整段。

## 9. 统计口径

| 指标 | 含义 |
|---|---|
| `live_bytes` | 活跃 lease 与在途生产者 reservation，包含相应的对齐/绕回开销 |
| `pending_retire_bytes` | 用户已放弃持有、但访问尚未全部结束的 reservation |
| `free_bytes` | segment 内逻辑空闲总量，包含暂时被环形队头阻塞的空间 |
| `blocked_free_bytes` | `free_bytes` 中不能越过活跃前缀的子集，不重复计入总量 |
| `padding_bytes` | live/pending reservation 中的对齐和绕回开销，不是额外 HBM |
| `committed_bytes` / `peak_committed_bytes` | 数据 segment 当前实际提交量及其峰值 |
| `reclaimable_segment_bytes` | 可以整段安全归还的容量 |
| `largest_free_range` | 满足环形水位和连续性限制的最大可分配区间 |
| `metadata_bytes` | 已计入的控制存储；必须说明是否包含 Host 容器和线程开销 |
| `growth_count` / `trim_count` / `failure_count` | 扩容、整段归还和失败次数 |

记账满足 `committed_bytes = live_bytes + pending_retire_bytes + free_bytes`，分类之间不重复。生产者
的实际大小尚未发布时采取保守口径：未分配的 committed 空间全部计入 `live_bytes`，不报告为可回收，
且 `reclaimable_segment_bytes` 为零。元数据发布后按 Device commit 校验并一次性登记 reservation；
提前导出 lease 不重复记账，也不提前解除 producer 的回收保护。

当前 `metadata_bytes` 包含映射 mailbox 和 Device PoolState，**不包含** Host map/deque、lease/event
或 service-thread 对象开销。Torch allocator 不统计这些外部 segment，因此不能单凭
`torch.npu.memory_allocated()` 推断总 HBM，trim 也不会自动清空 Torch cache。

## 10. 通信集成与失败处理

### 10.1 当前集成形态

当前实现先把最终的本地输出池化，保留既有的通信 staging，再复制到 output。报告时必须分别列出
output、staging、辅助 flags/guard 和复制流量；**不称作通信零拷贝**，也不能用另一份隐藏的最大输出
缓冲来抵消节省。

直接向池内输出进行 peer-memory/UDMA 写入属于独立扩展，其前提是：发布地址之前完成映射/注册和对端
描述符交换；注册也必须支持在原 kernel 驻留期间推进。UDMA 只注册允许的 owner 本地分配，不注册远端
IPC mapping，也不在 `InitThread` 中注册；WQE 在 UB 构建，经 MTE3 发布完成后用 `st_dev` 敲 doorbell；
释放和注销需等待远端协议完成。transport 的选择依据能力、注册状态和传输大小，**不只依据拓扑**。

### 10.2 必须保证的行为

| 情况 | 必须的行为 |
|---|---|
| 第一次非空调用 | 从零数据 HBM 经扩容获得空间；分配前的副作用只执行一次 |
| 空接收或全 inactive | 正确返回零尺寸输出，通信协议仍然完成 |
| 当前段不足或队头阻塞 | 先尝试其他段；均不足则原 kernel 等待 Host 扩容 |
| 超预算、OOM、必要注册或记账失败 | 发布错误，结束参与 rank 的相关等待，终止调用/域及排队请求 |
| 描述符过期、重复或越界 | 拒绝交付和重复释放，进入相应错误处理 |
| kernel 或通信失败 | 保留首个错误；未确认访问结束的资源予以隔离，不能提前回收 |
| 部分 output 已写 | 结果无效，不回滚、不重启；访问停止后清理资源 |

不得通过删 token、改变路由或降低 dtype 来伪装成功。错误也不能只在一个 Host 线程抛出而留下 Device
永久等待。当无法确认设备访问已经停止时，实现采取 **fail-closed 隔离**：保留 context 绑定的资源和
输入 owner 直到进程结束，而不是在未确认停止的情况下 free 或 reset。若设备或通信无法协作退出，使用
现有 Runtime/作业失败机制；ElasticMemory 本身不提供故障恢复系统。清理资源不表示内容已恢复，也不
表示失败域可以继续复用。

[![图 5：Host 扩容失败的传播与清理](diagrams/elastic-memory/failure-sequence.drawio.png)](diagrams/elastic-memory/failure-sequence.drawio.png)

图 5 中的清理只在确认访问停止后进行；失败前可能已经发生写入，清理不表示回滚或恢复执行。

## 11. 性能目标与验证要求

### 11.1 分配热路径目标

无扩容的正常命中和非空绕回分配，热路径目标为 **≤2.5µs**，统计范围从进入 Device 分配接口到结果发布
完成，包含初始化、状态读取、回收水位应用、分配计算、commit、发布和必要同步。段切换与 Host 扩容单列
统计，不能把正常每次调用的成本移走。记录逐 rank 的 median/min/max/P95、样本数和超目标样本数，不把
观测到的最大值当作所有负载的上界。

[![图 6：Device 分配流程与逐 rank profiling 耗时](diagrams/elastic-memory/device-allocation-profile.drawio.png)](diagrams/elastic-memory/device-allocation-profile.drawio.png)

图 6 的内部阶段来自 SDK 插桩测量，完整 Dispatch 对照来自关闭设备诊断的独立批次。阶段之间嵌套、且
可与其他核的工作重叠，**不能相加或直接相减**来推导纯分配收益。图 6 及更早的分配/Dispatch 数值属于
metadata-only / 5µs poll 改动之前的基线，不能据旧数据宣称新的异步加速或延迟保证。

### 11.2 算子对照方法

核心算子性能只比较设备 kernel 执行时间：在相同 shape、输入、路由、dtype、rank、通信路径、编译和
诊断配置下，池空间充分且测量期间 `growthCount` 不变，比较三种方案——最大容量预分配、Host 实际大小
参考、Device pool。Host 参考仅用于对照，不得进入生产路径充当 size Planner。

正式对照必须移除设备诊断插桩，保持相同的 Host stage barrier 与 framework profiler，以命名
Dispatch/Combine kernel 的 msprof Task Duration 测量，并保留算子内的分配、同步和跨 rank 等待，不用
Host wall 或 Task Wait Time 替代。至少覆盖 bs16/h128 和 bs256/h4096、BF16/topK2/2 ranks，保留预热、
足够样本及逐 rank/跨 rank 分布；中位数改善不能代替尾延迟结论。

报告还须包含 backend、native status、kernel 版本、数据量、适用带宽、实际 output bytes、
committed/peak/trim、blocked/padding 和仍然保留的 staging。Host 提交/排队、materialize、CPU 占用
及扩容/注册/trim 延迟作为辅助观测，不计入核心 kernel 性能。

### 11.3 诊断插桩

诊断使用仓库 `TileXR::Profiling` SDK 的真实 service-stream capture；Collect 在调用完成之后执行，
不能阻塞扩容服务。当前接入使用 standalone SDK 1.0.0 / DeviceView ABI 2，Host 与 kernel 必须成对
重编译。`tests/elastic_memory/profile_compact.py` 在 launch 之前声明参与者和工作量，完成后合并
rank-owned manifest、completion 与 Host events，再由公共 capture-set API 生成重复采样报告。mapping
模板保留空的时钟换算，必须显式填写有证据的 scale 和依据；allocator 结果取 owner lane，不能用最慢
lane 的空阶段代替。

还需验证 timer 换算、全 lane/关键 owner marker、overflow/drop 和插桩扰动，不把 HWTS/AIV/sysClock
频率直接当作 `GetSystemCycle` 频率，也不未经证明就跨 lane 相减时间戳。

### 11.4 功能验证清单

功能验证须覆盖：真实扩容时 kernel 尚未结束、前序副作用恰好一次、新地址读写、失败退出不重放、FIFO
队首优先使用新空间、初始零数据 HBM、绕回/段切换/溢出/零尺寸、生命周期与整段归还，以及真实多 rank
Dispatch/Combine。Host 模型和 M0 可行性实验不能替代完整接入测试。

异步验收还须单独保持 producer 在途以证明元数据确实提前返回，确认完整 `Wait` 语义，并覆盖立即 Torch
消费、跨 stream 重复 materialize、零输出依赖、晚到错误以及提前删除引用。

每次硬件 launch 之前重新检查所有目标物理卡的 proc-mem 和 identity mapping；busy、query/parse 失败
或 mapping 不明均拒绝，不以 `fuser` 为空放行，不 kill/reset 外部任务。相同环境已有的 HCCL
qualification 必须复用，证据不全按边界记录，不按任务重跑。

## 12. 当前支持边界

| 项目 | 当前范围 |
|---|---|
| 平台 | CANN 9.2.0、A5/Ascend950DT 实测；其他版本与硬件不作兼容承诺 |
| 编译与部署 | native C++14；Torch extension 匹配安装 Torch 的 C++17/ABI；纯 AICore 外置 binary 注册启动，不在 Host 库嵌入 kernel |
| EP | 本地输出池化的 Memory Dispatch/Combine；无独立 Planner |
| Torch | 普通 FP16/BF16 连续 ND Tensor，int32 routes、FP32 Combine weights；inference-only |
| 异步 | 已实现 metadata-only materialize、consumer event 依赖和显式最终错误检查；native `Wait` 与 Combine 保持完整完成语义，CANN 9.2 功能验证已通过，时延与边界见验证记录 |
| 生命周期 | Storage/view、登记 stream、pending input owner、close 保持活跃 Storage |
| 未覆盖扩展 | autograd/backward、graph capture、UDMA 直接写输出、multi-host、量化、shared-expert 和 TP 路径 |

当前分配与完整 kernel 的实测、未覆盖项和事故披露以[验证记录](validation/elastic-memory.md)为准。
开发侧负责实现、审查、全部测试与证据；设计验收依据提交结果核对本文各项规定，不要求设计侧自行编译或实机复验。

## 13. 图源、代码与维护入口

### 13.1 图源

本文图片已直接嵌入各相关章节，PNG 内嵌可编辑图源，完整尺寸可通过图片链接查看：

- [分段环形池 draw.io](diagrams/elastic-memory/segmented-ring-allocation.drawio) ·
  [流程与实测说明](diagrams/elastic-memory/segmented-ring-allocation.md)
- [Device profiling draw.io](diagrams/elastic-memory/device-allocation-profile.drawio) ·
  [阶段口径与证据](diagrams/elastic-memory/device-allocation-profile.md)
- [场景、Dispatch、失败和回收四页 draw.io](diagrams/elastic-memory/elastic-memory.drawio)

### 13.2 代码入口

| 内容 | 路径 |
|---|---|
| Native API | [tilexr_elastic_memory.h](../src/include/tilexr_elastic_memory.h) |
| 共享 ABI | [tilexr_elastic_memory_abi.h](../src/include/tilexr_elastic_memory_abi.h) |
| 环形算法 | [allocator.h](../src/elastic_memory/allocator.h) |
| Host service 实现 | [pool.cpp](../src/elastic_memory/pool.cpp) |
| Device 分配入口 | [include/device/tilexr_elastic_memory_device.h](../src/include/device/tilexr_elastic_memory_device.h) |
| Device profiling slot | [include/device/tilexr_elastic_memory_profile.h](../src/include/device/tilexr_elastic_memory_profile.h) |
| compact EP 接口 | [tilexr_elastic_ep.h](../src/include/tilexr_elastic_ep.h) |
| Dispatch kernel | [tilexr_ep_dispatch_memory_kernel.h](../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h) |
| Torch adapter | [README](../integrations/elastic_memory_torch/README.md) |
| 功能测试 | [tests/elastic_memory](../tests/elastic_memory)、[Torch tests](../integrations/elastic_memory_torch/tests) |
| 相关设计 | [EP Memory](specs/ep-memory.md)、[Profiling SDK](specs/profiling.md) |

### 13.3 维护约定

Device 头安装在 `include/device/`，Host API 与共享 ABI 安装在 `include/`。移动公共头时要同时维护
include、CMake dependency 和安装规则，并用干净的安装目录编译外部消费者。

维护本文时同步更新图源、PNG 和中英文正文，中英文版本的规定必须一致。在图旁要始终区分元数据就绪、
producer 完成和已验证的行为，不能把 event 排序当作错误检查，也不能把设计实现当作硬件验证。具体
环境、原始日志、提交及 binary 哈希留在验证记录中，避免设计文档退化成执行历史。
