# Profiling 测试与接续指南

面向人工操作和 AI 接续。架构与不变量见 [当前设计](../../docs/specs/profiling.md)，API 见 [API_REFERENCE.md](API_REFERENCE.md)，协议/统计见 [FORMAT.md](FORMAT.md)。本文命令默认从仓库根目录执行；临时构建、venv、报告和日志留在忽略的 build 目录或外部 evidence 目录，不提交生成物。

## 1. 开始前先确认什么

1. 阅读仓库 `AGENTS.md`、当前设计及本次变更对应合同；若在排错，先读 [调试经验](../../docs/moonep/MINDSPEED_DEBUGGING_EXPERIENCE.md)。
2. 记录 `git rev-parse HEAD`、`git status --short`；保留已有修改，不自动 checkout/reset。源码、Host library、外置 kernel 必须能追溯到实际构建来源。
3. 根据下表选择测试层。无 NPU 环境时先完成离线/Host 测试，将目标设备层记为未运行；不要自动连接集群或把替身当硬件。
4. 检查 tools 版本、Python 解释器和实际导入路径。安装成功、`--help` 成功、CTest 枚举成功都不等于行为测试通过。

| 层级 | 所需环境 | 能证明 | 不能证明 |
|---|---|---|---|
| P：离线包 | Python ≥3.9；打包另需 build/setuptools/wheel | 解析、配对、统计、报告、独立发行消费 | ACL、设备行为、真实 timer 换算 |
| H：独立 SDK | CMake ≥3.16、C++14 工具链；round-trip 另需 Python | ABI/布局、CPU writer、mock 生命周期、安装消费；Linux 上另含文件分支 | CANN 地址空间/指令、NPU 可见性与性能 |
| C：CANN 编译 | Linux、实际 CANN 9.1、目标编译工具 | 真实 ACL 头/链接；Add4 构建另覆盖其实际设备编译 | 成功编译不证明已运行、数值或低开销 |
| N：Add4 硬件 | 明确选定的 Ascend950/Linux、真实 CANN/driver、已确认 timer 换算 | 独立单 kernel 数值、4 lane 记录、报告和搬迁消费 | 多 rank、其他 SoC、A5 UDMA、生产异步保持 |
| I：生产集成 | 配套 Host/Kernel、多 rank 设备和业务环境 | collectives 数值、资源/重试、rank 阶段、异步和采样边界 | 未使用的数据面、设备或模式 |

## 2. 当前已有证据与未关闭边界

以下是 **2026-10-20、实现提交 `73e4c9d7`** 的历史本地结果，不自动适用于后续代码、别的机器或未执行的 CI。完整命令及接续修复见 [整改计划第 9 节](../../docs/plans/PROFILING_CLEANUP.md#9-2026-10-20-实现接续与本地验收)。

| 检查 | 已记录结果 |
|---|---|
| Python 3.12.14 | 81 项通过；A5 mapping 另 3 项通过 |
| 独立 SDK/CPU stub/mock | 编译通过；排除下述系统阻止项后，全 CTest 12/12 通过，含 Python suite 和往返测试 |
| 安装与发行 | SDK 仓外 C++14 executable/shared library/多 TU consumer 通过；wheel 与 sdist 重建 wheel 各自在独立 venv、仓外 cwd、清 PYTHONPATH、`-I` 下通过 CLI 0/1/2 与 API 三报告 |
| A5 参数与开关 | C++14 layout/192-byte KernelArgs ABI 通过；非法宏值 2 被编译器按预期拒绝 |
| 默认测试发现 | 有 Python 时枚举 13 项，无 NPU 测试；枚举本身不代表执行 |
| shell | 两个入口分别 Bash `-n` 通过；不等于整套 Linux CI 通过 |
| Windows 阻止项 | `profiling_switch_host_1` 编译成功，但在 main 前被应用控制策略禁止启动；完整 Host CTest 为 11/12，不能称全绿 |

仍需单独取证：Linux hard-link/O_EXCL 并发/故障分支、Python 3.9、真实 CANN/设备编译开关、NPU GM 可见性、Add4 数值及搬迁、生产多 rank/48 lane/异步保持、marker/flush 开销与 spill。GitHub workflow 已配置，不表示已执行通过。

## 3. P：离线 Python 与独立发行

已有构建依赖时，从源码安装并运行行为测试：

```bash
python3 -m pip install --no-build-isolation --no-deps -e tools/profiling
python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
python3 -B tests/collectives/unit/test_alltoall_a5_profiling_mapping.py
```

只做源码测试且不安装包时，可对每条命令设置 `PYTHONPATH=tools/profiling/python`。不要依赖旧 `tools.profiling` 导入或恰好处于仓库 cwd；单独运行 CTest 时也要核对它启动的解释器/导入环境。

Windows PowerShell 的无安装测试入口（`python` 指向待验证解释器）：

```powershell
$profilingPreviousPath = $env:PYTHONPATH
$env:PYTHONPATH = Join-Path (Get-Location) 'tools/profiling/python'
try {
    python -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
    if ($LASTEXITCODE -ne 0) { throw 'profiling Python tests failed' }
    python -B tests/collectives/unit/test_alltoall_a5_profiling_mapping.py
    if ($LASTEXITCODE -ne 0) { throw 'A5 mapping tests failed' }
} finally {
    $env:PYTHONPATH = $profilingPreviousPath
}
```

发行验证前准备 `build`、`setuptools>=61`、`wheel`。runner 使用本地构建产物，不从公网安装待测包；传一个可写 evidence 根目录，每次运行在其下创建独立工作目录：

```bash
python3 scripts/ci/check_profiling_python.py --work-dir build/profiling-package-evidence
```

通过标准是结尾 `PASS: wheel and sdist install independently; isolated CLI 0/1/2 and API reports verified.`，并保存完整日志。runner 实际生成完整、不完整、损坏 fixture，检查安装来源与三报告；只检查 wheel 存在或 CLI help 不足。最低 Python 3.9 与 Python 3.12 要分别跑，参见 [workflow](../../.github/workflows/profiling.yml)。

## 4. H：无 CANN 的 SDK 行为与安装消费

此入口不需要初始化 TileXR 子模块或配置整个根项目：

```bash
cmake -S src/profiling -B build/profiling-host \
  -DTILEXR_PROFILING_BUILD_TESTS=ON -DTILEXR_PROFILING_WITH_ACL=OFF
cmake --build build/profiling-host --parallel 2
ctest --test-dir build/profiling-host -N
ctest --test-dir build/profiling-host --output-on-failure -L profiling_host
ctest --test-dir build/profiling-host --output-on-failure -L profiling_python
```

多配置 generator 在 build/ctest 命令中分别补 `--config Debug` / `-C Debug`。CMake 未发现 Python ≥3.9 时会明确跳过 Python suite 与 round-trip；不能用变少的通过数量声称它们已验证。

| CTest 名称 / 入口 | 核心断言 | 源码证据 |
|---|---|---|
| `profiling_sdk_host` | 预算/边界、对齐、非对齐有效 count、完成状态、timestamp=0、stable ties、v1 编码；Linux 文件故障和多进程单赢家发布 | [test_host.cpp](../../tests/profiling/sdk_host/test_host.cpp) |
| `profiling_sdk_acl_mock` | context/stream 归属、初始化清理、同步/D2H/free 重试、disabled、移动、snapshot 和分配字节数生命周期 | [test_acl.cpp](../../tests/profiling/sdk_host/test_acl.cpp) |
| `profiling_capture_queue_mock` | 有界队列、耗尽不阻塞业务、后续 acquire 无采集/释放、Drain 重试；共同清理阶段与可选 owner 分离 | [test_capture_queue.cpp](../../tests/profiling/sdk_host/test_capture_queue.cpp) |
| `profiling_device_protocol` | 真实 writer 头 + CPU stub；稀疏/空槽、复写槽、溢出、零/最大时间戳、非法/复用 lane 和 ABI 边界 | [test_device.cpp](../../tests/profiling/sdk_device/test_device.cpp) |
| `profiling_switch_host_{undefined,0,1}`、`profiling_switch_device_{undefined,0,1}` | 六种配置编译并运行；device 组仍是 CPU stub | [test_switch.cpp](../../tests/profiling/sdk_device/test_switch.cpp) |
| `profiling_sdk_installed_consumer` | 复制发布文件→安装→移除 staging 源树→仓外 executable/shared library/多 TU 构建运行，只提供 prefix | [installed_consumer.cmake](../../tests/profiling/sdk_host/installed_consumer.cmake) |
| `profiling_sdk_python_roundtrip` | 实际 Recorder/DirectWriter→mock ACL D2H→snapshot/v1→Python reader/三报告；含稀疏槽、ties、overflow、空提交及未提交 | [check_roundtrip.py](../../tests/profiling/sdk_host/check_roundtrip.py) |
| `profiling_python` | P 层 unittest suite；主要覆盖协议、配对、统计、provenance、输出和文档 mapping 示例 | [python tests](../../tests/profiling/python) |

round-trip 同时属于 `profiling_host` 和 `profiling_python`，按两个标签运行会重复它。需要一次运行所有已注册检查时，可直接 `ctest --test-dir build/profiling-host --output-on-failure`。

非法宏值为编译失败合同，当前不单独注册 CTest。按 [device README](../../tests/profiling/sdk_device/README.md) 的编译方式将宏设为 `2`，要求失败原因是 0/1 static_assert；缺编译器/头文件导致失败不算通过。

Windows 应用控制策略若拒绝启动，记录具体 exe、错误与发生阶段，标为环境不可运行；不得改名或关闭策略绕过。为继续检查其他项可显式排除，但最终结论必须同时保留原失败项。Linux 文件分支在 Windows 不执行，即使 `profiling_sdk_host` 通过也不能关闭它们。

## 5. C/N：真实 CANN 编译及 Add4

仅在明确支持的 Linux/CANN 环境执行。先 source 实际 CANN 9.1 环境，配置真实 SoC；保留 compiler、CANN、driver、firmware 和实际设备身份：

```bash
cmake -S src/profiling -B build/profiling-cann \
  -DTILEXR_PROFILING_BUILD_TESTS=ON \
  -DTILEXR_PROFILING_WITH_ACL=ON \
  -DTILEXR_PROFILING_BUILD_EXAMPLES=ON \
  -DTILEXR_PROFILING_SOC=Ascend950
cmake --build build/profiling-cann --parallel 2
ctest --test-dir build/profiling-cann --output-on-failure -L profiling_cann
cmake --install build/profiling-cann --prefix "$PWD/build/profiling-prefix"
```

`profiling_cann` 当前只注册 `profiling_acl_compile`，证明 ACL 编译/链接；Add4 的设备编译来自 build 阶段，不是这个标签本身。只需 ACL 编译时省略 BUILD_EXAMPLES/SOC。**当前无自动注册的 NPU 测试**，包括启用 Add4 后；`ctest -L profiling_npu` 找不到测试不能当作通过。

随后按 [Add4 README](../../examples/profiling/README.md) 将安装示例复制到仓外，以安装 prefix 构建。设置其必需环境变量，尤其是真实 `TILEXR_PROFILING_CYCLES_PER_US` 和 `TILEXR_PROFILING_CLOCK_BASIS`，再从复制的目录执行：

```bash
bash run_demo.sh /path/to/new-evidence-directory
```

runner 要求输出路径不存在，不能重复覆盖。它自行构建并执行；它的 build 路径与上面的 CMake 构建不是同一个证据。保存 runner 实际使用的 binary 和编译参数。验收检查：

- `operator.log` 有数值校验成功，4 个 COMMITTED lane × 20 records，dropped=0；只出现“文件已写出”不足。
- records 与 mapping 被 canonical 包读入，报告 complete、三产物均存在，UUID/时钟来源/环境信息一致。
- `environment.txt`、实际外置 kernel 路径及构建日志可追溯；主机程序未嵌入 binary，runtime 解析真实 driver 库。
- 按 README 手动安装例子、移动整个安装 prefix 后，在新位置再运行并核对实际加载路径。runner 本身不证明安装后搬迁。
- 对目标设备另行验证 profiling 宏未定义/0/1 的编译；当前 Add4 构建固定为 1，不能用它代替完整开关矩阵。

开销/spill 需在同一设备、输入和同步边界下比较“编译禁用”“仅本地 marker”“marker+flush”三组，保存原始时长、样本分布和编译资源/spill 证据。当前没有自动性能 runner；没有这些证据时不下“低开销/无 spill”结论。

## 6. I：collectives 生产接入验收

根项目依赖按 [BUILD_VERIFICATION.md](../../docs/BUILD_VERIFICATION.md) 准备；业务命令、支持拓扑和正确性工具见 [collectives README](../../tests/collectives/README.md)。此处列必须保留的测试合同，避免复制可能漂移的完整启动参数。

| 场景 | 应核对的证据 |
|---|---|
| profiling OFF / ON | 同输入业务数值一致，Host/Kernel 来自成对构建；记录实际开关和 binary |
| warmup 与采样 | warmup 无 capture；开启后每次 capture 对应一次 kernel，不按事件次序猜 iteration |
| 预算/形状变化 | 超过最多 4 captures/64 MiB 或无匹配 stream/lane 时，有跳过诊断且业务继续；不要求每次调用都有文件 |
| 普通/A5 lane | 普通为实际参与 AIV 数；A5 为 48 lane，不能套 blockDim=33；同时核对业务数值及 required ranks |
| 异步保持 | 分开记录首次准备和后续 Acquire；后续 launch 不因采集插入同步/D2H/export/free；在原安全清理点才等待 |
| 清理与故障 | 发布/Close 失败可重试，成功项不重复；rank 可选 owner 不同仍进入同一共同阶段，context/stream 最后销毁 |
| rank 文件 | 同组分发 fresh seed，匹配启动次序/采样配置；逐个 capture 目录对照外部 required-rank 清单，禁止混 UUID |

分析命令可参考 [根 README](../../README.md)，重复 `--records` 仅用于同一次 capture 的不同 rank。现有离线格式没有参与者 manifest；提供部分 rank 仍可能退出 0，必须由测试方额外检查所需 rank 是否齐全。A5 layout 与 mapping 回归只能证明参数/事件合同，不能代替真实 launcher、通信或异步运行。

仓中 collectives mapping 当前写有 `cycles_per_us=1000.0`，这只是输入文件的值，不是 timer 自动探测或跨 SoC 保证。目标换算不同则在 evidence 目录生成对应 mapping 并保留换算依据，再进行分析；不得直接用默认文件制造看似合理的时延结论。

通信路径排错前先查同环境已留存的 HCCL Test 一次性资格记录，严格按 AGENTS 的环境定义复用，不为本任务重复跑。纯离线/Host/Add4 不需要通信资格。若扩到 MoonEP，保持其公开 API、group_credit 默认模式、框架 profiler 和 stage barrier，并满足跨 rank min/median/max 等性能报告要求；不要引入 legacy 路径。

## 7. 变更后该选哪些回归

| 修改范围 | 最小相关集合 | 上层仍需补的证据 |
|---|---|---|
| types/layout/writer/encoder | H 的 host/device/switch/round-trip；P 的 records/protocol | C/N 开关、可见性及必要开销/spill |
| AclCapture/文件发布 | H 的 host/acl_mock/queue/round-trip，Linux 文件分支 | 真实 ACL、context/stream 错误恢复 |
| collectives launcher/kernel/清理 | queue mock、A5 layout/mapping、相关 H 测试 | 成对 CANN 构建、多 rank 数值/异步/重试 |
| mapping/pairing/analysis/provenance/renderers | P 全 suite；相关 round-trip | 真实记录回放、三报告人工核对 |
| 包/CMake/安装依赖 | SDK installed consumer + wheel/sdist runner | 干净环境、最低 Python、Linux/目标安装消费 |
| Add4/runner | shell 语法、mapping parser、相关 SDK 测试 | N 层数值/记录/报告、安装搬迁 |

不要新增匹配生产源码 token 的守卫；用编译、链接、导入、协议 fixture 和行为验证。shell 多文件语法扫描要逐文件调用，`bash -n file1 file2` 只解析第一个。API 的 None/空非法值、有效 count/对齐、空提交/未提交、发布警告/失败应分别覆盖。

## 8. 交接证据模板与 AI 接续约束

每次验收至少记录下表，结果使用“通过 / 失败 / 未运行”，不把未运行写成通过。日志与 binary 放 evidence 目录，维护文档保留索引和结论即可。

| 字段 | 填写内容 |
|---|---|
| 变更来源 | commit、dirty diff、源码路径；Host/kernel/package 实际来源和版本 |
| 环境 | OS/架构、编译器/标准、Python/导入路径、CANN/driver/firmware；硬件运行加设备子集/模式/拓扑 |
| 合同与命令 | 本次要证明的合同、完整命令、开关、cwd、起止时间与退出状态 |
| 产物与断言 | 日志路径、数值正确性、lane/rank/UUID、完整性诊断、三报告；文件/故障测试的具体断言 |
| 未覆盖 | 替身范围、缺失解释器/硬件/权限、跳过或阻止的测试、仍待关闭门槛 |
| 后续 | 第一处失败边界、最小下一步；如做性能比较，记录同配置与统计口径 |

AI 接续时可直接采用以下任务描述，并补充实际环境：

> 阅读 AGENTS.md、docs/specs/profiling.md、tools/profiling/TESTING.md 及对应源码。先检查当前 Git 状态和可用环境，按本次改动选择相关测试。区分 P/H/C/N/I 证据，不把历史 73e4c9d7 的结果当作本次运行。保留已有修改和业务合同；不绕过系统策略，不自动连接未授权环境，不猜 timer 换算或缺失 rank。报告实际命令、断言、产物及未覆盖项。需要调试时先定位第一处失败边界，再修改实现与对应维护文档。

计划 ID 的全量关闭标准继续由 [整改计划](../../docs/plans/PROFILING_CLEANUP.md) 维护；一次局部测试通过不自动关闭 I01–I06 或延期 E 项。
