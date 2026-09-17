#!/usr/bin/env python3
"""Clean one deployment's run/ across its hostfile, without runtime setup."""

import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace


def clean_node(root, dry_run=False):
    root = Path(root)
    if not root.is_absolute() or root == Path("/") or ".." in root.parts:
        raise ValueError("invalid deployment root")
    if any(path.is_symlink() for path in (root, *root.parents)):
        raise ValueError("deployment root traverses a symlink")
    marker = root / ".tilexr-validation-root"
    managed = (marker.is_file() and not marker.is_symlink()
               and marker.read_text() == "TileXR validation deployment\n")
    checkout = (root / ".git").exists() and (root / "run.sh").is_file()
    if not managed and not checkout:
        raise ValueError("not a TileXR checkout or managed validation deployment")
    run = root / "run"
    if run.is_symlink():
        raise ValueError("refusing a symlinked run directory")
    if not run.exists():
        return f"EMPTY path={run} (absent)"
    if not run.is_dir():
        raise ValueError("run is not a directory")
    # Preserve active/unconfirmed task evidence. Never follow links into other
    # deployments; child links are unlinked during cleanup, not traversed.
    for directory, dirs, files in os.walk(run, followlinks=False):
        for name in (".operation.lock", "node.lock"):
            if name in dirs or name in files:
                raise ValueError(f"active or unconfirmed validation lock: {Path(directory) / name}")
    entries = list(run.iterdir())
    if not dry_run:
        for entry in entries:
            if entry.is_symlink() or not entry.is_dir():
                entry.unlink()
            else:
                shutil.rmtree(entry)
    return f"{'DRY_RUN' if dry_run else 'CLEANED'} path={run} entries={len(entries)}"


def clean_hosts(root, hosts, user, dry_run=False):
    # Import only on the coordinator. The same file is streamed over SSH, so
    # remote nodes need neither an updated script nor a working CANN install.
    from workflow import SSH_OPTIONS, check_coordinator

    def target(host):
        return f"{user}@{host}" if user else host

    # The existing identity check can write a diagnostic; keep it outside run/.
    with tempfile.TemporaryDirectory(prefix="tilexr-clean-") as scratch:
        check_coordinator(SimpleNamespace(target=target, logs=Path(scratch)), hosts)
    worker = Path(__file__).read_text(encoding="utf-8")

    def clean(index, host):
        try:
            if index == 0:
                result = clean_node(root, dry_run)
            else:
                args = ["python3", "-", "--node", str(root)]
                if dry_run:
                    args.append("--dry-run")
                completed = subprocess.run(["ssh", *SSH_OPTIONS, target(host), shlex.join(args)],
                    input=worker, capture_output=True, text=True, timeout=180)
                if completed.returncode:
                    raise RuntimeError(completed.stderr.strip() or completed.stdout.strip()
                                       or f"ssh exit={completed.returncode}")
                result = completed.stdout.strip()
            return True, f"{host}: {result}"
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            return False, f"{host}: CLEAN_FAIL {error}"

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, len(hosts))) as pool:
        futures = [pool.submit(clean, index, item["host"]) for index, item in enumerate(hosts)]
        results = [future.result() for future in futures]
    for ok, message in results:
        print(message, file=sys.stdout if ok else sys.stderr, flush=True)
    return 0 if all(ok for ok, _ in results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--hostfile")
    parser.add_argument("--node", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.node:
            print(clean_node(args.node, args.dry_run))
            return 0
        from workflow import hosts_from_file
        root = Path(__file__).resolve().parents[2]
        hostfile = args.hostfile if args.hostfile is not None else os.environ.get("TILEXR_VALIDATION_HOSTFILE", "hostfile")
        if not hostfile:
            raise ValueError("hostfile must not be empty")
        path = (root / hostfile).resolve()
        path.relative_to(root)
        hosts = hosts_from_file(path)
        user = os.environ.get("TILEXR_VALIDATION_SSH_USER", "")
        if user and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", user):
            raise ValueError("SSH_USER 格式错误")
        return clean_hosts(root, hosts, user, args.dry_run)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"CLEAN_FAIL {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
