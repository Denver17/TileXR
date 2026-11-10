#!/usr/bin/env python3
"""Prepare compact Dispatch captures and analyze them with tilexr_profiling.

The producer declares ranks, lanes, workload and capture identities before launch.
Analysis binds SDK completion/Host-event shards to that plan, then delegates
pairing, statistics and reports to the standalone profiling package. This tool
does not select devices, launch kernels or establish a timer frequency.
"""

import argparse
import json
import math
from pathlib import Path
from uuid import UUID, uuid4

from tilexr_profiling import (
    attach_completion, generate_capture_set_reports,
    load_manifest, mapping_fingerprint, merge_manifest_shards,
)


def save(path, value):
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, allow_nan=False)
        output.write("\n")


def capture_id(seed, ordinal):
    identity = bytearray(UUID(seed).bytes)
    for index in range(4):
        identity[12 + index] ^= (ordinal >> (index * 8)) & 255
    if not any(identity):
        raise ValueError("derived capture UUID must be nonzero")
    return str(UUID(bytes=bytes(identity)))


def prepare(args):
    if (not math.isfinite(args.cycles_per_us) or args.cycles_per_us <= 0 or
            not 2 <= args.lanes <= 65536 or not 1 <= args.samples <= 1024 or
            not 1 <= args.warmup <= 1000 or not 1 <= args.bs <= 256 or
            not 128 <= args.h <= 4096 or args.h % 16 or
            len(args.devices) != 2 or min(args.devices) < 0 or
            args.devices[0] == args.devices[1]):
        raise ValueError("invalid timer, workload, sampling or device configuration")
    mapping = json.loads(Path(__file__).with_name("compact.mapping.template.json").read_text())
    mapping["cycles_per_us"] = args.cycles_per_us
    if args.detail == "TOTAL":
        mapping["stages"] = mapping["stages"][:1]
    fingerprint = mapping_fingerprint(mapping)
    seed = str(uuid4())
    args.directory.mkdir(parents=True, exist_ok=False)
    save(args.directory / "mapping.json", mapping)
    plan = dict(seed=seed, mapping_sha256=fingerprint, bs=args.bs, h=args.h,
                warmup=args.warmup, samples=args.samples, lanes=args.lanes,
                devices=args.devices, detail=args.detail)
    save(args.directory / "plan.json", plan)
    members = []
    for ordinal in range(args.warmup, args.warmup + args.samples):
        identity = capture_id(seed, ordinal)
        participants, metrics = [], []
        for rank, device in enumerate(args.devices):
            rows = sum((token * 2 + k + source) % 4 // 2 == rank
                       for source in range(2) for token in range(args.bs) for k in range(2))
            output_bytes = rows * (args.h * 2 + 12)
            for lane in range(args.lanes):
                sid = (rank << 16) | lane
                participants.append(dict(stream_id=sid, rank=rank, logical_lane=lane,
                    sampled=True, host=args.host,
                    device=dict(id=str(device), namespace="host-local ACL ordinal"),
                    acl_stream_label="ElasticMemory Pool service stream"))
                for stage_id, stage in enumerate(mapping["stages"]):
                    metric = dict(stream_id=sid, stage_id=stage_id, stage=stage["name"],
                                  source="test_compact_ep deterministic route oracle before launch")
                    if stage_id == 0:
                        metric.update(bytes=output_bytes // args.lanes + (lane < output_bytes % args.lanes),
                            byte_semantics="logical final hidden+assist output bytes apportioned across lanes; not physical traffic",
                            scope="per_lane_total")
                    else:
                        metric.update(status="not_applicable",
                            reason="control stage or physical lane traffic is not independently counted")
                    metrics.append(metric)
        manifest = dict(schema="tilexr_profiling_manifest.v1", attempt_id=identity,
            capture_id=identity, mapping_sha256=fingerprint, participants=participants,
            stage_metrics=metrics, environment=dict(soc=args.soc,
                cann_version=args.cann, driver_version=args.driver),
            versions=dict(sdk_version="1.0.0", device_view_abi="2",
                kernel_version="compact-memory-abi3-sdk2-" + args.detail.lower()),
            execution=dict(kind="communication", stages=[
                dict(rank=rank, stage_id=stage_id, stage=stage["name"], backend="native_memory",
                     native_status="native Ascend C; output checked by test_compact_ep",
                     kernel_version="tilexr_ep_dispatch_compact_kernel")
                for rank in range(2) for stage_id, stage in enumerate(mapping["stages"])]),
            attributes=dict(shape=dict(bs=args.bs, h=args.h, top_k=2, experts=4,
                ranks=2, dtype="bf16", routing="(token*2+k+source)%4"),
                clock_basis=args.clock_basis, source_id=args.source_id,
                diagnostic=args.detail, stage_barrier="Host AllGather before Dispatch and Combine",
                timing_scope="GetSystemCycle kernel body excludes recorder Flush and final task tail"))
        save(args.directory / f"expected-{ordinal}.json", load_manifest(manifest, mapping))
        members.append(dict(measurement_id=str(ordinal), iteration=ordinal,
            attempt_id=identity, capture_id=identity, warmup=False, mapping="mapping.json",
            records=[f"capture-{ordinal}-rank{rank}.records.bin" for rank in range(2)],
            manifest=f"manifest-{ordinal}.json"))
    save(args.directory / "capture-set.json", dict(schema="tilexr_profiling_capture_set.v1",
        experiment_id=seed, measurements=members))
    print(json.dumps(dict(TILEXR_ELASTIC_CAPTURE_ID=UUID(seed).hex,
        TILEXR_ELASTIC_MAPPING_SHA256=fingerprint,
        TILEXR_ELASTIC_PROFILE_DIR=str(args.directory.resolve()))))


def analyze(args):
    directory = args.directory
    plan = json.loads((directory / "plan.json").read_text())
    mapping = directory / "mapping.json"
    for ordinal in range(plan["warmup"], plan["warmup"] + plan["samples"]):
        shards = []
        for rank in range(2):
            path = directory / f"capture-{ordinal}-rank{rank}.records.bin"
            shard = json.loads(Path(str(path) + ".manifest.json").read_text())
            completion = json.loads(Path(str(path) + ".completion.json").read_text())
            if completion["generation"] != ordinal + 1:
                raise ValueError("completion generation differs from the planned invocation")
            shards.append(attach_completion(shard, completion))
        manifest = merge_manifest_shards(directory / f"expected-{ordinal}.json", shards, mapping)
        save(directory / f"manifest-{ordinal}.json", manifest)
    report = generate_capture_set_reports(directory / "capture-set.json", directory / "repeat-report",
        artifacts=("markdown", "html", "perfetto", "json", "csv"))
    print(json.dumps(dict(status=report.status, output=str(report.output_dir))))
    if report.status != "complete":
        raise ValueError("capture set is incomplete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pre = commands.add_parser("prepare")
    pre.add_argument("directory", type=Path)
    pre.add_argument("--bs", type=int, required=True)
    pre.add_argument("--h", type=int, required=True)
    pre.add_argument("--lanes", type=int, required=True)
    pre.add_argument("--devices", type=int, nargs=2, required=True)
    pre.add_argument("--host", required=True)
    pre.add_argument("--soc", required=True)
    pre.add_argument("--cann", required=True)
    pre.add_argument("--driver", required=True)
    pre.add_argument("--source-id", required=True)
    pre.add_argument("--cycles-per-us", type=float, required=True)
    pre.add_argument("--clock-basis", required=True)
    pre.add_argument("--detail", choices=("TOTAL", "STAGES"), default="STAGES")
    pre.add_argument("--warmup", type=int, default=20)
    pre.add_argument("--samples", type=int, default=20)
    post = commands.add_parser("analyze")
    post.add_argument("directory", type=Path)
    args = parser.parse_args()
    (prepare if args.command == "prepare" else analyze)(args)


if __name__ == "__main__":
    main()
