"""Parse CLI options, resolve cases and start one controlled correctness run.

Reject invalid input before Worker launch; help and case validation need no CANN.
Delegate phase deadlines and owned-process cleanup to the controller.
"""
import argparse
from datetime import datetime, timezone
import math
import json
from pathlib import Path
import socket
import signal
import sys
import uuid

from .case import load_case
from .controller import run
from .report import summary
from .config import resolve


def devices(value):
    try:
        result = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("devices must be comma-separated integers") from error
    if not 2 <= len(result) <= 8 or min(result) < 0 or max(result) > 2**31-1 or len(set(result)) != len(result):
        raise argparse.ArgumentTypeError("select 2..8 distinct nonnegative local device IDs")
    return result


def duration(value):
    result = float(value)
    if not math.isfinite(result) or not 0 < result <= 86400:
        raise argparse.ArgumentTypeError("timeout must be finite and in (0,86400] seconds")
    return result


def port_number(value):
    result = int(value)
    if not 1024 <= result <= 65535:
        raise argparse.ArgumentTypeError("port must be in 1024..65535")
    return result


def main(worker=None, cases=None):
    if sys.argv[1:] == ["--node-agent"]:
        from .node_agent import main as node_main
        return node_main(worker=worker)
    parser = argparse.ArgumentParser(description="Correctness tests for installed TileXR EP Memory operators")
    parser.add_argument("--case", required=True, help="JSON path or installed ep_dispatch / ep_combine example")
    deployment = parser.add_mutually_exclusive_group(required=True)
    deployment.add_argument("--devices", type=devices, help="local device IDs, e.g. 0,1")
    deployment.add_argument("--placement", type=Path, help="placement.v1 JSON for installed remote hosts")
    parser.add_argument("--inventory", type=Path, help="existing tools/tssh/clusters.json inventory")
    parser.add_argument("--cluster", help="cluster name in the maintained inventory")
    parser.add_argument("--comm-id", help="rank-zero data IPv4:port for remote execution")
    parser.add_argument("--output-dir", type=Path, help="new directory for case, report and rank logs")
    parser.add_argument("--comm-port", type=port_number, help="free local TileXR socket port (default: OS-selected)")
    parser.add_argument("--init-timeout", type=duration, default=300)
    parser.add_argument("--timeout", type=duration, default=120, help="per-phase timeout in seconds")
    parser.add_argument("--exit-timeout", type=duration, default=60)
    parser.add_argument("--config", type=Path, help="run.v1 JSON; explicit CLI overrides each field")
    parser.add_argument("--mode", choices=("single", "correctness", "perf"))
    parser.add_argument("--iterations", type=int)
    parser.add_argument("--warmup", type=int)
    parser.add_argument("--batches", type=int)
    parser.add_argument("--timing", choices=("host_batch",))
    parser.add_argument("--check-schedule", choices=("powers_of_two", "every_iteration"))
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--profiling", dest="profiling", action="store_true", default=None)
    profile.add_argument("--no-profiling", dest="profiling", action="store_false")
    parser.add_argument("--profiling-tool", choices=("msprof", "internal"))
    parser.add_argument("--profiling-options-file")
    parser.add_argument("--profiling-selection", type=json.loads, help='JSON: {"kind":"all"} or {"kind":"selected","rounds":[...]}')
    args = parser.parse_args()
    try:
        run_options, option_sources = resolve(args)
        placement = None
        if args.placement:
            if not all((args.inventory, args.cluster, args.comm_id)) or args.comm_port:
                raise ValueError("placement requires inventory, cluster and comm-id; comm-port is local only")
            from .cluster import load_placement
            placement = load_placement(args.inventory, args.cluster, args.placement, args.comm_id)
            # Case world_size is global; physical device IDs remain in placement.
            case_devices = list(range(placement["world_size"]))
        else:
            if any((args.inventory, args.cluster, args.comm_id)):
                raise ValueError("inventory/cluster/comm-id require placement")
            case_devices = args.devices
        path = Path(args.case)
        if cases is not None and args.case in ("ep_dispatch", "ep_combine", "ep_dispatch_combine", "ep_dispatch_barrier"):
            path = Path(cases) / (args.case + ".json")
        case = load_case(path, case_devices)
        if not placement and (worker is None or not Path(worker).is_file()):
            raise ValueError("installed Worker is missing; configure, build and install the framework first")
        # Release the probe immediately before launching TileXR's own listener.
        # A concurrent bind can still race: fail visibly, never retry silently.
        port = 0
        if not placement:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", args.comm_port or 0))
                port = probe.getsockname()[1]
        output = args.output_dir or (Path.cwd() / "unitest-results" /
            (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]))
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        (output / "case.json").write_text(json.dumps(case, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        report = run(Path(worker).resolve() if worker else None, case, case_devices, output, port,
                     args.init_timeout, args.timeout, args.exit_timeout,
                     run_options, option_sources, str(args.config.resolve()) if args.config else None,
                     placement=placement)
    finally:
        signal.signal(signal.SIGTERM, previous)
    summary(output, report)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
