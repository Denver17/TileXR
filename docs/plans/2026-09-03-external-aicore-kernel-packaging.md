# External AICore Kernel Packaging Plan

## Goal and Scope

Implement the approved design in
`docs/specs/2026-09-03-external-aicore-kernel-packaging-design.md`: replace all
production `.o` embedding in Collectives, EP Memory, and MoonEP with
runtime-relative external kernel artifacts. Preserve public APIs, kernel
signatures, algorithm selection, and C++14/CANN 9.1 compatibility. Test-only
and diagnostic embedding remains out of scope.

## Tasks

### 1. Add the private kernel loader

- **Objective:** Provide one C++14 implementation for locating, validating,
  reading, and process-lifetime caching external kernel objects.
- **Scope:** `src/common/` plus focused Host tests and test build wiring.
- **Dependencies:** The installed path contract is
  `<host-library-dir>/tilexr/kernels/<basename>`.
- **Constraints:** Use an anchor from the owning Host library, reject unsafe
  basenames and invalid files, avoid absolute build paths and environment
  overrides, and keep returned bytes alive for runtime registration.
- **Acceptance:** Host tests exercise success, cache stability, invalid name,
  missing file, and empty file without NPU hardware.
- **Artifact:** A private loader header/source usable by each owning shared
  library.

### 2. Migrate Collectives and EP

- **Objective:** Register the generic/A5 Collectives kernels and all four
  A5-only EP kernels from external files.
- **Scope:** `src/collectives`, `src/ep`, and their focused tests.
- **Dependencies:** Task 1 loader contract.
- **Constraints:** Preserve public API and exact kernel argument order. Remove
  EP's `TILEXR_SOC_NAME`/`TILEXR_EP_SOC_TYPE` build selection, compile only A5
  EP objects with `dav-c310-vec`/`CATLASS_ARCH=3510`, reject every EP launch on
  non-A5 before state mutation, and do not retain Host `kernel<<<...>>>`
  wrappers or EP device-kernel shared libraries.
- **Acceptance:** No production embed source or EP device-kernel `.so` is
  compiled; an option-free EP build stages and installs all four A5 objects and
  no 910B variant; registration/launch mocks cover ordinary and Memory
  signatures; non-A5 mocks observe no binary load or magic advance.
- **Artifacts:** Three Collectives objects plus Dispatch, Combine, Dispatch
  Memory, and Combine Memory EP objects.

### 3. Migrate MoonEP

- **Objective:** Convert every production MoonEP kernel registration to a
  filename-based external load.
- **Scope:** `src/moonep/cmake`, Dispatch, Planner V2/V3, Combine V2,
  PrefetchWeight, ReduceGrad, and focused MoonEP tests.
- **Dependencies:** Task 1 loader contract.
- **Constraints:** Preserve the API contract exercised by
  `tools/moonep/test_npu_e2e.py`, all signatures and return shapes, and allowed
  grouped modes.
- **Acceptance:** The common build helper only compiles/links AICore objects;
  all seven MoonEP objects are staged/installed and existing launch tests pass
  with loader mocks.
- **Artifacts:** Seven external MoonEP kernel objects and simplified common
  build/registration helpers.

### 4. Remove obsolete embedding and update maintained guidance

- **Objective:** Remove unused production embed scripts and align architecture
  and build-verification documentation with the deployed layout.
- **Scope:** Production embed CMake scripts, `README.md`,
  `docs/specs/collectives.md`, `docs/specs/moonep-runtime.md`, relevant module
  docs, and `docs/BUILD_VERIFICATION.md`.
- **Dependencies:** Tasks 2 and 3.
- **Constraints:** Do not add source-text guard tests; validate the contract via
  build, install, symbols, and behavior.
- **Acceptance:** Maintained docs name the external artifacts, failure boundary,
  and hardware-validation limit; no production CMake path references an embed
  script or generated embed source.

### 5. Verify the integrated change

- **Objective:** Establish credible Host/build evidence for all affected
  boundaries.
- **Scope:** Focused Host tests, CMake configure/build where the local CANN
  environment permits, install inspection, ELF symbol inspection, and static
  checks.
- **Dependencies:** Tasks 1–4.
- **Constraints:** Do not claim A5/UDMA data-plane validation without supported
  hardware. Preserve unrelated working-tree changes.
- **Acceptance:** `git diff --check` passes; focused tests pass; staged and
  installed kernel manifests contain all 14 objects; production Host libraries
  no longer contain the former embedded binary-data symbols. Any unavailable
  CANN or hardware boundary is reported explicitly.

## Risks and Compatibility Boundaries

- Moving a Host library without its `tilexr/kernels` directory makes its
  kernels unavailable by design.
- The loader cache must retain byte-address stability after subsequent loads.
- Planner V3 is linked into the Planner V2 shared library and must resolve its
  kernel relative to that final owning library.
- Registration failure and retry behavior must remain unchanged apart from the
  new file-loading failure point.
- Host-only validation cannot establish target compiler, AICore execution,
  transport correctness, or performance.
