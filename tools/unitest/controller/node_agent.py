"""Serve one installed host's owned Workers over a bounded stdin/stdout RPC.

The reader remains live during a blocking rank phase. Channel loss closes only
this agent's Worker/profiler process groups; success keeps outputs for export.
"""
import json
import os
from pathlib import Path
import queue
import selectors
import signal
import socket
import subprocess
import sys
import threading

from . import protocol


class Agent:
    def __init__(self, worker):
        self.worker = worker
        self.group = None
        self.closed = threading.Event()
        self.close_lock = threading.Lock()
        self.output = self.options = self.case = self.report = None
        self.waited = False
        self.analysis_process = None
        self.tools_started = False

    def close(self):
        with self.close_lock:
            if self.closed.is_set():
                return
            self.closed.set()
            if self.analysis_process is not None:
                self._kill_analysis(self.analysis_process)
            if self.group is not None:
                self.group.close()

    @staticmethod
    def _kill_analysis(process):
        # The offline analyzer and its generic-tool children have a dedicated
        # process group. Channel loss never searches or kills unrelated jobs.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def analyze(self, timeout):
        request = self.output / "analysis-request.json"
        result = self.output / "analysis-result.json"
        request.write_text(json.dumps({"responses": self.report["phases"].get("profiler_stop", {}),
            "options": self.options, "case": self.case, "ranks": self.group.ranks, "root": str(self.output), "output": str(self.output / "profiling"),
            "timeout": timeout}), encoding="utf-8")
        # A separate owned group lets channel loss cancel the exporter subtree.
        internal = self.options["profiling"]["tool"] == "internal"
        operation = ("from controller.profiling import analyze; "
            "r=analyze(Path(p['root']),p['case'],p['options'],p['ranks'],p['timeout'],owned_group=False); "
            if internal else "from controller.external_tools import export_msprof; "
            "r=export_msprof(p['responses'],Path(p['output']),p['timeout'],owned_group=False); ")
        code = ("import json,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                "p=json.loads(Path(sys.argv[2]).read_text()); " + operation +
                "Path(sys.argv[3]).write_text(json.dumps(r,allow_nan=False))")
        with (self.output / "analyzer.log").open("wb") as log:
            with self.close_lock:
                if self.closed.is_set():
                    raise ValueError("controller disconnected before analyzer launch")
                process = subprocess.Popen([sys.executable, "-c", code,
                    str(Path(__file__).resolve().parent.parent), str(request), str(result)],
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                self.analysis_process = process
            try:
                try:
                    status = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    from .controller import RunFailure
                    raise RunFailure("timeout", "remote analyzer timed out; see analyzer.log") from error
                if status or not result.is_file():
                    raise ValueError("remote analyzer failed; see " + str(self.output / "analyzer.log"))
                if result.stat().st_size > protocol.MAX_MESSAGE // 2:
                    raise ValueError("remote analyzer invocation records exceed message limit")
                return json.loads(result.read_text(encoding="utf-8"))
            finally:
                self._kill_analysis(process)
                process.wait(timeout=3)
                with self.close_lock:
                    self.analysis_process = None

    def save_report(self):
        if self.output is not None and self.report is not None:
            (self.output / "node-report.json").write_text(
                json.dumps(self.report, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    def dispatch(self, request):
        from .controller import WorkerGroup
        action = request["action"]
        payload = request.get("payload", {})
        if not isinstance(payload, dict):
            raise ValueError("RPC payload must be an object")
        timeout = protocol.duration(payload.get("timeout", 60))
        if action == "start":
            if self.group is not None:
                raise ValueError("NodeAgent already started")
            host, case = payload["host"], payload["case"]
            root = Path(host["output_root"])
            if not root.is_absolute() or not root.is_dir():
                raise ValueError("placement output_root must be an existing absolute directory")
            root = root.resolve(strict=True)
            if self.closed.is_set():
                raise ValueError("controller disconnected before Worker launch")
            output = root / ("unitest-" + request["run_id"])
            output.mkdir(exist_ok=False)
            for name in ("tmp", "cache", "logs"):
                (output / name).mkdir()
            os.environ.update(TMPDIR=str(output / "tmp"), XDG_CACHE_HOME=str(output / "cache"),
                              ASCEND_WORK_PATH=str(output / "logs"), ASCEND_PROCESS_LOG_PATH=str(output / "logs"),
                              ASCEND_CACHE_PATH=str(output / "cache"), PYTHONDONTWRITEBYTECODE="1")
            if host["ranks"][0] == 0:
                address, _ = payload["comm_id"].rsplit(":", 1)
                with socket.socket() as probe:
                    probe.bind((address, 0))
            (output / "case.json").write_text(json.dumps(case, indent=2) + "\n", encoding="utf-8")
            options = payload.get("run_options")
            if options is not None:
                (output / "run.json").write_text(json.dumps(dict(options, schema="tilexr.unitest.run.v1"), indent=2) + "\n", encoding="utf-8")
            report = {"case": case, "phases": {}}
            worker = self.worker or Path(host["framework_prefix"]) / "libexec/tilexr-unitest/tilexr_unitest_worker"
            self.output, self.options, self.case, self.report = output, options, case, report
            self.group = WorkerGroup(Path(worker), host["devices"], output, 0, report,
                                     ranks=host["ranks"], comm_id=payload["comm_id"],
                                     run_options=options)
            if self.closed.is_set():
                self.group.close()
                raise ValueError("controller disconnected during Worker launch")
            return {"ranks": host["ranks"], "output": str(output), "host_identity": {
                "hostname": socket.gethostname(),
                "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text(encoding="ascii").strip()}}
        if self.group is None:
            raise ValueError("NodeAgent has not started")
        if action == "collect":
            return self.group.collect(payload["phase"], timeout)
        if action == "phase":
            return self.group.phase(payload["phase"], timeout)
        if action == "wait":
            self.group.wait(timeout)
            self.waited = True
            return {}
        if action == "tools":
            if not self.waited or "release" not in self.report["phases"]:
                raise ValueError("tools require completed release and Worker exit")
            if self.tools_started:
                raise ValueError("tool collection may only run once")
            self.tools_started = True
            profile = (self.options or {}).get("profiling", {})
            if not profile.get("enabled"):
                return {}
            return {"tool": "tilexr_profiling" if profile["tool"] == "internal" else "msprof", "invocations": self.analyze(timeout),
                    "collection_completeness": "unverified"}
        if action == "close":
            self.close()
            return {}
        raise ValueError("unsupported NodeAgent action")


def main(worker=None):
    agent = Agent(worker)
    incoming = queue.Queue(maxsize=2)

    def reader():
        try:
            # Buffered stdin.readline holds a Python IO lock while blocked;
            # leaving that daemon reader alive makes interpreter shutdown abort.
            # Poll the owned fd so close also stops the reader without EOF.
            buffer = bytearray()
            with selectors.DefaultSelector() as readable:
                readable.register(sys.stdin.fileno(), selectors.EVENT_READ)
                while not agent.closed.is_set():
                    if not readable.select(0.2):
                        continue
                    chunk = os.read(sys.stdin.fileno(), 65536)
                    if not chunk:
                        if buffer:
                            raise ValueError("incomplete NodeAgent request at EOF")
                        break
                    buffer.extend(chunk)
                    if len(buffer) > protocol.MAX_MESSAGE:
                        raise ValueError("NodeAgent request exceeds message limit")
                    while b"\n" in buffer:
                        line, _, rest = buffer.partition(b"\n")
                        buffer[:] = rest
                        # One request may be outstanding; bound queued input.
                        incoming.put_nowait(protocol.decode(bytes(line) + b"\n"))
        except BaseException as error:
            try:
                incoming.put_nowait(error)
            except queue.Full:
                pass
        finally:
            agent.close()
            try:
                incoming.put_nowait(None)
            except queue.Full:
                pass

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()
    run_id, sequence = None, 0
    try:
        while not agent.closed.is_set():
            request = incoming.get()
            if request is None:
                break
            if isinstance(request, BaseException):
                raise request
            if run_id is None:
                run_id = request["run_id"]
            if request["run_id"] != run_id or request["sequence"] != sequence + 1:
                raise ValueError("NodeAgent run or sequence mismatch")
            sequence += 1
            try:
                result = agent.dispatch(request)
                response = protocol.envelope(run_id, sequence, request["action"], status="passed", data=result)
                if request["action"] == "tools":
                    agent.report["tool_result"] = result
            except Exception as error:
                partial = {}
                if agent.group is not None:
                    phase = request.get("payload", {}).get("phase")
                    partial = agent.group.report.get("phases", {}).get(phase, {})
                tool_record = getattr(error, "record", None)
                if request["action"] == "tools" and agent.output is not None:
                    index = agent.output / "profiling/tool-invocations.json"
                    if index.is_file() and index.stat().st_size <= protocol.MAX_MESSAGE // 2:
                        tool_record = {"tool": agent.options["profiling"]["tool"],
                                       "invocations": json.loads(index.read_text(encoding="utf-8"))}
                    agent.report["tool_result"] = tool_record
                response = protocol.envelope(run_id, sequence, request["action"], status="failed", data=partial,
                                             error=str(error), category=getattr(error, "status", "runtime_error"),
                                             tool_record=tool_record)
            agent.save_report()
            sys.stdout.buffer.write(protocol.encode(response))
            sys.stdout.buffer.flush()
            if response["status"] != "passed" or request["action"] == "close":
                break
    finally:
        agent.close()
        reader_thread.join(timeout=1)
    return 0
