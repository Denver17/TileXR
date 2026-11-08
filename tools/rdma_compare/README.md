# Matched QP1 AIV WRITE comparison

This harness prepares matching TileXR and NDA workloads. CPU validation and
compilation do not qualify NPU data transfer or bandwidth.

The default case is one QP, one SGE, 512 WQEs, unidirectional traffic, cyclic
source/destination addresses, MTU4096, GID1, independent UB DMA, hard doorbells,
batch64, signal every batch end and at most 128 WQEs in flight. Each sample uses
fresh processes/QPs. Source and destination spans are disjoint. `streaming`
instead advances both addresses by payload+64; `reverse` swaps sender direction;
`duplex` prepares a simultaneous-traffic plan, whose directional values and sum
must be reported separately. Its execution is rejected until a device start
coordinator establishes overlap: a Host barrier cannot qualify short-packet
overlap. Initial execution modes are forward/reverse. Payload sizes use powers
of two, 512B through 1MiB.

The matched QP1 kernel explicitly sets the application cap to 128 WRs to retain
the legacy NDA comparison contract. This is not the public WRITE default or
the multi-QP benchmark policy.

TileXR's `RDMAPutPipeline` is opt-in and single-SGE. It reserves a stop entry,
publishes the WQE tail before the valid head using MTE3, and rings through
`st_dev` after publication. The 4608B work scratch contains a 4096B descriptor batch
and separate stop/CQE/record storage; retained software state uses a separate UB context.
Both the control CR and task signal bits
are cleared on unsignaled descriptors. CQ consumers and WR ordinals are distinct.
The V3 view/queue are 168B/120B; the state header stays 64B and references QP-owned
pending metadata. Rebuild Host/library/kernel/headers together. Old APIs reject
a pipeline-tagged QP. A drained legacy QP may enter the pipeline, but a
fresh QP is required for matched samples. Full-call SQ/CQ no-wrap admission occurs
before posting. Errors retain outstanding ownership and poison the QP.

`RDMAPutPipeline`, `RDMAPutSgeListPipeline` and the public `RDMAWrite*`
adapters return after submission, without an unconditional tail drain. They
may still wait for the oldest batch under WR-window/SQ/record backpressure; this is
not a strictly non-blocking API. `TileXR::RDMAWait(view, scratch, maxPolls)` in
`tilexr_rdma_device.h` drains the selected QP's shared pending batches across
calls and MR views. It does not wait for other QPs or synchronize remote consumers.
Healthy empty/repeated waits succeed without CQ polling. Host Put/Get and
non-pipeline legacy device APIs retain their completion semantics.

`bench_kernel.cpp` explicitly waits after successful submission and before its
end timestamp, using the same scratch and 10000000-snapshot poll budget. WRITE
or Wait errors are returned in `BenchResult.status`; a failed sample is not a
bandwidth result. This preserves completed-transfer timing, workload and launch
timing semantics. Host and kernel must be rebuilt together after the appended
profiling descriptor ABI field. The submit/wait change alone establishes no device correctness or performance
gain; matching CANN compilation and hardware validation remain required.

Build the Host library first, then:

```sh
cmake -S tools/rdma_compare -B build-rdma-compare \
  -DTILEXR_ROOT="$PWD" -DTILEXR_INSTALL="$PWD/install"
cmake --build build-rdma-compare -j4
```

`bench ... --describe` prints its effective layout before any ACL call. Actual
execution requires `--run`. `RDMA_BENCH_CPU_ONLY` blocks execution. The executable
is deliberately a single measurement per QP; repetitions belong to the harness.
It calibrates the cycle counter against ACL events before measurement, checks
full source/destination/guard contents after both ranks finish, and records
directional kernel timing separately from Host launch-to-sync times.

An optional final `NUM_SGE` argument selects 1/2/4/8/16 segments:

```sh
bench RANK DEVICE BYTES COUNT DIRECTION ADDRESS_MODE KERNEL_ELF --run NUM_SGE
```

`BYTES` is fixed **per SGE**, so each WQE sends `BYTES * NUM_SGE`. Multi-SGE
sources have 64B gaps in reverse physical order and gather into one continuous
remote span. Their source and target strides differ. Cyclic slot count is based
on the aggregate bytes per WQE. Logs retain both byte counts and both strides;
reported Gb/s uses every segment's payload, excluding gaps and descriptor bytes.
SGE1 preserves the old layout and invocation. The multi-SGE pipeline requires
20992B UB, accounts SQ space in 64B blocks, and keeps CQ identities in WR units.
Rebuild the Host and pure AIV ELF together when the launch ABI changes.
`compare.py` remains the matched single-SGE TileXR/NDA harness; multi-SGE
experiments need a controller that passes the final argument to both ranks and
checks their complete case agreement and received data.

The reference NDA build needs the opt-in matched controls described in its
`docs/nda-matched-benchmark.md`; ordinary upstream builds are incompatible.
`compare.py` passes the same count/address mode to both peers and alternates
backend order. It defaults to writing `plan.json`, with no process launches:

```sh
python3 tools/rdma_compare/compare.py \
  --tilexr build-rdma-compare/bench --kernel build-rdma-compare/bench_kernel.o \
  --nda /path/to/matched/ib_write_bw --devices 0,2 --nics nic0,nic1 \
  --payload all --direction forward --address cyclic --output comparison-plan
```

On the authorized Linux host, execution additionally requires `--execute`,
timezone-aware `--window-start/--window-end`, and `--idle-check-script`,
`--inventory`, `--guard-devices`. The inventory must have one `ssh: local`
environment covering exactly the authorized pool. The checker must report every
device idle immediately before each process pair. No workload starts with fewer
than 150 seconds left in the window. The runner never terminates other workloads.
Do not execute it during a compile-only authorization window.

Every process log, effective resource record, occupancy result, case and binary
hash is retained. Generic NDA report lines can aggregate duplex traffic: use its
per-sender kernel sample rather than treating the aggregate as a direction.
Historical Excel points belong to a different run unless their physical path,
WQE count, address pattern, direction and timing have been matched explicitly.

The matched NDA build must also acknowledge full source/destination/guard
verification and peer success on both processes after the timed kernel. A
sender timing record with `payload_verified=0` is accepted only when the later
`RDMA_BENCH_VERIFY` record confirms the complete extent, role, address mode,
direction, all verification flags and `status=PASS`. Exit status or CQ success
alone is insufficient. The runner observes its own NDA server's LISTEN socket
through `/proc` before starting the client; it never probes by connecting to
the one-shot control socket. This avoids a client startup race without
consuming the server's only accept.

If QP RTR fails before any WQE, preserve its provider return value and errno,
and inspect the selected NIC's GID and neighbor resolution before changing a
kernel. A stale or failed neighbor is an environment boundary. Scoped test
neighbor changes must retain/restore the prior entries, and neither a neighbor
entry nor RoCEv2 selection proves packets traversed an L3 router. Preserve
independent forwarding evidence when reporting routed-network qualification.

CPU checks are `ctest --test-dir build-cpu`: the new pipeline model validates
encoded descriptors, address sequences, tail publication ordering, sparse
completion identities, admission, errors and state ownership; the socket control
test exercises real two-process TCP and production transport exchanges with a
CPU provider. Neither establishes device DMA/cache/MMIO correctness. Fake ACL
fixtures must keep assertions enabled in Release because their cleanup contracts
are asserted; they must never resolve to real ACL during CPU tests.

## Multiple independent QPs

`multiqp` measures WRITE with 1/2/4/8/16 independent communicator-owned QPs in
one provider/PD. It initializes domain 1 with `TileXRCommInitRankWithDomain`,
checks device-direct capability, and collectively calls `TileXRRDMAConfigureQps`
before registering memory. Domain 0 remains the separate phase/case socket.
Set `TILEXR_ENABLE_RDMA=1`, `TILEXR_RDMA_MODE=aiv`,
`TILEXR_RDMA_DMA_MODE=indep_ub` and `TILEXR_RDMA_DB_MODE=hard` for execution.
It launches one AIV per QP in one pure AIV kernel, with a requested batch64 and
an independent SQ/WR-adaptive in-flight window per QP:

```sh
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --describe
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --run
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --run NUM_SGE
```

Directions 0 and 1 select separate unidirectional tests. This executable does
not run duplex traffic. Every QP sends `COUNT_PER_QP` WQEs, so total payload is
`QPS * COUNT_PER_QP * BYTES * NUM_SGE`. Omitting `NUM_SGE` preserves SGE1;
the optional value selects 1/2/4/8/16 entries, each containing exactly `BYTES`.
The kernel uses `RDMAWrite` / `RDMAWriteSge` for QP1 and their `MultiQp` wrappers
with the owning AIV's index for multiple QPs. `RDMAWriteOptions` carries counts,
strides, batch/in-flight limits, cyclic slots and poll budget;
`RDMAWriteWorkScratchBytes(NUM_SGE)` sizes each AIV's private UB work scratch;
`RDMAWriteContextBytes(actualPendingCapacity)` separately sizes its retained UB context.
The multi-QP kernel keeps the public adaptive default: for each QP,
`L=ceil((48+16*NUM_SGE)/64)`, `window=min((sqDepth-1)/L,maxSendWr)` and
`batch=min(64,window)`. It has no fixed two-batch or 128-WR limit. Runtime
`MULTIQP_EFFECTIVE` reports `max_in_flight`, `batch` and `pending_capacity`;
CPU-only describe reports the policy, not fabricated negotiated depths.
After successful WRITE submission, each AIV calls
`RDMAWait(views + q, scratch, context, options.maxPolls)` before its end timestamp and propagates any Wait error. No
cross-QP wait loop or barrier is added. The aggregate interval below therefore
still measures completed transfers, not just posting.
The SGL path retains reversed, scattered sources, separate source/target strides
and 20992B work scratch plus the queue-sized context per AIV. SQ admission and
producer/completed counters use `COUNT_PER_QP * WqeBlocks(NUM_SGE)` blocks,
plus a reserved stop block; CQ counters stay in completion units
(`ceil(COUNT_PER_QP/batch)`, eight for 512 WR only when the window admits batch64).
For example, QP16 with SGE8 and 1MiB per entry transfers 64GiB per sample.
Aggregate Gb/s uses that volume divided by the
common-clock interval from the earliest QP start to the latest QP completion.
Summing individual QP bandwidths can overstate performance when their intervals
differ. Logs retain all intervals, start skew and overlap; calibration checks
the common cycle envelope against the ACL event. Host/event time remains a
separate metric. Use actual QP queue depths for no-wrap admission.

Each QP uses an independent payload pattern and full source, destination and
guard verification. Both peers agree on cases and failures before progressing.
Each QP keeps a small, separate buffer/MR registered through `TileXRRDMARegister`.
Every MR is valid on all configured QPs: `TileXRRDMAGetDeviceViews` requires the
full host array with exactly `QPS` entries. For MR `q`, the benchmark selects
entry `q` for the kernel's view array; it never constructs endpoints, keys or
queue descriptors. One AIV owns each selected queue across every MR and stream.

`COUNT_PER_QP` accepts 1..16384, sufficient for 8MiB at QP1/SGE1/512B. This
CLI bound does not override the runtime's actual SQ/CQ no-wrap admission or
the per-QP in-flight window. A larger requested transfer can still be rejected
before posting when the selected queue lacks capacity.

For a fixed total WQE budget `W`, use `COUNT_PER_QP = W / QPS` only when it is
an integer. A 48-WQE budget therefore means 48/24/12/6/3 per QP for
QP1/2/4/8/16, not 48 per QP. For an aggregate payload cap `L`, use
`floor(L / (QPS * NUM_SGE * BYTES))`; a zero result is N/A, not permission to
shrink an SGE or silently exceed the cap. Record actual payload bytes and count
with every result. Use binary MiB for payload caps and distinguish Gb/s from
GB/s (divide by eight); cyclic-address payload volume counts repeated transfers,
not unique MR allocation bytes.

WRITE returns after WQE publication, so its UB scratch and kernel-local SGE
descriptors can be reused immediately. Referenced source HBM must remain
unchanged, and MRs, QPs, shared state and relevant GM views must remain valid
until completion; the QP's single-owner rule extends across calls, MR views,
streams and kernels. Wait reuses existing WRITE scratch, kept valid during Wait.
Synchronizing a submit-only kernel's stream alone does not prove RDMA completion.
Even successful Wait requires separate receiver/cache synchronization, while a
timeout/error does not cancel NIC references or permit immediate memory release.

Local allocation and collective registration use separate agreed phases so a
local allocation failure cannot strand the peer in registration. Cleanup
synchronizes streams after the explicit waits (or stops work on error),
collectively calls `TileXRRDMAShutdown`, then destroys the communicator before
freeing payloads. A cleanup failure retains allocations.
SGL QP tags use a multiplier distinct from the SGE entry tag: sharing one would
let a swapped QP/entry pair cancel under XOR and hide misrouting. CPU tests check
tag uniqueness, cross-QP corruption, ordered gathering and the complete MR.
`RDMA_BENCH_CPU_ONLY` also guards this executable before ACL initialization.
Repetitions require fresh processes and QPs. Compare another implementation
only with explicit per-QP WQE counts, scheduling, address and timer semantics;
using different hosts does not constitute a controlled same-host A/B test.

## One WRITE call and one doorbell per WQE

For posting-cost measurements of all four WRITE entries, build `multiqp` with
`-DTILEXR_RDMA_SINGLE_WQE_CALLS=ON` in both Release and Debug. This opt-in mode
does not change the library default, `bench`, or the launch ABI. Each API call
uses `count=1, postBatch=1`, retains its UB context between calls, advances the
source/target offsets, and rings one doorbell. Each QP calls `RDMAWait` once after
all `COUNT_PER_QP` API calls. The default build still uses one batched API call.

单 WQE 模式为每次调用分配独立的 source/target slot，并令 `cyclicSlots=0`；
调用循环推进 offset，WRITE 内部不再叠加循环 slot 偏移。默认批量模式仍保留原循环布局。
Host 的实际运行和 `--describe` 共用 `MultiQpCaseLayout`，避免分配范围与下发地址不一致。

Single-WQE mode allocates `COUNT_PER_QP` distinct source/target slots and passes
`cyclicSlots=0`: the call loop owns offset advancement. Do not combine per-call
offset advancement with the batched mode's cyclic offsets. With 48 calls over a
32-slot allocation this can exceed the MR even when the SQ has ample capacity.
Both execution and `--describe` use `MultiQpCaseLayout`; CPU regressions cover
slot counts across 32, full destination contents and guards in both directions.

例如，使用 `BYTES=512, COUNT_PER_QP=48`，分别选择 `(QPS, NUM_SGE)` 为
`(1,1)`、`(1,2)`、`(2,1)`、`(2,2)`，覆盖 `RDMAWrite`、`RDMAWriteSge`、
`RDMAWriteMultiQp`、`RDMAWriteSgeMultiQp`。多 QP 为每 QP 48 次调用，不是所有 QP 合计 48 次。
Host 检查整轮 WR/pending/SQ/CQ 容量，不允许为了完成此测法而在 Write 阶段因队列不足回收。

单 WQE 模式在相同 kernel/QP/UB context 中先调用 32 次 Write 并 Wait，再测量 48 次下发。
warmup 复用有效 slot，测量阶段覆盖所有 slot；容量检查包含总共 80 个 WQE、两次 Wait 和 CQ 进度，
不能重置 producer 或只检查测量阶段的 SQ 使用量。`warmup` / `measured_issue` marker 明确划分两个阶段。

For each sender lane, warm up 32 calls and wait on the same kernel/QP/context,
then require 48 measured entry/API and doorbell intervals, no backpressure
inside the measured issue interval, and one final Wait. Total progress is 80
CQEs, with warmup and measured Waits kept separate. Warmup reuses valid slots;
measured calls overwrite every slot. Admission includes all 80 WQEs without
resetting producer/CQ counters. CQ polling is not disabled globally.
Use the API intervals for per-call posting latency and its distribution. The
existing result duration includes the call loop and Wait, and cannot be divided
by 48 and labelled Write-only latency. SGE address-list updates in the loop are
outside the API intervals. Outer API and nested wrapper intervals overlap: do
not sum them. Report each QP separately before computing cross-QP summaries.

实测必须使用目标 timer 校准，并说明 Debug 检查、UB marker 和计时本身的开销。
Release 不提供逐 API marker；`qps_detail.issue_duration_ns / api_calls_per_qp` 是 warmup 后
不含最终 Wait 的平均下发耗时（包含调用循环和 SGE 地址准备），不是逐调用分位数。
`wait_duration_ns` 单独报告；ACL/Host 时间仍包含 warmup、Wait、保存及其他 kernel 工作。
分析 Debug 时按 `measured_issue` 区间筛选，不把 warmup 的首调用或 Wait 混入测量分布。
Release issue timing excludes the final Wait, but includes the call loop and SGE address setup;
it gives a per-call average, not per-call percentiles. Filter Debug intervals by `measured_issue`.

显式 context 在 Restore 缓存不可变 view，后续四种 Write 和 Wait 均不重新读取 GM view；
比较缓存优化前后时必须使用相同 warmup 和计时版本，并分别运行多轮。无 context 的入口仍逐调用加载 view。
不要在一次绑定中更换 MR view 指针；先 Wait/Save，再 Restore 新 view。完整生命周期见 `docs/RDMA_API.md`。
The explicit context caches its immutable view at Restore for all four Write entries and Wait.
Compare warmed builds with identical timing over multiple independent rounds. Compatibility calls
still reload views. Rebind another MR view only after Wait/Save and a new Restore.
CPU 替身会在 CQE 尚未提供时连续调用四个入口各 48 次，检查 48 个 doorbell、无 CQ 读取、
无 GM 元数据 I/O，最后才提供 CQE 并统一 Wait；该回归不证明真实设备时序或性能。

批量模式也先在相同 QP/context 中 warmup 32 个单 WQE 并 Wait，随后以默认 batch 64
提交 `COUNT_PER_QP` 个测量 WQE。warmup 关闭 cyclic 偏移，固定复用测量会覆盖的 `1 % slots`
位置；不能遍历所有循环 slot，否则小 count 用例的未写区域会被 warmup 污染。
32 次 warmup 恰好覆盖所有可选循环长度的整数周期，测量阶段的 slot 起点保持不变。
容量和完成进度均包含 warmup 的 32 个 WQE/CQE。测量 payload 不含 warmup 字节；
批量模式的 issue 时间可能包含窗口背压回收，不能解释为纯 WQE 编码耗时。

Batched mode warms up 32 single-WQE calls on the same QP/context, waits, then
issues the measured `COUNT_PER_QP` WRs with the default batch of 64. Warmup
disables cyclic offsets and repeats measured slot `1 % slots`, leaving other
slots untouched. Its 32 WRs preserve the measured cyclic phase for every
supported cycle length. Admission and completion
include the additional 32 WQEs/CQEs; measured payload excludes warmup traffic.
Batched issue timing may include capacity-driven CQ polling, not just encoding.

## WRITE stage profiling (Debug only)

Debug 插桩只使用现有 `tilexr_profiling.h` 的 `DirectWriter` 和 `AclCapture`，不扩展公共 SDK
或采集格式。每个 AIV/QP 独占 UB context 和一个 GM trace lane；writer 在首个 marker 前
Initialize，RecordAt 逐条追加事件，最后 Publish。UB context 只保存 QP state/pending，
不缓存 profiling 事件，也不再分配 profiling UB。
Release 将调用方参数检查和 RDMA marker 编译掉，容量保护、超时及完成错误处理仍保留。

Build paired Host/kernel artifacts after sourcing the target CANN environment:

```sh
cmake -S tools/rdma_compare -B build-rdma-release \
  -DTILEXR_ROOT="$PWD" -DTILEXR_INSTALL="$PWD/install" -DCMAKE_BUILD_TYPE=Release
cmake --build build-rdma-release -j2
cmake -S tools/rdma_compare -B build-rdma-debug \
  -DTILEXR_ROOT="$PWD" -DTILEXR_INSTALL="$PWD/install" \
  -DCMAKE_BUILD_TYPE=Debug -DTILEXR_RDMA_PROFILE=ON
cmake --build build-rdma-debug -j2
```

The CMake command explicitly passes `TILEXR_RDMA_DEBUG` and `TILEXR_ENABLE_PROFILING`
to both compilers. Debug remains optimized (`-O2`); `TILEXR_RDMA_PROFILE=OFF` removes
RDMA instrumentation while retaining Debug validation. Do not compare a Debug
instrumented sample directly with Release and attribute the entire difference
to a single stage.

在两个 rank 的启动环境中设置 `TILEXR_RDMA_PROFILE_DIR` 为已有的可写目录，再使用原有
`--run` 命令启动 bench/multiqp。两 rank 必须同时启用或同时禁用；校准 launch 不采集。
每个正常 launch 分配独立 capture，两 rank 共享 rank0 生成的 UUID。导出文件名为
`rdma-write-<UUID>-rank0.records.bin` 和 `...-rank1.records.bin`；不会覆盖旧文件。
Each process prints `PROFILE_RECORDS path=... complete=... dropped=...`. Check every
required rank and lane, not merely the process exit status.

每个 GM lane 最多保留 4096 个事件，超出容量保留前缀并累计 dropped。
dropped 或缺少结束事件的结果是 incomplete，不能作为完整阶段统计。
每个 marker 的 cycle counter 读取和 GM 写入会扰动 Debug 耗时，不能据此推断无插桩性能。
WRITE context、work 和结果仍需 UB，预算与设备编译兼容性必须通过真实 CANN 构建验证。

The existing DirectWriter appends events to GM and Publish commits the lane. No profiling UB or
new public recorder is required. Overflow retains the prefix and reports dropped events. Debug
measurements include timestamp/GM-write overhead; use uninstrumented Release for performance baselines.

Copy `write.mapping.template.json` to a working mapping and fill `cycles_per_us`
using the confirmed conversion for the target timer. Its deliberate `null` is
not a default or a calibration value. Then use the existing analyzer (Python 3.9+):

```sh
PYTHONPATH=tools/profiling/python python3 -m tilexr_profiling \
  --mapping write.mapping.json \
  --records /path/to/rdma-write-UUID-rank0.records.bin \
  --records /path/to/rdma-write-UUID-rank1.records.bin \
  --output-dir write-profile-report
```

The output directory must be absent or empty. Outputs include `analysis.md`,
`report.html` and `perfetto_trace.json`. Never mix UUIDs from different launches.
The mapping attributes public API overhead, view loading, validation, UB state,
WQE encoding, stop/tail/head publication, doorbells, capacity backpressure,
explicit Wait and context Restore/Save. API-specific wrapper intervals may nest;
they are inclusive and must not be added to their children. `api` is the outer
public-call interval. Receiver lanes may have only Restore/Save and no Write/Wait.

kernel 的 start/end 仍只测 WRITE+Wait，不包括 Restore、Save 或 writer Initialize/Publish；profiling
的 total 覆盖 Restore 至 Save，但不包括 Initialize/Publish。ACL event 包括 kernel 内的这些操作；
启用采集时，Host wall time 还包括 SDK 分配、采集和文件导出。因此端到端性能基线使用
Release 无采集结果；Debug trace 用于分解阶段，不能当作无插桩延迟。
CPU tests validate UB metadata access and existing SDK/analyzer compatibility only, not
CANN code generation, real pipeline ordering, transfer correctness or bandwidth.
