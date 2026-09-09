# EP Memory Server Dedup 调试进度

更新：2026-09-09。以[最新设计](specs/ep-memory-server-dedup.md)的已确认接口为准；运行统一使用
[七步验证](EP_VALIDATION_GUIDE.md)，修改原配置的 checkpoint 即可，不增加独立 runner 或重复预检。
每个检查点可包含实现、修复等多笔提交；只有对应测试通过才登记“已通过”和通过 SHA。
第二至第四阶段仅列已有测试方向，检查点边界和提交拆分待设计确认，不作为当前实现要求。

## 阶段 1：Dispatch 公共框架与空算子

| 检查点 | 每次测试重点 | 当前状态 | 通过测试的 commit |
| --- | --- | --- | --- |
| 1 `ep-sdedup-p1-abi-host` | 三类 metadata ABI、layout、Host 校验、错误码；七步生产构建与 Host 回归 | **已通过**，用户确认最新提交完成服务器验证 | [`acb976cb6c5527e02a0d931e20a954eb1c891d37`](https://github.com/LingquLab/TileXR/commit/acb976cb6c5527e02a0d931e20a954eb1c891d37) |
| 2 `ep-sdedup-p1-empty-kernel` | A5 空 kernel、ELF/安装、生产库链接和相对加载、设备注册；Host 验证 launch 参数和失败返回 | **已通过**，production 模式 2/2 主机、每台 18 项测试成功 | [`eae264dc2e3d33169ffd2e78e04c3430d41b7559`](https://github.com/LingquLab/TileXR/commit/eae264dc2e3d33169ffd2e78e04c3430d41b7559) |
| 3 `ep-sdedup-p1-dry-run` | 64P/128P 全量 rank/device 映射及有界退出，不建 communicator | production 16P 已通过；64P/128P 与目标清理演练待验证 | —（完整验收待完成） |
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

检查点 2 的提交与测试记录：

| 提交 | 修改及测试重点 | 记录 |
| --- | --- | --- |
| `0f8f6fa` | 空 A5 kernel、安装、注册和 launch；复用七步入口 | 本地 17/17 EP 测试通过；脚本 91 项通过、3 项 MSYS symlink 跳过；服务器 production ABI/Host 回归通过 |
| `8dcca5b` | 修正固定为 ABI/Host 的摘要范围，JSON 与终端按 checkpoint 一致显示 | 原双节点回归 2/2 通过，测试判定与七步入口不变 |
| `eae264d` | 登记前次 ABI/Host 回归，随后切换 empty-kernel 完成服务器验证 | **检查点 2 通过提交**，2/2 主机、每台 18 项测试成功 |

本次通过记录来自用户回传的服务器日志：`checkpoint=ep-sdedup-p1-empty-kernel`、`mode=production`、
`status=PASS`，run_id 为 `f86df093945c42f8985be62e6e934ab9`，结果位于
`/home/h00580772/TileXR-git-chec/run/validation/f86df093945c42f8985be62e6e934ab9/summary.json`。
范围为 ABI/Host、A5 空 kernel 安装与设备注册；**未执行 NPU launch 或去重通信**。本地未直接读取远端 JSON。
提交来源保留日志中的 `unverified`：它表示未校验源码与提交内容一致性，不表示测试失败，也不重新引入源码摘要门禁。

此前 `0f8f6fa` 的 ABI/Host 回归结果为
`/home/h00580772/TileXR-git-chec/run/validation/b57e91fb58d64b8b87ac880c4322d2a0/summary.json`；
另一次同范围 PASS 未提供路径。两次均未执行设备注册，保留为回归记录，不替代本次 empty-kernel 验收。

检查点 3 的提交与测试记录：

| 提交 | 修改及测试重点 | 记录 |
| --- | --- | --- |
| [`67ad16f2e917bd1e1fd17a83262c69b3d7b5680c`](https://github.com/LingquLab/TileXR/commit/67ad16f2e917bd1e1fd17a83262c69b3d7b5680c) | 复用七步入口；每台 8 个 rank、完整映射/marker 校验、共享期限和异常清理 | 本地映射与异常清理回归通过；用户回传 production 2/2 主机、每台 17 项测试、16/16 rank 通过 |

本地验证在 Windows/MSYS2 POSIX Python 上执行：完整脚本回归 108 项中 105 项通过，3 项因 MSYS 未建立真实 symlink 而跳过；Shell 语法、UTF-8、Markdown 链接及差异空白检查通过。本次未改 C++，未重跑 CANN 构建或访问 NPU；64/128 rank 在本机逐节点运行，双节点 SSH/rsync 使用传输替身，不能登记为真实集群验收。此提交未修改公共 Dispatch 接口或提前实现下一检查点。

用户回传的服务器 16P 结果如下，登记为该配置通过：

```text
VALIDATION_PASS checkpoint=ep-sdedup-p1-dry-run mode=production hosts=2/2 tests_per_host=17 ranks=16/16 commit=67ad16f2e917bd1e1fd17a83262c69b3d7b5680c (unverified)
```

本次覆盖 ABI/Host、16P rank/device 映射与进程退出，不创建设备上下文或 communicator，未执行 NPU launch 或去重通信。用户尚未提供该次 run_id 或 summary.json 路径，本地未直接读取远端结果。`unverified` 保留为提交来源标签，不表示测试失败；这条正常运行 PASS 不替代 64P/128P 验收或目标节点的故障清理演练。

| 检查点 3 必需记录 | 当前状态 | 通过 commit / 结果 |
| --- | --- | --- |
| 目标集群 64P（8×8） | 待执行 | — |
| 目标集群 128P（16×8） | 待执行 | — |
| 目标 Linux 卡住/HUP 清理演练 | 待执行；复用七步指南中的两项行为测试 | — |

同一实现提交的全部必需记录通过后，再登记检查点 3 的通过 SHA；小规模 PASS、来源标签 `unverified` 和完整验收状态分别记录。

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
