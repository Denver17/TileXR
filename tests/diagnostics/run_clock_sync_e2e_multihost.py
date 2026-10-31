#!/usr/bin/env python3
"""Launch and aggregate the TileXR clock synchronization test across hosts."""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-file", required=True)
    parser.add_argument("--remote-root", required=True)
    parser.add_argument("--remote-install-prefix", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ranks-per-host", type=int, default=8)
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--root-port", type=int, default=29501)
    parser.add_argument("--timeout-sec", type=int, default=600)
    parser.add_argument("--soc-name", default="Ascend950")
    parser.add_argument("--max-precision-ns", type=int)
    parser.add_argument("--require-idle-devices", type=int)
    return parser.parse_args()


def read_hosts(path):
    hosts = []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        host = line.split("#", 1)[0].strip()
        if host:
            hosts.append(host)
    if not hosts or len(set(hosts)) != len(hosts):
        raise ValueError("host file must contain unique non-empty host names")
    return hosts


def check_simple_remote_path(value, name):
    if not value.startswith("/") or any(char.isspace() for char in value):
        raise ValueError("{} must be an absolute remote path without whitespace".format(name))


def terminate(processes):
    for process, output in processes:
        if process.poll() is None:
            process.terminate()
        output.close()


def main():
    args = parse_args()
    if args.ranks_per_host <= 0 or args.rounds <= 0 or args.timeout_sec <= 0:
        raise ValueError("ranks per host, rounds, and timeout must be positive")
    if args.root_port <= 0 or args.root_port > 65535:
        raise ValueError("root port must be in 1..65535")
    if args.require_idle_devices is not None and args.require_idle_devices <= 0:
        raise ValueError("required idle-device count must be positive")
    check_simple_remote_path(args.remote_root, "remote root")
    check_simple_remote_path(args.remote_install_prefix, "remote install prefix")

    hosts = read_hosts(args.host_file)
    rank_size = len(hosts) * args.ranks_per_host
    output_dir = pathlib.Path(args.output_dir).resolve()
    if output_dir.exists():
        raise ValueError("output directory already exists: {}".format(output_dir))
    output_dir.mkdir(parents=True)
    remote_output = "{}/run/clock-sync-{}".format(
        args.remote_root.rstrip("/"), output_dir.name)
    remote_worker = "{}/tests/diagnostics/run_clock_sync_e2e_multihost_worker.sh".format(
        args.remote_root.rstrip("/"))
    comm_id = "{}:{}".format(hosts[0], args.root_port)

    processes = []
    try:
        for node_index, host in enumerate(hosts):
            log_path = output_dir / "node{}_launcher.log".format(node_index)
            log_file = log_path.open("wb")
            node_remote_output = "{}/node{}".format(remote_output, node_index)
            command = [
                "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                "root@{}".format(host), "env", "TILEXR_SOC_NAME={}".format(args.soc_name),
                "bash", remote_worker, args.remote_root, args.remote_install_prefix,
                node_remote_output, str(rank_size), str(node_index),
                str(args.ranks_per_host), str(args.rounds), comm_id,
                str(args.require_idle_devices or 0),
            ]
            processes.append((subprocess.Popen(command, stdout=log_file,
                                               stderr=subprocess.STDOUT), log_file))

        deadline = time.monotonic() + args.timeout_sec
        while any(process.poll() is None for process, _ in processes):
            if time.monotonic() >= deadline:
                terminate(processes)
                raise RuntimeError("multi-host clock synchronization timed out")
            time.sleep(0.2)
        failed_nodes = [index for index, (process, _) in enumerate(processes)
                        if process.returncode != 0]
        for _, log_file in processes:
            log_file.close()
        if failed_nodes:
            raise RuntimeError("worker failed on node indexes {}".format(failed_nodes))

        for node_index, host in enumerate(hosts):
            node_dir = output_dir / "node{}".format(node_index)
            node_dir.mkdir()
            subprocess.run([
                "scp", "-q", "-r",
                "root@{}:{}/node{}/.".format(host, remote_output, node_index), str(node_dir),
            ], check=True, timeout=args.timeout_sec)
    finally:
        for process, log_file in processes:
            if process.poll() is None:
                process.terminate()
            if not log_file.closed:
                log_file.close()

    rank_results = {}
    provenance = []
    for node_index in range(len(hosts)):
        node_dir = output_dir / "node{}".format(node_index)
        provenance_path = node_dir / "node{}_provenance.txt".format(node_index)
        provenance.append(provenance_path.read_text(encoding="utf-8"))
        for result_path in node_dir.glob("clock_rank*.json"):
            result = json.loads(result_path.read_text(encoding="utf-8"))
            rank = int(result["rank"])
            if rank in rank_results:
                raise RuntimeError("duplicate result for rank {}".format(rank))
            rank_results[rank] = result
            log_text = (node_dir / "rank{}.log".format(rank)).read_text(
                encoding="utf-8", errors="replace")
            if "PASS clock sync e2e" not in log_text:
                raise RuntimeError("rank {} is missing its PASS marker".format(rank))

    expected_ranks = set(range(rank_size))
    if set(rank_results) != expected_ranks:
        missing = sorted(expected_ranks - set(rank_results))
        extra = sorted(set(rank_results) - expected_ranks)
        raise RuntimeError("rank result mismatch: missing={} extra={}".format(missing, extra))
    for rank, result in rank_results.items():
        if int(result["rank_size"]) != rank_size or int(result["rounds"]) != args.rounds:
            raise RuntimeError("rank {} reported unexpected test dimensions".format(rank))
        if int(result["warmup_samples_per_round"]) != 32 or \
                int(result["measured_samples_per_round"]) != 128:
            raise RuntimeError(
                "rank {} did not run the required 32 warmup and 128 measured samples".format(rank))
        if len(result["last_rank_bounds_ns"]) != rank_size:
            raise RuntimeError("rank {} reported incomplete clock bounds".format(rank))

    precision_tuples = {
        (int(result["precision_p50_ns"]), int(result["precision_p95_ns"]),
         int(result["precision_max_ns"]))
        for result in rank_results.values()
    }
    if len(precision_tuples) != 1:
        raise RuntimeError("ranks reported inconsistent communication-domain precision")
    precision_p50, precision_p95, precision_max = precision_tuples.pop()
    if args.max_precision_ns is not None:
        if args.max_precision_ns <= 0:
            raise ValueError("maximum precision must be positive")
        if max(precision_p50, precision_p95, precision_max) >= args.max_precision_ns:
            raise RuntimeError(
                "precision ceiling failed: p50={} p95={} max={} ns, required < {} ns".format(
                    precision_p50, precision_p95, precision_max, args.max_precision_ns))
    artifact_names = ["test_tilexr_clock_sync_e2e", "libtilexr-diagnostics.so", "libtile-comm.so",
                      "tilexr_diagnostics_op.o"]
    artifact_hashes = {}
    for artifact_name in artifact_names:
        hashes = []
        for text in provenance:
            hash_lines = [line for line in text.splitlines() if line.endswith("/" + artifact_name)]
            if len(hash_lines) != 1:
                raise RuntimeError("missing {} hash in node provenance".format(artifact_name))
            hashes.append(hash_lines[0].split()[0])
        if len(set(hashes)) != 1:
            raise RuntimeError("nodes executed different {} artifacts".format(artifact_name))
        artifact_hashes[artifact_name] = hashes[0]

    summary = {
        "schema": "tilexr_clock_sync_multihost_e2e.v1",
        "host_count": len(hosts),
        "hosts": hosts,
        "rank_size": rank_size,
        "ranks_per_host": args.ranks_per_host,
        "rounds": args.rounds,
        "warmup_samples_per_round": int(rank_results[0]["warmup_samples_per_round"]),
        "measured_samples_per_round": int(rank_results[0]["measured_samples_per_round"]),
        "passed_ranks": len(rank_results),
        "soc_name": args.soc_name,
        "binary_sha256": artifact_hashes["test_tilexr_clock_sync_e2e"],
        "diagnostics_library_sha256": artifact_hashes["libtilexr-diagnostics.so"],
        "comm_library_sha256": artifact_hashes["libtile-comm.so"],
        "diagnostics_kernel_sha256": artifact_hashes["tilexr_diagnostics_op.o"],
        "precision_p50_ns": precision_p50,
        "precision_p95_ns": precision_p95,
        "precision_max_ns": precision_max,
        "required_max_precision_ns": args.max_precision_ns,
        "required_idle_devices_per_host": args.require_idle_devices,
        "precision_definition": "maximum accepted per-rank minimum RTT divided by two",
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("PASS clock sync multihost: {}/{} ranks, p50={} ns p95={} ns max={} ns".format(
        len(rank_results), rank_size, precision_p50, precision_p95, precision_max))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("ERROR: {}".format(error), file=sys.stderr)
        sys.exit(1)
