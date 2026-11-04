"""Coordinate one case across one owned Worker process per local rank.

Enforce phase deadlines, validate replies and retain complete or partial reports.
Success waits for Worker release; failure terminates only owned process groups
without attempting to drain uncertain NPU work.
"""
import json
import os
import platform
import selectors
import signal
import socket
import subprocess
import time

from .report import write_report
from .config import checkpoints


class RunFailure(RuntimeError):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def validate_checks(case, message):
    checks = message["data"].get("checks")
    outputs = ({"expandXOut", "assistInfoForCombineOut", "sendCountsOut", "expertTokenNumsOut"}
               if case["operator"] == "ep_dispatch" else {"yOut"})
    if case["operator"] == "ep_dispatch_combine":
        outputs = {"combine.yOut"}
    valid = isinstance(checks, list) and len(checks) == len(outputs)
    if valid:
        valid = all(
            isinstance(item, dict) and isinstance(item.get("output"), str)
            and item.get("status") in ("passed", "failed")
            and type(item.get("errors")) is int and item["errors"] >= 0
            and type(item.get("checked_elements")) is int and item["checked_elements"] >= 0
            and (item["status"] == "passed") == (item["errors"] == 0)
            for item in checks)
    if valid:
        valid = ({item["output"] for item in checks} == outputs and
                 (message["status"] == "passed") == all(item["status"] == "passed" for item in checks))
    if not valid:
        raise RunFailure("runtime_error", "invalid validation checks from rank {}".format(message["rank"]))


class WorkerGroup:
    def __init__(self, worker, devices, output, port, report, ranks=None, comm_id=None, run_options=None):
        self.selector = selectors.DefaultSelector()
        self.processes = []
        self.sockets = []
        self.logs = []
        self.buffers = {}
        self.report = report
        self.run_options = run_options or {}
        self.ranks = list(range(len(devices))) if ranks is None else list(ranks)
        if len(self.ranks) != len(devices) or len(set(self.ranks)) != len(devices):
            raise ValueError("Worker global ranks must match local devices uniquely")
        try:
            environment = os.environ.copy()
            environment["TILEXR_COMM_ID"] = comm_id or "127.0.0.1:{}".format(port)
            for rank, device in zip(self.ranks, devices):
                parent, child = socket.socketpair()
                self.sockets.append(parent)
                self.buffers[rank] = bytearray()
                try:
                    log = (output / "rank-{}.log".format(rank)).open("wb")
                    self.logs.append(log)
                    environment.pop("TILEXR_PROFILING_SLOTS_FILE", None)
                    profile = self.run_options.get("profiling", {})
                    if profile.get("enabled") and profile.get("tool") == "internal":
                        from .profiling import prepare_slots
                        environment["TILEXR_PROFILING_SLOTS_FILE"] = prepare_slots(
                            output, rank, report["case"], self.run_options)
                    process = subprocess.Popen(
                        [str(worker), str(rank), str(device), str(child.fileno()), str(output / "case.json")],
                        stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                        pass_fds=(child.fileno(),), env=environment, cwd=output, start_new_session=True)
                finally:
                    child.close()
                self.processes.append(process)
                parent.setblocking(False)
                self.selector.register(parent, selectors.EVENT_READ, rank)
        except BaseException:
            self.close()
            raise

    def collect(self, phase, timeout):
        responses = self.report["phases"][phase] = {}
        deadline = time.monotonic() + timeout
        pending = set(self.ranks)
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RunFailure("timeout", "{} timed out; pending ranks {}".format(phase, sorted(pending)))
            for key, _ in self.selector.select(min(remaining, 0.2)):
                rank = key.data
                chunk = key.fileobj.recv(65536)
                if not chunk:
                    self.selector.unregister(key.fileobj)
                    if rank in pending:
                        raise RunFailure("runtime_error", "rank {} exited during {}; see rank log".format(rank, phase))
                    continue
                buffer = self.buffers[rank]
                buffer.extend(chunk)
                if len(buffer) > 65536:
                    raise RunFailure("runtime_error", "Worker message exceeds 64 KiB")
                while b"\n" in buffer:
                    line, _, rest = buffer.partition(b"\n")
                    buffer[:] = rest
                    try:
                        message = json.loads(line)
                    except (ValueError, UnicodeError) as error:
                        raise RunFailure("runtime_error", "invalid Worker JSON") from error
                    if (not isinstance(message, dict) or
                        message.get("schema") != "tilexr.unitest.worker.v1" or
                        type(message.get("rank")) is not int or message["rank"] != rank or
                        message.get("case_id") != self.report["case"]["id"] or
                        message.get("phase") != phase or rank not in pending or
                        not isinstance(message.get("data"), dict)):
                        raise RunFailure("runtime_error", "unexpected Worker response at rank {}".format(rank))
                    responses[str(rank)] = message
                    pending.remove(rank)
                    status = message.get("status")
                    if status != "passed" and not (phase == "validate" and status == "mismatch"):
                        category = "unsupported" if status == "unsupported" else "runtime_error"
                        raise RunFailure(category, "rank {} {}: {}".format(rank, phase, message["data"].get("message", status)))
                    if phase == "validate":
                        validate_checks(self.report["case"], message)
        return responses

    def phase(self, name, timeout):
        for process in self.processes:
            try:
                process.stdin.write((name + "\n").encode("ascii"))
                process.stdin.flush()
            except (BrokenPipeError, OSError) as error:
                raise RunFailure("runtime_error", "Worker closed before " + name) from error
        return self.collect(name, timeout)

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        for rank, process in zip(self.ranks, self.processes):
            try:
                code = process.wait(timeout=max(0.001, deadline-time.monotonic()))
            except subprocess.TimeoutExpired as error:
                raise RunFailure("timeout", "Worker shutdown timed out") from error
            if code:
                raise RunFailure("runtime_error", "rank {} exited with status {}".format(rank, code))

    def close(self):
        # Kill only process groups created by this controller. Do not touch any
        # pre-existing workload; do not wait for uncertain NPU stream cleanup.
        for process in self.processes:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for process in self.processes:
            if process.stdin:
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        self.selector.close()
        for channel in self.sockets:
            channel.close()
        for log in self.logs:
            log.close()


def run(worker, case, devices, output, port, init_timeout, timeout, exit_timeout,
        run_options, option_sources=None, config_path=None, placement=None):
    report = {"schema": "tilexr.unitest.result.v1", "status": "running", "case": case,
              "devices": devices, "worker": str(worker), "comm_id": "127.0.0.1:{}".format(port),
              "host": platform.node(), "platform": platform.platform(),
              "environment": {key: os.environ.get(key) for key in ("ASCEND_HOME_PATH", "LD_LIBRARY_PATH")},
              "run_options": run_options, "option_sources": option_sources, "config_path": config_path,
              "phases": {}, "checkpoints": [], "last_passed_checkpoint": 0,
              "operator_result": {"status": "not_run"},
              "tool_result": {"status": "pending" if run_options["profiling"]["enabled"] else "disabled"}}
    group = None
    if placement:
        report["comm_id"] = placement["comm_id"]
        report["devices"] = [{"host": host["name"], "rank": rank, "device": device}
                             for host in placement["hosts"] for rank, device in zip(host["ranks"], host["devices"])]
    started = time.monotonic()
    (output / "run.json").write_text(json.dumps(dict(run_options, schema="tilexr.unitest.run.v1"), indent=2) + "\n", encoding="utf-8")
    write_report(output, report)
    try:
        if placement:
            from .cluster import ClusterGroup
            group = ClusterGroup(placement, case, output, report, run_options=run_options)
        else:
            group = WorkerGroup(worker, devices, output, port, report, run_options=run_options)
        init = group.collect("init", init_timeout)
        write_report(output, report)
        group.phase("prepare", timeout)

        def validate(label, completed):
            results = group.phase("validate", timeout)
            passed = all(item["status"] == "passed" for item in results.values())
            report["checkpoints"].append({"label": label, "completed_iterations": completed, "validate": results})
            if passed:
                report["last_passed_checkpoint"] = completed
            else:
                report["failed_checkpoint"] = completed
            write_report(output, report)
            if not passed:
                raise RunFailure("mismatch", "numerical validation failed at " + label)

        external = run_options["profiling"]["enabled"] and run_options["profiling"]["tool"] == "msprof"
        if external and run_options["mode"] != "perf":
            group.phase("profiler_start", timeout)

        if run_options["mode"] == "single":
            group.phase("execute", timeout)
            validate("single", 1)
        elif run_options["mode"] == "correctness":
            previous = 0
            for point in checkpoints(run_options["iterations"], run_options["check_schedule"]):
                report["current_checkpoint"] = point
                group.phase("ready", timeout)
                responses = group.phase("run_segment " + str(point - previous), timeout)
                for rank, response in responses.items():
                    data = response["data"]
                    if data.get("iterations") != point - previous or data.get("completed_iterations") != point:
                        raise RunFailure("runtime_error", "segment count mismatch at rank " + rank)
                validate("checkpoint-" + str(point), point)
                previous = point
                report.pop("current_checkpoint", None)
            report["correctness_scope"] = "Fixed inputs; checkpoint final outputs only, not overwritten rounds or stale-but-equal results."
        else:
            from .metrics import aggregate_host
            group.phase("execute", timeout)
            validate("precheck", 1)
            group.phase("host_warmup", timeout)
            if external:
                group.phase("profiler_start", timeout)
            report["host_batches"] = []
            for index in range(run_options["batches"]):
                group.phase("host_ready", timeout)
                samples = group.phase("host_batch", timeout)
                results = group.phase("validate", timeout)
                report["host_batches"].append({"samples": samples, "validate": results})
                write_report(output, report)
                if any(response["status"] != "passed" for response in results.values()):
                    raise RunFailure("mismatch", "batch validation failed at " + str(index))
            report["measurement"] = aggregate_host(report["host_batches"], init, case,
                                                   run_options["warmup"], run_options["iterations"])
            report["measurement"].update(backend="memory", native_status="public API completed successfully",
                kernel_version="loaded installed kernel; see init provenance")
        report["operator_result"] = {"status": "passed"}
        if external:
            group.phase("profiler_stop", timeout)
        if "measurement" in report:
            report["measurement"].update(profiler_enabled=run_options["profiling"]["enabled"],
                instrumentation=(run_options["profiling"]["tool"] + " runtime collection enabled"
                                 if run_options["profiling"]["enabled"] else
                                 "runtime collection disabled; production build flags are separate"),

                capture_selection=run_options["profiling"]["selection"])
            if external:
                report["measurement"].update(profiler_collection_window=
                    "after warmup through final validation; includes inter-batch preparation and validation",
                    collection_completeness="unverified; official tool interpretation required")
        group.phase("release", exit_timeout)
        group.wait(exit_timeout)
        if external:
            if placement:
                from .external_tools import compare_msprof_hosts
                hosts = group.tools(timeout)
                report["tool_result"]["hosts"] = hosts
                compare_msprof_hosts(hosts, placement)
                report["tool_result"].update(status="export_executed", collection_completeness="unverified")
            else:
                from .external_tools import export_msprof
                report["tool_result"]["invocations_path"] = str(output / "profiling" / "tool-invocations.json")
                records = export_msprof(report["phases"]["profiler_stop"], output / "profiling", timeout)
                report["tool_result"] = {"status": "export_executed", "invocations": records,
                                         "collection_completeness": "unverified"}
        if run_options["profiling"]["enabled"] and run_options["profiling"]["tool"] == "internal":
            if placement:
                hosts = group.tools(timeout)
                report["tool_result"]["hosts"] = hosts
                from .profiling import captures
                expected_magics = [item["magic"] for item in captures(case, run_options)]
                for host in placement["hosts"]:
                    result = hosts.get(host["name"], {})
                    invocations = result.get("invocations", [])
                    if (result.get("tool") != "tilexr_profiling" or
                        [item.get("magic") for item in invocations] != expected_magics or
                        any(item.get("ranks") != host["ranks"] or item.get("exit_code") != 0 for item in invocations)):
                        raise ValueError("incomplete internal profiling host/rank/call set")
                report["tool_result"].update(status="executed", scope="host-local analysis; no cross-host clock alignment")
            else:
                from .profiling import analyze
                records = analyze(output, case, run_options, list(range(len(devices))), timeout)
                report["tool_result"] = dict(status="executed", invocations=records)
        report["status"] = "passed"
    except RunFailure as error:
        report.update(status=error.status, error=str(error))
    except subprocess.TimeoutExpired as error:
        report.update(status="timeout", error="profiling tool timed out: " + str(error))
    except KeyboardInterrupt:
        report.update(status="interrupted", error="controller interrupted")
    except (OSError, ValueError, RuntimeError) as error:
        report.update(status="runtime_error", error=str(error))
    finally:
        if report["status"] != "passed":
            if report["operator_result"]["status"] == "not_run":
                report["operator_result"] = {"status": report["status"]}
            if run_options["profiling"]["enabled"]:
                report["tool_result"].update(status="failed", error=report.get("error"))
        if report["status"] != "passed" and "current_checkpoint" in report:
            report["failed_checkpoint"] = report["current_checkpoint"]
        if group is not None:
            group.close()
        report["elapsed_seconds"] = round(time.monotonic()-started, 3)
        write_report(output, report)
    return report
