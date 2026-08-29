# tssh Design

## Goal

Add a native-Windows multi-host SSH command under `tools/tssh` for short-lived,
one-shot cluster work. The tool must let an agent broadcast a multi-line Bash
program from standard input without creating a task-specific local or remote
script file.

The public command name is `tssh`. There is no `tilexr-mssh` alias.

## Scope

`tssh` will:

- run one remote command concurrently on a configured cluster;
- optionally copy standard input byte-for-byte to every remote command;
- use the installed Windows OpenSSH `ssh.exe`, including its SSH config and
  identities, and automatically add first-seen host keys to `known_hosts`;
- limit concurrency to a user-selected value from 1 through 64;
- report stdout, stderr, exit status, connection failures, and timeouts for
  every host;
- return nonzero when any host does not complete successfully; and
- ship with example definitions for C02-A04, C02-A07, and C02-A09.

It will not manage packages, schedule resources, copy files, create remote
temporary scripts, or replace a cluster scheduler.

## Command-Line Interface

The checked-in Windows entry point is `tools/tssh/tssh.cmd`. Documentation may
refer to it as `tssh` once that directory is on `PATH`.

Short commands:

```powershell
tssh -C c02-a07 -- hostname
tssh -C c02-a04 -j 4 -- "npu-smi info"
```

Multi-line Bash is sent through standard input:

```powershell
@'
set -euo pipefail
repo=$1
build_dir=$2
cmake -S "${repo}" -B "${build_dir}"
cmake --build "${build_dir}" -j"$(nproc)"
'@ | tssh -C c02-a09 -I -- bash -s -- /path/to/repo /path/to/build
```

Supported options:

- `-C, --cluster NAME`: required cluster name from the JSON configuration.
- `-c, --config PATH`: override the default adjacent `clusters.json` file.
- `-j, --jobs N`: maximum concurrent SSH processes, from 1 through 64.
- `-I, --send-input`: read local stdin once and send identical bytes to every
  host. Without this option, remote stdin is closed.
- `--connect-timeout SECONDS`: OpenSSH connection timeout.
- `--timeout SECONDS`: local wait bound for each SSH process. A local timeout
  is not proof that the remote process was terminated.
- `--ssh PATH`: override `ssh.exe`, primarily for diagnostics and tests.
- `--install-key`: use a hidden password prompt and optional Paramiko dependency
  to install the selected local public key on the cluster.
- `--public-key PATH`: override the public key used by `--install-key`.
- `--`: end local options; all remaining arguments form the remote command.

Remote command arguments are POSIX-shell quoted before they are passed to
OpenSSH. A single argument such as `"npu-smi info"` remains a shell command;
separate arguments such as `bash -s -- value` preserve argument boundaries.

## Configuration

The default `tools/tssh/clusters.json` schema is versioned:

```json
{
  "version": 1,
  "clusters": {
    "c02-a07": {
      "user": "root",
      "hosts": [
        {"name": "cpu1", "address": "141.61.52.35"},
        {"name": "cpu2", "address": "141.61.52.39"}
      ]
    }
  }
}
```

Cluster and host order is significant and is preserved in the final report.
Names are case-insensitive at lookup time and must be unique within their
scope. Users may add clusters and hosts without modifying Python code.

The checked-in examples contain:

- C02-A04: `141.61.53.150`, `141.61.53.83`, `141.61.53.142`,
  `141.61.53.138`, `141.61.53.110`, `141.61.53.106`, `141.61.53.146`,
  `141.61.53.98`.
- C02-A07: `141.61.52.35`, `141.61.52.39`, `141.61.52.43`,
  `141.61.52.47`, `141.61.52.75`, `141.61.52.79`, `141.61.52.83`,
  `141.61.52.87`.
- C02-A09: `141.61.52.116`, `141.61.52.120`, `141.61.52.128`,
  `141.61.52.124`, `141.61.52.156`, `141.61.52.160`, `141.61.52.164`,
  `141.61.52.167`.

## Execution Model

The implementation uses the Python standard library and launches one Windows
OpenSSH process per host through a bounded thread pool. It does not depend on
the `parallel-ssh` Python package.

Every SSH process uses non-interactive host-key settings. First-seen keys are
written to the user's `known_hosts`, while changed keys are still rejected.
Error-level OpenSSH diagnostics remain enabled while informational login banners
are hidden:

```text
-T [-n when -I is absent] -o LogLevel=ERROR -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UpdateHostKeys=no
```

`UpdateHostKeys` is disabled because concurrent Windows OpenSSH processes race
while replacing `known_hosts.old`. This does not disable the first-seen key
write performed by `StrictHostKeyChecking=accept-new`.

The connection timeout is passed through `ConnectTimeout`. The tool reads
stdin into memory only when `--send-input` is selected, then gives each worker
the same immutable byte string. Without `--send-input`, OpenSSH receives `-n`
and its local stdin is also connected to `DEVNULL`, ensuring remote EOF on
Windows. Each worker captures stdout and stderr and
returns a structured result. Results are printed in configuration order with
host identity and exit status, rather than completion order.

The effective worker count is `min(jobs, host_count)`. Values above the number
of selected hosts are accepted up to 64 but cannot create additional useful
parallelism.

## Failure Semantics

- Invalid arguments or configuration fail before any SSH process starts.
- A missing SSH executable, process-launch error, host timeout, SSH failure,
  or nonzero remote exit status is recorded against that host.
- All hosts are attempted unless local setup fails before dispatch.
- Overall exit status is zero only when every host exits zero without a local
  exception or timeout.
- Timeout handling terminates the local SSH process. The documentation must
  not claim that this always terminates an already-started remote process.

No command string, script body, private key, or environment secret is written
to a temporary file by `tssh`.

## Default Concurrency Benchmark

The C02-A04, C02-A07, and C02-A09 examples provide 24 distinct hosts. The
benchmark measured `-j 1,2,4,8,16,24,32,64` with randomized level order,
one warm-up per level, and five measured runs per level. The remote
workload is a bounded read-only command suitable for SSH startup measurement;
`hostname` is also run to confirm host identity and result completeness.

For 32 and 64, effective concurrency remains 24 because only 24 hosts are in
scope. Reports must state that boundary rather than presenting those cases as
true 32-way or 64-way execution.

Levels 24, 32, and 64 have identical effective concurrency, so an additional
interleaved five-run check was used to separate label-order noise from a real
worker-count effect. The check produced medians of 1.915, 2.146, and 1.929
seconds. The best effective setting is full host concurrency. The default is
64, meaning all configured hosts up to the hard 64-host cap; it creates only
24 workers for this inventory. The complete first-pass table is retained in
`tools/tssh/README.md`.

## Verification

Automated tests cover:

- JSON schema and duplicate validation;
- cluster lookup and stable host ordering;
- concurrency bounds and effective worker count;
- remote command quoting;
- stdin disabled and byte-identical stdin broadcast paths;
- per-host stdout, stderr, exit status, process-launch error, and timeout;
- deterministic report ordering and aggregate exit status; and
- `.cmd` launcher behavior on Windows;
- optional Paramiko isolation from normal command execution; and
- hidden-password, idempotent public-key installation.

Live validation runs read-only commands on all 24 configured hosts, verifies
their identity and per-host exit status, then executes the concurrency
benchmark. `AGENTS.md` and `tools/tssh/README.md` will record the validated
invocation, chosen default, benchmark boundary, and reusable operational
pitfalls.
