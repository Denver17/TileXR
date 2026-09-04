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
cmake -S . -B /tmp/tilexr-build -DCMAKE_INSTALL_PREFIX="$PWD/install"
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

EP uses one installation layout for every deployment environment. Do not pass
`TILEXR_SOC_NAME` or an EP SoC CMake option when configuring:

```bash
cmake -S . -B /tmp/tilexr-ep-build \
  -DTILEXR_BUILD_EP=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-ep-build --target tilexr-ep -j"$(nproc)"
cmake --install /tmp/tilexr-ep-build
```

The four EP kernels are A5-only objects even when the compiler runs on a 910B
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
  tilexr_ep_combine_memory_kernel.o; do
  test -f "${ep_lib_dir}/tilexr/kernels/${object}"
done
! find "${ep_lib_dir}" -maxdepth 1 -type f \
  \( -name 'libtilexr_ep_dispatch_kernel.so' -o \
     -name 'libtilexr_ep_combine_kernel.so' \) | grep -q .
```

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

Expected artifact and removed-surface checks:

```bash
test -f install/lib/libtilexr-collectives.so
test ! -e install/lib/libtilexr-grouped-alltoall.so
test ! -e install/include/tilexr_grouped_alltoall.h
readelf -d install/lib/libtilexr-collectives.so | grep -E 'RPATH|RUNPATH' || true
```

No RUNPATH/RPATH entry may contain `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib`.
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
| Cannot find `ascend_hal` | `devlib` path missing | Use the current top-level CMake configuration |
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
