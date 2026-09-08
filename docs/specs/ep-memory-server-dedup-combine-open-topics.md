# EP Memory Server Dedup Combine 议题索引

## 文档状态

第一轮 Combine 问题已与 Dispatch 共同归并到[主设计](ep-memory-server-dedup.md)及
[第二轮待确认项 R01-R12](ep-memory-server-dedup-open-topics.md)。
本文只保留 C01-C14 旧编号的定位映射，不再维护独立的候选、范围或容量结论。
后续讨论和状态更新统一在 R 项进行；已确认结论写回主设计。

特别是 Server 内逐 expert 返回、公共转发包、relay 原子取号、七分区 `7*BS*K` 容量和
按本 relay 合计不同原始 Token 数 `X>BS` 拒绝，均以主设计的新约定为准。
不再沿用旧单 expert 示例、静态全 source/token 结果区或逐 Token credit 作为当前方案。

## 编号映射

| 原编号 | 原议题 | 归并状态 | 第二轮位置 |
| --- | --- | --- | --- |
| [C01](#c01) | 多 Expert 贡献身份与 Group 边界 | 逐 expert 贡献身份已合入主设计；纯本地 group 与多本地行仍待明确 | [R01](ep-memory-server-dedup-open-topics.md#r01)、[R04](ep-memory-server-dedup-open-topics.md#r04) |
| [C02](#c02) | ExpertOut 行序、有效行与 Plan 一致性 | 行引用布局与 expert 计算后的行序/有效长度契约仍待明确 | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10) |
| [C03](#c03) | DIRECT、Group 与 Source 寻址及 Mask | 原始 k、七分区地址与 mask 选择语义已合入；结果类型和数值责任未闭合 | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R08](ep-memory-server-dedup-open-topics.md#r08) |
| [C04](#c04) | Route Weight 来源与单次加权 | 随 Dispatch 传递及终端按 expert 匹配已确定；应用位置、快照与无 weight 语义待确认 | [R08](ep-memory-server-dedup-open-topics.md#r08) |
| [C05](#c05) | 两级归约顺序、精度与量化 | 组内及源端 k 顺序保留；各阶段 dtype、量化和误差判据待确认 | [R08](ep-memory-server-dedup-open-topics.md#r08)、[R12](ep-memory-server-dedup-open-topics.md#r12) |
| [C06](#c06) | 两段回传格式与 Ready | 逻辑寻址已合入；实际 stride、Flag 覆盖和发布粒度待确认 | [R03](ep-memory-server-dedup-open-topics.md#r03)、[R06](ep-memory-server-dedup-open-topics.md#r06)、[R09](ep-memory-server-dedup-open-topics.md#r09) |
| [C07](#c07) | Combine 分核与调度 | Expert 发送、group 归约、partial 发送和 source 归约的执行安排待确认 | [R09](ep-memory-server-dedup-open-topics.md#r09) |
| [C08](#c08) | H 分块、累加驻留与慢成员 | H tile、累加器与进度状态仍待确认 | [R09](ep-memory-server-dedup-open-topics.md#r09) |
| [C09](#c09) | Workspace 容量、Slot 复用与流控 | `7*BS*K`、X>BS 拒绝及原子编号已合入；总预算、失败与跨轮复用待确认 | [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C10](#c10) | Plan 代际、执行轮次与并发生命周期 | Plan 与 Combine epoch 的绑定、重复/并发调用和窗口复用待确认 | [R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C11](#c11) | 去重入口与跨 Rank 契约 | 双入口及拒绝原则已合入；函数签名、行域和参数约束待确认 | [R10](ep-memory-server-dedup-open-topics.md#r10) |
| [C12](#c12) | 完成边界、错误传播与退出 | 容量拒绝条件已合入；完整责任结束、协调退出及资源释放待确认 | [R07](ep-memory-server-dedup-open-topics.md#r07)、[R09](ep-memory-server-dedup-open-topics.md#r09)、[R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C13](#c13) | 正确性参考与验证矩阵 | 最新多 expert 示例已合入；独立参考和必测矩阵待确认 | [R12](ep-memory-server-dedup-open-topics.md#r12) |
| [C14](#c14) | 通信字节与端到端收益 | 保留真实协议字节口径；性能目标、基线与验收标准待确认 | [R12](ep-memory-server-dedup-open-topics.md#r12) |

## 旧议题入口

## C01

**多 Expert 贡献身份与 Group 边界。** 逐 expert 贡献身份已合入主设计；纯本地 group 与多本地行仍待明确。后续见 [R01](ep-memory-server-dedup-open-topics.md#r01)、[R04](ep-memory-server-dedup-open-topics.md#r04)。

## C02

**ExpertOut 行序、有效行与 Plan 一致性。** 行引用布局与 expert 计算后的行序/有效长度契约仍待明确。后续见 [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10)。

## C03

**DIRECT、Group 与 Source 寻址及 Mask。** 原始 k、七分区地址与 mask 选择语义已合入；结果类型和数值责任未闭合。后续见 [R04](ep-memory-server-dedup-open-topics.md#r04)、[R08](ep-memory-server-dedup-open-topics.md#r08)。

## C04

**Route Weight 来源与单次加权。** 随 Dispatch 传递及终端按 expert 匹配已确定；应用位置、快照与无 weight 语义待确认。后续见 [R08](ep-memory-server-dedup-open-topics.md#r08)。

## C05

**两级归约顺序、精度与量化。** 组内及源端 k 顺序保留；各阶段 dtype、量化和误差判据待确认。后续见 [R08](ep-memory-server-dedup-open-topics.md#r08)、[R12](ep-memory-server-dedup-open-topics.md#r12)。

## C06

**两段回传格式与 Ready。** 逻辑寻址已合入；实际 stride、Flag 覆盖和发布粒度待确认。后续见 [R03](ep-memory-server-dedup-open-topics.md#r03)、[R06](ep-memory-server-dedup-open-topics.md#r06)、[R09](ep-memory-server-dedup-open-topics.md#r09)。

## C07

**Combine 分核与调度。** Expert 发送、group 归约、partial 发送和 source 归约的执行安排待确认。后续见 [R09](ep-memory-server-dedup-open-topics.md#r09)。

## C08

**H 分块、累加驻留与慢成员。** H tile、累加器与进度状态仍待确认。后续见 [R09](ep-memory-server-dedup-open-topics.md#r09)。

## C09

**Workspace 容量、Slot 复用与流控。** `7*BS*K`、X>BS 拒绝及原子编号已合入；总预算、失败与跨轮复用待确认。后续见 [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11)。

## C10

**Plan 代际、执行轮次与并发生命周期。** Plan 与 Combine epoch 的绑定、重复/并发调用和窗口复用待确认。后续见 [R11](ep-memory-server-dedup-open-topics.md#r11)。

## C11

**去重入口与跨 Rank 契约。** 双入口及拒绝原则已合入；函数签名、行域和参数约束待确认。后续见 [R10](ep-memory-server-dedup-open-topics.md#r10)。

## C12

**完成边界、错误传播与退出。** 容量拒绝条件已合入；完整责任结束、协调退出及资源释放待确认。后续见 [R07](ep-memory-server-dedup-open-topics.md#r07)、[R09](ep-memory-server-dedup-open-topics.md#r09)、[R11](ep-memory-server-dedup-open-topics.md#r11)。

## C13

**正确性参考与验证矩阵。** 最新多 expert 示例已合入；独立参考和必测矩阵待确认。后续见 [R12](ep-memory-server-dedup-open-topics.md#r12)。

## C14

**通信字节与端到端收益。** 保留真实协议字节口径；性能目标、基线与验收标准待确认。后续见 [R12](ep-memory-server-dedup-open-topics.md#r12)。

## 验证边界

本次为设计文档归并，没有实现或运行去重 Combine。
已有普通 Memory 或 MoonEP 测试不能证明本设计的行为；后续验证与环境要求统一见
[R12](ep-memory-server-dedup-open-topics.md#r12)及[主设计验证边界](ep-memory-server-dedup.md#验证边界)。
