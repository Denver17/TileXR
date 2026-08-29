# tssh Specification

## Scope

`tools/tssh/tssh.cmd` is the Windows entry point for temporary one-shot commands
on a configured Linux cluster. `tools/tssh/tssh.py` owns inventory parsing,
OpenSSH process execution, bounded concurrency, output attribution, and the
one-time public-key setup mode.

The tool is not a persistent deployment system and does not replace retained,
reviewed task scripts.

## Command Contract

The command selects a cluster from `tools/tssh/clusters.json`, then runs the
arguments after `--` on the selected hosts. With `-I`, stdin is forwarded so
non-trivial Bash can be piped to `bash -s -- ARGS`. A host selector may narrow
the inventory. `-j N` bounds concurrency from 1 through 64; without it, all
selected configured hosts run concurrently, capped at 64.

The wrapper preserves command argument boundaries and exit status. It does not
join remote shell fragments or interpret Bash syntax itself.

## Inventory

`clusters.json` is the single source of host lists, SSH users, ports, and
cluster defaults. Agent-generated orchestration must not duplicate those lists.
Malformed clusters, duplicate hosts, invalid concurrency, or an empty selection
fail before any SSH process is started.

## Execution and Output

Each host runs in an independent Windows OpenSSH process. Output remains
attributed to its host and is drained concurrently to avoid pipe deadlock. The
overall result is nonzero when any selected host fails or cannot be launched,
while preserving per-host diagnostics.

Known-host behavior is noninteractive and bounded for one-shot operation.
`UpdateHostKeys` is disabled because concurrent clients can race while updating
the same file.

## Key Setup

`--install-key` is a separate one-time mode. It may use Paramiko to authenticate
and install the local public key, but normal command execution always uses
OpenSSH. Key setup validates the exact target host and reports partial success;
it is never run implicitly by a command broadcast.

## Remote Script Boundary

For temporary multi-command Bash, pipe the script through stdin:

```powershell
@'
set -euo pipefail
# one-shot remote logic
'@ | tools\tssh\tssh.cmd -C cluster -I -- bash -s -- arg1 arg2
```

Create and transfer a task-scoped `.sh` only when the script itself must be
retained, reviewed, or reused.

## Verification Boundary

`tests/tools/tssh/test_tssh.py` covers parsing, inventory selection, command
construction, concurrency, stdin, output, and failure aggregation with bounded
process doubles. A live smoke validates only the selected Windows/OpenSSH and
host environment. Operational examples and setup are maintained in
`tools/tssh/README.md`.
