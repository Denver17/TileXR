# TileXR

**TileXR** (eXtreme Rendezvous for Asynchronous Tile Communication) is a data-centric asynchronous communication runtime for Huawei Ascend NPUs. It moves communication control from coarse BSP-style kernel phases toward tile-level, AICore-driven rendezvous: data readiness, transport choice, and synchronization become explicit runtime state instead of a fixed all-ranks barrier.

The project currently contains a core communication library, an optional TileXR collectives library, a standalone Expert Parallelism (EP) dispatch MVP, a registered-memory UDMA prototype for A5 / Ascend950 hardware, an opt-in on-card SDMA copy transport, and simulator/test infrastructure for Ascend C kernels.

## Design Direction

TileXR is designed around three ideas from the current architecture deck:

- **Tile as the unit of progress**: split large BSP communication phases into smaller data tiles that can be produced, transferred, synchronized, and consumed independently.
- **AICore-driven asynchronous rendezvous**: let device code observe data readiness and runtime state, then advance communication without repeatedly returning to host scheduling.
- **Dynamic communication semantics**: choose among IPC/MTE, direct-drive UDMA/RDMA-style paths, notify/data-as-flag synchronization, and future offload paths according to data size, link state, peer readiness, and resource pressure.

The current codebase implements the base communication runtime, flag-based synchronization, standalone collectives and EP paths, and an A5 UDMA registered-memory path. Broader dynamic scheduling, CMO best-effort scheduling, and CCU offload are design targets and should be treated as roadmap unless a specific implementation file says otherwise.

<p align="center">
  <img src="docs/diagrams/tile-rendezvous.drawio.svg" alt="From BSP phases to tile-level asynchronous rendezvous" width="900"/>
</p>

Instead of stalling every rank at coarse barriers, TileXR splits a phase into tiles and lets each tile be produced, transferred, flag-synchronized, and consumed independently — so work on different tiles overlaps and the device advances without repeated host round-trips.

## Features

- **Core communication runtime**: `libtile-comm.so` initializes ranks, shared buffers, peer memory mappings, socket exchange, device `CommArgs`, and DFX state. It builds only against CANN runtime/ACL/driver APIs and TileXR-owned types; it does not include or link HCCL, shmem, or reference-only source trees.
- **Optional TileXR collectives**: `libtilexr-collectives.so`, built only when `TILEXR_BUILD_COLLECTIVES=ON`, layers standalone `TileXRAllGather` and equal-size `TileXRAllToAll` APIs on top of `libtile-comm.so`.
- **Standalone EP dispatch/combine MVP**: `libtilexr-ep.so`, `libtilexr_ep_dispatch_kernel.so`, and `libtilexr_ep_combine_kernel.so` provide TileXR-native MoE EP dispatch/combine routes under `src/ep`, independent from HCCL window helpers, `ops-transformer`, and shmem. The peer-memory backend supports both same-node and cross-node dispatch/combine; the UDMA backend uses TileXR-registered workspaces.
- **Tile-level synchronization**: device-side flag regions and magic values support reusable fine-grained synchronization rounds.
- **Registered-memory UDMA path**: host code registers ordinary `aclrtMalloc` device memory with `TileXRUDMARegister`; device kernels use `tilexr_udma.h` wrappers for put/get/signal.
- **On-card SDMA transport**: an opt-in (`TILEXR_ENABLE_SDMA=1`) local GM-to-GM copy path. Host code queries it with `TileXRSDMAAvailable` / `TileXRGetSDMAWorkspaceDev`; device kernels use `tilexr_sdma.h` (`SDMACopyNbi`, `SDMAWait`). Separate from UDMA: SDMA is local to one device, UDMA targets registered remote memory.
- **Operator simulator**: `op-simulator/` supports functional/performance simulation for selected AICore kernels without physical hardware.

## System Requirements

- **User**: root access or membership in the Ascend driver user group is typically required for CANN runfile installation and NPU device operations
- **NPU driver**: 25.1.rc1 or later, check with `npu-smi info`
- **CANN**: current build scripts and CMake are aligned to CANN 9.1.0
- **Core supported chips**: Ascend 910B, 910A5
- **UDMA runtime validation target**: A5 / Ascend950 / 950 only

UDMA builds or smoke tests on 910B or other non-A5 devices are not valid UDMA data-plane validation.

### System Dependencies

```bash
apt install -y build-essential git python3
```

```bash
yum install -y gcc gcc-c++ make git python3
```

## Quick Start

### 1. Clone Repository

```bash
git clone --recursive https://github.com/LingquLab/TileXR.git
cd TileXR
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

For Codex-assisted development, install the Ascend C skills and start a new Codex task:

```bash
codex plugin marketplace add https://github.com/LingquLab/skills.git
codex plugin add ascendc-development@lingqulab
```

### 2. Prepare Environment

For a fresh checkout, install the repo-managed CANN 9.1.0 toolkit and ops package before building:

```bash
bash scripts/cann_download_install.sh
source scripts/common_env.sh
```

If the host already has a readable CANN 9.1.0 install, you can use it instead:

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
source scripts/common_env.sh
```

`scripts/common_env.sh` sets `TILEXR_HOME`, `TILEXR_CANN_HOME`, `TILEXR_TEMP_HOME`, architecture, SOC name, and CANN paths.
For non-root builds, if the system driver headers are not readable, it automatically uses readable driver headers from the repo-managed CANN install while still linking against the system driver libraries.

For first-time setup of local utilities:

```bash
bash scripts/prepare.sh
```

### 3. Build Core Runtime

```bash
source scripts/common_env.sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j"$(nproc)"
cmake --install build
```

Expected output:

```text
install/lib*/libtile-comm.so
```

To build the optional TileXR collectives library and its tests/tools:

```bash
source scripts/common_env.sh
cmake -S . -B build-collectives \
  -DTILEXR_BUILD_COLLECTIVES=ON \
  -DTILEXR_BUILD_TESTS=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build-collectives -j"$(nproc)"
cmake --install build-collectives
```

Additional expected output:

```text
install/lib*/libtilexr-collectives.so
install/include/tilexr_collectives.h
```

### 4. Run Basic Tests

```bash
bash scripts/test_build.sh
bash scripts/test_allreduce.sh
bash scripts/plog_grep.sh ERROR
```

## Repository Structure

```text
TileXR/
|-- src/
|   |-- comm/                 # Core communication runtime
|   |   |-- udma/             # TileXR-owned HCCP/RA UDMA transport
|   |   `-- sdma/             # On-card PTO SDMA local copy transport
|   |-- collectives/          # Optional TileXR collectives library
|   |-- ep/                   # Standalone TileXR EP dispatch MVP
|   `-- include/              # Public C/C++ and device headers
|-- op-simulator/             # Ascend C kernel simulation
|-- tests/                    # Host, communication, integration, and UDMA tests
|   |-- collectives/          # Collectives source/unit checks and manual runners
|   |-- comm/
|   |-- ep/                   # EP Host checks, build helper, and 2-rank demo
|   |-- udma/
|   `-- sdma/                 # SDMA unit tests, integration test, and data-plane demo
|-- scripts/                  # Build, setup, test, and utility scripts
|-- 3rdparty/                 # spdlog submodule
|-- reference/                # ignored reference-only source, including hcomm and ops-transformer
`-- docs/                     # Design, migration, and validation notes
```

## Architecture

The runtime is layered: applications and integrations sit on top of the TileXR libraries, which expose a public API and device headers over three interchangeable transports, all built on the CANN runtime, driver HAL, and Ascend NPU hardware.

<p align="center">
  <img src="docs/diagrams/architecture-overview.drawio.svg" alt="TileXR layered architecture" width="940"/>
</p>

### Core Runtime

`src/comm/` builds `libtile-comm.so` and exposes the public API in `src/include/tilexr_api.h`. This library is intentionally independent of HCCL, shmem, and reference-only source trees. It uses CANN runtime/ACL/driver APIs plus TileXR-owned communication metadata and datatypes.

Important host-side entry points, grouped by role:

- **Lifecycle**: `TileXRGetUniqueId`, `TileXRCommInitRankLocal`, `TileXRCommInitRank`, `TileXRCommInitRankWithDomain`, `TileXRCommDestroy`.
- **CommArgs access**: `TileXRGetCommArgsHost` (host view), `TileXRGetCommArgsDev` (device pointer for kernels).
- **Synchronization rounds**: `TileXRCommNextMagic` hands out a fresh magic value so callers can reuse flag memory across rounds; the optional collectives library uses it to schedule per-launch synchronization.

The runtime allocates shared IPC buffers, exchanges peer mappings, uploads `CommArgs` to device memory, and records topology/capability flags in `CommArgs::extraFlag`. UDMA and SDMA are brought up as best-effort capabilities — if either is unavailable, initialization continues without setting its `extraFlag` bit.

<p align="center">
  <img src="docs/diagrams/comm-init-flow.drawio.svg" alt="Communicator initialization flow" width="640"/>
</p>

### Device Synchronization

`src/include/tilexr_sync.h` provides device-side flag synchronization. Flags use magic values so multiple rounds can reuse the same flag memory without a full reset.

### Optional TileXR Collectives

`src/collectives/` builds `libtilexr-collectives.so` when `TILEXR_BUILD_COLLECTIVES=ON`. The split is intentional:

- `libtile-comm.so` owns communicator setup, peer memory, `CommArgs`, UDMA metadata, and the infra public API in `tilexr_api.h`.
- `libtilexr-collectives.so` owns collectives host validation, launch, embedded CCE kernel registration, and the public collectives API in `tilexr_collectives.h`.
- Installing only the default core runtime does not install `tilexr_collectives.h`.

Initial collectives APIs:

- `TileXRAllGather`
- `TileXRAllToAll` for equal per-peer counts

`TileXRAllGather` supports the validated multi-rank path. Multi-rank `TileXRAllToAll` is currently enabled only when the communicator reports the supported `TOPO_910_93` topology; unsupported topologies return a parameter-check error instead of launching an invalid kernel path. Single-rank loopback is supported for both APIs.

### Standalone EP Dispatch

`src/ep/` builds the first TileXR-native Expert Parallelism dispatch path:

- `libtilexr-ep.so` exposes the canonical `TileXRMoeEpDispatch`, `TileXRMoeEpCombine`, `TileXRMoeEpDispatchMemory`, and `TileXRMoeEpCombineMemory` entries through `src/include/tilexr_ep.h`.
- `libtilexr_ep_dispatch_kernel.so` contains the Ascend C dispatch kernels.
- `libtilexr_ep_combine_kernel.so` contains the Ascend C combine kernels, including the cross-node UDMA combine path.
- Host code validates MoE shape, dtype, communicator state, and IPC window size before launch.
- The memory route uses `CommArgs::peerMems[]`, `TileXR::IPC_DATA_OFFSET`, and `SyncCollectives` for peer-memory communication across both same-node and cross-node peers. Each rank writes its own peer-memory window, and peers read from that window after synchronization.
- The UDMA route uses a workspace registered with `TileXRUDMARegister`; remote ranks exchange window slots and ready values through the UDMA registry.
- Shared EP window metadata is written through MTE/UB copies so peer ranks observe slot headers and assist tuples consistently.

This EP path is intentionally independent from HCCL window helpers, `ops-transformer`, and shmem while reusing TileXR core IPC/UDMA runtime state.

### Transports Overview

TileXR offers three data-plane transports that kernels select by data size, link state, peer readiness, and capability flags. Memory uses peer-memory windows across same-node or cross-node peers and is generally better for small transfers. UDMA targets registered remote memory on A5 / Ascend950 and is generally better for large transfers. SDMA performs a local on-card copy within a single device.

<p align="center">
  <img src="docs/diagrams/transport-paths.drawio.svg" alt="TileXR transport data paths between ranks" width="940"/>
</p>

### UDMA Registered Memory

The current UDMA path is TileXR-owned:

- `TileXRComm::InitUDMA()` tries to initialize UDMA for multi-rank communicators.
- `src/comm/udma/tilexr_hccp_loader.*` dynamically loads CANN HCCP/RA runtime libraries such as `libra.so` and `libtsdclient.so`.
- `src/comm/udma/tilexr_udma_transport.*` creates contexts, queues, route metadata, and a device-side `TileXR::UDMAInfo` image.
- `TileXRUDMARegister` registers ordinary device memory and exchanges remote region metadata.
- `CommArgs::udmaInfoPtr` and `CommArgs::udmaRegistryPtr` make queue and registered-memory metadata visible to kernels.
- `src/include/tilexr_udma.h` provides `UDMAPutNbi`, `UDMAGetNbi`, `UDMAPutSignalNbi`, and `UDMAQuiet`.

If UDMA is unavailable, communicator initialization continues without setting `ExtraFlag::UDMA`. UDMA-specific registration or demo paths then report that UDMA is unavailable.

### SDMA Local Transport

SDMA is a first-class local on-card GM-to-GM copy path, separate from UDMA. It is disabled by default and enabled with `TILEXR_ENABLE_SDMA=1`.

- `TileXRComm::InitSDMA()` owns a `TileXRSDMATransport` beside the UDMA transport. When enabled, it creates a PTO `pto::comm::sdma::SdmaWorkspaceManager`, stores its device workspace address in `CommArgs::sdmaWorkspacePtr`, and sets `ExtraFlag::SDMA`.
- Host queries: `TileXRSDMAAvailable(comm, &available)` and `TileXRGetSDMAWorkspaceDev(comm, &workspace)`. The workspace pointer is owned by `TileXRComm` and must not be freed.
- Device API: `src/include/tilexr_sdma.h` provides `TileXR::SDMACopyNbi` and `TileXR::SDMAWait`, accepting raw same-device GM pointers. It does not register memory or validate buffer ownership.
- PTO SDMA header differences across CANN 9.0.0 / 9.1.0 are isolated in `src/include/tilexr_sdma_compat.h`.

Enabled initialization is best-effort: if PTO SDMA headers or runtime resources are unavailable, communicator initialization continues without setting `ExtraFlag::SDMA`, and `SDMACopyNbi` returns event handle `0` while `SDMAWait` reports completion. See [docs/SDMA_TRANSPORT.md](docs/SDMA_TRANSPORT.md) for the full transport guide.

## Dependencies

| Component | Version / Source | Purpose |
| --- | --- | --- |
| CANN | 9.1.0 | Required for `libtile-comm.so` and optional `libtilexr-collectives.so`: Ascend ACL/runtime/driver headers and libraries |
| spdlog | submodule | Header-only optional backend for TileXR logging; `src/comm/tilexr_log.h` falls back to direct stdout/stderr logging when unavailable |

Optional components:

| Component | Version / Source | Used by | Notes |
| --- | --- | --- | --- |
| hcomm | on-demand checkout under `reference/` | Legacy HCCL resource/context comparison | Downloaded with `bash reference/download_cann_repos.sh hcomm`; not a TileXR build dependency |
| ops-transformer | on-demand checkout under `reference/` | Upstream implementation comparison | Downloaded with `bash reference/download_cann_repos.sh ops-transformer`; not a TileXR build dependency |
| MoonEP | on-demand checkout under `reference/` | MoonEP Planner and MoE operator behavior comparison | Downloaded with `bash reference/download_moonep.sh`; not a TileXR build dependency |

## UDMA Validation

Use the dedicated UDMA guides when validating A5 / Ascend950 / 950 hardware:

```bash
cd tests/udma
bash build.sh
./install/bin/test_tilexr_udma_transport_layout
./install/bin/test_tilexr_udma_registry
```

Run data-plane demos only on A5 / Ascend950 / 950:

```bash
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

See:

- [tests/udma/README.md](tests/udma/README.md)
- [tests/udma/QUICKSTART.md](tests/udma/QUICKSTART.md)
- [tests/udma/demo/ASCEND_VERIFICATION.md](tests/udma/demo/ASCEND_VERIFICATION.md)

## SDMA Validation

Build and run the SDMA unit tests against a selected CANN install, then run the data-plane demo on a device:

```bash
bash tests/sdma/build.sh /path/to/cann
bash tests/sdma/run_tests.sh /path/to/cann
bash tests/sdma/demo/run_tilexr_sdma_demo.sh /path/to/cann 0 64 4096 1048576
```

Expected demo success line:

```text
PASS TileXR SDMA copied <bytes> bytes correctly
```

The unit tests are hardware-free; the demo requires a usable driver HAL/device runtime and resolves `libascend_hal.so` from `/usr/local/Ascend/driver/lib64/driver`. See [docs/SDMA_TRANSPORT.md](docs/SDMA_TRANSPORT.md) for enablement, the host/device API, CANN 9.0.0 / 9.1.0 acceptance steps, and current validation status.

## Collectives Validation

Configure the collectives build as shown in [Quick Start §3](#3-build-core-runtime), then run the hardware-free source and CLI smoke checks registered with CTest:

```bash
ctest --test-dir build-collectives --output-on-failure
```

These checks verify headers, the library split, scripts, docs, and tool wiring without an NPU. Physical multi-NPU runs are manual.

Manual multi-NPU correctness checks live under `tests/collectives/`, while performance and profiling tools live under `tools/collectives/`:

```bash
cd tests/collectives
TILEXR_INSTALL="$PWD/../../install"
TILEXR_LIBDIR="$(find "$TILEXR_INSTALL" -maxdepth 1 -type d -name 'lib*' | head -n 1)"

LD_LIBRARY_PATH="$TILEXR_LIBDIR:${LD_LIBRARY_PATH:-}" \
  ./run_collectives_correctness.sh 2 16 0 ../../install/bin allgather

LD_LIBRARY_PATH="$TILEXR_LIBDIR:${LD_LIBRARY_PATH:-}" \
  ../../tools/collectives/run_collective_perf.sh 2 0 ../../install/bin \
    --op allgather --min-bytes 4 --max-bytes 4096 \
    --step-factor 2 --iters 20 --warmup-iters 5 \
    --datatype int32 --check 1
```

The perf tool prints latency, algorithm bandwidth, bus bandwidth, and error counts, with optional CSV output.

Operator-internal profiling can be enabled for the collectives perf tool with `TILEXR_COLLECTIVES_ENABLE_PROFILING=ON`:

```bash
source scripts/common_env.sh
cmake -S . -B build-profile \
  -DTILEXR_BUILD_COLLECTIVES=ON \
  -DTILEXR_COLLECTIVES_ENABLE_PROFILING=ON
cmake --build build-profile --target tilexr_collective_perf -j"$(nproc)"

cd tests/collectives
../../tools/collectives/run_collective_perf.sh 2 0 ../../build-profile/tests/collectives \
  --op allgather --min-bytes 67108864 --max-bytes 67108864 \
  --profile 1 --profile-dir run/prof/collectives --profile-ai-prompt 1
```

Each sampled measured launch writes per-rank/per-launch artifacts under
`run/prof/collectives/rank<N>/launch<M>/`. After all ranks finish successfully,
`run_collective_perf.sh` also writes aggregate root-level profiling artifacts:

```text
run/prof/collectives/report.html
run/prof/collectives/trace_index.json
run/prof/collectives/analysis.md
run/prof/collectives/ai_prompt.md   # only when prompt export is enabled
```

The aggregate `report.html` keeps the bottleneck-first summary and adds a zoomable chronological timeline across ranks, cores, and sampled measured launches, with links back to each per-launch report. Warmup iterations remain controlled by `--warmup-iters` and are reported as metadata; warmup launches are not profiled by this path. Existing profile directories can be re-analyzed with `tools/collectives/tilexr_collective_profile_report.py`.

See [tests/collectives/README.md](tests/collectives/README.md) for script arguments, skip behavior, timeout handling, topology limitations, and profiling report details.

## EP Dispatch Validation

The standalone EP checks live under `tests/ep/` and can be built without external HCCL or operator stacks:

```bash
source scripts/common_env.sh
bash tests/ep/build.sh source-only
ctest --test-dir tests/ep/build --output-on-failure
```

On a configured multi-NPU Ascend host with `bisheng`, build the full EP library, demo, and kernel:

```bash
source scripts/common_env.sh
bash tests/ep/build.sh full
```

The deterministic 2-rank demo validates peer-memory EP dispatch and combine outputs:

```bash
bash tests/ep/demo/run_tilexr_ep_dispatch_demo.sh 2 2 0
```

For remote PR validation, set the target host and scratch directory explicitly. The deployment helper creates a clean remote checkout under `${TILEXR_EP_REMOTE_BASE}/TileXR`, builds the EP artifacts, and runs the demo:

```bash
TILEXR_EP_REMOTE=<ssh-target> \
TILEXR_EP_REMOTE_BASE=<remote-scratch-dir> \
bash tests/ep/demo/deploy_and_run_remote.sh
```

Expected demo logs include `rank 0 validation PASS` and `rank 1 validation PASS`. See [tests/ep/README.md](tests/ep/README.md) for details and current route-2 UDMA TODOs.

## Operator Simulator

`op-simulator/` contains the basic Ascend C simulator demo and MoE EP-card trace comparison tools. See [op-simulator/README.md](op-simulator/README.md) for commands and trace summary usage.

## Common Commands

```bash
source scripts/common_env.sh
bash scripts/device_connect.sh
bash scripts/plog_grep.sh "search_term"
bash scripts/driver_fix.sh
```

## Documentation

- [scripts/README.md](scripts/README.md): script reference and workflows
- [docs/BUILD_VERIFICATION.md](docs/BUILD_VERIFICATION.md): current build and verification checklist
- [docs/UDMA_INTEGRATION_SUMMARY.md](docs/UDMA_INTEGRATION_SUMMARY.md): current UDMA architecture summary
- [docs/SDMA_TRANSPORT.md](docs/SDMA_TRANSPORT.md): on-card SDMA transport guide, enablement, and validation
- [reference/README.md](reference/README.md): ignored reference-only source checkouts
- [docs/CANN_VERSION_MIGRATION.md](docs/CANN_VERSION_MIGRATION.md): CANN 9.1.0 migration notes
- [tests/collectives/README.md](tests/collectives/README.md): optional collectives correctness and performance tools
- [tests/ep/README.md](tests/ep/README.md): standalone EP dispatch/combine build, demo, and cross-node transport notes
- [AGENTS.md](AGENTS.md): repository guidance for AI coding agents
- [docs/diagrams/](docs/diagrams/): editable draw.io sources for the README architecture diagrams (SVGs embed the diagram XML, so they reopen in draw.io)

## Troubleshooting

Driver or device issues:

```bash
bash scripts/driver_fix.sh
npu-smi info
```

Build failures:

- Run `git submodule update --init --recursive`.
- Run `source scripts/common_env.sh` before CMake or scripts.
- Check `ASCEND_HOME_PATH`, `TILEXR_CANN_VER`, and CANN 9.1.0 include/library layout.
- Confirm `install/lib/libtile-comm.so` links only to the expected CANN runtime/driver libraries and does not require HCCL, shmem, or reference-only source trees.
- Do not put `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib` into runtime RPATH/RUNPATH. That path is a link-time fallback and may contain stub libraries such as `libascend_hal.so`; runtime should resolve the real driver HAL from `/usr/local/Ascend/driver/lib64/driver`.

Log analysis:

```bash
bash scripts/plog_grep.sh ERROR
bash scripts/plog_grep.sh WARNING
```

## License

Copyright (c) 2026 Huawei Technologies Co., Ltd.

This program is free software. You may redistribute it and/or modify it under the terms and conditions of CANN Open Software License Agreement Version 2.0.

See the repository license notice for details.
