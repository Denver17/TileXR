# TileRelay Core execution plan

Historical execution record. The [ABI-v1-only design](../specs/2026-10-19-tilerelay-v1-only-design.md)
supersedes pipeline/v0 implementation and compatibility requirements below.
Those sources are frozen at `7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019`; use the
[current integration guide](../TILELANG_INTEGRATION.md) for active commands.

Approved contract: [Core spec](../specs/2026-10-14-tilerelay-core-design.md).
Repository constraints: `AGENTS.md`; validation context:
`docs/TILELANG_ALLGATHER_GEMM_VALIDATION.md`, `docs/TILELANG_INTEGRATION.md`.
This durable plan accompanies implementation, not a plan-only commit.

## Boundaries

Two same-node 910B ranks, CANN 9.1, C++14 Host, pinned compiler, external ELFs,
one synchronous mixed launch. Keep legacy API/96-byte ABI usable. No lookahead,
transport expansion, asynchronous execution or new third-party dependency.
Compiler ABI retention and FP32 mixed reduction are gates, not assumptions.

## Ordered workstreams

1. **Device primitives and ABI gate (main agent).** Extract DMA-copy, epoch
   publication/polling/ACK and paired handoff macros into the Python package.
   Keep explicit scratch ownership and current specialized schedule/ABI.
   Compile legacy kernels and compare correctness on repeated generations.
   Add a sixteen-slot/scalar echo kernel and registered-launch hardware probe;
   prove retention of unused arguments before v1 migration. Own Python device
   package, legacy generator and device tests. No compiler modifications or
   source rewriting. Outputs: reusable macros and measured ABI evidence.
2. **Native runtime (independent Host workstream).** Own `src/relay`,
   `src/include/tilexr_relay.h`, focused native tests and optional CMake wiring.
   Define typed versioned deep-copied descriptors, finite binding sources,
   checked concrete capacities, generic registration/cache/launch and poisoned
   state. Depends on approved ABI contract, not operator formula/enum. Prove
   layout, validation, first error, lifetime and one launch with C++14 mocks;
   install/link a consumer. No Python/JSON/collectives dependencies. Coordinate
   the descriptor contract with the Python workstream before either binds it.
3. **Python planning/build/runtime.** Depends on native descriptor agreement
   and ABI gate. Own immutable contracts, checked regions/channels, content
   manifest/artifact builder and lazy ctypes facade. Unit-test overflow,
   collision, deterministic identity, versions and compiler-free loading.
   Produce device-only content-addressed ELF using existing pinned toolchain.
4. **AG-GEMM migration.** Depends on 1–3. Generate v1 from unchanged schedule,
   preserve explicit legacy path and install both kinds separately. Prove
   golden results, delay, shape/plan alternation and one launch on two cards.
5. **Python-only GEMM-AllReduce witness.** Depends on qualified v1 AG-GEMM.
   Implement FP32 partial GEMM/exchange/SUM then FP16 output, no extra kernel.
   Compare CPU reference with repeated changed inputs, delayed rank and mixed
   operator order. Freeze/record native runtime hash before adding operator;
   require identical runtime binary afterward.
6. **Qualification and delivery.** Simplify without changing behavior, obtain
   independent review, resolve findings, verify final diff and installed build.
   Compare original serial/overlap, legacy Relay and generic variants with
   >=5 warmups/20 measured rounds, identical profiler and timing boundaries,
   per-rank median/min/max and raw samples. Record lessons and actual hardware
   coverage in maintained integration/validation docs. Reuse retained HCCL
   baseline; no per-change rerun. Deliver scoped changes on the existing branch.

## Gates and progress

- [x] Extracted primitives preserve legacy behavior (two ranks x 12 rounds).
- [x] Canonical ABI compile/echo gate passed on target (two ranks x 6 rounds).
- [x] Native and Python contracts/unit tests passed (3 CTests; 22 Python tests on blue).
- [x] Generic AG-GEMM two-card and installed-consumer checks passed.
- [x] Python-only GEMM-AllReduce and mixed-plan reuse passed (FP32 discriminator included).
- [x] Warmed repeated comparison, simplification and independent review.
- [x] Final installed prepared-path regression (24 mixed rounds per rank).

Delivery target: scoped commit and normal push on the existing `hapi-tilescale`
branch and PR #182; no merge or history rewrite.

Generic AG-GEMM's first two-card case (256/512/128, 2 blocks, 12 rounds/rank)
passed with frozen runtime SHA256
`aed647dbc80c4ae37b9cc6c9838dbce229cb624d6e7d82ff67abc04368791e66`.
Keep that binary unchanged when building/qualifying the Python-only second op;
the broader three-shape/two-operator and actual CMake-installed consumer gates
subsequently passed. Explicit prepared bindings retain that same native binary;
native checks still execute on every launch.

If ABI retention, FP32 support or lifecycle assumptions fail, record the first
failing boundary and pause for the smallest design revision; do not silently
change the approved ABI or weaken correctness/one-launch acceptance.
