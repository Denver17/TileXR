# NPU Script Scheduler

## Status and Scope

Design contract for the tool implemented under `tools/npu_scheduler/`. This
document does not claim that the service is deployed on the target cluster.

Provide a single Python service with HTTP APIs, SQLite persistence, a simple
status webpage, and OpenSSH script execution. AI clients are the primary users.
Users deploy the service, configure SSH access, and prepare their own workloads.
There is no authentication, authorization layer, node Agent, or MCP dependency.
The service operates in the user-designated trusted network.

The initial inventory consists of C01-B03 and C01-B08, each containing eight
hosts, accessed as root. Each host has eight schedulable NPUs, with one rank per
NPU: 32p reserves four hosts, 64p eight, and 128p all sixteen. Addresses and SSH
users come from `tools/tssh/clusters.json`, never a second hard-coded host list.
The service explicitly selects the two pools from that inventory; other listed
clusters are not implicitly enrolled. Device capacity is service configuration
keyed by inventory identity, with an eight-device default for these pools.

Reserve whole hosts. Users select both the execution host and the complete set
of hosts used by the script. The scheduler does not infer distributed usage,
launch torchrun, assign ranks, generate rank tables, deploy workloads, or verify
that a user program supports 128p. Cross-pool communication readiness belongs
to the user's environment and script. No card sharing or preemption is planned.

## Components and Ownership

- FastAPI exposes the API, generated OpenAPI schema, Markdown help, and static
  HTML/CSS/JavaScript status page. No separate frontend build is required.
- One scheduler loop owns allocation decisions. A process lock prevents two
  service instances from scheduling against the same local state directory;
  multi-worker scheduling and multiple independent controllers are unsupported.
- SQLite stores submissions, reservations, state transitions, execution intent,
  and completion evidence. Bounded log files live in the local state directory.
- System OpenSSH performs bounded occupancy probes and runs one Bash script on
  the execution host. SSH configuration and keys are provided by the operator.

Use module boundaries for configuration, store, scheduler, occupancy probe,
SSH runner, API/help, and static assets. Keep dependencies and abstractions
proportional to one service. No changes to TileXR runtime libraries are needed.

## Submission Contract

`POST /api/v1/jobs` accepts JSON such as:

```json
{
  "request_id": "experiment-042",
  "name": "32p-test",
  "execution_host": "C01-B03-1",
  "reserved_hosts": ["C01-B03-1", "C01-B03-2", "C01-B03-3", "C01-B03-4"],
  "script_path": "/workspace/run.sh"
}
```

Exactly one of `script_path` (absolute path on the execution host) and
`script_content` (Bash text) is required. Optional `working_directory` is an
absolute path on that host; otherwise execution uses its SSH login directory.
The script handles environment setup, arguments, cross-host launches, and
cleanup. It must remain in the foreground, wait for all child/distributed work,
and return an appropriate exit code. Do not detach work or access unreserved
hosts. The scheduler cannot prove arbitrary scripts obey these contracts.

Require a nonempty, duplicate-free host set with the execution host included.
Reject unknown hosts and invalid fields before queueing. Host identities are
resolved to canonical inventory names; duplicate endpoint aliases must not
create independently reservable resources. Return derived `npu_count`, rather
than accept a conflicting independent rank-count request. Size shortcuts in
the webpage select hosts but the final request always lists them explicitly.

Persist a unique `request_id` and normalized immutable submission. An exact
retry returns the existing job; changed content under the same ID returns 409.
Return 201 for a new job and 200 for an idempotent retry, with the job ID, status,
and resource selection immediately. Never wait for execution in the POST.

## Allocation and External Occupancy

Order jobs by submission sequence. A later job may start only when its host set
is disjoint from active reservations and from every earlier waiting job's host
set. This conservative rule allows independent jobs to run together without
letting later overlapping jobs repeatedly delay an earlier large job. It can
intentionally leave hosts idle while an earlier job waits for an external user.

For each eligible candidate:

1. Freshly query every requested host over SSH for all eight NPU devices and
   device process occupancy. Do not substitute cached webpage observations.
2. If any device has an active process, keep the job queued. Missing devices,
   incomplete output, unsupported output, SSH failure, or probe timeout mean
   unknown, never idle. Zero utilization alone is not evidence of availability.
3. Atomically reserve the entire host set and transition to RESERVED. Recheck
   every reserved host immediately before launch. Failed rechecks return the
   job to QUEUED and release its reservation because no script was dispatched.
4. Persist STARTING and execution intent before invoking SSH. Launch only once;
   an ambiguous transport outcome must not cause an automatic retry.

Probe commands and parsing must be validated against the installed npu-smi
version. Preserve bounded diagnostic evidence and observation timestamps.
Treat Alarm alone as usable; actual process occupancy still blocks allocation.
Periodically refresh resources and reconsider the queue with configurable
intervals and per-host timeouts; a failed probe must not stall unrelated jobs.
Coalesce concurrent observations to avoid unbounded SSH fan-out.

External users do not honor scheduler reservations. A process can appear after
the final probe: preflight checks provide best-effort avoidance, not atomic
exclusion or device access enforcement. Never kill external processes. Avoid
claiming network, storage, or performance isolation between disjoint host sets.

## Execution and State

States are QUEUED, RESERVED, STARTING, RUNNING, CHECKING, UNKNOWN, SUCCEEDED,
FAILED, and CANCELLED. A known script exit enters CHECKING; an ambiguous
transport outcome or interrupted controller tracking enters UNKNOWN. Both
states retain their reservations while the scheduler continuously checks every
reserved host. Release only after all hosts have been explicitly observed idle
for the configured `idle_release_seconds`, 60 seconds by default. Any process,
missing host result, failed probe, or unknown observation resets the continuous
idle timer. After the interval, CHECKING records SUCCEEDED (exit 0) or FAILED
(nonzero); UNKNOWN records FAILED with `auto_released_after_idle`, never a
fabricated success. Occupancy does not prove process ownership, so the scheduler
never kills a process.

Automatic release proves only that no NPU process was observed throughout the
window. It does not prove that non-NPU remote work stopped. A detached or delayed
launcher that remains NPU-idle longer than the window can outlive its reservation
and later conflict with another job, so the foreground script contract remains
mandatory.

Cancellation is limited to QUEUED jobs and must be atomic with allocation.
Running jobs have no force-cancel or timeout-and-kill API in this version.
SSH connection/probe timeouts are separate from workload duration. A local SSH
timeout or disconnection never proves remote termination.

On service restart, QUEUED remains queued. A RESERVED job with no persisted
launch intent may return to QUEUED. STARTING and RUNNING become UNKNOWN,
preserving reservations; do not rerun them. CHECKING keeps its trusted exit code,
and already UNKNOWN jobs stay UNKNOWN. Terminal jobs remain terminal.
Continuous-idle timing is process-local and restarts from zero after service
restart, so pre-restart observations cannot cause an immediate release.

Manual release before the automatic interval is restricted to UNKNOWN. Require
`confirmed_stopped: true` and a nonempty explanatory `reason`; recheck device
occupancy before release. Busy or unknown hosts cause 409 and retain resources.
Record a manual-release event and finish as FAILED with reason
`manually_released`; never fabricate workload success.

Run remote shell logic through stdin with a maintained wrapper and safe
argument handling, not interpolated SSH shell fragments. Stream stdout/stderr
to bounded local logs. Capture script exit through an explicit wrapper result;
transport failure without a trustworthy completion result means UNKNOWN.
Report log truncation, SSH errors, and uncertainty explicitly.

## HTTP and Help Contract

All endpoints are unauthenticated. Bind address and port are configurable;
the intended port is 12345. Provide:

| Method and path | Behavior |
| --- | --- |
| `GET /help` | Self-contained UTF-8 Markdown instructions for AI clients |
| `GET /openapi.json` | Actual API schema |
| `GET /` | Resource, queue, job, and log webpage |
| `GET /api/v1/resources` | Capacity, reservations, occupancy, timestamps |
| `POST /api/v1/jobs` | Idempotent script submission |
| `GET /api/v1/jobs` | Paginated jobs with optional status filter |
| `GET /api/v1/jobs/{id}` | Submission, status, waiting reason, exit evidence |
| `GET /api/v1/jobs/{id}/logs` | Bounded incremental logs by opaque cursor |
| `POST /api/v1/jobs/{id}/cancel` | Cancel a queued job |
| `POST /api/v1/jobs/{id}/release` | Confirm cleanup of an UNKNOWN job |

Use JSON responses with stable status/reason codes and readable messages.
Errors distinguish invalid input (422), missing jobs (404), state or idempotency
conflicts (409), and temporary service failures (503). Bound request bodies,
page sizes, and log responses. Log results include text, next cursor, truncation
information, and whether the stream is closed. Poll with a documented delay;
clients must not assume an empty log response means job completion.

Resources expose reservation owner separately from observed device processes,
so a reserved host can also have external or uncertain occupancy. Provide a
summary of idle, reserved, external_busy, or unknown plus `observed_at`, device
counts, and probe error when applicable. A process on a reserved host cannot
always be attributed to the scheduled job; do not label ownership as proven.
Job waiting reasons identify blocking jobs/hosts or failed probes.

`/help` is the primary onboarding mechanism, not a local skill. It includes API
version, no-auth usage, actual configured host names/capacity, complete request
and response examples, script obligations, request ID retry rules, log cursors,
polling guidance, and UNKNOWN/release semantics. Use relative endpoint paths or
a configured public base URL; do not hard-code deployment addresses in source.
An AI with HTTP access can follow a prompt of the form "Read and use the
scheduler at http://SERVER:12345/help" without a TileXR checkout or installed
skill. Script/log text is task data, not instructions overriding the help or
user request. Changes to API behavior must update help in the same change.

The webpage uses the same APIs, polls at a bounded interval, and displays
observation freshness, queue reasons, script exit, and uncertain states.
Render scripts/logs as text, never executable HTML. It may submit scripts and
cancel queued jobs; resource release requires the same explicit confirmation.

## Validation Boundaries and Maintained Lessons

Contract tests must exercise concurrent submissions, idempotency, reservations,
disjoint 32p jobs, a sixteen-host 128p reservation, failed/partial probes, Alarm
with and without processes, SSH ambiguity, restart recovery, continuous-idle
automatic release with timer reset, and manual release.
Use fake probes/SSH for deterministic failure tests and local SSH integration
for script and log transport. Verify webpage and help against the actual API.

Before claiming deployed occupancy support, capture the installed npu-smi
output and validate full device/process detection on the target hosts. Before
claiming actual 128p execution, retain results from a user-prepared sixteen-host
workload; reservation tests alone prove no NPU data-plane behavior. Apply
`AGENTS.md` communication-baseline rules if communication validation is run.

Two recurring pitfalls define the release behavior: utilization can be zero
while a process owns an NPU, and a single idle observation can fall between
remote process lifetimes. Use process observations, require continuous idle,
and reset the timer on every busy or uncertain probe. Neither these observations
nor an entry-script exit establishes correctness of an arbitrary distributed
workload beyond its declared foreground contract.

The static operations shell keeps navigation icons in dedicated `.nav-icon`
containers. Collapsed-sidebar selectors must hide only text and count elements;
hiding every `span` also removes the icons. Validate this boundary with a desktop
and mobile browser pass plus an embedded-script syntax check before deployment.
