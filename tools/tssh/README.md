# tssh

`tssh` is TileXR's native-Windows entry point for one-shot commands on a
configured group of Linux hosts. It uses the installed `ssh.exe`, starts at
most one process per host, and reports every host in inventory order.

Add the tool directory to the current PowerShell session once:

```powershell
$env:PATH = "$PWD\tools\tssh;$env:PATH"
```

Run a short command:

```powershell
tssh -C c02-a07 -- hostname
tssh -C c02-a04 -j 4 -- "npu-smi info"
```

Broadcast a multi-line Bash program directly from memory. `-I` reads stdin
once and sends the same bytes to every host; no local or remote script file is
created by `tssh`.

```powershell
@'
set -euo pipefail
repo=$1
build_dir=$2
install_dir=$3
cmake -S "${repo}" -B "${build_dir}" -DTILEXR_BUILD_EP=ON
cmake --build "${build_dir}" -j"$(nproc)"
cmake --install "${build_dir}"
'@ | tssh -C c02-a07 -I -- bash -s -- /path/to/TileXR /path/to/build /path/to/install
```

## Key Setup

Normal command execution uses only Python's standard library and Windows
OpenSSH. Paramiko is an optional dependency used only to install a public key
with password authentication:

```powershell
python -m pip install -r tools/tssh/requirements-key-setup.txt
tssh -C c02-a09 --install-key
```

The password is read once through a hidden console prompt. It is not accepted
in JSON or as a command-line option and is not written to a file. The default
public key is `%USERPROFILE%\.ssh\id_ed25519.pub`; override it with
`--public-key PATH`. Installation is idempotent and applies mode 700 to
`~/.ssh` and mode 600 to `authorized_keys`.
Hardware-backed OpenSSH public keys are accepted in the standard
`sk-ssh-ed25519@openssh.com` and `sk-ecdsa-sha2-nistp256@openssh.com` formats.

OpenSSH uses `StrictHostKeyChecking=accept-new`: first-seen keys are added to
the user's `known_hosts`, while changed keys are rejected. `UpdateHostKeys` is
disabled because concurrent Windows clients otherwise race while replacing
`known_hosts.old`; this does not disable first-seen key registration.

## Inventory

Edit the adjacent `clusters.json` to add hosts. Cluster lookup is
case-insensitive, and host order controls report order.

```json
{
  "version": 1,
  "clusters": {
    "c02-a07": {
      "user": "root",
      "hosts": [
        {"name": "cpu1", "address": "141.61.52.35"}
      ]
    }
  }
}
```

`-j/--jobs` accepts 1 through 64 and defaults to 64. This means all hosts run
concurrently unless the cluster contains more than 64 hosts; effective
concurrency is always `min(jobs, host count)`. `--connect-timeout` bounds SSH connection setup and
`--timeout` bounds the local SSH process wait; a local timeout does not prove
that an already-started remote process was terminated. The overall exit status
is nonzero if any host fails.

## Concurrency Benchmark

Measured on 2026-08-29 from Windows with Python 3.13 and the 24 hosts in
C02-A04, C02-A07, and C02-A09. The workload was remote `true`; levels were
randomized, with one warm-up and five measured runs per level and a one-second
cooldown between launches.

| `-j` | Effective | Median | Minimum | Maximum |
| ---: | --------: | -----: | ------: | ------: |
| 1 | 1 | 37.933 s | 36.937 s | 40.720 s |
| 2 | 2 | 17.430 s | 16.643 s | 21.634 s |
| 4 | 4 | 8.920 s | 8.800 s | 9.751 s |
| 8 | 8 | 4.529 s | 4.434 s | 4.624 s |
| 16 | 16 | 3.121 s | 3.068 s | 3.184 s |
| 24 | 24 | 1.967 s | 1.839 s | 2.003 s |
| 32 | 24 | 1.945 s | 1.902 s | 2.156 s |
| 64 | 24 | 1.784 s | 1.708 s | 1.877 s |

Because 24, 32, and 64 all create exactly 24 workers, they were measured again
in an interleaved five-run check. Their medians were 1.915 s, 2.146 s, and
1.929 s respectively. The best effective setting is full host concurrency.
The default is therefore 64, expressing "all configured hosts, capped at 64";
a configured eight-host cluster creates eight workers and this 24-host
benchmark creates 24.
