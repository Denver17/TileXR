# Common Straggler Diagnostics Migration Plan

## Goal And Scope

Implement the approved design in
`docs/specs/2026-08-25-common-straggler-diagnostics-design.md`: move clock
synchronization and fast/slow-rank detection out of Collectives into an
independent `libtilexr-diagnostics.so` component consumed by communication
producers. Remove the unpublished `TileXRCollectiveStraggler*` ABI without a
compatibility layer.

The work preserves C++14, CANN 9.1.0, communicator behavior, registered AICore
binary launch, the current classification behavior, and the existing 64-rank
precision definition. It does not instrument every EP or MoonEP production
kernel in this migration.

## Authoritative Context

- Repository rules: `AGENTS.md`
- Approved architecture: `docs/specs/2026-08-25-common-straggler-diagnostics-design.md`
- Existing implementation: `src/collectives/host/straggler_*`,
  `src/collectives/kernels/straggler_kernel.h`, and
  `src/include/tilexr_collective_straggler.h`
- Build and hardware procedure: `docs/BUILD_VERIFICATION.md`

## Task 1: Establish The Public Diagnostics Contract

**Objective and role:** Define producer-neutral Host, device, and layout APIs
that downstream modules can compile against before implementation ownership is
moved.

**Background and prerequisites:** The approved spec removes all
Collective-specific public names. Existing clock/result layout is the behavioral
baseline.

**Modification scope:** `src/include/tilexr_straggler.h`,
`src/include/tilexr_straggler_device.h`, and
`src/include/tilexr_straggler_layout.h`; corresponding header tests.

**Constraints and non-goals:** Preserve stable field widths and C ABI layout
where names remain generic. Do not expose Collectives enums or private session
types. Keep device helpers CANN 9.1-compatible.

**Acceptance and verification:** C++14 public-header compile and static ABI
tests pass. Repository search finds no new public dependency on Collectives.

**Artifacts and interfaces:** Generic detector, operation, observation, clock,
rank observation, and report contracts used by Tasks 2 and 3.

## Task 2: Build The Independent Diagnostics Component

**Objective and role:** Move detector lifecycle, analysis, JSON reporting,
clock-kernel compilation/embedding/registration, and launch into
`src/diagnostics` and `libtilexr-diagnostics.so`.

**Background and prerequisites:** Task 1 public types are authoritative. The
current Collectives implementation supplies behavior to preserve.

**Modification scope:** New `src/diagnostics/**`, top-level CMake option and
subdirectory wiring, install rules, and diagnostics-focused tests under
`tests/diagnostics`.

**Constraints and non-goals:** Depend only on `tile-comm` and CANN/driver
runtime libraries. Use pure embedded AICore binary registration and
`rtKernelLaunchWithFlagV2`. Reject unknown SoC selection and explicitly map
Ascend950 to `dav-c310-vec`. Do not include producer source directories.

**Acceptance and verification:** Independent diagnostics configure/build,
focused unit tests, kernel ownership/source guards, and install-prefix consumer
test pass.

**Artifacts and interfaces:** `libtilexr-diagnostics.so`, installed generic
headers, and explicit observation begin/end API.

## Task 3: Adapt Collectives As A Producer

**Objective and role:** Keep aggregate communication instrumentation behavior
while removing all diagnostics ownership from the Collectives library.

**Background and prerequisites:** Tasks 1 and 2 provide the public integration
contract and implementation library.

**Modification scope:** Collective launch context/arguments, Host launch path,
device kernel instrumentation calls, Collectives CMake/install rules, and the
Collectives integration test.

**Constraints and non-goals:** Do not change collective algorithms, topology
selection, data correctness, or normal behavior when no observation is active.
Do not restore TLS ownership inside Collectives. Remove the standalone clock
entry from the Collectives AICore binary.

**Acceptance and verification:** Collectives builds against diagnostics,
correctness/unit tests pass, the aggregate fast/slow-rank integration test still
classifies the injected cases, and source guards prove no diagnostics
implementation remains under `src/collectives`.

**Artifacts and interfaces:** A narrow Collectives producer adapter passing the
generic observation payload through its existing kernel argument ABI.

## Task 4: Prove EP And MoonEP Integration Boundaries

**Objective and role:** Demonstrate that both producer families can consume the
common API without any dependency on `libtilexr-collectives.so`.

**Background and prerequisites:** The diagnostics component and installed
headers from Tasks 1 and 2 must exist.

**Modification scope:** EP and MoonEP source/compile or install-consumer tests;
minimal target linkage only where needed for the boundary proof.

**Constraints and non-goals:** Do not change the MoonEP Python API contract and
do not claim full production event coverage. Do not introduce a Collectives
link dependency.

**Acceptance and verification:** Both boundary tests compile/link using
diagnostics plus their existing dependencies, and source guards reject a
Collectives dependency.

**Artifacts and interfaces:** Reusable integration examples for later
per-kernel EP/MoonEP instrumentation.

## Task 5: Remove The Old ABI And Complete Regression Validation

**Objective and role:** Finish the breaking migration cleanly and prove the
result at source, build, integration, and hardware boundaries.

**Background and prerequisites:** Tasks 1-4 must no longer reference the old
ABI.

**Modification scope:** Delete `tilexr_collective_straggler.h`, old symbols,
obsolete tests and CMake wiring; update maintained build/validation docs with
the reusable SoC-selection lesson and new commands.

**Constraints and non-goals:** Preserve unrelated dirty-worktree changes. Reuse
the retained matching HCCL qualification instead of rerunning it.

**Acceptance and verification:** Repository search finds no
`TileXRCollectiveStraggler` symbol. Focused unit/integration tests and complete
enabled-target build pass. On the existing eight-node Ascend950 environment,
the independent 64-rank clock test reports 64/64 PASS and bounded precision with
explicit binary/SoC provenance; the aggregate communication detector test also
passes. Claims remain scoped to exercised hardware.

**Artifacts and interfaces:** Final source tree, validation logs/results, and
updated maintained documentation delivered together.

## Ordering And Risks

Tasks 1-3 are serial because they share ABI and launch arguments. Task 4 follows
the stable public target contract. Task 5 runs only after all consumers migrate.

Primary risks are Host/device ABI drift, duplicate kernel registration,
silently wrong SoC binaries, stream-owned record lifetime, and accidental
Collectives linkage from EP/MoonEP. Static layout assertions, source guards,
focused lifecycle tests, explicit architecture configuration, and hardware
provenance checks address these risks.

## Task 6: Remove Multi-Rank Clock-Service Queueing

**Objective and role:** Keep the NTP-style four-timestamp estimator while
removing rank 0 service queueing from the measured round trip, then improve
sample stability with discarded warmups and a larger measured population.

**Modification scope:** Public diagnostics configuration/result fields, the
registered direct-launch Host/Kernel ABI, the clock protocol state machine,
session tests, source guards, multi-host result aggregation, and maintained
validation guidance.

**Constraints and non-goals:** Do not adjust NPU hardware clocks, change the
offset/error-bound formulas, hide outliers by changing report aggregation, or
weaken timeout convergence. Warmups use the real device path and never enter
the estimator. Preserve C++14 Host code, CANN 9.1, `dav-c310-vec`, embedded
binary registration, and one public diagnostics implementation.

**Acceptance and verification:** Default configuration is 32 discarded warmups
plus 128 measured samples; layout/launch assertions and focused tests pass; the
target compiler regenerates the diagnostics AICore binary; 4P aggregate
detection remains correct; and the specified eight-node Ascend950 environment
reports 64/64 PASS with p50, p95, and maximum precision all below 1000 ns.

### Task 6 Validation Status

The final protocol artifact was validated on 2026-08-25 with 32 discarded
warmups and 128 measured exchanges per calibration:

- two hosts, one rank per host, five rounds: p50 866 ns, p95 889 ns, maximum
  889 ns;
- one host, eight ranks, five rounds: p50 631 ns, p95 632 ns, maximum 632 ns;
- one host, four-rank aggregate detector, twenty clock rounds: p50 546 ns, p95
  552 ns, maximum 552 ns; AllReduce correctness, all-normal classification,
  and injected rank 1/rank 2 slow classification passed;
- focused CTest selection: 8/8 passed; the CANN 9.1 `dav-c310-vec` diagnostics
  kernel build passed.

The requested eight-host, 64-rank result is not claimed. The one-time matching
HCCL AIV-only qualification on the supplied environment was terminated when a
rank process was killed and produced no AllReduce correctness or latency
result. Per the communication environment baseline policy, TileXR 64-rank
operator testing must stop on that environment until it is materially repaired
or replaced by a known-good matching environment; the failed qualification
must not be rerun unchanged.
