# EP Memory Server Dedup 调试进度

只记录检查点的服务器通过结果和对应 commit；完整命令见[七步验证](EP_VALIDATION_GUIDE.md)。
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
| 7 `ep-sdedup-p1-memory-boundary` | 每 Rank 4 GiB 建链、本卡末尾 32B DataCopy 写入/读回及 Host 校验，待上机 | — |

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
| server grouping / 第一跳 | 未开始 | — |
| relay 展开 / metadata | 未开始 | — |
| Dispatch correctness / 完整验收 | 未开始 | — |

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

阶段 2–4 仅保留既有方向，检查点边界待设计确认；当前仅执行 Dispatch 单算子。
