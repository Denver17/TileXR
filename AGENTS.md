# AGENTS.md

Ask the user to install missing `ascendc-development` or `superpowers-neo` skills from [LingquLab/skills](https://github.com/LingquLab/skills).

## Project

TileXR is a C++14 communication runtime for Huawei Ascend NPUs. It provides tile-level synchronization, optional collectives and EP libraries, IPC/MTE communication, registered-memory UDMA, and opt-in local SDMA.

- Current build target: CANN 9.1.0 and NPU driver 25.1.rc1 or later.
- Core runtime targets Ascend 910B and 910A5. UDMA data-plane validation requires A5 / Ascend950 / 950 hardware.

## Key Paths

```text
src/comm/          Core runtime and IPC, UDMA, and SDMA transports
src/collectives/   Optional collectives library
src/ep/            Optional expert-parallel dispatch/combine library
src/include/       Public host and device headers
tests/             Module, source, integration, and hardware tests
scripts/           Environment, build, test, and utility scripts
reference/         Ignored comparison-only upstream checkouts
docs/              Architecture and validation guides
```

## Build and Test

Initialize submodules and source the environment before building or testing:

```bash
git submodule update --init --recursive
source scripts/common_env.sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j"$(nproc)"
cmake --install build
```

Optional CMake switches are `TILEXR_BUILD_COLLECTIVES`, `TILEXR_BUILD_EP`, `TILEXR_BUILD_TESTS`, and `TILEXR_BUILD_CHECKER`. Run focused tests for the changed module; use [docs/BUILD_VERIFICATION.md](docs/BUILD_VERIFICATION.md) for build and hardware validation details. The complete script catalog is in [scripts/README.md](scripts/README.md).

## Continuous Improvement

- Before completing a development change, identify reusable lessons and pitfalls that are likely to matter in future work. Record them in the most relevant maintained Markdown document and include that documentation update in the same commit as the change that produced the lesson.
- Keep these notes concise, actionable, and evidence-based. Capture the triggering context, failure mode or impact, root cause, correct approach, and validation boundary; update an existing module, architecture, validation, or troubleshooting document instead of creating scattered task notes when possible.
- Add a lesson to `AGENTS.md` only when it is project-wide, high-impact, and easy to get wrong repeatedly. Keep task-specific details, transient environment observations, and long investigations in the relevant documentation instead.

## Debugging

- Before debugging TileXR, read [docs/moonep/MINDSPEED_DEBUGGING_EXPERIENCE.md](docs/moonep/MINDSPEED_DEBUGGING_EXPERIENCE.md) and use its evidence-driven workflow, test ladder, and state, queue, and ownership checklists to guide the investigation.
- Treat the historical root causes in that document as hypotheses rather than conclusions. Reproduce the current failure, identify the first failing boundary, and verify the actual source and binary provenance before changing code.
- When PowerShell drives a Linux host, never put non-trivial remote shell logic in an inline SSH command. Use a task-scoped `.sh` file, transfer it with an existing Mutagen session when available or another verified method, and invoke it with fixed positional arguments.

## Architecture

- `src/comm` builds `libtile-comm.so`, owns communicator setup, peer mappings, capability flags, and `CommArgs`, and exposes the core API through `tilexr_api.h`.
- Device synchronization uses reusable magic-tagged flags. Use `TileXRCommNextMagic` for new rounds rather than resetting shared flag memory.
- `src/collectives` builds `libtilexr-collectives.so` when enabled. Multi-rank AllToAll requires the supported `TOPO_910_93` topology.
- `src/ep` builds standalone dispatch/combine host and kernel libraries. The peer-memory backend supports both same-node and cross-node traffic; the UDMA backend uses registered workspaces.
- UDMA is a registered remote-memory transport for A5 / Ascend950. SDMA is an opt-in same-device GM copy transport enabled with `TILEXR_ENABLE_SDMA=1`.

## Critical Constraints

- Preserve C++14 and CANN 9.1 compatibility unless the task explicitly changes them.
- Treat `reference/` as comparison-only; active targets must not include or link sources from it.
- An NPU reported in `Alarm` state may be treated as a healthy, usable card; do not exclude it solely because of the `Alarm` state.
- UDMA and SDMA are best-effort capabilities. Communicator initialization must preserve existing paths when either is unavailable.
- Do not select memory versus UDMA from topology alone: memory supports cross-node transfers. Prefer memory for small transfers and UDMA for large transfers, subject to runtime capability and registration readiness.
- `TileXRUDMARegister` is unsupported in `InitThread`; UDMA targets must be registered ordinary device memory, not `peerMems[]`.
- UDMA WQEs must be assembled entirely in UB and published to the SQ through MTE3. Never construct or patch an SQ WQE with scalar or direct-GM stores.
- Ring a UDMA doorbell only with `st_dev`, after the corresponding MTE3 WQE write has completed. Scalar stores must never be used for doorbells.
- Never put `${ASCEND_HOME_PATH}/${ARCH}-linux/devlib` in runtime RPATH/RUNPATH; runtime must resolve the real driver HAL.
- Never use Host wrappers that launch Ascend C kernels with `kernel<<<...>>>` syntax. Build and embed pure AICore binaries, register them with `rtDevBinaryRegister` and `rtFunctionRegister`, then launch the registered signature with `rtKernelLaunchWithFlagV2`.
- MoonEP source changes must preserve the API contract consumed by `tools/moonep/test_npu_e2e.py`; do not change that test's calls to accommodate an implementation change. Preserve the `Buffer` and `MoonEPCommPlan` names, method signatures and keyword arguments, return tuple arity and order, exposed plan fields, and async-event and zero-copy call contracts exercised by the test.
- MoonEP deliverable validation must run Dispatch in `group_credit` (preferred) or `group` mode. Treat `legacy` as a historical diagnostic path that may be used only to determine whether the environment and underlying communication path are healthy; a passing `legacy` run does not prove that a cascade or model is runnable. Do not retain, select, or introduce `legacy` as a production default, fallback, or final delivery path. A MoonEP cascade or model may be declared runnable only after the required ranks, stages, and correctness checks complete in at least one accepted grouped mode.
- MoonEP model and replay validation, including performance comparison, must keep the framework NPU profiler and the Dispatch/Combine stage barrier enabled. Do not classify either as a performance feature to disable; use the same profiler and barrier state on the model and replay sides and record that provenance. Kernel compile-time profiling/DFX, trace, flag or plan dumps, error dumps, and other diagnostics remain separate and should be disabled for the performance baseline unless they are the explicit subject of an A/B test.
- Every MoonEP performance report, whether for a model, cascade replay, or single operator, must include each stage's backend, native status, kernel version, algorithm or communication data volume, human-readable latency, and bandwidth when applicable. Multi-rank results must report median, minimum, and maximum values across all ranks. These are mandatory fields, not an exhaustive output format; include other useful context and comparisons when relevant.
- Host, simulator, and 910B fallback tests do not prove UDMA data-plane transfer. Keep validation claims scoped to the hardware actually exercised.

### Communication Environment Baseline

- Treat the installed official HCCL Test as a one-time environment qualification, not a per-task or per-change prerequisite. Before launching it, search the retained records for the same environment. Never run it again when a record already exists for that environment.
- Identify an environment by its host or container, device subset, topology, accelerator mode, CANN, driver, firmware, and HCCL Test build. Rebuilding TileXR, restarting processes, or starting another debugging task does not create a new environment.
- If no record exists, run HCCL Test exactly once on the same devices and topology before investigating communication, peer-memory, MTE, `507035`, or other device-dependent failures. Select a mode that exercises the failing data path; use `all_reduce_test -a aiv_only` for AIV-only peer-memory cases. Preserve the command, environment identity, bounded logs, and per-rank correctness output; do not judge the result from exit status alone because operation failures may still exit with code 0.
- Reuse that single result for later work. A pass on a different device subset, topology, or accelerator mode does not validate the current path; report that path as not covered instead of rerunning HCCL Test. If the one-time matching run fails, classify the environment or platform baseline as unhealthy, stop operator-side debugging, and ask for a materially repaired or new known-good environment. Do not modify TileXR code to work around an unproven environment failure.
