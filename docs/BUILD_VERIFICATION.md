# TileXR Build Verification

> **A5 grouped AllToAll launch configuration:** The A5 kernel reserves 64 KiB
> of each vector core's UB for SIMT DCache and passes the remaining UB through
> `rtTaskCfgInfo_t::localMemorySize`; at least another 64 KiB must remain for
> dynamic UB. A missing or failed
> `ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE` query is a launch error; do not fall back
> to `localMemorySize == 0`. Host-only checks validate the configuration path,
> but an AIV exception fix still requires the original Ascend950 multi-host
> reproduction to confirm that device error 259 is absent and correctness
> passes.

**Updated:** 2026-09-04

This checklist reflects the current TileXR codebase. The core runtime builds `libtile-comm.so` without a compile-time or link-time shmem dependency.

## Environment

```bash
cd /path/to/TileXR
source scripts/common_env.sh
npu-smi info
```

Expected:

- CANN 9.1.0 environment is visible through `ASCEND_HOME_PATH`.
- `scripts/common_env.sh` detects architecture and SOC information.
- NPU driver version is 25.1.rc1 or later.

## Build Core Runtime

```bash
cmake -S . -B /tmp/tilexr-build -DCMAKE_INSTALL_PREFIX="$PWD/install" \
  -DTILEXR_BUILD_UNITEST=OFF
cmake --build /tmp/tilexr-build --target tile-comm -j"$(nproc)"
cmake --install /tmp/tilexr-build
```

Expected:

```bash
test -f install/lib/libtile-comm.so
```

Check dynamic dependencies:

```bash
ldd install/lib/libtile-comm.so | grep -E "ascendcl|runtime|ascend_hal|profapi"
ldd install/lib/libtile-comm.so | grep -i shmem || true
```

Expected:

- CANN runtime libraries are resolved.
- The shmem grep prints nothing for the current TileXR UDMA implementation.

SDK tests that mock ACL must prepend their mock headers with
`target_include_directories(... BEFORE PRIVATE ...)` so inherited CANN headers
cannot select incompatible declarations and cause unresolved `aclrtMalloc` or
`aclrtMemcpy` symbols. When validating these targets, include the CANN header
search paths used by the root build; a standalone mock-only build does not
exercise this include-order boundary.

## Build TileXR and UniTest Together

The root `TILEXR_BUILD_UNITEST` option defaults to ON. It includes core,
diagnostics and EP dependencies even when `TILEXR_BUILD_EP=OFF`. Initialize
the source dependencies and build all targets before installing:

```bash
git submodule update --init 3rdparty/spdlog 3rdparty/picojson
cmake -S . -B /tmp/tilexr-unitest-integrated \
  -DCMAKE_INSTALL_PREFIX="$PWD/install" -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_TESTING=OFF -DTILEXR_BUILD_TESTS=OFF
cmake --build /tmp/tilexr-unitest-integrated -j"$(nproc)"
cmake --install /tmp/tilexr-unitest-integrated
install/bin/tilexr-unitest --help
```

This path links the Worker to the current build's TileXR targets without a
preinstalled TileXR dependency. `BUILD_TESTING` does not control the tool.
Core-only and package-only builds must explicitly disable UniTest if they do
not build/install its Worker. Installed operator runs still require an idle,
compatible A5 environment and real runtime libraries; `--help` alone does not
prove Worker initialization. Standalone builds against an existing installation
remain supported; see [UniTest instructions](../tools/unitest/README.md).

### UniTest link-only hosts

On an x86_64 compile-only host without a real driver HAL, if the core link fails
with `cannot find -lascend_hal`, load the CANN SDK and set its link-time search path
before configuring/building:

```bash
export LIBRARY_PATH="$ASCEND_HOME_PATH/x86_64-linux/devlib${LIBRARY_PATH:+:$LIBRARY_PATH}"
```

Use the SDK platform directory matching the target architecture. This selects an
official link stub for build checks only; never add it to runtime
`LD_LIBRARY_PATH` or RPATH/RUNPATH. Real execution still requires a compatible
NPU and driver HAL. This core link issue does not require changing standalone
UniTest CMake rules. Revision-specific builds and hardware evidence are retained
in [UniTest validation records](validation/unitest.md).

## Build UDMA Tests

```bash
cd tests/udma
bash build.sh
```

Expected host-side artifacts:

```bash
test -x install/bin/test_tilexr_udma_transport_layout
test -x install/bin/test_tilexr_udma_registry
test -x install/bin/test_tilexr_udma
```

If `bisheng` is available, the AICore demo artifacts should also exist:

```bash
test -x install/bin/tilexr_udma_demo
test -f install/lib/libtilexr_udma_demo_kernel.so
```

If `bisheng` is unavailable, `build.sh` may skip only the demo target while still building host-only tests.

## Build SDMA Tests

```bash
cd tests/sdma
bash build.sh
```

Standalone SDMA tests that compile a production `.cpp` directly must define
the capability state they exercise and provide test doubles for every runtime
or backend boundary referenced by that translation unit. A disabled branch at
runtime does not remove unresolved symbols from the linker input.

## Build Expert Parallelism Package

EP Memory Server Dedup 的接口与布局见
[Dispatch/Combine 设计](specs/ep-memory-server-dedup-dispatch-combine-design.md)。

EP uses one installation layout for every deployment environment. Do not pass
`TILEXR_SOC_NAME` or an EP SoC CMake option when configuring:

```bash
cmake -S . -B /tmp/tilexr-ep-build \
  -DTILEXR_BUILD_EP=ON \
  -DTILEXR_BUILD_UNITEST=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-ep-build --target tilexr-ep -j"$(nproc)"
cmake --install /tmp/tilexr-ep-build
```

The six EP kernels are A5-only objects even when the compiler runs on a 910B
host. The package must contain exactly these EP artifacts and no EP kernel
shared libraries or 910B variants:

```bash
ep_lib_dir="$(dirname "$(find install/lib* -maxdepth 1 -type f \
  -name 'libtilexr-ep.so' -print -quit)")"
test -f "${ep_lib_dir}/libtilexr-ep.so"
for object in \
  tilexr_ep_dispatch_kernel.o \
  tilexr_ep_combine_kernel.o \
  tilexr_ep_dispatch_memory_kernel.o \
  tilexr_ep_combine_memory_kernel.o \
  tilexr_ep_dispatch_memory_server_dedup_kernel.o \
  tilexr_ep_combine_memory_server_dedup_kernel.o; do
  test -f "${ep_lib_dir}/tilexr/kernels/${object}"
done
! find "${ep_lib_dir}" -maxdepth 1 -type f \
  \( -name 'libtilexr_ep_dispatch_kernel.so' -o \
     -name 'libtilexr_ep_combine_kernel.so' \) | grep -q .
```

Server Dedup profiling compatibility changes require separate OFF and ON build
directories, using `-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=OFF` or `ON` at
configuration. Build `tilexr-ep` to include the communicator compatibility owner,
Host launch code and all six kernels. `TILEXR_EP_SERVER_DEDUP_ENABLE=auto|0|1`
selects the dedup policy; the default is `auto`. Keep Host and device artifacts
from the same build, including every consumer of the expanded `CommArgs` ABI.

Building on 910B proves cross-compilation and packaging only. Host tests must
also verify that every public EP entry returns `TILEXR_ERROR_NOT_SUPPORT` there
before loading an object or advancing communicator magic; only an A5 hardware
run proves kernel execution.

## Qualify EP Compile-Time Specialization

Treat compile-time specialization as a measured optimization, not a successful
build. Keep generic and candidate artifacts separate, require exact-shape
all-rank correctness, and time individual ACL Events in interleaved and
reverse-order legs. Retain a candidate only when it improves the cross-rank
median by the larger of 2 us or 3 percent.

Experiments on Ascend950DT with CANN 9.1.0 and the peer-memory Dispatch path
specialized rank, world size, shape, layout, quantization, UB sizes, send counts,
address arithmetic, and the launch ABI. All candidates passed 8-rank
BF16-to-MXFP8-E4M3 correctness, but none cleared the gate: full specialization
measured `73.468 us` generic versus `73.475 us` static at `S=256`, and
`33.784875 us` versus `33.803249 us` at `S=96` (`K=6`, `H=7168`, `E=16`). At
`S=96`, `E=384`, the result was `34.413499 us` versus `34.426625 us`. Partial
shape and route-loop specializations also failed. Do not add fixed-shape kernel
variants based on compiler success or Host-only tests.

A CANN registered-operator trial confirmed that rank-specific static task names
were executed. At `S=96`, direct launch measured `64.688 us` and static ACLNN
measured `63.426 us` across 12,800 samples, a `1.262 us` (`1.95 percent`) change.
Profiler data located the apparent saving: low-tail Kernel duration fell from
`42.184 us` to `40.270 us`, but the required per-call magic H2D took `2.011 us`
and immediately preceded the Kernel on the same stream. Leg medians varied by
more than 13 us. The critical-path saving is therefore below the gate and the
registered path must not be integrated. Moving magic to a device-side counter
would change round ordering and concurrent communicator semantics.

Other bounded costs do not supply a hidden 3-5 us opportunity. Scalar setup was
`0.263 us` median (`0.391 us` on each rank's slowest core); device queries took
`1.248 us`; and direct Host submission did not starve the device queue. ACL
Graph capture saved about `0.44 us` of Host submission, regressed device time by
about `4.90 us`, and froze `TileXRCommNextMagic`, replaying a stale generation.
Graph support requires a replay-safe round protocol or dynamic task update.
Compiler experiments were also rejected: removing automatic synchronization
hung on hardware, `-O3` was slower than `-O2`, and `-funroll-loops` generated an
identical object.

Sparse token-mask validation exposed a separate correctness rule. Per-token
`topK * sizeof(int32_t)` copies can start at unaligned GM addresses. Copy the
full aligned expert-ID tensor to UB, wait for MTE2, compact the increasing active
token indices in UB, and establish the scalar-to-vector dependency. Validate a
sparse mask on every rank; mask-free and aligned cases do not cover this path.

For a one-time multi-host HCCL qualification, source the selected CANN
environment and verify its resolved OPP/AICPU `ccl_kernel.json` on every host.
Checking `ldd` alone is insufficient: a mismatched OPP path can fail the first
ranks during communicator initialization and leave peers timing out without any
submitted operation.

## Build MoonEP Five-Stage Runtime

MoonEP native five-stage support requires CANN 9.1 and an A5 target compiler. Building
does not require launching an NPU kernel:

```bash
source /path/to/cann-9.1.0/set_env.sh
cmake -S . -B build-moonep \
  -DTILEXR_BUILD_MOONEP=ON \
  -DTILEXR_BUILD_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build-moonep -j"$(nproc)"
cmake --install build-moonep
```

MoonEP custom-kernel commands must list the implementation header selected by
the kernel entry in `DEPENDS`. Otherwise an implementation-header edit can
leave a stale installed kernel object during an incremental build. Verify this
through an incremental target-toolchain rebuild, then compare the rebuilt and
staged objects under the owning library's `tilexr/kernels` directory;
production-source substring checks are not accepted as a substitute for
compile, link, or behavior evidence.

After installation, require the production kernel directory and verify that
every enabled component installed its objects:

```bash
find install/lib*/tilexr/kernels -maxdepth 1 -type f -name '*.o' -print
```

Moving or packaging a TileXR Host library without its sibling
`tilexr/kernels` directory is unsupported and must fail before kernel
registration.

When staging a generated kernel from another CMake directory, make the staging
target depend on the kernel's producer target rather than only on the generated
file path. File-only cross-directory dependencies can leave Make without a
visible generation rule or trigger the same kernel command concurrently.

For Combine V2 schedule, workspace, Host validation, launch ABI, and public ABI,
run:

```bash
bash scripts/validate_combine_v2_host.sh
```

Set `TILEXR_CMAKE_BIN` when CMake is not on `PATH`, and
`TILEXR_COMBINE_V2_HOST_BUILD_DIR` to keep the build outside the source tree.
These tests verify Credit routing and Host contracts, but do not validate
Ascend C compilation, MTE ordering on hardware, or multi-rank correctness.

Before a multi-rank MoonEP hardware run, execute the official HCCL Test on the
same devices and data path. Check the per-rank correctness output as well as the
process status: HCCL operation failures can still exit with status 0. Stop the
TileXR run when the matching HCCL baseline reports an interface or correctness
failure.

Run the Host suite separately:

```bash
cmake -S tests/moonep -B tests/moonep/build-host
cmake --build tests/moonep/build-host -j"$(nproc)"
ctest --test-dir tests/moonep/build-host --output-on-failure
python -m pytest tests/moonep/python -q
```

Inspect the installed public library:

```bash
nm -D install/lib*/libtilexr-moonep.so.1 | \
  grep -E 'TileXRMoonEp(Planning|Dispatch|PrefetchWeight|ReduceGrad)V1'
nm -D install/lib*/libtilexr-moonep-combine-v2.so.2 | \
  grep -E 'TileXRMoonEpCombine(GetWorkspaceSize|Stage|StageV2Fused|V2)'
readelf -d install/lib*/libtilexr-moonep.so.1 | \
  grep -E 'NEEDED|RPATH|RUNPATH'
```

Expected:

- the remaining V1 Planning, Dispatch, PrefetchWeight, and ReduceGrad symbols are
  exported, while Combine is exported only by the V2 library;
- planner, dispatch, Combine V2, prefetch-weight, and reduce-grad Host libraries are NEEDED;
- RPATH is `$ORIGIN` and contains no CANN `devlib` directory;
- Host and Python fake-runtime tests pass without device execution.

Do not describe these checks as NPU correctness. When runtime testing is authorized,
run the compatibility-critical Python E2E probe on the intended physical A5 ranks in
an allowed grouped Dispatch mode, preserving its public call contract:

```bash
ASCEND_RT_VISIBLE_DEVICES=4,5,6,7 \
  TILEXR_MOONEP_DISPATCH_PEER_MODE=group_credit \
  torchrun --nproc-per-node=4 tools/moonep/test_npu_e2e.py
```

Require all ranks and all stages to pass. Keep the framework profiler and stage
barrier enabled for model or replay performance validation; use the MoonEP specs
for the runtime and Combine validation boundaries.

### MoonEP Python Correctness Reference

The CPU suite checks five-stage contracts, deterministic fixtures, the upstream
Planning oracle, downstream reference semantics, fault localization, CLI mode routing,
and report aggregation:

```bash
python -m pytest tests/moonep/python -q
```

This does not prove Torch-NPU or HCCL execution. On a CANN 9.1 host with four visible
A5/Ascend950 devices, run the independent reference separately:

```bash
python tools/moonep/run_benchmark.py \
  --mode reference \
  --cases tools/moonep/cases/correctness.json \
  --world-size 4 --physical-device-count 4 \
  --output-dir temp/moonep-reference
```

Require all five stage artifacts to pass on every rank. `performance_valid` must remain
false. Differential execution defaults to the built-in TileXR adapter. Run one rank
first against the installed libraries:

```bash
ASCEND_RT_VISIBLE_DEVICES=4 python -m tools.moonep.launcher \
  --mode correctness \
  --cases tools/moonep/cases/correctness.json \
  --case-ids planning-no-dedup \
  --world-size 1 --physical-device-count 1 \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir temp/moonep-correctness-1r
```

Then validate the same-node four-rank path:

```bash
ASCEND_RT_VISIBLE_DEVICES=4,5,6,7 python -m tools.moonep.launcher \
  --mode correctness \
  --cases tools/moonep/cases/correctness.json \
  --case-ids planning-no-dedup \
  --world-size 4 --physical-device-count 4 \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir temp/moonep-correctness-4r
```

The adapter rejects missing/stub stages, mismatched dimensions or device/rank metadata,
cross-node topology before Planning, and repeated destination ranks before the current
URMA Dispatch. External adapters remain available through
`--candidate-backend MODULE:FACTORY`. These runs are correctness-only and must retain
`performance_valid=false`.

## Run Host Checks

```bash
cd /path/to/TileXR/tests/udma
./install/bin/test_tilexr_udma_transport_layout
./install/bin/test_tilexr_udma_registry

source ../../scripts/common_env.sh
export LD_LIBRARY_PATH="$PWD/install/lib:../../install/lib:${LD_LIBRARY_PATH:-}"
RANK=0 RANK_SIZE=1 ./install/bin/test_tilexr_udma
```

## Verify Common Straggler Diagnostics

Build the independent diagnostics component, its install-prefix consumer, the
Collectives producer adapter, and the four-rank acceptance binary:

```bash
source scripts/common_env.sh
cmake -S . -B build \
  -DTILEXR_BUILD_COLLECTIVES=ON -DTILEXR_BUILD_TESTS=ON -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure \
  -R 'straggler|diagnostics|collectives_(api|header_compile|kernel_ownership)'
```

Before diagnosing TileXR on hardware, search the retained HCCL Test qualification for
the same host/container, NPU subset, topology, mode, CANN, driver, firmware, and test
build. Reuse a matching result. Only when no matching record exists, run the installed
official HCCL Test once on the same data path and retain per-rank correctness output;
exit code alone is insufficient.

Then run the final four-NPU acceptance test:

```bash
export LD_LIBRARY_PATH="$PWD/build/src/collectives:$PWD/build/src/diagnostics:$PWD/build/src/comm:${LD_LIBRARY_PATH:-}"
bash tests/collectives/run_collective_straggler_e2e.sh \
  build/tests/collectives run/collective-straggler-final 0
```

Require correct small and large AllReduce output, complete normal reports with all
ranks `NORMAL`, and three measured reports each for injected rank 1 and rank 2 with
only the selected rank `SLOW`. Retain `summary.json` and all rank logs. Report clock
precision as the p50, p95, and maximum `precision_ns` across at least 20 calibration
rounds of at least 32 samples. This value is the maximum selected half-RTT error bound
across ranks and applies only to the tested communication domain and software stack.

### Reported clock synchronization hardware acceptance

The following results were supplied by the validation owner for source commit
`7da31af`, based on `origin/main` `121541a`. CANN 9.1 incremental compilation
succeeded and the focused tests passed **8/8**. These are retained prior-run results,
not a hardware rerun of subsequent conflict-resolution commits.

| Domain | Error bound p50 / p95 / max (ns) |
| --- | --- |
| 2P | 331 / 333 / 333 |
| 8P | 587 / 588 / 588 |
| 64P | 587 / 590 / 590 |

The formal 64P run completed **64/64 ranks over 20 rounds**, with **32 warmup
samples and 128 measured samples per round**. The maximum synchronization error
bound was **590 ns (0.590 us)**. The post-test three-way checks confirmed that all
eight machines returned to **8/8 idle devices**.

Supplied patch SHA-256:
`02A064BA37860C40BE9F9913DA045AE94D3F63C72A08609371AA309C983129FA`.
The patch and raw per-rank logs were not supplied with this report, so this record
does not independently verify that digest or establish binary equivalence between
`7da31af` and the PR branch. Driver/firmware identity and retained artifact paths
were also not supplied; preserve them with the original run evidence when available.

Conflict integration against `origin/main` `b31c6c6a` was checked separately in a
Host-only environment: the existing analysis, runtime-hook session, public-header,
kernel-ownership, and external-binary-loader checks passed. The first three used
temporary minimal ACL declarations and fail-closed runtime stubs, not CANN libraries.
Collectives Host utility sources passed C++14 syntax compilation; the full test was
not linked because its communicator/kernel runtime dependencies were unavailable.
Shell syntax, Python compilation/CLI help, and an isolated CMake artifact staging and
installation smoke check also passed. Full CANN build, diagnostics install-prefix
consumer execution, and NPU precision/Collectives acceptance were not rerun here.

For the eight-node clock-only acceptance test, synchronize the same source and install
prefix to the same absolute path on every host, and put the hosts in rank order in a
one-host-per-line file. Launch from the controlling machine; it invokes only the fixed
worker script on each remote host and copies every rank artifact back for validation:

```bash
python tests/diagnostics/run_clock_sync_e2e_multihost.py \
  --host-file /path/to/hosts_64p.txt \
  --remote-root /tmp/tilexr-diagnostics \
  --remote-install-prefix /tmp/tilexr-diagnostics/install \
  --output-dir run/clock-sync-64p-final \
  --ranks-per-host 8 --rounds 20 --soc-name Ascend950 \
  --max-precision-ns 1000
```

Require `64/64 ranks`, identical SHA-256 values for the test binary,
`libtilexr-diagnostics.so`, `libtile-comm.so`, and the external
`tilexr/kernels/tilexr_diagnostics_op.o` on every node, and one consistent
communication-domain p50/p95/maximum precision tuple across all ranks. Checking only
the executable is insufficient because its dynamic libraries can differ by node. The
coordinator requires all three precision statistics to be strictly below the supplied
ceiling and rejects an existing output directory so stale rank JSON cannot make an
incomplete run look complete. Each node writes to its own local filesystem; always
collect and verify the exact global rank set rather than counting files on only the
root node. If the
deployment uses multiple bidirectional synchronization sessions from one source tree,
pause them during the run or exclude `run/`: propagating rank artifacts between hosts
creates duplicate results and adds unrelated filesystem traffic to the measurement.

Inspect the final shared library rather than an intermediate object:

```bash
readelf -d build/src/collectives/libtilexr-collectives.so | grep -E 'RPATH|RUNPATH'
ldd build/src/collectives/libtilexr-collectives.so | grep -E 'libascend_hal|libnnopbase'
```

The RPATH/RUNPATH must not contain `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib`, and
`libascend_hal.so` must resolve from the installed driver directory. For CCE source or
header changes, the build log must show regeneration of `tilexr_collectives_op.o` and
the staged `tilexr/kernels/tilexr_collectives_op.o`; otherwise the loader may still
read a stale operator binary. Diagnostics likewise installs
`tilexr/kernels/tilexr_diagnostics_op.o` beside its owning Host library. When merging
changes across kernel packaging revisions, preserve the external loader and stage/install
the final linked binary; rebuilding a temporary CCE object alone is insufficient.
Host checks cannot establish device precision after such an integration change.

On PCIe communication domains, the specialized `2npu` AllReduce path is valid only
for two ranks. Multi-rank domains must use the generic peer-memory AllReduce path;
selecting the two-rank path can silently reduce only a subset of ranks even though the
kernel launch succeeds. Cover both small and large messages with low-magnitude integer
inputs so the expected sum does not depend on signed-overflow behavior.

For synchronized collectives, do not classify only from each rank's local duration.
A rank that starts late may spend less time inside the collective because its peers
already waited, yet still finish last. Use the calibrated aligned completion time as a
second robust signal and retain separate start, duration, and completion reason flags.
Also retain a communication-ready timestamp inside the same real collective kernel.
An injected or real rank-local stall before communication makes peers wait inside the
collective, so start/end alone can make all ranks finish together and cannot identify
the initiator. Classify a positive `start -> ready` outlier separately with the
pre-communication reason only when it meets the absolute, relative-to-median-duration,
and MAD gates. A separate marker launch is not equivalent evidence.

Clock synchronization and report collection are collective control-plane operations.
Every rank must enter the same bounded Host exchange even when a local preflight or
runtime step fails; returning early on one rank can otherwise strand its peers in the
socket exchange. Clock calibration rejects terminal operator failures and requires at
least 75 percent and at least four valid samples. Multi-stream observation must retain
the producer stream for each ring slot, synchronize that stream before copying the
record, and synchronize every outstanding producer stream before freeing detector
device memory. The control exchange is a fixed 64-byte envelope and must converge the
operation, sequence, launch ID, generation, configuration hash, rank, and status before
reading device results. After asynchronous clock-buffer initialization, retain the
clock stream until synchronization succeeds; on a synchronization failure, retry that
same stream on the next calibration or destroy call instead of clearing unrelated
record-stream ownership. A terminal clock sample fails the complete round with timeout;
only non-terminal invalid timing samples participate in the 75-percent validity rule.
Acquire the reusable device magic only after clock preflight converges successfully;
otherwise healthy ranks can consume a round while a rank with an unrecovered stream
does not, and the next calibration will use inconsistent magic values.

For multi-rank clock calibration, publish a per-peer READY state before starting the
four-timestamp interval; otherwise rank 0 service queueing grows the measured RTT with
rank count. Have the requester push REQUEST to rank 0 instead of making rank 0 pull
remote state, and timestamp the midpoint of each measured MTE operation window to avoid
assigning the full transfer latency to one endpoint. REQUEST and ACK must use distinct
per-rank 64-byte slots: adjacent state transitions in the same remotely updated cache
line can remain stale to the polling receiver and cause a timeout even when the MTE
operation completed.
The RESPONSE MTE3 midpoint cannot be embedded in the same payload because it is known
only after that publication completes. Publish it through a third per-rank TIMESTAMP
slot after the peer has already captured `t3`; this follow-up transfer stays outside
the corrected RTT while making all four timestamps use symmetric MTE window midpoints.
If collection cannot synchronize a producer stream, retain that launch in its ring
slot and retry the same stream on the next collective collect call. Do not advance the
launch ID or permit slot reuse while any producer remains outstanding; ranks that
already synchronized the slot may reuse the copied device record during the collective
retry. Once every producer is synchronized, an incomplete observation report is safe
to consume even though collection still returns its classification error.

Observation collection is FIFO. Every communication launched after a detector owns
the communicator reserves a ring entry, including warmups. Tests must collect or drain
each such communication before asserting a later injected launch; otherwise a valid
older record can be mistaken for missing instrumentation in the newer launch.

Mock Host all-gather implementations must identify control protocols by a type tag or
discriminator as well as payload size. `TileXRClockRankResult` and the current timing
record are both 80 bytes; dispatching only on `sendBytes` silently treats one as the
other and corrupts fields such as `readyCycle` while leaving the mock call successful.

Expected:

- `TileXR UDMA transport layout checks passed`
- `TileXR UDMA JSON checks passed`
- `TileXR UDMA registry checks passed`
- `test_tilexr_udma` exits with `Failed: 0`

## Verify Integrated A5 AllToAll

Build collectives with the CANN 9.1 A5 compiler environment. This produces one
Host library containing the generic collective binary and the separately built
64/128-rank A5 binaries:

```bash
cmake -S . -B /tmp/tilexr-collectives-a5 \
  -DTILEXR_BUILD_COLLECTIVES=ON \
  -DTILEXR_BUILD_TESTS=ON \
  -DTILEXR_COLLECTIVES_SOC_TYPE=Ascend950 \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-collectives-a5 -j"$(nproc)"
ctest --test-dir /tmp/tilexr-collectives-a5 --output-on-failure \
  -R 'alltoall_a5_layout|comm_next_magic|collectives'
cmake --install /tmp/tilexr-collectives-a5
```

When a collectives Host source consumes a private header from another TileXR
component, declare that component directory as a `PRIVATE` include directory on
`tilexr-collectives`. Validate from a clean build directory so same-directory or
stale compiler include paths cannot hide a missing target dependency. Each Host
header must also include the defining headers for every type in its declarations;
keep the matching source's own header first in its include list to expose missing
dependencies during compilation. Test targets that directly consume private Host
headers must declare the private include directories required by those headers;
linking `tilexr-collectives` does not propagate its `PRIVATE` include paths.

The full `tilexr-collectives` target is the compile gate: it must rebuild both the
generic collective binary and every grouped A5 binary from a clean directory.
Compiling only a grouped binary can miss shared-header API drift in the generic
kernel, including profiling recorder signature changes, because all embedded
binaries are required inputs of the same Host library.

The shared 256-rank ABI also enlarges rank-sized local state in the generic
AllGather kernels. CANN 9.1 targeting `dav-c220-vec` reports a 20,160-byte
frame, above the former 16,000-byte function limit. Set both `-cce-aicore-function-stack-size` and
`-cce-aicore-stack-size` to `0x8000` (32 KiB), retaining the compiler's stack
limit check. Rebuild the generic `tilexr_collectives_op` target for both
`Ascend950` (`dav-c310-vec`) and `Ascend910B` (`dav-c220-vec`) after rank-limit
changes; an EP-only build does not compile these consumers of the shared ABI.
A successful cross-build does not establish 256-rank runtime correctness or
performance.

Expected artifact and removed-surface checks:

```bash
test -f install/lib/libtilexr-collectives.so
test ! -e install/lib/libtilexr-grouped-alltoall.so
test ! -e install/include/tilexr_grouped_alltoall.h
readelf -d install/lib/libtilexr-collectives.so | grep -E 'RPATH|RUNPATH' || true
```

No RUNPATH/RPATH entry may contain `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib`.
When `devlib` is required as a link-time fallback, keep the search order as
`${ASCEND_DRIVER_PATH}/lib64/driver`, `${ASCEND_HOME_PATH}/${ARCH}-linux/lib64`,
then `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib`; otherwise stub libraries can
shadow the real driver HAL or runtime libraries during linking.
Host tests establish layout, exact profile classification, and communicator
resource teardown only. They do not prove A5 UDMA transfers.

For data-plane qualification, retain the matching one-time HCCL Test baseline,
then validate 2x8, 3x8, 4x8, and 8x8 in that order. The 3x8 run is the required
non-power-of-two rank-count gate. For every scale cover generic
AllToAll regression, repeated A5 calls, a larger call that grows the workspace,
communicator teardown, and profiling with complete fixed-slot rounds for every
active core. Record correctness and performance per rank; do not claim an
unrun scale from Host, simulator, 910B, or target-compilation evidence.

The integrated A5 smoke must initialize the communicator with the same
shared-QP-domain contract as the validated grouped baseline. For the standalone
performance tool, use `--comm-mode shared-qp --comm-domain 61` and enable both
`TILEXR_ENABLE_CREDIT_IPC=1` and `TILEXR_ENABLE_SDMA=1` on every rank. Also set
`TILEXR_UDMA_SHARED_QP_COUNT=39`; the shared-QP initializer otherwise defaults
to a different profile. A socket communicator created by `TileXRCommInitRank`,
or a shared-QP communicator without the exact validated 39-QP profile, does not
satisfy the A5 dispatch predicate. A successful generic fallback is not evidence
for the grouped path. The multi-host profiling suite applies these settings
automatically to int32 AllToAll cases while leaving generic cases unchanged.

After changing UDMA teardown, validate lifecycle reuse rather than accepting a
single clean data transfer. At 16P, 24P, 32P, and 64P, run at least 20 consecutive
`init -> run -> destroy -> init` cycles with the grouped shared-QP profile. Every
destroy and immediate reinitialization must succeed, logs must contain no TileXR
`unimport failed`, `cleanup incomplete`, `128103`, double-free, or invalid-pointer
records, and RA/QP resource counts must remain stable. On b131, lower-level plog
may still report `RaCtxQpUnimport ... ret(-93)`: the service rejected explicit
unimport, while the client consumed the Host handle and TileXR continued through
RA context teardown. Preserve and fail on every other raw RA error. Host-only
checks do not satisfy this hardware teardown boundary.

## Run UDMA Data-Plane Demos

Run only on A5 / Ascend950 / 950 hardware:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

Expected:

- every rank exits 0;
- every rank log includes `TileXR UDMA demo success`;
- every rank log includes `TileXRUDMARegister success`;
- no rank log includes `DATA MISMATCH`, signal mismatch text, or `ERROR`.

Quick log check:

```bash
latest=$(ls -td logs/tilexr_udma_demo_* | head -n1)
grep -R "TileXR UDMA demo success" "$latest"
grep -R "TileXRUDMARegister success" "$latest"
grep -R "DATA MISMATCH\\|expected non-local signals\\|TileXR UDMA demo failed\\|ERROR" "$latest" || true
```

The final grep should print nothing for a clean run.

## Common Failures

| Symptom | Likely Cause | Action |
| --- | --- | --- |
| Missing CANN headers | `common_env.sh` not sourced or CANN path mismatch | Source the environment and confirm CANN 9.1.0 layout |
| Cannot find `ascend_hal` | Driver HAL path is absent or loader paths select the toolkit stub | Source the supported environment and require `libascend_hal.so` to resolve from `${ASCEND_DRIVER_PATH}/lib64/driver`; never add toolkit `devlib` to runtime RPATH |
| Demo target skipped | `bisheng` unavailable | Install/compiler configure `bisheng`, or run host-only tests |
| UDMA disabled in demo | Unsupported hardware or HCCP/RA runtime unavailable | Use A5 / Ascend950 / 950 and check CANN driver/runtime libraries |
| shmem appears in `ldd libtile-comm.so` | Unexpected dependency regression | Inspect `src/comm/CMakeLists.txt` and source includes |

## Report Template

```text
Machine:
NPU model:
NPU count:
CANN version:
Driver version:
TileXR commit:

Core build:
ldd shmem check:
UDMA host tests:
UDMA all-gather demo:
UDMA put-signal demo:
Log directory:

Errors or warnings:
```
