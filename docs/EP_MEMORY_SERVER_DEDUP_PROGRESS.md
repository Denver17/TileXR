# EP Memory Server Dedup 调试进度

更新：2026-09-09。以[最新设计](specs/ep-memory-server-dedup.md)的已确认接口为准；运行统一使用
[七步验证](EP_VALIDATION_GUIDE.md)，修改原配置的 checkpoint 即可，不增加独立 runner 或重复预检。
每个检查点可包含实现、修复等多笔提交；只有对应测试通过才登记“已通过”和通过 SHA。
第二至第四阶段仅列已有测试方向，检查点边界和提交拆分待设计确认，不作为当前实现要求。

## 阶段 1：Dispatch 公共框架与空算子

| 检查点 | 每次测试重点 | 当前状态 | 通过测试的 commit |
| --- | --- | --- | --- |
| 1 `ep-sdedup-p1-abi-host` | 三类 metadata ABI、layout、Host 校验、错误码；七步生产构建与 Host 回归 | **已通过**，用户确认最新提交完成服务器验证 | [`acb976cb6c5527e02a0d931e20a954eb1c891d37`](https://github.com/LingquLab/TileXR/commit/acb976cb6c5527e02a0d931e20a954eb1c891d37) |
| 2 `ep-sdedup-p1-empty-kernel` | A5 空 kernel、ELF/安装、生产库链接和相对加载、设备注册；Host 验证 launch 参数和失败返回 | **代码已实现，待执行 empty-kernel**；`0f8f6fa` 的 production ABI/Host 回归已通过 | 尚无 empty-kernel 通过记录 |
| 3 `ep-sdedup-p1-dry-run` | 64P/128P 全量 rank/device 映射及有界退出，不建 communicator | 未开始 | — |
| 4 `ep-sdedup-p1-chain-single` | 64P/128P 建链、一次空 launch、stream 完成、轮末 barrier | 未开始 | — |
| 5 `ep-sdedup-p1-chain-multi` | 64P/128P 多轮 magic、顺序完成与资源复用 | 未开始 | — |
| 6 `ep-sdedup-p1-profile` | FP16/BF16、warmup、完整 profile 记录和跨 rank 汇总，仅报告框架开销 | 未开始 | — |

检查点 1 的提交记录如下；中间提交不单独视为服务器验收通过。

| 提交 | 修改及测试重点 | 记录 |
| --- | --- | --- |
| `050b2d8` | 独立 ABI、布局和 Host 检查 | 本地 17/17 EP 测试通过 |
| `313063a` | 接通七步构建、同步、运行和汇总 | 初始流程实现 |
| `1b937b9` | 初次环境配置的 hostfile 处理 | 后续流程已改为手动配置 |
| `67168ba` | 普通文件同步、无 `.git` 构建 | 流程修复 |
| `04e16e9` | spdlog 保持可选 | 流程修复 |
| `22dbef9` | 梳理流程中的检查与重复项 | 审计记录 |
| `2da6d00` | 精简七步流程、统一失败分类 | 当前流程基础 |
| `acb976c` | 按完整 C ABI 导出名检查，修复生产库误拒绝 | **检查点 1 通过提交** |

检查点 2 的实现提交为 `0f8f6fa8b579b9e13f45a1d8bcbcd736a5d1c5f6`。
本地脚本回归共 94 项，91 通过、3 项因 MSYS 无真实 symlink 支持而跳过，包含模拟双节点的设备选择和失败汇总。
实现时本地未执行 CANN/A5 验证。用户已回传上述提交的一次服务器运行，来源标记为 `unverified`，
run_id 为 `b57e91fb58d64b8b87ac880c4322d2a0`，结果位于
`/home/h00580772/TileXR-git-chec/run/validation/b57e91fb58d64b8b87ac880c4322d2a0/summary.json`。
用户补充确认该结果及另一次 summary 均为 `checkpoint=ep-sdedup-p1-abi-host`、`mode=production`、`status=PASS`。
据此登记 `0f8f6fa` 的生产构建和 ABI/Host 回归通过；它们没有执行安装包设备注册测试，不代表检查点 2 通过。
另一次 summary 的 run_id/路径未提供，不补写运行身份。检查点 1 原通过记录 `acb976c` 保留。

下一次在原 `validation.local.sh` 中将 checkpoint 改为 `ep-sdedup-p1-empty-kernel`，保持 `production`，
按原流程重新 source、build_all、sync、run；构建会加入安装包注册 consumer，随后每台主机验证一次设备注册。

本次回传暴露了摘要显示遗漏：`0f8f6fa` 将 `scope` 和终端范围固定写成 ABI/Host，无法区分空 kernel 注册检查。
现已按 checkpoint 生成同一份范围文本供 JSON 和终端使用，复用原双节点回归验证，不改变测试判定或七步入口。
`unverified` 仍仅说明提交来源未经源码内容校验，不表示测试失败，也不为消除该标记重新引入源码摘要门禁。

## 阶段 2：Dispatch 算子逻辑

| 检查方向（拆分待确认） | 每次测试重点 | 状态 / 提交 |
| --- | --- | --- |
| server grouping 与第一跳 | 分组/路由及第一跳发送正确性 | 未开始 / — |
| relay 展开与 metadata | 转发、expert 行对应、assistEx/sourceMask/group 内容 | 未开始 / — |
| Dispatch 完整验收 | 独立 correctness、边界路由及通信字节收益 | 未开始 / — |

## 阶段 3：Combine 空算子工程

| 检查方向（拆分待确认） | 每次测试重点 | 状态 / 提交 |
| --- | --- | --- |
| 公共 API 与 Host/layout | 已稳定 Dispatch metadata 的输入契约与资源生命周期 | 未开始 / — |
| 空 kernel 与注册/launch | 独立 artifact、安装后调用和错误返回 | 未开始 / — |
| 单轮/多轮框架 | stream 完成、资源复用与测试汇总 | 未开始 / — |

## 阶段 4：Combine 算子逻辑

| 检查方向（拆分待确认） | 每次测试重点 | 状态 / 提交 |
| --- | --- | --- |
| expert 返回 | 使用可区分的 expert 输出验证返回归属 | 未开始 / — |
| group 归约与 partial 返回 | 成员收齐、weight 只应用一次、partial 内容 | 未开始 / — |
| source 归约 | sourceMask 消费、DIRECT/partial 组合与数值正确性 | 未开始 / — |
| 完整验收与性能 | 独立 correctness、边界形状和跨 rank 性能汇总 | 未开始 / — |

登记规则：失败返工继续归属原检查点，追加修复 commit 与首个失败边界；确认通过后填入完整 SHA。
用户确认的检查点 1 结果尚未在本地附带服务器日志，因此本文不补写未提供的 host/run_id 或设备执行证据。
空 kernel 的注册/提交成功均不等于真实 Dispatch/Combine 功能通过。
