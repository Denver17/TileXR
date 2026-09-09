# TileXR EP Dispatch/Combine Tests

This tree tests the standalone TileXR EP module under `src/ep`. It is independent from hcomm, HCCL window helpers,
and `ops-transformer`. The memory backend uses TileXR peer-memory windows for both same-node and cross-node
dispatch/combine, while the UDMA backend uses TileXR-registered workspaces. Memory generally performs better for
small transfers, while UDMA generally performs better for large transfers.

All EP Dispatch and Combine variants are supported only on A5-class SoCs
(`Ascend950`, `Ascend910_9`, or `A5`). The build produces the four existing A5
AICore objects plus the empty Server Dedup object, and never produces 910B EP kernels. The public API symbols remain
available in the same Host library on non-A5 systems and return
`TILEXR_ERROR_NOT_SUPPORT` before loading a kernel or consuming launch state.

Internal Memory classes, profiling artifacts, trace tools, and test helpers use unversioned names. Version suffixes
are reserved for public ABI compatibility and MoonEP-owned identifiers.

## Host Tests

服务器验证统一使用[EP 七步验证](../../docs/EP_VALIDATION_GUIDE.md)：PC 固定提交并同步后，
依次执行根目录 `source set_env.sh`、`bash build_all.sh`、`bash sync.sh`、`bash run.sh`。
当前支持 `abi-host`、`empty-kernel`、`dry-run` 和 `chain-single`；沿用现有配置，脚本自动汇总各主机结果。
检查点状态与通过提交见[调试进度](../../docs/EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。

From `tests/ep`:

```bash
source ../../scripts/common_env.sh
bash build.sh source-only
./install/bin/test_tilexr_ep_layout
./install/bin/test_tilexr_ep_host_validation
```

`source-only` mode builds and installs host layout and validation tests without
building the hardware demo. CCE compilation and hardware correctness must still
be validated separately.

## Server Dedup 第一阶段检查点

独立入口 `TileXRMoeEpDispatchMemoryServerDedup` 使用 caller-owned 的
`assistExOut/sourceMaskOut/groupOut`，普通 Memory ABI 不变。第二个检查点已接通 A5 空 kernel：
通过 Host 校验后注册独立 artifact，再获取一次 magic 并调用 `rtKernelLaunchWithFlagV2`。
成功仅表示异步提交成功；空 kernel 不读取输入或 comm，不写输出、ready 或 profiling。

开发者可在无 NPU 环境运行现有 Host suite；服务器交付仍使用上面的七步入口：

```bash
cmake -S tests/ep -B tests/ep/build-sdedup -DBUILD_TILEXR_EP_DEMO=OFF
cmake --build tests/ep/build-sdedup -j4
timeout 180s ctest --test-dir tests/ep/build-sdedup --output-on-failure
```

Host 测试沿用已有 ACL/runtime 替身。`public_abi` 只验证公共声明与调用边界；
`test_tilexr_ep_kernel_launch` 编译真实注册和 launch 源码，检查全部 20 个参数、默认/显式核数、
FP16/BF16、可空 weight/旧 assist、非 A5、artifact 缺失、注册失败和提交失败。
`TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM` 默认使用设备 Vector 核数，覆盖值范围为 `[1,Vector核数]`，
不沿用普通 Dispatch 的双 Group 限制。注册失败不消耗 magic；提交失败已消耗本次 magic，不能回退重用。

`empty-kernel` 的 production 构建额外编译安装包 consumer，仅使用已安装公共头和真实 Host 库。
它在每台主机的首个配置 device 上执行真实 binary/function 注册，复用 Host 库相对路径加载；不创建 communicator。
ELF/导出/依赖检查仍只在构建完成后执行一次；同步、超时、清理和结果汇总复用原流程。
维护时将新检查加入现有测试及 manifest，不另建 runner、重复预检或源码摘要门禁。

容量仍为 `R*BS*min(K,L)` 行，AssistEx 每行 32B、sourceMask 每项 4B、group 每行
`align32(4*(1+2*(K-1)))`，按最新设计预留 **8*BS 行**。裸指针入口只检查可见对齐与 shape；内部 allocation helper 检查声明容量、
设备和区间不重叠。不能借用要求旧三字段 assist、拒绝 weight 的普通 Memory 校验器。
空算子不扫描 route，不生成 metadata，也不确定后续通信容量或 Combine 契约。

`chain-single` 在 production 模式编译安装 `test_tilexr_ep_server_dedup_chain`，由同一 controller 每台启动 8 个进程；不增加每节点 CTest 项。它创建已有 MemoryDomain communicator，验证 Host/device peer window，调用一次公开空 Dispatch，检查 stream 同步和两次 TCP Host barrier，最后检查资源释放。空 kernel 不使用旧 Dispatch 的 2 GiB 分区；窗口检查采用实际 MemoryDomain 的 8 MiB 数据区加 4 MiB flag 区。异步完成或轮末 barrier 未确认时，进程报错退出，不提前释放可能仍被使用的资源；已有 controller 负责有界退出和日志收集。

本次本地验证：MSYS2 的 C++14 Debug 构建与 17/17 EP Host/Python 测试通过；七步脚本回归 115 项中 112 项通过、3 项因 MSYS symlink 限制跳过。`python3 tests/ep/integration/test_server_dedup_chain.py -v` 的 5 项行为测试编译真实 consumer，使用 ACL/communicator 替身及真实 TCP barrier，覆盖两种 dtype 的 16 进程单轮和失败路径。本次 CANN 9.1 生产构建、真实建链及 NPU launch 待服务器七步验证；设备结果按进度文档中的配置及通过提交分别记录。

## Full Hardware Demo

From `tests/ep`:

```bash
source ../../scripts/common_env.sh
bash build.sh full
bash demo/run_tilexr_ep_dispatch_demo.sh 2
```

`full` mode builds and installs `tile-comm` and `tilexr-ep` under the repository
`install` directory, then builds the EP demo. Dispatch, Combine, Dispatch Memory,
and Combine Memory are fixed A5 CCE objects installed under
`lib/tilexr/kernels` and loaded by `libtilexr-ep.so`; they are not embedded or
built as separate Host shared libraries. No `TILEXR_SOC_NAME` or EP SoC CMake
option is needed, including when the package is built on a 910B host.

The EP demo is linked without RPATH/RUNPATH. CANN `devlib` is a link-time fallback only; embedding it in the demo's
runtime search path can make the loader select the stub `libascend_hal.so` before the real driver HAL, causing
`aclInitImpl: init SoC version failed`. After linking, verify `readelf -d install/bin/tilexr_ep_dispatch_demo` has no
RPATH/RUNPATH and use `ldd install/bin/tilexr_ep_dispatch_demo` in the target runtime environment to confirm
`libascend_hal.so` resolves below `${ASCEND_DRIVER_PATH}` rather than CANN `devlib`. These checks prove library
provenance only; NPU execution is still required to validate the demo.

`tilexr_types.h` is also included by EP device sources. Keep Host-only standard-library includes and objects outside
`__CCE__`; Host tests do not replace a CANN 9.1 CCE compile.

The demo has exactly two execution backends, selected with `TILEXR_EP_DEMO_IMPL=udma|memory`. API version and
same-node/cross-node topology are handled inside the TileXR library and are not demo branches. Select the operator
sequence with `TILEXR_EP_DEMO_RUN_MODE=dispatch|combine|dispatch_combine`.

When the backend is not specified, the runner selects `udma` on Ascend950 and
`memory` on other A5-class SoCs. The hardware demo has no supported backend on
Ascend910B. Pass the fifth argument or set `TILEXR_EP_DEMO_IMPL` to override the
default on a supported SoC.

The runner accepts:

```text
run_tilexr_ep_dispatch_demo.sh rank_size npu_count first_npu loop_count impl bs h topk expert_ids \
  run_mode expert_mode expert_seed quant_mode mxfp8_format comm_quant_mode
```

`expert_ids` is a comma-, semicolon-, or space-separated list with exactly `bs * topk` entries. The same settings
can be supplied with `TILEXR_EP_DEMO_IMPL`, `TILEXR_EP_DEMO_BS`, `TILEXR_EP_DEMO_H`,
`TILEXR_EP_DEMO_TOPK`, `TILEXR_EP_DEMO_MOE_EXPERT_NUM`, `TILEXR_EP_DEMO_EXPERT_IDS`, `TILEXR_EP_DEMO_RUN_MODE`,
`TILEXR_EP_DEMO_EXPERT_MODE`, `TILEXR_EP_DEMO_EXPERT_SEED`, `TILEXR_EP_DEMO_QUANT_MODE`,
`TILEXR_EP_DEMO_MXFP8_FORMAT`, and `TILEXR_EP_DEMO_COMM_QUANT_MODE`. All values are runtime inputs; changing them
does not require rebuilding the operator.

The run modes behave as follows:

- `dispatch`: host constructs dispatch inputs and expected dispatch outputs, runs dispatch `loop_count` times on
  one communicator, and validates only the final device outputs.
- `combine`: host constructs `expertOut`, assist tuples, send/receive counts, and expected `yOut`, runs combine
  `loop_count` times, and validates only the final `yOut`.
- `dispatch_combine`: host constructs dispatch inputs and expected final output, runs dispatch `loop_count` times,
  then passes the final `expandXOut`, assist tuples, and counts directly to one combine call.

The expert modes are `uniform`, `random`, and `explicit`. `uniform` follows the commercial balanced-routing input:
experts are ordered by local expert index across destination MoE ranks, and EP rank `r` takes `bs * topk` entries
from the cyclic order starting at `r * bs * topk`. `random` samples `topk` distinct IDs from the full MoE expert
range for every global token; `expert_seed` makes the generated tables reproducible while different EP ranks use
different tables. `explicit` uses `expert_ids` and requires exactly `bs * topk` IDs. Expert IDs are in
`[0, moeExpertNum - 1]`, and `topk` must not exceed `moeExpertNum`. Shared-expert routes are generated separately
and are not included in these MoE expert-ID tables.

### MXFP8 Golden Tensors

The test-side MXFP8 golden path uses `quant_mode=4` and supports `mxfp8_format=e4m3|e5m2`. It mirrors the
`pta-moe-test-main/quantize.py::mx_quantize` contract:

- each 32 hidden elements share one E8M0 scale;
- the scale exponent is `floor(log2(maxAbs)) - emax`, with `emax=8` for E4M3 and `emax=15` for E5M2;
- MXFP8 element mantissas use round-to-nearest, ties-to-even;
- the per-row scale count is `align_up(ceil(h / 32), 2)`, with zero-block/padding scale byte `0x00`.

The host constructs expected routed `expandX` as FP8 bytes and expected `dynamicScalesOut` as E8M0 bytes in memory
dispatch row order. Dispatch MXFP8 remains restricted to the memory backend and dispatch-only mode until the
expert-compute/dequant stage is available. The byte-level golden implementation is covered by
`test_tilexr_ep_layout` for both E4M3 and E5M2.

Combine communication quantization uses the independent `comm_quant_mode` setting: `0` disables it, `3` selects
MXFP8 E5M2, and `4` selects MXFP8 E4M3. For combine-only tests the host fills every valid `expertOut` row from its
assist tuple's source rank/token, rather than using constant ones. The golden path pads hidden rows to 32 elements,
performs the MXFP8 quantize/dequantize round trip, multiplies MoE routes by deterministic FP32 `expertScales`, adds
shared-expert routes with scale 1, and accumulates in token/topK order before comparing `yOut`. The same golden is
used when non-quantized dispatch output is passed directly to combine. Nonzero `comm_quant_mode` is limited to the
memory backend and supports MXFP8 E5M2 (`3`) and E4M3 (`4`).

For example, run memory dispatch-only with deterministic uniform routes:

```bash
bash demo/run_tilexr_ep_dispatch_demo.sh 8 8 0 100 memory 4 256 2 '' dispatch uniform 1
```

Run UDMA combine-only with random routes generated from seed 2026:

```bash
bash demo/run_tilexr_ep_dispatch_demo.sh 8 8 0 100 udma 4 256 2 '' combine random 2026
```

Run dispatch+combine with an explicit expert list:

```bash
bash demo/run_tilexr_ep_dispatch_demo.sh 8 8 0 100 memory 4 256 2 \
  '0,1,2,3,4,5,6,7' dispatch_combine explicit 1
```

The memory dispatch path ports the A5 full-mesh dispatch algorithm and launches it in one kernel call. It supports
same-type FP16/BF16 dispatch and MXFP8 quantized dispatch (`quant_mode=4`) to E4M3 or E5M2. MXFP8 uses one E8M0
scale per 32 hidden elements, pads the scale count to an even number, and returns the scales through
`dynamicScalesOut`. The path currently requires TP size one. It supports ordinary MoE experts, shared experts
deployed on one or more ranks per shared expert, and token masks shaped `[bs]`.
The reference state/data windows and receive-count workspace are carved from TileXR IPC
peer memory internally; callers do not provide a workspace.

For the memory API, `sendCountsOut` keeps the reference cumulative expert-major layout. Its element count is
`epWorldSize` on a shared-expert rank and `epWorldSize * localMoeExpertNum` on a MoE-expert rank.

The memory combine path ports the A5 MTE combine flow into one kernel launch. It supports non-quantized FP16/BF16
and MXFP8 E5M2/E4M3 communication quantization, with TP size one, token masks, and shared experts. Expert masks,
TP, and AddRmsNorm remain unsupported. Run dispatch and combine together with:

```bash
TILEXR_EP_DEMO_IMPL=memory \
  bash demo/run_tilexr_ep_dispatch_demo.sh 2
```

The fourth argument is the loop count and defaults to `150`. In dispatch-only or combine-only mode it controls the
number of operator calls. In dispatch+combine mode it controls the number of paired rounds, with each dispatch
immediately followed by the combine that consumes its outputs. Each repeated call reaches a stream/rank completion
point before its communication window is reused, and only the final output is copied back. For example, this runs
100 paired memory dispatch+combine rounds:

```bash
bash demo/run_tilexr_ep_dispatch_demo.sh 8 8 0 100 memory
```

Set `TILEXR_EP_DEMO_LOOP=<n>` when using the environment instead of the fourth argument. Use a fourth argument of
`1` for a single dispatch+combine smoke test.

Set `TILEXR_EP_DEMO_PERF=1` for externally profiled performance runs. `TILEXR_EP_DEMO_WARMUP` controls the warmup
count and defaults to `50`; `loop_count` defaults to `150`. After warmup and a rank barrier, the demo submits all
measured iterations and synchronizes the stream once at the end. It does not insert ACL Runtime timing events into
the measured batch. Performance mode supports dispatch-only or combine-only execution, not `dispatch_combine`.

The current demo and Memory API accept `TILEXR_EP_DEMO_ACTIVE_MASK_TYPE=none|token`. The legacy
`TILEXR_EP_DEMO_ACTIVE_MASK=1` setting remains an alias for token masking. Expert-mask mode is unsupported by the
Memory backend.
Use `TILEXR_EP_DEMO_DTYPE=fp16|bf16` to select the input/output data type. For example:

```bash
TILEXR_EP_DEMO_IMPL=memory TILEXR_EP_DEMO_ACTIVE_MASK_TYPE=token TILEXR_EP_DEMO_DTYPE=bf16 \
  bash demo/run_tilexr_ep_dispatch_demo.sh 2
```

For sparse token masks, Dispatch compacts expert IDs for counting but preserves each active token's original token
index in the assist triple. Combine must skip flag polling for inactive tokens and write zero rows for them. When
Kernel UB buffers change, keep the Host reference estimator synchronized with every `InitBuffer` allocation; source
contract tests complement, but do not replace, target-toolchain compilation.

## Remote Verification

Configure a Mutagen session that synchronizes the repository to the intended remote path, then flush that session
before building or running:

```bash
export TILEXR_EP_MUTAGEN_SESSION=<mutagen-session-name>
export TILEXR_EP_REMOTE=<ssh-target>
export TILEXR_EP_REMOTE_REPO=<absolute-remote-repository-path>

mutagen sync flush "${TILEXR_EP_MUTAGEN_SESSION}"
ssh "${TILEXR_EP_REMOTE}" \
  "cd '${TILEXR_EP_REMOTE_REPO}' && \
   source scripts/common_env.sh && \
   bash tests/ep/build.sh full && \
   bash tests/ep/demo/run_tilexr_ep_dispatch_demo.sh 2 2 0 1 memory"
```

`mutagen sync flush` requires an existing session; it does not create or retarget one. The session destination must
match `TILEXR_EP_REMOTE_REPO`.

## UDMA Workspace

The UDMA backend always allocates and registers its aligned workspace. The same demo path is used for same-node and
cross-node runs; topology-specific behavior remains inside TileXR.
