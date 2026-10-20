# TileXR NPU Script Scheduler

This tool reserves whole eight-NPU hosts and runs a user-provided Bash script
over SSH. Its HTTP API is intended for AI clients; `/help` is the authoritative
online usage guide. It has no authentication and must run only on a trusted
network.

## Install and Run

Use Python 3.10 or later on the controller host:

```bash
python3 -m venv /opt/tilexr-npu-scheduler-venv
/opt/tilexr-npu-scheduler-venv/bin/pip install -r tools/npu_scheduler/requirements.txt
cp tools/npu_scheduler/config.example.json /etc/tilexr-npu-scheduler.json
/opt/tilexr-npu-scheduler-venv/bin/python -m tools.npu_scheduler \
  --config /etc/tilexr-npu-scheduler.json
```

Run from the TileXR repository root so `tools.tssh` is importable. The example
binds port 12345 and selects only `c01-b03` and `c01-b08` from
`tools/tssh/clusters.json`. Only one service process may use a state directory.
Do not configure multiple Uvicorn workers.

The operator supplies root SSH keys and host-key trust. Verify noninteractive
access from the controller to all hosts before starting. The scheduler does not
install TileXR, scripts, Python packages, or SSH keys on compute hosts.

If the controller cannot reach its Python package index, download wheels on a
connected machine for the controller's Python version and architecture, copy
the wheelhouse to the controller, and install with `pip --no-index --find-links`.
For example, a Python 3.11 aarch64 controller uses:

```bash
python -m pip download --dest wheelhouse --platform manylinux2014_aarch64 \
  --python-version 311 --implementation cp --abi cp311 --only-binary=:all: \
  -r tools/npu_scheduler/requirements.txt
pip install --no-index --find-links wheelhouse \
  -r tools/npu_scheduler/requirements.txt
```

HTTP clients behind an enterprise proxy must put the scheduler's private host
in `NO_PROXY`/`no_proxy`; otherwise a proxy error page can mask a healthy local
service. Validate both `GET /help` and `GET /api/v1/resources` after deployment.

For a standalone installation under `/opt/tilexr-npu-scheduler`, use the
maintained `deploy/tilexr-npu-scheduler.service` unit. Its application directory
must contain both `tools/npu_scheduler` and `tools/tssh`; the scheduler imports
the shared inventory parser. Keep the SQLite database and logs under the
configured writable `state_dir`. Back up that directory together; deleting it
loses reservation and uncertainty state.

## AI Entry Point

Give an HTTP-capable AI this prompt:

```text
读取并使用调度器 http://141.61.94.70:12345/help，在指定服务器执行任务。
```

The AI can learn submission, polling, incremental logs, and manual recovery
from that page without a local TileXR checkout or skill installation.

## Workload Contract

The submission explicitly names an execution host and every reserved host. The
script must already exist on the execution host, or be sent as `script_content`.
It owns all distributed launch details and must stay in the foreground until
all work completes. Detached children can make the entry script exit before the
actual workload; the scheduler then retains resources if NPU processes remain.
Scripts are noninteractive and receive `/dev/null` as stdin. This isolation is
required because MPI launchers may otherwise consume the SSH wrapper's remaining
control input, hiding the completion marker and turning a successful exit into
an `UNKNOWN` result.

At each launch the service queries all devices through `npu-smi info`. Any NPU
process, missing device record, unsupported output, SSH failure, or timeout
blocks launch. An external user can still start work after this check because
the scheduler cannot lock users outside the service.

After execution tracking ends, the scheduler keeps probing all reserved hosts.
It releases the reservation only after every host has been continuously idle
for `idle_release_seconds` (60 seconds by default); any busy, missing, or failed
observation resets the timer. A known exit code produces `SUCCEEDED` or
`FAILED`. An SSH disconnect, or a controller restart before a trusted exit code
was stored, produces `UNKNOWN`; automatic idle release then records `FAILED`
with `auto_released_after_idle` rather than inventing a successful outcome. A
restart preserves CHECKING jobs and their exit codes but restarts idle timing
from zero. An operator may still use the manual release API sooner after
separately confirming that the remote workload stopped.
Automatic release observes NPU processes only. A detached workload that waits
longer than the idle window before using an NPU can therefore outlive its
reservation; entry scripts must keep all remote work in the foreground.

## Validation

Run deterministic tests from the repository root:

```bash
python -m pip install -r tools/npu_scheduler/requirements-test.txt
python -m pytest tests/tools/npu_scheduler -q
```

These tests cover API and scheduling behavior with fake SSH/NPU responses. They
do not validate the installed `npu-smi` format or a 128p workload. Before using
the service, compare real idle and busy output with the parser and run a
user-prepared foreground workload. Follow the repository communication baseline
rules if that workload is used to validate NPU communication.
