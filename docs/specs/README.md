# TileXR Feature Specifications

This directory contains the current design contract for each maintained TileXR
feature. Specifications describe the implemented architecture and durable
compatibility boundaries; execution history belongs in Git, validation guides,
or focused performance reports.

All specifications inherit the project baseline: C++14, CANN 9.1.0, driver
25.1.rc1 or later, core support for Ascend 910B/910A5, and A5/Ascend950 hardware
for UDMA data-plane claims. Sources under `reference/` are comparison-only.

## Index

| Feature | Specification |
| --- | --- |
| Collective runtime and profiling | [collectives.md](collectives.md) |
| Independent profiling SDK and offline analysis | [profiling.md](profiling.md) |
| vllm-ascend collective integration | [vllm-ascend-collectives.md](vllm-ascend-collectives.md) |
| Offline checker | [checker.md](checker.md) |
| Data-as-flag device protocol | [data-as-flag.md](data-as-flag.md) |
| SDMA transport | [sdma-transport.md](sdma-transport.md) |
| UDMA transport | [udma-transport.md](udma-transport.md) |
| Installed-operator correctness, Host timing and profiling | [unitest.md](unitest.md) |
| EP memory backend | [ep-memory.md](ep-memory.md) |
| EP memory Server Dedup Dispatch/Combine and EP profiling compatibility | [Current design](ep-memory-server-dedup-dispatch-combine-design.md) |
| MoonEP runtime and planner | [moonep-runtime.md](moonep-runtime.md) |
| MoonEP Dispatch | [moonep-dispatch.md](moonep-dispatch.md) |
| MoonEP Combine | [moonep-combine.md](moonep-combine.md) |
| MoonEP weight stages | [moonep-weight-stages.md](moonep-weight-stages.md) |
| MoonEP model replay | [moonep-model-replay.md](moonep-model-replay.md) |
| Windows cluster SSH tool | [tssh.md](tssh.md) |
| NPU script scheduler | [npu-scheduler.md](npu-scheduler.md) |

## Related design documents

The ElasticMemory design is maintained directly under `docs/`:
[中文](../elastic-memory.md) · [English](../elastic-memory.en.md).
It defines its own validated platform scope and separates current behavior from
unimplemented interface goals.

## Maintenance Rules

- Keep one current specification per feature. Extend the existing document when
  the feature evolves instead of adding a dated design or implementation plan.
- Record public interfaces, ownership, layouts, protocols, supported modes,
  failure behavior, and validation boundaries that future changes must preserve.
- Do not retain approval transcripts, task breakdowns, branch or commit
  baselines, host addresses, raw experiment logs, superseded alternatives, or
  completed migration checklists.
- Treat public headers and active source as authoritative when a specification
  drifts. Update the specification in the same change that alters its contract.
- Keep commands and current test coverage in maintained guides such as
  `docs/BUILD_VERIFICATION.md`, module READMEs, and focused status reports.
