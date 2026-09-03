# TileXR Direct CCU Handoff

This document is the maintained handoff for TileXR-owned direct CCU support.
It replaces the earlier investigation log. Keep it factual and update it only
when the production path, public API, or validation gate changes.

## Current Status

TileXR can prepare, install, submit, and validate a two-rank direct CCU flow
without a production dependency on hcomm, HCCL private CCU producers, or
`libmc2_client.so`.

Validated path:

- Direct CCU repository and mission installation.
- Mission/key/taskInfo generation for `rtCCULaunch`.
- XN, CKE, channel, PFE, and jetty lower-layer resource installation.
- Barrier smoke through CCU synchronization instructions.
- P2P data-plane validation through CCU memory-copy microcode.

Completion evidence from the 950 validation server:

```text
tilexr_ccu_direct_evening_smoke finalStatus prepare=pass submit=pass barrier=pass p2p=pass
tilexr_ccu_direct_smoke p2pCcuCopy ... mismatches=0 ... passed=1
TileXRDirectCcuTrace program.sync[5] decoded=TransRmtMemToLocMem ...
TileXR CCU dependency guard passed: no hcomm/HCCL private CCU dependency or symbol reference
```

## Production Boundary

`src/comm` must not link or include hcomm or HCCL private CCU producer APIs.
The allowed production dependency surface is CANN runtime/ACL plus TileXR-owned
CCU code under `src/comm/ccu`.

Keep these out of production code:

- `libhcomm.so`, `libhccl_v2.so`, `libhccl_fwk.so`, `libmc2_client.so`
- `HcclGetCcuTaskInfo`, `HcomGetCcuTaskInfo`
- hcomm private resource allocators, repositories, and channel abstractions
- AscendC kernel-side `Hccl<HCCL_SERVER_TYPE_CCU>` integration until TileXR has
  a stable TileXR-owned context producer for that exact ABI

Run the dependency guard after every CCU production edit:

```bash
source scripts/common_env.sh
bash tests/ccu/check_tile_comm_no_hcomm_deps.sh build/src/comm/libtile-comm.so
```

## Architecture

Main modules:

- `tilexr_ccu_direct_runtime.*`: owns direct runtime interaction and basic CCU
  information discovery.
- `tilexr_ccu_driver_adapter.*`: wraps the low-level custom-channel driver
  operations used for CCU resource and repository installation.
- `tilexr_ccu_hccp_loader.*`: resolves TileXR-owned HCCP/RA entry points.
- `tilexr_ccu_install_provider.*`: installs lower-layer CCU resources,
  repository images, and missions.
- `tilexr_ccu_repository.*`: builds and uploads the instruction repository.
- `tilexr_ccu_resource_allocator.*`: reserves mission, instruction, XN, CKE,
  GSA, and channel ranges.
- `tilexr_ccu_producer_plan.*`: builds the direct CCU program and submit tasks.
- `tilexr_ccu_barrier_program.*`: emits synchronization microcode.
- `tilexr_ccu_memory_program.*`: emits CCU memory-copy microcode.
- `tilexr_ccu_direct_orchestrator.*`: joins allocation, lower-layer planning,
  repository installation, mission installation, and launch-package creation.

Host integration:

- `TileXRComm::PrepareDirectCcuInstallAttempt(...)` prepares generic direct CCU
  tasks.
- `TileXRComm::PrepareDirectCcuMemoryCopyInstallAttempt(...)` prepares P2P CCU
  copy tasks.
- `TileXRCommPrepareDirectCcu(...)` and
  `TileXRCommPrepareDirectCcuMemoryCopy(...)` expose the public C API.

Runtime launch:

- The final launch path uses `TileXRDirectCcuSubmitPrepared(...)` or
  `TileXRDirectCcuSubmitPreparedTask(...)`.
- `rtCCULaunch` is reached through `libruntime.so`; no hcomm launch wrapper is
  required.

## Public API

`src/include/tilexr_api.h` exposes:

```c
int TileXRCommPrepareDirectCcu(
    TileXRCommPtr comm,
    const TileXRDirectCcuPrepareOptions* options,
    TileXRDirectCcuPreparedTasksPtr* prepared,
    TileXRDirectCcuPrepareReport* report);

int TileXRCommPrepareDirectCcuMemoryCopy(
    TileXRCommPtr comm,
    const TileXRDirectCcuMemoryCopyPrepareOptions* options,
    TileXRDirectCcuPreparedTasksPtr* prepared,
    TileXRDirectCcuPrepareReport* report);

int TileXRDirectCcuSubmitPrepared(
    TileXRDirectCcuPreparedTasksPtr prepared,
    aclrtStream stream,
    TileXRDirectCcuSubmitReport* report);

int TileXRDirectCcuSubmitPreparedTask(
    TileXRDirectCcuPreparedTasksPtr prepared,
    uint32_t taskIndex,
    aclrtStream stream,
    TileXRDirectCcuSubmitReport* report);

int TileXRDirectCcuDestroyPrepared(TileXRDirectCcuPreparedTasksPtr prepared);
```

Memory-copy direction constants:

```c
TILEXR_DIRECT_CCU_MEMORY_COPY_REMOTE_TO_LOCAL
TILEXR_DIRECT_CCU_MEMORY_COPY_LOCAL_TO_REMOTE
```

The direct P2P smoke currently validates remote-to-local transfer on both ranks.

## Key Lessons

- The AscendC CCU `HcclCombineOpParam` route is not a validated TileXR
  integration path. Public MC2 allocation probes returned AICPU/MC2 context
  shapes, not the CCU context consumed by `HCCL_SERVER_TYPE_CCU`.
- `rtCcuTaskInfo_t` and `rtCCULaunch` are reachable through CANN runtime, but
  the hard part is producing the repository, mission key, lower-layer resources,
  and task fields correctly.
- Repository installation requires the RA context resource-window route. A zero
  or synthetic resource-window token can let lower-layer calls appear partially
  valid while `SET_INSTRUCTION` still fails.
- The passing lower-layer route uses hcomm-compatible semantics without linking
  hcomm: reverse endpoint EID and imported peer TPN are important.
- P2P validation must use CCU copy microcode. The old marker/IPC harness did not
  prove the direct CCU data plane and has been removed from the maintained smoke.
- Process bring-up overrides for runtime task fields, submit-task arguments,
  peer remote-XN/CKE bindings, and hcomm-trace resource remapping were removed
  from maintained code. The formal path relies on exchanged TileXR resource
  evidence.
- CCU direct work is independent of UDMA. Do not use UDMA code as a reference
  for CCU resource installation or data movement.
- CANN 9.1 runtime declares `rtUbDevQueryInfo` in
  `runtime/rts/rts_device.h`; the `tile-comm` target must include
  `${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/runtime/`.

## Maintained Validation

Local source/unit checks:

```bash
python -m unittest \
  tests.ccu.test_tilexr_ccu_direct_smoke_probe \
  tests.ccu.test_tilexr_ccu_public_comm_api \
  tests.ccu.test_tilexr_ccu_direct_smoke_runner \
  tests.ccu.test_tilexr_ccu_direct_orchestrator \
  tests.ccu.test_tilexr_ccu_memory_program \
  tests.ccu.test_tilexr_ccu_microcode
```

Broader CCU checks:

```bash
python -m unittest discover tests/ccu
```

Remote build and no-hcomm guard:

```bash
source scripts/common_env.sh
cmake --build build --target tile-comm -j2
bash tests/ccu/check_tile_comm_no_hcomm_deps.sh build/src/comm/libtile-comm.so
```

Hardware smoke:

```bash
source scripts/common_env.sh
env \
  TILEXR_CCU_SMOKE_DEVICES=0,1 \
  TILEXR_CCU_SMOKE_ALLOW_UNHEALTHY_NPU=1 \
  TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE=ra_ctx \
  TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES=prepare,submit,barrier,p2p \
  TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES='ra_ctx_full:acl:full_repository:instruction_bytes:lower_layer_first' \
  TILEXR_CCU_EVENING_TOTAL_TIMEOUT=180 \
  bash tests/ccu/run_tilexr_ccu_direct_evening_smoke.sh
```

Expected final line:

```text
tilexr_ccu_direct_evening_smoke finalStatus prepare=pass submit=pass barrier=pass p2p=pass
```

Expected P2P evidence in rank logs:

```text
tilexr_ccu_direct_smoke p2pCcuCopy ... mismatches=0 ... passed=1
decoded=TransRmtMemToLocMem
```

## Development Rules

- Keep direct CCU code under `src/comm/ccu` and public API declarations under
  `src/include/tilexr_api.h`.
- Keep hardware smoke scripts default-safe; they must require explicit env
  opt-in before touching NPU devices.
- Do not reintroduce marker-based P2P success criteria. It does not prove CCU
  transfer.
- Do not add runtime env overrides that mutate prepared task fields, prepared
  task arguments, or peer binding proof. They make the smoke hard to interpret
  and are not a production integration path.
- Keep verbose CCU instruction/resource traces behind env gates. They are useful
  for hardware failures, but should not be required for normal validation.
- Prefer small unit tests for microcode, repository layout, resource allocation,
  and submit-task packaging. Use hardware smoke only for the final device-plane
  proof.
