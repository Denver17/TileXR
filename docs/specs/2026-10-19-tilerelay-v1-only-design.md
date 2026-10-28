# TileRelay: ABI-v1 only, frozen historical baselines

## Status and decision

The user approved the concise design on 2026-10-19: keep only ABI-v1 on the
active branch, archive ABI-v0 and the Host pipeline on a historical baseline
branch, and remove their implementations from PR #182. The user confirmed this
written spec on 2026-10-19. It supersedes the compatibility requirements
in the original TileRelay/Core designs; it does not change the ABI-v1 protocol.

## Historical baseline

- Freeze branch `baseline/tilerelay-v0-pipeline-20261019` at commit
  `7ccaa16768ae2b2cb0da71d47e864352721de8b0`, before any removal. Create and verify
  the remote reference before deleting implementations on the active branch.
- Do not move or develop this branch. Reproduction instructions pin the full
  commit, not just the branch name. Do not overwrite an existing conflicting ref.
- The snapshot retains both launchers, Host pipeline, builders, tests and
  documentation. Keep existing performance evidence labelled with its original
  revision and provenance; archival does not retroactively revalidate it.
- Build historical artifacts in a separate checkout/install prefix. Never mix
  historical DSOs or kernels into the current production installation, or delete
  existing user installations as part of this migration.

## Active architecture and removals

- `src/relay`, `tilexr_relay.h` and `libtilexr-relay.so` are the only production
  Relay Host path, selected by the existing default-OFF `TILEXR_BUILD_RELAY`.
  Keep ABI-v1's 152-byte canonical frame, prepared execution, registration cache
  identity/lifetime and error semantics unchanged. No new shared registration
  component is needed once the second implementation is removed.
- Remove `TileXRAllGatherGemmRelay`, `tilexr_tilelang.h`, the specialized Host
  source/target, `TILEXR_BUILD_TILELANG`, its kernel-install setting and legacy
  consumer/failure tests. Keep common ELF loading used by other production code.
- Remove `scripts/build_tilelang_relay.py`, `src/tilelang/relay_kernel.py`, the
  96-byte generator branch and v0-only diagnostic instrumentation. Preserve the
  ABI-v1 AllGather-GEMM schedule, GEMM-AllReduce, communication primitives and
  Python package location. Do not add an adapter for old binaries or signatures.
- Remove the Host pipeline C++ implementation/header, standalone build target,
  its dedicated runner/tests and their CI wiring. Preserve shared test mocks by
  placing those still needed under the retained tests, not a deleted subsystem.
- Remove the legacy mode from `tests/relay/hardware.py`; retain ABI-v1 ABI echo,
  AllGather-GEMM, GEMM-AllReduce and mixed/prepared execution coverage.

## Benchmark boundary

Retain one small comparison runner for ABI-v1 versus an explicitly supplied
historical pipeline DSO. Only the foreign-function adapter and standalone GEMM
generation needed to call that baseline remain; do not copy its per-tile Host
orchestration back into the active tree or import a removed runner implicitly.

Keep serial, serial-tiled and overlapping baseline modes, plus generic Relay
and optional prepared execution. Remove the specialized Relay comparison and
v0-only diagnostics. Select the current operator using its manifest; validate
operator/shape/measurement consistency before allocation or launch. Preserve
warmup, repeated measurements, numerical checks, launch-count checks, mode-order
controls and artifact/compiler/build provenance. Hashing for benchmark records
does not reintroduce production ELF rehashing.

The baseline DSO is an explicit benchmark dependency, never a production build,
link, install or fallback dependency. A historical comparison requires the
documented compatible core/collectives/compiler environment and records the
actual loaded artifacts. Pure ABI-v1 use requires no historical checkout.

## Migration and documentation

Update current README/build/integration/example/script guidance to use only
`build_tilexr_relay.py`, `TILEXR_BUILD_RELAY` and content-addressed ABI-v1
artifacts. Document this intentional API removal and how to rebuild the frozen
baseline. Existing v0 artifacts are not loadable through v1; regenerate them.
Keep historical specs, plans and measurements with clear supersession notes
rather than rewriting their results as if produced by the new runtime.

## Acceptance and verification

1. Verify the frozen remote branch matches the exact archival commit; keep the
   active PR on its existing branch without force pushes or history rewriting.
2. Run Host CI, Python contracts and installed C/C++ consumers. Build/install
   ABI-v1 with collectives OFF into a fresh prefix; inspect exports/dependencies
   and installed files to confirm no v0/pipeline production surface remains.
3. Build the simplified ABI-v1 AllGather-GEMM generator in the pinned toolchain;
   verify both retained operator builders and comparison CLI imports.
4. On idle blue devices 0/1, run retained ABI-v1 correctness/launch/lifetime
   regression including prepared execution and both operators. Run the external
   baseline comparison with at least 5 warmups and 20 measured rounds per mode,
   with numerical checks and framework profiling enabled.
5. Review and simplify the final diff. Record exact verification results and
   any unavailable boundary; do not infer NPU correctness from Host tests or
   claim performance improvement from this code-removal change.

Non-goals: kernel scheduling/performance changes, ABI-v2, new transports/SoCs,
runtime compiler integration, new diagnostics, changes to the generic native
registration policy, and automatic cleanup of previously installed artifacts.
