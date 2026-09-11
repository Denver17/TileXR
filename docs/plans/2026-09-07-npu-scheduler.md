# NPU Script Scheduler Implementation Plan

## Goal and Scope

Implement `docs/specs/npu-scheduler.md` under `tools/npu_scheduler/` as a small
API-first script scheduler with whole-host reservations, startup occupancy
checks, SQLite state, SSH execution, and a status webpage. This is a durable
implementation handoff plan. The user deploys the resulting tool; target-cluster
deployment is outside implementation scope.

Read `AGENTS.md`, `tools/tssh/README.md`, `tools/tssh/clusters.json`, and the spec
before implementation. Preserve existing inventory changes. Use its C01-B03
and C01-B08 entries, root SSH user, eight NPUs per host, and sixteen-host 128p
capacity. Do not duplicate addresses in code or this plan. No authentication,
Agents, MCP service, torchrun management, rank generation, workload deployment,
automatic running-job cancellation, or TileXR runtime changes are in scope.

## Ordered Work

### 1. Establish configuration and persistent contracts

- **Objective/role:** Provide validated inventory, submissions, job state, and
  exclusive reservations as the foundation for scheduling.
- **Prerequisites:** Spec submission, restart, and state contracts; inspect
  existing Python test conventions and tssh inventory parser before choosing
  shared helpers. Avoid a broad tssh refactor.
- **Scope:** `tools/npu_scheduler/` configuration, models, store, package entry
  points and dependencies; `tests/tools/npu_scheduler/` storage tests.
- **Constraints:** One service process, local SQLite/state directory, process
  lock, canonical host identity, no embedded addresses or credentials. Preserve
  script content exactly for execution and idempotency comparison.
- **Acceptance:** Concurrent duplicate submissions create one job; changed
  payloads conflict; overlapping reservations cannot commit; invalid inventory
  aliases and requests fail; restart transitions follow the spec.
- **Artifacts/interfaces:** Submission model, resource records, transactional
  job store and reserve/transition operations for tasks 3 and 4.

### 2. Implement occupancy and SSH execution adapters

- **Objective/role:** Supply bounded all-device observations and foreground
  script execution with trustworthy completion versus transport uncertainty.
- **Prerequisites:** Task 1 host identity and job models. Read target-version
  npu-smi documentation/output before finalizing probe parsing. For any Windows
  target inspection use the existing `tools/tssh/tssh.cmd` entry point.
- **Scope:** Probe and SSH runner modules, runner wrapper, focused adapter tests
  and sanitized probe fixtures in the scheduler test directory.
- **Constraints:** System OpenSSH, bounded parallel probes, safe stdin-based
  remote Bash transport; no Agent or workload deployment. Never kill processes.
  Preserve script paths/content including spaces, quotes, and newlines. Missing
  devices or unrecognized output yield unknown; Alarm alone does not block.
- **Acceptance:** Tests cover idle/busy/partial/unsupported probe results,
  timeouts, nonzero SSH exit, normal and failing scripts, large/incremental logs,
  and interrupted transport. Demonstrate distinction between a script exit and
  an SSH failure, including exit-code ambiguity. Log storage remains bounded.
- **Artifacts/interfaces:** Probe observation with timestamp/device coverage;
  runner log stream and known-exit/unknown outcome consumed by task 3.

### 3. Implement queue, launch, and reconciliation

- **Objective/role:** Execute disjoint jobs concurrently without double booking
  or falsely releasing uncertain work.
- **Prerequisites:** Tasks 1 and 2.
- **Scope:** Scheduler loop, allocation, recovery, cancellation/release services,
  deterministic scheduler and fault-injection tests.
- **Constraints:** Earlier overlapping queued jobs have precedence; all-host
  fresh check, atomic reservation, final recheck, then persisted launch intent.
  One slow or failing host must not block disjoint work. Do not retry ambiguous
  launches. Retain CHECKING and UNKNOWN reservations until every reserved host
  is continuously idle for the configured interval or an UNKNOWN job receives
  an explicit verified release.
- **Acceptance:** Two four-host jobs run concurrently; overlap queues; sixteen
  hosts reserve atomically for 128p; external occupancy delays launch and its
  disappearance allows progress. Test final-recheck races, enqueue/cancel
  races, completion probes, continuous-idle timer reset, restarts at every launch
  boundary, and rejection of manual release when confirmation is absent or any
  probe is busy/unknown.
- **Artifacts/interfaces:** Job operations and resource snapshots for the API;
  stable waiting and failure reason codes.

### 4. Expose HTTP API and self-contained AI help

- **Objective/role:** Allow an HTTP-capable AI to operate from `/help` alone.
- **Prerequisites:** Tasks 1 and 3 public operations and task 2 log cursor model.
- **Scope:** FastAPI endpoints, request/response schemas, `/help`, OpenAPI output,
  HTTP contract tests, configurable public URL/bind/port.
- **Constraints:** No authentication or local skill dependency. Explicit host
  sets, bounded bodies/pages/logs, idempotency, stable error/status codes. Help
  uses the actual configured inventory and API version, with no fixed address.
- **Acceptance:** Exercise every documented example against a test service;
  verify new/retry/conflicting submissions, filters, pagination, incremental
  logs, queue cancellation, and UNKNOWN release. From `/help` alone, submit a
  fake workload and query its result with no repository or skill context.
- **Artifacts/interfaces:** Working REST API, generated OpenAPI and online
  Markdown runbook consumed by task 5 and external AI clients.

### 5. Add webpage and user deployment documentation

- **Objective/role:** Provide a compact secondary UI and user-operated setup.
- **Prerequisites:** Task 4 API contract and completed lifecycle behavior.
- **Scope:** Static HTML/CSS/JavaScript, `tools/npu_scheduler/README.md`, generic
  example configuration and optional maintained service-unit example.
- **Constraints:** No frontend build pipeline; same HTTP API for all actions.
  Distinguish observation from reservation; show stale/unknown status. Escape
  all script/log content. Do not install or start a service on target machines.
- **Acceptance:** Desktop/mobile browser checks cover resources, submit, queue,
  logs, cancellation, and explicit release confirmation. Documentation states
  prerequisites, inventory selection, root SSH setup, port 12345, writable state,
  one-process operation, script obligations, limits, and restart recovery.
- **Artifacts/interfaces:** Usable static UI; documented launch command
  `python -m tools.npu_scheduler --config /path/to/config.json`; example prompt
  for reading `http://SERVER:12345/help`. Implement this CLI during this task.

### 6. Verify integrated behavior and finalize guidance

- **Objective/role:** Establish evidence for the completed contracts and state
  precisely which target-host behaviors remain unverified.
- **Prerequisites:** Tasks 1-5; execution review of final code and documentation.
- **Scope:** Scheduler tests, local SSH integration harness, browser verification,
  README and spec validation/lessons sections.
- **Constraints:** No source-text guard tests. No automatic target deployment
  or 128p workload launch. Hardware checks require user-provided workload and
  deployment context; follow retained HCCL baseline rules if applicable.
- **Acceptance:** Run
  `python -m pytest tests/tools/npu_scheduler -q` after implementing those tests;
  run opt-in local SSH integration using the documented harness; launch locally
  and verify `/help`, `/openapi.json`, and webpage against the actual service.
  Include restart/transport-failure cases and the 16-host fake-inventory case.
  Report unavailable SSH/browser/hardware checks explicitly, not as passes.
- **Artifacts:** Tested implementation, maintained operational lessons, and a
  concise record of verification commands/results and remaining boundaries.

## Risks and Validation Boundary

The NPU output format and SSH completion protocol need evidence before being
declared supported. Deterministic mocks test scheduling, not real NPU transfer.
Outside users can start after the last check; the service cannot provide hard
exclusion without their cooperation. Root scripts can escape reservations;
declared scope and foreground completion are user contracts, not enforcement.
UNKNOWN outcomes never become successful automatically. Reservations are
released only after a full continuous-idle interval or verified manual recovery.

Tasks are ordered by their interface dependencies; no parallel agent work is
required. Keep any target inspection read-only during implementation unless the
user requests a workload run. Retain this plan for the implementation handoff
and include documentation with implementation delivery, not a spec-only commit.
