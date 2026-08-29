# Offline Checker Specification

## Purpose

The optional checker under `tools/checker/` executes production or
production-aligned communication algorithms against a deterministic Host-side
world. It detects ordering, visibility, address, synchronization, and oracle
violations without claiming to reproduce NPU instruction timing or network
performance.

`docs/CHECKER_STATUS.md` is the current capability matrix and
`tools/checker/README.md` is the operational guide.

## Architecture

The checker is isolated from production targets and enabled through
`TILEXR_BUILD_CHECKER`. It consists of:

- a simulated multi-rank memory world;
- an event scheduler and executor;
- Ascend C compatibility shims used only by checker builds;
- trace adapters that materialize selected production algorithm behavior;
- functional oracles for collective and EP invariants;
- diagnostics and machine-readable/human-readable reports.

Production sources remain authoritative. A checked-in trace or adapter records
its source identity and coverage. Header probes and source-preservation tests
prevent checker-only changes from altering production files to make simulation
easier.

## Execution Model

Ranks and cores issue abstract events into the checker world. The executor
applies program-order and modeled synchronization dependencies, updates the
simulated memory, and records the first violated boundary. Functional oracles
compare the resulting buffers and protocol state with operation-specific
expectations.

The checker may explore deterministic schedules and simplified topology or
buffer models. Such models are analysis aids, not measured hardware models.
Performance estimates, when present, must identify their assumptions and
calibration source.

## Trace Contract

A trace bundle includes a manifest, source identity, supported shapes or modes,
and adapter data needed to execute it. Coverage gates reject missing, stale, or
unexpected bundles. Automatically generated adapters must be reviewable and
must not erase unsupported constructs; unsupported behavior is reported as a
coverage gap.

## Failure and Reporting

Reports identify rank, core, event, address or buffer, expected state, actual
state, and the earliest known causal boundary. Later symptoms may be included
but must not replace the first failure. Malformed traces, unsupported shim calls,
and incomplete schedules fail explicitly.

## Verification Boundary

Unit tests cover the world, scheduler, executor, oracles, diagnostics, reports,
manifest gates, and installed trace adapters. Checker success proves only the
modeled contract. It does not prove AICore compilation, device memory ordering,
UDMA/SDMA transfer, hardware progress, or performance.
