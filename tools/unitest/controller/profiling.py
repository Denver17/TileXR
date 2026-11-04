"""Fixed magic slots and calls to the existing analyzer; no records decoding."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid


def invocation_count(case, options):
    rounds = options["iterations"]
    if options["mode"] == "perf":
        rounds = 1 + options["warmup"] + options["batches"] * rounds
    return rounds * len(case["steps"])


def captures(case, options):
    stages = [step["operator"] for step in case["steps"]]
    mode = options["mode"]
    selection = options["profiling"]["selection"]
    chosen = {(row.get("batch", 0), row["iteration"]) for row in selection.get("rounds", [])}
    batches = range(1, options["batches"]+1) if mode == "perf" else [0]
    seed = bytearray.fromhex(options["profiling"]["capture_seed"])
    prefix = (1 + options["warmup"]) * len(stages) if mode == "perf" else 0
    for batch in batches:
        for iteration in range(1, options["iterations"]+1):
            if selection["kind"] != "all" and (batch, iteration) not in chosen:
                continue
            for stage, operator in enumerate(stages):
                magic = prefix + ((max(batch-1, 0)*options["iterations"] + iteration-1)*len(stages)) + stage + 1
                capture_id = seed.copy()
                capture_id[10:] = magic.to_bytes(6, "big")
                yield dict(magic=magic, batch=batch, iteration=iteration, operator=operator,
                           capture_uuid=str(uuid.UUID(bytes=bytes(capture_id))))


def prepare_slots(output, rank, case, options):
    output = Path(output)
    budget = options["profiling"]["options"]["max_workspace_bytes"]
    count = invocation_count(case, options)
    if count > budget // 72:
        raise ValueError("fixed profiling slot table exceeds workspace budget")
    records = []
    lines = [str(count) + " " + str(budget)]
    for invocation in captures(case, options):
        directory = output / "internal-captures" / invocation["capture_uuid"]
        directory.mkdir(parents=True, exist_ok=True)
        path = str(directory / ("rank-" + str(rank) + ".records.bin"))
        if any(ord(ch) < 32 for ch in path):
            raise ValueError("profiling output path contains a control character")
        invocation.update(rank=rank, records_path=path)
        records.append(invocation)
        lines.append("{} {} {}".format(invocation["magic"], invocation["capture_uuid"].replace("-", ""),
                                       json.dumps(path, ensure_ascii=False)))
    (output / ("rank-" + str(rank) + "-captures.json")).write_text(json.dumps(records, indent=2), encoding="utf-8")
    plan = output / ("rank-" + str(rank) + "-slots.txt")
    plan.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return str(plan)


def analyze(output, case, options, ranks, timeout, owned_group=True):
    output = Path(output)
    destination = output / "profiling"
    destination.mkdir(exist_ok=False)
    manifests = {}
    expected = list(captures(case, options))
    for rank in ranks:
        manifest = json.loads((output / ("rank-" + str(rank) + "-captures.json")).read_text(encoding="utf-8"))
        if len(manifest) != len(expected):
            raise ValueError("incomplete capture manifest")
        for actual, wanted in zip(manifest, expected):
            if any(actual.get(key) != value for key, value in wanted.items()) or actual.get("rank") != rank:
                raise ValueError("capture invocation identity mismatch")
            path = output / "internal-captures" / wanted["capture_uuid"] / ("rank-" + str(rank) + ".records.bin")
            if actual.get("records_path") != str(path) or not path.is_file() or not path.stat().st_size:
                raise ValueError("requested capture file is missing")
        manifests[rank] = manifest
    deadline = time.monotonic() + timeout
    records = []
    index = destination / "tool-invocations.json"
    for i, invocation in enumerate(expected):
        mapping = destination / (str(invocation["magic"]) + ".mapping.json")
        mapping.write_text(json.dumps(dict(schema="tilexr_profiling_mapping.v1", operator=invocation["operator"],
            cycles_per_us=options["profiling"]["options"]["cycles_per_us"], stages=[
                dict(name="kernel_total", category="total", critical=True, begin=1, end=2)])), encoding="utf-8")
        command = [sys.executable, "-m", "tilexr_profiling", "--mapping", str(mapping),
                   "--output-dir", str(destination / invocation["capture_uuid"]),
                   "--attribute", "clock_basis=" + options["profiling"]["options"]["clock_basis"]]
        for rank in ranks:
            command += ["--records", manifests[rank][i]["records_path"]]
        record = dict(invocation, ranks=ranks, command=command, exit_code=None,
                      log=str(destination / (str(invocation["magic"]) + ".log")))
        records.append(record)
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise TimeoutError("profiling analyzer deadline exceeded")
            with Path(record["log"]).open("wb") as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                    stderr=subprocess.STDOUT, start_new_session=owned_group)
                try:
                    record["exit_code"] = process.wait(timeout=remaining)
                finally:
                    if owned_group:
                        try: os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    elif process.poll() is None: process.kill()
                    process.wait()
            if record["exit_code"] != 0:
                raise ValueError("existing profiling analyzer failed or reported incomplete data; see " + record["log"])
        finally:
            index.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    return records
