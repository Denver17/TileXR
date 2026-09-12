# EP Memory Server Dedup 调试进度

记录检查点的服务器通过结果及当前增量状态；完整命令见[人工验证](EP_VALIDATION_GUIDE.md)。
通过范围按实际配置登记，64P/128P 与目标 Linux 卡住/HUP 清理演练仍待验证。

## 阶段 1：Dispatch 公共框架与空算子

| 检查点 | 已通过范围 / 待验证项 | 通过 commit |
| --- | --- | --- |
| 1 `ep-sdedup-p1-abi-host` | ABI/layout/Host 已通过 | [acb976c](https://github.com/LingquLab/TileXR/commit/acb976cb6c5527e02a0d931e20a954eb1c891d37) |
| 2 `ep-sdedup-p1-empty-kernel` | production 2 主机安装/注册通过，未 launch | [eae264d](https://github.com/LingquLab/TileXR/commit/eae264dc2e3d33169ffd2e78e04c3430d41b7559) |
| 3 `ep-sdedup-p1-dry-run` | production 16P 映射/退出通过 | [67ad16f](https://github.com/LingquLab/TileXR/commit/67ad16f2e917bd1e1fd17a83262c69b3d7b5680c) |
| 4 `ep-sdedup-p1-chain-single` | production 16P 建链/单轮空 launch 通过 | [f5bfa58](https://github.com/LingquLab/TileXR/commit/f5bfa58e93ba7b8d7103c67fc20b9ae08044fb09) |
| 5 `ep-sdedup-p1-chain-multi` | production 16P 空 launch/批末同步/barrier 通过；用户已确认后续多轮批测通过 | [e5a4bdb](https://github.com/LingquLab/TileXR/commit/e5a4bdbcc91052e2aa7a0975e9c9886ff75fe283) |
| 6 profile：performance | 用户确认当前 kernel 计时批测通过；具体 case 参数以运行 summary 为准 | [3b03a4f](https://github.com/LingquLab/TileXR/commit/3b03a4f7e96aa4a1aff8b57f6e95e15a1db95e4f) |
| 6 profile：breakdown | 最后一轮设备打点与耗时拆解待验证 | — |
| 7 `ep-sdedup-p1-memory-boundary` | 每 Rank 4 GiB 分配/建链、本卡末尾 32B DataCopy 写入/读回及 Host 校验通过；免构建/同步重跑通过 | [f79267d](https://github.com/LingquLab/TileXR/commit/f79267d1c0bdeac92100c8272453438f9ab432de) |

2026-09-11：用户确认 `bash run_commit_validation.sh` 和 `bash run.sh run_mode=functional loops=1 warmup=0` 均执行成功。
按最近交付的 `f79267d` 登记，未直接读取本次 summary 核对实际 SHA 和卡数；通过范围为通信域分配/映射与本卡边界访问，不包含跨卡 payload 传输。

2026-09-10：用户确认当前检查点通过，按最近交付的 `3b03a4f` 登记；尚未直接读取此次结果目录，不推断新增规模、dtype 或 breakdown 覆盖。下一步为阶段 1 的 breakdown 设备打点验收，只执行新增 case，不回归已通过检查点。

此前 chain-multi 结果：2/2 主机、17 项测试/主机、16/16 rank PASS；
`/home/h00580772/TileXR-git-chec/run/validation/f97fb895b7e84af28f482a3154ef4ddb/summary.json`。
此前 empty-kernel 结果：`/home/h00580772/TileXR-git-chec/run/validation/f86df093945c42f8985be62e6e934ab9/summary.json`。
以上依据用户回传，未直接读取远端 JSON；`unverified` 是提交来源标签，不表示测试失败。

`chain-multi` 每次只运行 `TILEXR_VALIDATION_RUN_MODE` 指定的一种模式，默认 functional/1 轮/无预热。
`mode=production` 不代表三模式全部执行；实际模式见 `summary.json` 的 `chain_configuration.run_mode`，
performance/breakdown 还需对应的 `measurements` 结果。1 轮 PASS 不证明多轮复用；阶段 1 均不证明去重通信或数值正确。

## 阶段 2：Dispatch 算子逻辑

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| cnt 发送/接收、cumsum | 用户确认最新提交验收通过 | [0bc6f1b](https://github.com/LingquLab/TileXR/commit/0bc6f1bbe64f25ab21a8fae23e70eed797dc48a5) |
| server grouping / 第一跳 | §2.3 用户确认单轮检查点 PASS；多轮及性能达标未验收 | [0df1db0](https://github.com/LingquLab/TileXR/commit/0df1db02bb139c447456c283afe05586ff9f3c5d) |
| DIRECT / 完整源端发送 | §2.4 用户确认单轮检查点 PASS；多轮及性能达标未验收 | [631a1ff](https://github.com/LingquLab/TileXR/commit/631a1ffcc9cc138e583c1b657cd73ef1a6e64792) |
| relay 展开 / metadata | 未开始 | — |
| Dispatch correctness / 完整验收 | 未开始 | — |

2026-09-12：依据用户“最新的提交验证通过”回传，按当前最新提交 `0bc6f1b` 登记 cnt/cumsum 检查点通过。
未直接读取本次 summary；具体规模、模式、轮次及性能数据以实际运行结果为准，不扩展为 Token 数据发送或完整 Dispatch 验收通过。

2026-09-12：用户确认“阶段 1 的第一个部分”测试通过并要求实现第二部分；按当前设计验证流程 §2.3 接入第一跳发送。该反馈延续已有 cnt/cumsum 基线，未读取新 summary，不推断额外 dtype、规模或时延。

上一检查点 `ep-sdedup-p2-first-hop` 仅开放单次 functional/performance/breakdown；当前 `ep-sdedup-p2-source-send` 在相同 16P/L6/BS96/H7168/K6/BF16/balance_shift/seed2026、每 rank 4 GiB 基线上启用 DIRECT。数据接收转发和 Combine 未实现。本地验证覆盖非量化 sender、184B ABI、单份布局、consumer 错包诊断和 v3 trace，不能作为 CANN/A5 通过记录。

2026-09-12：修复第一跳 `Duplicate` 掩码限定符编译错误后，用户回传“1轮测试通过，最终结果pass”。按最近交付的 `0df1db0` 登记 `ep-sdedup-p2-first-hop` 单轮检查点通过；未直接读取服务器 summary，实际 SHA、配置和运行模式以该次结果为准。通过范围为当前 cnt/cumsum + 第一跳包及源端输出校验，不扩展为量化组合、breakdown、多轮复用或性能达标已验收。下一增量按验证流程 §2.4 叠加 DIRECT 直发。

2026-09-12：用户确认 `ep-sdedup-p2-source-send` 测试通过。按最近交付的 `631a1ff` 登记单轮完整源端发送检查点通过；未直接读取服务器 summary，实际 SHA、配置、模式和性能数据以该次结果为准。通过范围为 cnt/cumsum、第一跳、DIRECT、完整 route 覆盖及 sourceMask/expertScalesOut 校验，不扩展为多轮复用、接收/relay、Combine 或性能达标已验收。

## 阶段 3：Combine 空算子工程

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| 公共 API / Host / layout | 未开始 | — |
| 空 kernel / 注册 / launch | 未开始 | — |
| 单轮 / 多轮框架 | 未开始 | — |

## 阶段 4：Combine 算子逻辑

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| expert 返回 | 未开始 | — |
| group 归约 / partial 返回 | 未开始 | — |
| source 归约 | 未开始 | — |
| correctness / 性能验收 | 未开始 | — |

后续阶段按最新设计逐模块推进；当前交付 `ep-sdedup-p2-source-send`，单轮验证第一跳 + DIRECT；16P/L6/BS96/H7168/K6/BF16/balance_shift/seed2026、每 rank 4 GiB 及 loops=1/warmup=0 基线延续。完整数据多轮复用仍待接收清理契约。
