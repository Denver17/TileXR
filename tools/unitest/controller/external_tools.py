"""Invoke official msprof export; keep all device-data interpretation external."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def export_msprof(responses, output, timeout, owned_group=True):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    executable = shutil.which("msprof")
    if not executable:
        raise ValueError("official msprof executable is missing")
    executable = Path(executable).resolve(strict=True)
    identity = {"path": str(executable), "sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    package = None
    for ancestor in executable.parents:
        candidate = ancestor / "share/info/msprof/version.info"
        if candidate.is_file() and candidate.stat().st_size <= 65536:
            content = candidate.read_bytes()
            package = {"path": str(candidate), "text": content.decode("utf-8"),
                       "sha256": hashlib.sha256(content).hexdigest()}
            break
    records, directories = [], set()
    deadline = time.monotonic() + timeout
    index = output / "tool-invocations.json"
    if not responses:
        raise ValueError("no rank collector results supplied")
    for rank, response in sorted(responses.items(), key=lambda item: int(item[0])):
        data = response["data"]
        if (data.get("enabled") is not True or data.get("status") != "collected" or
                data.get("tool") != "msprof" or data.get("official_api_calls") !=
                ["aclprofInit", "aclprofCreateConfig", "aclprofStart", "aclprofStop",
                 "aclprofDestroyConfig", "aclprofFinalize"]):
            raise ValueError("incomplete official collector lifecycle at rank " + rank)
        raw = Path(data["raw_output_dir"]).resolve(strict=True)
        if raw in directories or not raw.is_dir() or not any(path.is_file() and path.stat().st_size
                                                             for path in raw.rglob("*")):
            raise ValueError("missing or reused official raw collection directory at rank " + rank)
        directories.add(raw)
        command = [str(executable), "--export=on", "--output=" + str(raw)]
        record = {"rank": int(rank), "command": command, "executable": identity,
                  "version": None, "version_status": "see official tool log; executable hash recorded",
                  "official_package_version": package,
                  "exit_code": None, "raw_output_dir": str(raw),
                  "log": str(output / ("rank-" + rank + ".log")),
                  "collection_completeness": "unverified; no profiler-data parsing in UniTest"}
        records.append(record)
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("official export deadline exceeded")
            with Path(record["log"]).open("wb") as log:
                process = subprocess.Popen(command, cwd=output, stdin=subprocess.DEVNULL,
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=owned_group)
                # NodeAgent owns the enclosing analyzer group. Local exports
                # own a separate group including official analysis subprocesses.
                try:
                    record["exit_code"] = process.wait(timeout=remaining)
                finally:
                    if owned_group:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    elif process.poll() is None:
                        process.kill()
                    process.wait()
            # Inventory filenames only. CSV/SQLite contents and kernel sample
            # completeness belong to official tools, never to this framework.
            record["artifacts"] = [str(path) for path in sorted(raw.rglob("*")) if path.is_file()]
            if record["exit_code"]:
                raise ValueError("official msprof export failed; see " + record["log"])
        finally:
            index.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    return records


def compare_msprof_hosts(hosts, placement):
    if set(hosts) != {host["name"] for host in placement["hosts"]}:
        raise ValueError("official profiler host set is incomplete")
    for host in placement["hosts"]:
        record = hosts[host["name"]]
        invocations = record.get("invocations", [])
        if (record.get("tool") != "msprof" or len(invocations) != len(host["ranks"]) or
                sorted(item["rank"] for item in invocations) != host["ranks"] or
                any(item.get("exit_code") != 0 for item in invocations)):
            raise ValueError("official profiler export rank set is incomplete")
