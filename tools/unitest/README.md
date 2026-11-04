# UniTest：TileXR 算子校验与性能测量

UniTest 用于运行已安装的 TileXR EP Memory 算子，检查数值结果、重复执行稳定性和 Host batch 性能，并按需采集 profiling 数据。支持独立 Dispatch、独立 Combine，以及直接使用 Dispatch 设备输出的 Dispatch → Combine 链路。

## 支持范围与环境要求

- 当前算子要求 A5 / Ascend950，使用 `memory` backend、BF16 和 `quant_mode=0`。
- 本地运行选择 2–8 个设备。跨机通过 SSH 使用已安装的框架和生产库，各节点设备数须相等；软件接受 2–128 个全局 rank，实际可用规模取决于算子、环境和已有验证。
- 构建需要 CMake 3.16+、C++14 编译器、Python 3.8+、兼容的 CANN 开发环境和 TileXR 依赖。Python controller 只使用标准库。
- 项目兼容基线为 CANN 9.1.0、驱动 25.1.rc1 或更新的兼容版本。运行还需要真实驱动 HAL、匹配的设备二进制和空闲 NPU。

当前不支持 Server Dedup、量化、UDMA adapter 或设备 Barrier。多节点执行和具体硬件版本的验证范围见[验证记录](../../docs/validation/unitest.md)；可配置不代表已完成实机验收。

## 构建与安装

### 随 TileXR 一起安装

在仓库根目录执行：

```bash
git submodule update --init 3rdparty/spdlog 3rdparty/picojson
source scripts/common_env.sh
cmake -S . -B /tmp/tilexr-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/tilexr \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_TESTING=OFF -DTILEXR_BUILD_TESTS=OFF
cmake --build /tmp/tilexr-build -j "$(nproc)"
cmake --install /tmp/tilexr-build
```

将 `/opt/tilexr` 换成有写权限的目录。`TILEXR_BUILD_UNITEST` 默认 `ON`，会同时构建所需的 core、diagnostics、EP 和设备文件，即使 `TILEXR_BUILD_EP=OFF`。它不受 `BUILD_TESTING` 或 `TILEXR_BUILD_TESTS` 控制。只需要核心库时设置 `-DTILEXR_BUILD_UNITEST=OFF` 并关闭其他可选模块。

### 使用已有 TileXR 安装独立构建

已有完整、兼容的 TileXR 安装时，只构建 UniTest：

```bash
git submodule update --init 3rdparty/picojson
cmake -S tools/unitest -B /tmp/tilexr-unitest-build \
  -G 'Unix Makefiles' \
  -DTILEXR_INSTALL_PREFIX=/opt/tilexr \
  -DTILEXR_LIBRARY_DIR=/opt/tilexr/lib \
  -DASCEND_HOME_PATH="$ASCEND_HOME_PATH" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/tilexr-unitest"
cmake --build /tmp/tilexr-unitest-build -j "$(nproc)"
cmake --install /tmp/tilexr-unitest-build
```

该模式不重建生产库或 Kernel。TileXR 安装须包含公开头文件、EP/core/diagnostics Host 库、传递依赖及配套 `tilexr/kernels/*.o`。不要只复制两个 `.so` 或混用其他安装的设备文件。实际库目录为 `lib64` 时相应修改 `TILEXR_LIBRARY_DIR`；非默认 CANN 平台目录可用 `-DUNITEST_CANN_ARCH=aarch64` 等参数指定。

安装后包含 `bin/tilexr-unitest`、`libexec/tilexr-unitest/tilexr_unitest_worker`，以及 `share/tilexr-unitest/` 下的 controller、cases、configs 和 README。可以移动整个框架安装目录，运行不依赖源码目录。

## 首次运行

加载兼容的 CANN 和驱动运行环境，选择要测试的 TileXR 库目录：

```bash
export LD_LIBRARY_PATH="/opt/tilexr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
framework=/opt/tilexr
"$framework/bin/tilexr-unitest" --help
"$framework/bin/tilexr-unitest" --case ep_dispatch --devices 0,1
"$framework/bin/tilexr-unitest" --case ep_combine --devices 0,1
"$framework/bin/tilexr-unitest" --case ep_dispatch_combine --devices 0,1
```

独立安装时将 `framework` 改为框架安装前缀，库路径仍指向待测 TileXR。不要把 CANN `devlib` 链接桩放入运行时 `LD_LIBRARY_PATH` 或 RPATH/RUNPATH。

`--help` 不加载 Worker 或 CANN，不能据此判断设备可运行。实机执行前确认设备授权、空闲状态和对应通信环境的资格记录；复用已有匹配 HCCL Test 记录，无需每次重跑。环境检查方法见[构建与验证指南](../../docs/BUILD_VERIFICATION.md)。

## 选择执行模式

| 模式 | 用途 | 主要选项 |
|---|---|---|
| `single` | 执行并校验一次 | 默认模式 |
| `correctness` | 连续执行，在累计检查点校验 | `--iterations`、`--check-schedule` |
| `perf` | 正确性预检、warmup、分批计时与校验 | `--warmup`、`--iterations`、`--batches` |

```bash
"$framework/bin/tilexr-unitest" --case ep_dispatch_combine --devices 0,1 \
  --mode correctness --iterations 100
"$framework/bin/tilexr-unitest" --case ep_dispatch_combine --devices 0,1 \
  --mode perf --warmup 5 --iterations 100 --batches 3
```

也可用 `--config "$framework/share/tilexr-unitest/configs/perf.json"` 加载运行配置。逐字段优先级为显式 CLI > config > 默认值；改变轮次不需要重新编译。

`correctness` 默认检查累计 1、2、4、8……以及最后一轮；100 轮时为 1/2/4/8/16/32/64/100。`--check-schedule every_iteration` 可逐轮校验。检查点通过只说明该检查点输出正确，不能证明段内每一轮的中间状态。

`perf` 默认 warmup=5、iterations=100、batches=3。`host_batch_average_us` 是每批真实 API 调用及最终 stream sync 的总耗时除以 iterations，包含 Host 提交和设备等待，并非 Kernel Duration。对于 Dispatch → Combine，一次 iteration 是完整链路，不是两个独立样本。报告保留各 rank、各 batch 及跨 rank 的 median/min/max；不从该指标推导设备带宽或跨主机全局 makespan。

## 自定义用例

复制 `cases/` 中的 JSON 示例，修改后通过路径运行：

```bash
"$framework/bin/tilexr-unitest" --case /path/to/case.json --devices 2,3 \
  --output-dir /tmp/unitest-dispatch-run \
  --init-timeout 300 --timeout 120 --exit-timeout 60
```

输出目录必须不存在。`--init-timeout` 控制初始化，`--timeout` 控制各执行阶段，`--exit-timeout` 控制退出等待，单位均为秒。

用例使用 `tilexr.unitest.case.v1` schema。输入字段包括 `bs`、`h`、`topk`、`moe_expert_num`、`seed`，并通过 `operator` 或有序 `steps` 指定算子。可选 `world_size` 必须等于实际全局 rank 数。未知字段和不支持的组合会在启动前拒绝。

| 字段 | 当前范围 |
|---|---|
| `bs` | 1–256 |
| `h` | 16–4096，且为 16 的倍数 |
| `topk` | 1–8，且不超过 expert 数 |
| `moe_expert_num` | 1–128，能被 rank 数整除，每 rank 最多 32 个 |
| `seed` | 0–2³²−1 |

独立 Combine 和 Dispatch → Combine 都要求 `bs * topk >= moe_expert_num`。链路用例可参考 `cases/ep_dispatch_combine.json`：Combine 直接消费前一步的设备 payload、assist 和 counts，最终校验 `combine.output`，不以 Host 生成的中间数据替代。

## 采集 profiling

profiling 默认关闭，可通过 CLI 或运行配置开启。数值校验和工具执行结果分别记录；工具失败也会使整体运行失败。

### 官方 msprof

例如为 Dispatch single 采集 profiling：

```bash
"$framework/bin/tilexr-unitest" --case ep_dispatch --devices 0,1 \
  --profiling --profiling-tool msprof
```

需要可用的官方 ACLprof 与 `msprof`。single/correctness 从首次执行前开始采集；perf 从预检和 warmup 之后开始，到最后一批校验后结束。因此采集区间还包含批间准备和校验，并非纯计时窗口。

此模式仅支持 `selection.kind=all`，不接受指定轮次或非空自定义 options。每 rank 保留原始采集目录、API 记录和官方导出日志。导出命令退出 0 不代表所有 Kernel 样本已完整，完整性需单独核实。

### 内部 profiling

用 `-DTILEXR_EP_ENABLE_PROFILING=ON` 配套重建并安装生产 Host 库及 Kernel，同时安装分析工具：

```bash
python3 -m pip install /path/to/TileXR/tools/profiling
```

例如将以下内容放入 `perf` 模式的 `tilexr.unitest.run.v1` 运行配置的 `profiling` 字段，采集第二批的第二轮：

```json
{
  "enabled": true,
  "tool": "internal",
  "options_file": "internal-options.json",
  "selection": {"kind": "selected", "rounds": [{"batch": 2, "iteration": 2}]}
}
```

`internal-options.json` 必须提供经确认的 `cycles_per_us` 和非空 `clock_basis`，不能猜测设备时钟。可选 `max_workspace_bytes` 默认每 rank/整次运行 64 MiB，最大 1 GiB。config 中的相对 options 路径以 config 所在目录解析；CLI 指定的路径以当前目录解析。

`{"kind":"all"}` 采集全部实际执行轮次。`selected` 的轮次从 1 开始；perf 指定 batch 和 iteration，single/correctness 只指定 iteration。perf 的预检和 warmup 不采集。所选链路中 Dispatch/Combine 分别生成 capture，仅覆盖通用 begin/end，不代表原有算子内部阶段全部可见。

同一通信域必须串行提交并使用同一执行 stream，不混入额外消耗 magic 的调用。预算不足、缺少 rank/capture、记录不完整或分析工具失败都会报错。开启采集的性能结果包含打点开销；时钟未经校准时，不应宣称准确阶段延迟或跨 rank 时间对齐。

## 跨机执行

控制器在 POSIX/WSL 上运行，通过 SSH 访问已安装相同兼容框架和生产库的节点。UniTest 不部署、传源码或重建远端环境。SSH 连接信息复用 `tools/tssh/clusters.json`，placement 选择节点、设备及远端安装路径。

```bash
"$framework/bin/tilexr-unitest" --case ep_dispatch_combine \
  --config "$framework/share/tilexr-unitest/configs/perf.json" \
  --inventory /path/to/TileXR/tools/tssh/clusters.json --cluster CLUSTER \
  --placement /path/to/placement.json --comm-id RANK_ZERO_DATA_IPV4:PORT
```

placement 使用 `tilexr.unitest.placement.v1`，`hosts` 中每项包含：`name`（inventory 中的名称）、`devices`、`framework_prefix`、`environment_script`、`runtime_library_dirs` 和 `output_root`。路径为节点上的绝对 POSIX 路径，output_root 需预先存在。库路径包含真实驱动依赖，不可使用 devlib。rank 按 hosts 顺序和 devices 顺序分配，各节点设备数必须相等。

`--comm-id` 指向 rank 0 节点可绑定、且所有节点可达的数据网 IPv4 和端口。各 rank 使用本地时钟测量，不跨主机相减时间戳。SSH 断开时请求 Agent 清理本次进程；网络中断下，远端清理是否完成仍需独立确认。真实跨机数据面验收范围见[验证记录](../../docs/validation/unitest.md)。

## 结果与故障处理

默认结果写入 `./unitest-results/<UTC时间>-<唯一标识>/`，可通过 `--output-dir` 指定新目录：

- `case.json`、`run.json`：实际用例、解析后的配置及来源。
- `result.json`：整体状态、rank 映射、数值结果、性能、工具执行与错误信息。
- `rank-N.log`：各 Worker 的诊断输出；远端执行另保留节点日志和产物路径。
- profiling 产物：原始数据、capture manifest、工具命令和导出/分析日志，按采集方式保存。

`passed` 返回 0；运行失败、超时、校验不一致或中断返回非零。参数错误通常返回 2，并在创建运行报告前说明原因。任一 rank 失败使整体失败，尽可能保留部分结果。本次创建的 Worker 会被终止，不自动重试，也不清理其他工作负载。无法确认设备工作完成时，不将资源视为可安全复用。

| 现象 | 检查方法 |
|---|---|
| 缺少 Host 动态库 | 查看 rank 日志、`LD_LIBRARY_PATH` 和 `ldd`，确认选择了同一套兼容安装 |
| 缺少 Kernel | 检查所加载 Host 库旁的 `tilexr/kernels/`，不要混搭设备对象 |
| 链接报 `cannot find -lascend_hal` | 核对驱动和 CANN 环境；仅编译主机的 SDK 链接桩用法见[构建说明](../../docs/BUILD_VERIFICATION.md#unitest-link-only-hosts) |
| 内部采集被拒绝 | 核对 profiling 构建开关、Host/Kernel 配套、时钟依据、轮次和预算 |
| `ep_dispatch_barrier` 返回 unsupported | 当前没有适用的公开设备 Barrier API，Host ready 或 stream sync 不能替代 |
| 端口或 SSH 连接失败 | 核对空闲端口、数据网地址、SSH 配置和各节点环境；失败不会自动换端口或重试 |

架构和测量契约见 [UniTest 设计](../../docs/specs/unitest.md)。历史构建、实机证据及未验证项见[验证记录](../../docs/validation/unitest.md)。
