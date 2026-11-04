"""Coordinate installed NodeAgents with one global host-major rank placement.

Inventory owns connection addresses; placement owns explicit deployment paths.
No source, SDK or operator artifacts are copied by this transport.
"""
import ipaddress
import json
import os
from pathlib import Path
import selectors
import shlex
import signal
import subprocess
import time
import uuid

from . import protocol


def _absolute(value, name):
    if not isinstance(value, str) or not value.startswith("/") or "\x00" in value or "\n" in value:
        raise ValueError(name + " must be an absolute POSIX path")
    return value


def load_placement(inventory, cluster, placement, comm_id):
    inventory = json.loads(Path(inventory).read_text(encoding="utf-8"))
    if inventory.get("version") != 1:
        raise ValueError("unsupported maintained inventory version")
    config = inventory.get("clusters", {}).get(cluster)
    if not isinstance(config, dict):
        raise ValueError("unknown inventory cluster")
    known = {host["name"]: host for host in config["hosts"]}
    if len(known) != len(config["hosts"]):
        raise ValueError("duplicate inventory host names")
    source = json.loads(Path(placement).read_text(encoding="utf-8"))
    if source.get("schema") != "tilexr.unitest.placement.v1" or not isinstance(source.get("hosts"), list) or not source["hosts"]:
        raise ValueError("invalid placement schema or empty hosts")
    address, port = comm_id.rsplit(":", 1)
    endpoint = ipaddress.IPv4Address(address)
    if endpoint.is_unspecified or endpoint.is_multicast or not 1024 <= int(port) <= 65535:
        raise ValueError("invalid communicator IPv4 endpoint")
    if len(source["hosts"]) > 1 and endpoint.is_loopback:
        raise ValueError("cross-host communicator endpoint cannot be loopback")
    hosts, names, addresses, local_count = [], set(), set(), None
    rank = 0
    for entry in source["hosts"]:
        name = entry.get("name")
        if name not in known or name in names:
            raise ValueError("unknown or duplicate placement host")
        host = known[name]
        connection = host["address"]
        if not isinstance(connection, str) or not connection or connection.startswith("-") or any(c.isspace() for c in connection):
            raise ValueError("invalid inventory SSH address")
        if connection in addresses:
            raise ValueError("placement has duplicate SSH endpoints")
        # SSH configuration may resolve aliases, ports or proxy hops. Its
        # management endpoint is not the communicator NIC or physical host ID.
        addresses.add(connection)
        names.add(name)
        devices = entry.get("devices")
        if not isinstance(devices, list) or not devices or any(type(d) is not int or not 0 <= d <= 2**31-1 for d in devices) or len(set(devices)) != len(devices):
            raise ValueError("placement devices must be distinct nonnegative integers")
        local_count = local_count or len(devices)
        if len(devices) != local_count:
            raise ValueError("production communicator requires equal positive device counts per host")
        directories = entry.get("runtime_library_dirs")
        if not isinstance(directories, list) or not directories:
            raise ValueError("placement must explicitly specify runtime_library_dirs")
        for directory in directories:
            _absolute(directory, "runtime library directory")
            if "devlib" in Path(directory).parts or ":" in directory:
                raise ValueError("runtime directories must not contain SDK devlib or colon")
        user = config.get("user")
        if user is not None and (not isinstance(user, str) or not user or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in user)):
            raise ValueError("invalid inventory SSH user")
        hosts.append(dict(name=name, address=connection, user=user, devices=devices,
                          ranks=list(range(rank, rank + len(devices))),
                          framework_prefix=_absolute(entry.get("framework_prefix"), "framework_prefix"),
                          environment_script=_absolute(entry.get("environment_script"), "environment_script"),
                          output_root=_absolute(entry.get("output_root"), "output_root"),
                          runtime_library_dirs=directories))
        rank += len(devices)
    if not 2 <= rank <= 128:
        raise ValueError("EP Memory requires 2..128 global ranks (TILEXR_MAX_RANK_SIZE)")
    return dict(hosts=hosts, world_size=rank, comm_id=comm_id)


class ClusterGroup:
    def __init__(self, placement, case, output, report, run_options=None):
        from .controller import RunFailure
        self.failure = RunFailure
        self.selector = selectors.DefaultSelector()
        self.nodes = []
        self.run_id, self.sequence = uuid.uuid4().hex, 0
        self.report = report
        self.closed = False
        self.ranks = {str(rank) for host in placement["hosts"] for rank in host["ranks"]}
        self.placement = placement
        if os.name == "nt":
            raise ValueError("interactive NodeAgent transport requires a POSIX controller; use tssh for Windows one-shot operations")
        try:
            for host in placement["hosts"]:
                log = (output / ("host-" + str(len(self.nodes)) + ".log")).open("wb")
                # Runtime paths are explicit. Never inherit a stale development
                # library path or SDK link stub from the controller's shell.
                script = "set -e; export PYTHONDONTWRITEBYTECODE=1; source {} >&2; export LD_LIBRARY_PATH={}; exec {} --node-agent".format(
                    shlex.quote(host["environment_script"]), shlex.quote(":".join(host["runtime_library_dirs"])),
                    shlex.quote(host["framework_prefix"].rstrip("/") + "/bin/tilexr-unitest"))
                command = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"]
                if host["user"]:
                    command += ["-l", host["user"]]
                command += [host["address"], "bash -c " + shlex.quote(script)]
                try:
                    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                               stderr=log, start_new_session=True)
                except BaseException:
                    log.close()
                    raise
                node = dict(host=host, process=process, log=log, buffer=bytearray())
                self.nodes.append(node)
                os.set_blocking(process.stdin.fileno(), False)
                os.set_blocking(process.stdout.fileno(), False)
                self.selector.register(process.stdout, selectors.EVENT_READ, node)
            started = self._rpc("start", lambda node: dict(host=node["host"], case=case, comm_id=placement["comm_id"],
                                                          run_options=run_options), 60)
            for index, data in started.items():
                if data.get("ranks") != self.nodes[index]["host"]["ranks"]:
                    raise self.failure("runtime_error", "NodeAgent launch rank assignment mismatch")
            host_ids = [data.get("host_identity", {}).get("boot_id") for data in started.values()]
            if any(not isinstance(identity, str) or not identity for identity in host_ids) or len(set(host_ids)) != len(host_ids):
                raise self.failure("runtime_error", "NodeAgents did not identify distinct physical hosts")
            self.report["placement"] = placement
            self.report["remote_host_identity"] = {self.nodes[index]["host"]["name"]: data["host_identity"]
                                                   for index, data in started.items()}
            self.report["remote_outputs"] = {self.nodes[index]["host"]["name"]: data.get("output")
                                             for index, data in started.items()}
        except BaseException:
            self.close()
            raise

    def _rpc(self, action, payload, timeout):
        protocol.duration(timeout)
        if action in ("collect", "phase"):
            # A later batch must not mutate a dict retained as an earlier
            # batch's samples. Keep each attempt's partial replies separately.
            self.report["phases"][payload["phase"]] = {}
        self.sequence += 1
        deadline = time.monotonic() + timeout
        pending = set(range(len(self.nodes)))
        for node in self.nodes:
            body = payload(node) if callable(payload) else dict(payload)
            body["timeout"] = timeout
            request = protocol.envelope(self.run_id, self.sequence, action, payload=body)
            try:
                data = memoryview(protocol.encode(request))
                # A dead or blocked agent must not make even the write side
                # exceed the same whole-operation deadline.
                with selectors.DefaultSelector() as writable:
                    writable.register(node["process"].stdin, selectors.EVENT_WRITE)
                    while data:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise self.failure("timeout", "NodeAgent request write timed out")
                        if not writable.select(min(remaining, 0.2)):
                            continue
                        try:
                            written = os.write(node["process"].stdin.fileno(), data)
                            data = data[written:]
                        except BlockingIOError:
                            continue
            except (OSError, ValueError) as error:
                raise self.failure("runtime_error", "NodeAgent channel closed before " + action) from error
        responses = {}
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise self.failure("timeout", "NodeAgent {} timed out on hosts {}".format(action, sorted(pending)))
            for key, _ in self.selector.select(min(remaining, 0.2)):
                node = key.data
                index = self.nodes.index(node)
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    raise self.failure("runtime_error", "NodeAgent {} disconnected during {}".format(node["host"]["name"], action))
                node["buffer"].extend(chunk)
                if len(node["buffer"]) > protocol.MAX_MESSAGE:
                    raise self.failure("runtime_error", "NodeAgent message exceeds 32 MiB")
                while b"\n" in node["buffer"]:
                    line, _, rest = node["buffer"].partition(b"\n")
                    node["buffer"][:] = rest
                    response = protocol.decode(bytes(line) + b"\n")
                    if index not in pending or response["run_id"] != self.run_id or response["sequence"] != self.sequence or response["action"] != action:
                        raise self.failure("runtime_error", "duplicate, stale or out-of-phase NodeAgent response")
                    if action in ("collect", "phase") and isinstance(response.get("data"), dict):
                        phase = payload.get("phase")
                        expected = {str(rank) for rank in node["host"]["ranks"]}
                        if not set(response["data"]).issubset(expected):
                            raise self.failure("runtime_error", "NodeAgent returned unexpected global ranks")
                        self.report["phases"].setdefault(phase, {}).update(response["data"])
                    if action == "tools" and isinstance(response.get("data"), dict):
                        self.report.setdefault("tool_result", {}).setdefault("hosts", {})[node["host"]["name"]] = response["data"]
                    if response.get("status") != "passed":
                        if response.get("tool_record") is not None:
                            self.report.setdefault("tool_result", {}).setdefault("failures", {})[node["host"]["name"]] = response["tool_record"]
                        raise self.failure(response.get("category", "runtime_error"), "{}: {}".format(node["host"]["name"], response.get("error", "NodeAgent failed")))
                    if not isinstance(response.get("data"), dict):
                        raise self.failure("runtime_error", "invalid NodeAgent response data")
                    responses[index] = response["data"]
                    pending.remove(index)
        return responses

    def _rank_rpc(self, action, payload, timeout):
        replies = self._rpc(action, payload, timeout)
        merged = {}
        for index, data in replies.items():
            expected = {str(rank) for rank in self.nodes[index]["host"]["ranks"]}
            if set(data) != expected or expected.intersection(merged):
                raise self.failure("runtime_error", "NodeAgent rank set is incomplete or duplicated")
            merged.update(data)
        if set(merged) != self.ranks:
            raise self.failure("runtime_error", "global rank gate is incomplete")
        return merged

    def collect(self, phase, timeout):
        result = self._rank_rpc("collect", {"phase": phase}, timeout)
        self.report["phases"][phase] = result
        return result

    def phase(self, name, timeout):
        result = self._rank_rpc("phase", {"phase": name}, timeout)
        self.report["phases"][name] = result
        return result

    def wait(self, timeout):
        self._rpc("wait", {}, timeout)

    def tools(self, timeout):
        # One official export operation may consume every local rank. Preserve
        # its real host ownership instead of inventing per-rank copies.
        return {self.nodes[index]["host"]["name"]: record
                for index, record in self._rpc("tools", {}, timeout).items()}

    def close(self):
        # EOF reaches NodeAgent even while it waits for device work. Remote
        # cleanup owns Workers/profilers; local cleanup owns only these SSH groups.
        if self.closed:
            return
        self.closed = True
        for node in self.nodes:
            try:
                node["process"].stdin.close()
            except (OSError, ValueError):
                pass
        cleanup = self.report.setdefault("cleanup", {}).setdefault("hosts", {})
        deadline = time.monotonic() + 3
        forced = set()
        for index, node in enumerate(self.nodes):
            process = node["process"]
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                forced.add(index)
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + 3
        for index, node in enumerate(self.nodes):
            process = node["process"]
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
            cleanup[node["host"]["name"]] = {
                "ssh_exit_code": process.returncode, "forced_ssh_shutdown": index in forced,
                "remote_owned_process_cleanup": "unverified; EOF requested agent cleanup, no independent remote process check"}
            process.stdout.close()
            node["log"].close()
        self.selector.close()
