# Common Straggler Diagnostics Design

## Periodic clock maintenance

维护入口是 `tilexr_straggler.h`。下表对应实际调用路径；所有 clock offset、补偿和残差都以
communicator 的逻辑 rank 0 为基准，与 physical device ID 无关。调整 rank/device 映射后，
基准随 rank 0 移动；rank 0 的模型保持零 offset、零 rate。

| 文件 | 职责与调用关系 |
| --- | --- |
| `src/diagnostics/host/straggler_session.cpp` | 校验 owner thread 和 collective 配置，调用采样，交换并校验候选模型，写入文件 |
| `src/diagnostics/host/clock_kernel.cpp` | 加载独立 binary，注册并向调用方的 stream 提交 clock kernel |
| `src/diagnostics/kernels/clock_sync_kernel.h` | 执行 rank 0 与 peer 的四时间戳交换，写入采样或失败状态 |
| `src/diagnostics/host/clock_drift.cpp` | 初始拟合、反馈更新和本地时间戳校正 |
| `src/diagnostics/host/straggler_analysis.cpp` | 将观测时间戳校正到 rank 0 时间轴，计算 median/MAD 和分类 |

四时间戳中，`t0/t3` 来自本地 rank，`t1/t2` 来自 rank 0。模型的单位为 cycle，
`rateCorrection` 的单位为 cycle/cycle：

```text
measuredOffset = ((t1 - t0) + (t2 - t3)) / 2
correctedCycle = localCycle + offsetCycles
               + rateCorrection * (localCycle - anchorCycle)
residual = correctedLocalTime - rank0Time
```

正 residual 表示补偿后的本地时间领先 rank 0，因此需要减小补偿；负值需要增大补偿。
对外展示 ns/s 时，将 `rateCorrection` 乘以 `1e9`。`TileXRClockCorrectCycle` 对结果舍入，
并拒绝非 READY 模型、无效参数和超出 `int64_t` 范围的结果。

Enable `TILEXR_CLOCK_DRIFT_TRACK=1` before creating a detector to fit the initial
rate over 10 seconds and maintain it with short collective sampling calls. Supply
`config.outputDir` and at least 64 measured samples. An explicit
`TILEXR_CLOCK_DRIFT_WINDOW_US` must be `10000000` in this mode. Without tracking,
the existing optional drift model remains frozen after training.

The caller schedules `TileXRClockSynchronize` approximately every 60 seconds after
initial training, on the detector owner thread, with observations drained on all
ranks. There is no background worker. The first 16 exchanges detect the residual;
the next 16 confirm its sign and magnitude. Both medians must exceed 1000 ns in
the same direction before a feedback update. Residual means corrected local time
minus reference time, assuming symmetric effective communication delays.

At an accepted update, use the device sample midpoint as the new anchor, set its
offset to the measured offset, and blend the previous rate with the slope between
the previous accepted anchor and the new measurement, with gain 0.25. A probe
below threshold must not advance the rate anchor: dividing accumulated error by
only the last probe interval overcorrects. A positive residual requires a negative
rate increment. Re-anchoring also removes the current phase error; changing only
the rate would leave it present. Corrected timestamps can step at this boundary.

Every sampling call writes `rank<R>/clock_sync_generation<N>.json` with local raw
exchanges and all gathered models. `rank<R>/clock_drift_latest.json` is replaced
with the latest complete snapshot. Tracking metadata identifies the initial
training generation, pre-update residual, probe validity and actual model update.
The model generation advances only for initial fitting or an accepted update;
its sample/span metadata continues to describe the initial fit. Files are evidence
and checkpoints for inspection; automatic loading across restarts is not provided.
Sampling/persistence errors are returned collectively. A fresh successful probe
refreshes validation age, while its device anchor still records the last update.
A rejected tracking probe must not renew validation age even if enough remaining
exchanges satisfy the ordinary synchronization minimum-valid-sample threshold.

更新时取第 32 个样本的本地 midpoint 为 `newAnchor`，将前 32 个 measured offset 按旧 rate
移到这个 anchor 后取 median，得到 `newOffset`。随后计算：

```text
measuredRate = (newOffset - oldOffset) / (newAnchor - oldAnchor)
newRate = 0.75 * oldRate + 0.25 * measuredRate
```

`oldAnchor/oldOffset` 来自上次实际接受的更新，不是上次定时探测。后 32 个样本不参与更新，
可用于检查更新后的残差。候选 tracker 先经过 collective 探测检查及模型 gather/校验，
再写入 session。模型已写入 session 后才执行文件持久化；文件写入失败会返回 collective
错误，但不会回滚已经发布的内存模型。`latest` 使用临时文件加 rename 替换，generation
文件直接写入；这不等同于断电后的持久性保证，也不保证各 rank 文件同时替换。

调用方持有 communicator 和 stream，session 持有采样/记录 buffer 以及可选 RDMA allocation
和 registration。调用方应在创建线程上安排 collective 采样，先收集完已有 observation，
再约每 60 秒探测一次。`maxCalibrationAgeMs` 应覆盖维护间隔与实际调度延迟；默认值为
60000；`Collect` 检查超龄并返回 timeout，因此 60 秒维护间隔需要为调度抖动预留余量。
销毁时会等待已记录的 stream，随后尝试注销或关闭 RDMA 并释放 buffer；失败时保留 session 供重试，调用方不能
继续销毁它借用的 communicator。

For the integration runner, use a 1-second training cadence and a separate
60-second maintenance cadence, for example:

```bash
export TILEXR_CLOCK_DRIFT_TRACK=1
export TILEXR_CLOCK_DRIFT_WINDOW_US=10000000
# Launch collectively with rank/device arguments and an existing output directory:
test_tilexr_clock_sync_e2e --rank-size N --rank R --device-id D \
  --samples 64 --warmup 0 --rounds 17 --round-gap-ms 1000 \
  --maintenance-interval-ms 60000 --freeze-calibration 0 \
  --output-dir OUTPUT --comm-mode local
```

The 1 us trigger does not guarantee every sample or unobserved instant stays below
1 us. Report pre-update and held-out residuals separately from residuals evaluated
with the new model; RTT/2 (`precisionNs`) remains a model bound, not that residual.

## Status

Approved direction: move clock synchronization and straggler detection out of
the optional Collectives library into a reusable diagnostics component. The
existing `TileXRCollectiveStraggler*` API is not retained as a compatibility
layer.

## Goals

- Provide one communication-domain clock calibration service shared by
  Collectives, EP, MoonEP, and future communication producers.
- Detect fast and slow ranks for every instrumented communication event without
  making the diagnostics component depend on a producer library.
- Keep the clock synchronization operation independent from business kernels.
- Preserve C++14, CANN 9.1.0, and registered AICore binary launch requirements.
- Keep reports comparable across producers by using one event and rank model.

## Non-Goals

- This migration does not add EP or MoonEP instrumentation to every production
  kernel. It provides and validates their common integration boundary; producer
  coverage can then be added without another ownership change.
- It does not provide external reference-clock accuracy. Reported precision is
  the communication-domain error bound derived from minimum round-trip time.
- It does not retain the unpublished Collective-specific straggler ABI.

## Ownership And Dependencies

Add `src/diagnostics`, producing `libtilexr-diagnostics.so`:

```text
libtile-comm.so
       ^
libtilexr-diagnostics.so
       ^
Collectives / EP / MoonEP
```

`libtilexr-diagnostics.so` owns:

- detector creation, destruction, calibration state, and ownership checks;
- the communication-domain control exchange used by calibration and collection;
- clock sample estimation, event classification, and JSON reporting;
- compilation, loading, registration, and launch of the standalone clock
  synchronization AICore binary;
- generic producer-side Host observation APIs and device record layout.

The component depends on `libtile-comm.so` for `TileXRCommPtr`, `CommArgs`, peer
memory, magic generation, and bounded Host all-gather. It must not include or
link Collectives, EP, or MoonEP sources.

Producer libraries own the placement of observation calls around their own
communication kernels. They include the common device instrumentation header
and pass the reserved device record address through their existing kernel
argument path.

## Source Layout

```text
src/diagnostics/
  CMakeLists.txt
  host/
    clock_drift.h / clock_drift.cpp
    clock_kernel.h / clock_kernel.cpp
    straggler_analysis.cpp
    straggler_session.cpp
  kernels/
    CMakeLists.txt
    clock_sync_kernel.cpp / clock_sync_kernel.h
    clock_rdma_queue.h
    clock_rdma_profile.h

src/include/
  tilexr_straggler.h
  tilexr_straggler_device.h
  tilexr_straggler_layout.h
```

The implementation may keep private helper headers next to their owning source.
No diagnostics implementation remains under `src/collectives` after migration.

## Public Host API

The public handle and control API are producer-neutral:

```cpp
typedef void *TileXRStraggler;

int TileXRStragglerConfigDefault(TileXRStragglerConfig *config);
int TileXRStragglerCreate(TileXRCommPtr comm,
                          const TileXRStragglerConfig *config,
                          TileXRStraggler *detector);
int TileXRStragglerDestroy(TileXRStraggler detector);
int TileXRClockSynchronize(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRClockSyncResult *result);
int TileXRStragglerCollect(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRStragglerReport *report);
```

Producer integration is explicit and does not use Collectives-owned thread-local
state:

```cpp
int TileXRStragglerObservationBegin(
    TileXRStraggler detector,
    const TileXRStragglerOperation *operation,
    aclrtStream stream,
    TileXRStragglerObservation *observation);

int TileXRStragglerObservationEnd(
    TileXRStraggler detector,
    const TileXRStragglerObservation *observation,
    aclrtStream stream,
    int launchStatus);
```

`TileXRStragglerOperation` contains stable generic identity fields:

- component kind: Collectives, EP, MoonEP, or user-defined;
- operation kind and optional stage identifier;
- message bytes;
- caller operation identifier when available.

`TileXRStragglerObservation` returns the device record address, launch ID,
generation, rank, and normalized operation metadata needed by the producer's
kernel arguments. A zero device record address means observation is inactive;
producer behavior remains unchanged.

No `TileXRCollectiveStraggler*` type, symbol, or installed header is retained.

## Device Instrumentation

`tilexr_straggler_device.h` exposes producer-independent helpers equivalent to:

```cpp
TileXRStragglerTimingToken TileXRStragglerTimingBegin(
    GM_ADDR record);

void TileXRStragglerTimingEnd(
    TileXRStragglerTimingToken token,
    GM_ADDR record);
```

Each aligned ring slot contains a 64-byte device control block followed by the
80-byte timing record and alignment padding. Observation begin writes the control
block on the producer stream before the business-kernel launch. The producer passes
only the record pointer through its private launch ABI; the device helper derives the
adjacent control address and reads launch, operation, rank, generation, and optional
test-delay metadata from it. This avoids appending producer-specific scalar arguments
whose generated direct-launch metadata may not match an older kernel signature.

The record captures local start, communication-ready, and end cycles. The producer
determines the exact semantic boundary of communication-ready and must call the end
helper only after its communication data movement has completed. The helper uses the
existing MTE3-safe UB-to-GM publication path.

The layout header contains only stable Host/device structures, offsets, and
compile-time size checks. It does not include Collectives types.

## Clock Synchronization

Clock synchronization remains a standalone operator and uses only communicator
peer memory. Each round uses `TileXRCommNextMagic`; protocol slots are not reset
globally.

Each peer calibration uses a `READY -> REQUEST -> RESPONSE -> TIMESTAMP -> ACK`
four-timestamp exchange. Rank 0 publishes `READY` for exactly one peer and
sequence before that peer records `t0` and pushes `REQUEST` into rank 0 memory;
rank 0 then records `t1` and pushes `RESPONSE` into peer memory, after which the
peer records `t3`. Rank 0 measures the midpoint of its completed RESPONSE MTE3
window and publishes that value as `t2` through a separate TIMESTAMP slot; the
peer then pushes `ACK` into rank 0 memory. Timestamp delivery occurs after `t3`
and is outside the measured interval. Rank 0 does not advance to another peer
until it observes the ACK. Time spent waiting for rank 0 to service other peers
is therefore outside the measured `t0 -> t3` interval.

REQUEST, ACK, and TIMESTAMP use separate per-rank 64-byte slots. Reusing or
placing states in one remotely updated cache line can leave a receiver polling
a stale state even after MTE completion. Receivers fetch the complete 64-byte
slot with MTE2. The REQUEST write, REQUEST read, RESPONSE write, and RESPONSE
read timestamps use the midpoint of their respective MTE operation windows,
reducing the systematic endpoint bias from assigning the complete transfer
latency to one side. The estimator remains `((t1 - t0) + (t2 - t3)) / 2`, and
the conservative error bound remains half of the minimum corrected round trip.

Calibration runs configurable warmup exchanges before configurable measured
samples. Warmups exercise the identical device path but are never written to the
sample buffer or considered by the estimator. The default is 32 warmups followed
by 128 measured samples. `TileXRClockSyncResult::sampleCount` counts measured
samples only and `warmupSampleCount` records the discarded exchanges.

The diagnostics build installs a standalone AICore object and the Host loads and registers it with
`rtDevBinaryRegister` and `rtFunctionRegister`. Host launch uses
`rtKernelLaunchWithFlagV2` with a fixed argument ABI.

SoC selection uses a common cache variable derived from `TILEXR_SOC_NAME` and
must fail configuration when no supported target is known for an enabled
diagnostics build. It must not silently compile Ascend950 for `dav-c220-vec`.

## Session And Concurrency

- A detector belongs to exactly one `TileXRCommPtr` and one creating Host
  thread unless the public contract is extended later.
- Only one detector may own a communicator at a time.
- Calibration, observation reservation, observation completion, and collection
  are generation-tagged.
- Destroy rejects or drains outstanding observations according to the existing
  bounded timeout policy; it never frees device records still owned by a
  producer stream.
- Producer launch failure is recorded as observation failure and does not
  overwrite a prior device-side failure.

## Reporting

Clock offsets, affine corrections and signed residuals use the communicator's
logical rank 0 as their reference. Physical device IDs do not select the time
base: remapping rank 0 moves the reference to that device. A positive residual
means the corrected local timestamp is ahead of rank 0. Persisted clock
snapshots record `reference_rank: 0`; rank 0 retains zero offset and rate.

`TileXRStragglerReport` describes a generic communication event. It includes:

- component kind, operation kind, stage ID, and operation ID;
- communication bytes and calibration generation;
- median and MAD duration and pre-communication latency;
- clock precision and calibration age;
- per-rank aligned timestamps, duration, deltas, classification, and reasons.

The clock report retains per-rank offset, minimum and median RTT, valid/rejected
sample counts, and error bound. Precision remains the maximum accepted per-rank
`min_rtt / 2` bound for the domain.

## Build And Installation

Add `TILEXR_BUILD_DIAGNOSTICS`. It is enabled automatically when Collectives,
EP, or MoonEP enables straggler instrumentation, and can be built independently.
The installed artifacts are:

- `libtilexr-diagnostics.so`;
- `tilexr_straggler.h`;
- `tilexr_straggler_device.h`;
- `tilexr_straggler_layout.h`.

Collectives removes the migrated Host sources, clock kernel, registration entry,
and installed Collective-specific header. It links diagnostics only for the
observation adapter. EP and MoonEP source/compile tests link the same public
integration surface without any dependency on `libtilexr-collectives.so`.

## Migration Sequence

1. Add diagnostics build, public headers, clock binary, Host session, and
   analysis by moving the existing implementation without behavior changes.
2. Rename all public and internal Collective-specific types to generic types.
3. Replace active Collectives TLS coupling with explicit observation begin/end
   data passed through the Collective launch context.
4. Remove migrated sources and the clock entry from the Collectives binary.
5. Move unit and hardware tests under `tests/diagnostics`; keep one Collectives
   integration test as a producer adapter test.
6. Add EP and MoonEP compile/link boundary tests.
7. Remove `tilexr_collective_straggler.h` and all old symbols.

## Verification

- Header ABI and C++14 compile tests for all public structures and functions.
- Unit tests for clock estimation, classification, config validation, lifecycle,
  generation reuse, ownership, launch failure, and incomplete observations.
- Source guards proving diagnostics does not depend on producer modules and EP /
  MoonEP do not depend on Collectives for diagnostics.
- Independent diagnostics build and install-prefix consumer test.
- Collectives aggregate communication correctness plus fast/slow rank detection.
- Existing 64-rank, eight-node clock-only test on Ascend950 with the explicitly
  recorded SoC binary provenance.
- The 64-rank test must enforce a caller-supplied precision ceiling against p50,
  p95, and maximum communication-domain error bounds; the production acceptance
  ceiling for the reference-ready protocol is strictly below 1000 ns.

The retained 64-rank HCCL AIV-only result qualifies the current environment and
must be reused rather than rerun unless the environment identity changes.

## Reusable Build Lesson

An empty SoC cache variable previously selected `dav-c220-vec` for Ascend950 and
caused every rank to fail at the same scalar instruction PC. Any standalone
AICore component must make its SoC selection explicit, record the selected
architecture in build provenance, and reject an unknown target instead of
silently choosing a different hardware generation.
