from __future__ import annotations

import threading
from dataclasses import replace

import pytest
from fastapi.testclient import TestClient

from tools.npu_scheduler.app import create_app
from tools.npu_scheduler.config import HostConfig, SchedulerConfig
from tools.npu_scheduler.models import Submission
from tools.npu_scheduler.scheduler import Scheduler
from tools.npu_scheduler.store import ConflictError, JobStore
from tools.npu_scheduler.transport import Observation, RunResult, parse_npu_smi


class FakeTransport:
    def __init__(self):
        self.statuses: dict[str, str] = {}
        self.runs: list[str] = []
        self.gates: dict[str, threading.Event] = {}
        self.result = RunResult(True, 0)

    def probe_many(self, hosts):
        return {
            host.name: Observation(
                host.name,
                self.statuses.get(host.name, "idle"),
                1.0,
                host.device_count,
                tuple(range(host.device_count)),
                (),
            )
            for host in hosts
            if self.statuses.get(host.name) != "missing"
        }

    def run(self, host, script_path, script_content, working_directory, log):
        self.runs.append(host.name)
        log.append(b"hello\n")
        gate = self.gates.get(host.name)
        if gate:
            gate.wait(2)
        return self.result


class SequencedTransport(FakeTransport):
    def __init__(self, sequences):
        super().__init__()
        self.sequences = list(sequences)

    def probe_many(self, hosts):
        status = self.sequences.pop(0)
        return {
            host.name: Observation(
                host.name,
                status,
                1.0,
                host.device_count,
                tuple(range(host.device_count)),
                (),
            )
            for host in hosts
        }


class PartialTransport(FakeTransport):
    def probe_many(self, hosts):
        return super().probe_many(hosts[:-1])


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


@pytest.fixture
def config(tmp_path):
    hosts = tuple(
        HostConfig(f"h{i}", f"10.0.0.{i}", "root", "pool", 8) for i in range(1, 17)
    )
    return SchedulerConfig(hosts, tmp_path, poll_interval_seconds=0.02)


def submission(request_id="r1", hosts=("h1", "h2", "h3", "h4"), execution="h1"):
    return Submission(
        request_id, request_id, execution, tuple(hosts), script_content="echo ok"
    )


def post_run_job(store, config, status, exit_code=None):
    job, _ = store.submit(submission().validate(config.host_map()))
    assert store.reserve(job["id"], job["reserved_hosts"])
    assert store.transition(
        job["id"], ("RESERVED",), "STARTING", launch_intent=True
    )
    assert store.transition(job["id"], ("STARTING",), "RUNNING")
    if status == "CHECKING":
        assert store.transition(
            job["id"], ("RUNNING",), status, exit_code=exit_code
        )
    else:
        assert status == "UNKNOWN"
        assert store.transition(
            job["id"],
            ("RUNNING",),
            status,
            "ssh_outcome_unknown",
            "connection lost",
        )
    return job


def test_parser_requires_every_device_and_detects_processes():
    idle = "| NPU Chip | Process id | Process name |\n" + "\n".join(
        f"| No running processes found in NPU {i} |" for i in range(8)
    )
    assert parse_npu_smi(idle, "h", 8).status == "idle"
    busy = idle.replace(
        "| No running processes found in NPU 3 |", "| 3 0 | 123 | python |"
    )
    observed = parse_npu_smi(busy, "h", 8)
    assert observed.status == "external_busy" and observed.processes[0].pid == 123
    assert parse_npu_smi(idle.replace("NPU 7", "NPU 6"), "h", 8).status == "unknown"


def test_store_idempotency_and_conflict(config):
    store = JobStore(config.state_dir / "db")
    value = submission().validate(config.host_map())
    first, created = store.submit(value)
    second, retry_created = store.submit(value)
    assert created and not retry_created and first["id"] == second["id"]
    with pytest.raises(ConflictError):
        store.submit(replace(value, name="changed"))


def test_disjoint_jobs_start_and_128p_reserves_atomically(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    scheduler = Scheduler(config, store, transport)
    gate1, gate5 = threading.Event(), threading.Event()
    transport.gates.update({"h1": gate1, "h5": gate5})
    job1, _ = store.submit(
        submission("a", ("h1", "h2", "h3", "h4"), "h1").validate(config.host_map())
    )
    job2, _ = store.submit(
        submission("b", ("h5", "h6", "h7", "h8"), "h5").validate(config.host_map())
    )
    scheduler.tick()
    assert store.get(job1["id"])["status"] == "RUNNING"
    assert store.get(job2["id"])["status"] == "RUNNING"
    gate1.set()
    gate5.set()

    config2 = replace(config, state_dir=config.state_dir / "other")
    store2 = JobStore(config2.state_dir / "db")
    scheduler2 = Scheduler(config2, store2, transport)
    _large, _ = store2.submit(
        submission("128", tuple(f"h{i}" for i in range(1, 17)), "h1").validate(
            config2.host_map()
        )
    )
    transport.gates["h1"] = threading.Event()
    scheduler2.tick()
    assert len(store2.reservations()) == 16
    transport.gates["h1"].set()


def test_external_busy_waits_and_earlier_overlap_blocks(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    scheduler = Scheduler(config, store, transport)
    transport.statuses["h1"] = "external_busy"
    first, _ = store.submit(
        submission("first", ("h1", "h2"), "h1").validate(config.host_map())
    )
    later, _ = store.submit(
        submission("later", ("h2", "h3"), "h2").validate(config.host_map())
    )
    independent, _ = store.submit(
        submission("free", ("h5",), "h5").validate(config.host_map())
    )
    transport.gates["h5"] = threading.Event()
    scheduler.tick()
    assert store.get(first["id"])["status"] == "QUEUED"
    assert store.get(later["id"])["reason_code"] == "hosts_reserved"
    assert store.get(independent["id"])["status"] == "RUNNING"
    transport.gates["h5"].set()


def test_failed_final_probe_releases_without_launch(config):
    store = JobStore(config.state_dir / "db")
    transport = SequencedTransport(("idle", "external_busy"))
    scheduler = Scheduler(config, store, transport)
    job, _ = store.submit(submission().validate(config.host_map()))
    scheduler.tick()
    current = store.get(job["id"])
    assert current["status"] == "QUEUED"
    assert current["reason_code"] == "final_probe_failed"
    assert not store.reservations() and not transport.runs


def test_missing_probe_result_never_starts_or_releases(config):
    store, transport = JobStore(config.state_dir / "db"), PartialTransport()
    scheduler = Scheduler(config, store, transport)
    job, _ = store.submit(submission().validate(config.host_map()))
    scheduler.tick()
    assert store.get(job["id"])["status"] == "QUEUED"
    assert not store.reservations() and not transport.runs


def test_unknown_transport_result_keeps_reservation(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    transport.result = RunResult(False, None, "connection lost")
    scheduler = Scheduler(config, store, transport)
    job, _ = store.submit(submission().validate(config.host_map()))
    scheduler.tick()
    for _ in range(100):
        if store.get(job["id"])["status"] == "UNKNOWN":
            break
        threading.Event().wait(0.01)
    assert store.get(job["id"])["status"] == "UNKNOWN"
    assert len(store.reservations()) == 4


def test_unknown_job_auto_releases_after_one_minute_idle(config):
    config = replace(config, idle_release_seconds=60.0)
    store, transport, clock = (
        JobStore(config.state_dir / "db"),
        FakeTransport(),
        FakeClock(),
    )
    scheduler = Scheduler(config, store, transport, clock=clock)
    job = post_run_job(store, config, "UNKNOWN")

    scheduler.poll()
    clock.advance(59.0)
    scheduler.poll()
    assert store.get(job["id"])["status"] == "UNKNOWN"
    assert len(store.reservations()) == 4

    clock.advance(1.0)
    scheduler.poll()
    current = store.get(job["id"])
    assert current["status"] == "FAILED"
    assert current["reason_code"] == "auto_released_after_idle"
    assert not store.reservations()


@pytest.mark.parametrize("interrupted_status", ("external_busy", "unknown", "missing"))
def test_idle_release_timer_resets_when_any_host_is_not_idle(
    config, interrupted_status
):
    config = replace(config, idle_release_seconds=60.0)
    store, transport, clock = (
        JobStore(config.state_dir / "db"),
        FakeTransport(),
        FakeClock(),
    )
    scheduler = Scheduler(config, store, transport, clock=clock)
    job = post_run_job(store, config, "UNKNOWN")

    scheduler.poll()
    clock.advance(59.0)
    transport.statuses["h4"] = interrupted_status
    scheduler.poll()
    transport.statuses.clear()
    scheduler.poll()
    clock.advance(59.0)
    scheduler.poll()
    assert store.get(job["id"])["status"] == "UNKNOWN"
    assert len(store.reservations()) == 4

    clock.advance(1.0)
    scheduler.poll()
    assert store.get(job["id"])["status"] == "FAILED"
    assert not store.reservations()


@pytest.mark.parametrize(
    ("exit_code", "expected_status", "expected_reason"),
    ((0, "SUCCEEDED", None), (7, "FAILED", "script_failed")),
)
def test_known_exit_auto_releases_after_one_minute_idle(
    config, exit_code, expected_status, expected_reason
):
    config = replace(config, idle_release_seconds=60.0)
    store, transport, clock = (
        JobStore(config.state_dir / "db"),
        FakeTransport(),
        FakeClock(),
    )
    scheduler = Scheduler(config, store, transport, clock=clock)
    job = post_run_job(store, config, "CHECKING", exit_code)

    scheduler.poll()
    clock.advance(60.0)
    scheduler.poll()

    current = store.get(job["id"])
    assert current["status"] == expected_status
    assert current["reason_code"] == expected_reason
    assert current["exit_code"] == exit_code
    assert not store.reservations()


def test_completed_run_waits_for_full_idle_window(config):
    config = replace(config, idle_release_seconds=60.0)
    store, transport, clock = (
        JobStore(config.state_dir / "db"),
        FakeTransport(),
        FakeClock(),
    )
    scheduler = Scheduler(config, store, transport, clock=clock)
    job, _ = store.submit(submission().validate(config.host_map()))

    scheduler.tick()
    for _ in range(100):
        if store.get(job["id"])["status"] not in ("STARTING", "RUNNING"):
            break
        threading.Event().wait(0.01)

    assert store.get(job["id"])["status"] == "CHECKING"
    assert len(store.reservations()) == 4
    scheduler.poll()
    clock.advance(60.0)
    scheduler.poll()
    assert store.get(job["id"])["status"] == "SUCCEEDED"
    assert not store.reservations()


@pytest.mark.parametrize(
    ("exit_code", "expected_status"), ((0, "SUCCEEDED"), (7, "FAILED"))
)
def test_restart_preserves_known_exit_and_restarts_idle_timer(
    config, exit_code, expected_status
):
    config = replace(config, idle_release_seconds=60.0)
    store, transport, first_clock = (
        JobStore(config.state_dir / "db"),
        FakeTransport(),
        FakeClock(),
    )
    job = post_run_job(store, config, "CHECKING", exit_code)
    first_scheduler = Scheduler(config, store, transport, clock=first_clock)
    first_scheduler.poll()
    first_clock.advance(59.0)
    first_scheduler.poll()

    store.recover()
    assert store.get(job["id"])["status"] == "CHECKING"

    restarted_clock = FakeClock()
    restarted_scheduler = Scheduler(config, store, transport, clock=restarted_clock)
    restarted_scheduler.poll()
    restarted_clock.advance(59.0)
    restarted_scheduler.poll()
    assert store.get(job["id"])["status"] == "CHECKING"
    assert len(store.reservations()) == 4
    restarted_clock.advance(1.0)
    restarted_scheduler.poll()
    assert store.get(job["id"])["status"] == expected_status
    assert not store.reservations()


def test_service_stop_does_not_discard_known_exit(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    job = post_run_job(store, config, "CHECKING", 0)
    scheduler = Scheduler(config, store, transport)
    scheduler._running.add(job["id"])

    scheduler.stop()

    current = store.get(job["id"])
    assert current["status"] == "CHECKING"
    assert current["exit_code"] == 0
    assert len(store.reservations()) == 4


def test_execution_exception_becomes_unknown(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    transport.run = lambda *args: (_ for _ in ()).throw(OSError("log disk failed"))
    scheduler = Scheduler(config, store, transport)
    job, _ = store.submit(submission().validate(config.host_map()))
    scheduler.tick()
    for _ in range(100):
        if store.get(job["id"])["status"] == "UNKNOWN":
            break
        threading.Event().wait(0.01)
    current = store.get(job["id"])
    assert current["status"] == "UNKNOWN"
    assert current["reason_code"] == "execution_internal_error"


def test_restart_and_manual_release_are_conservative(config):
    store, transport = JobStore(config.state_dir / "db"), FakeTransport()
    job, _ = store.submit(submission().validate(config.host_map()))
    assert store.reserve(job["id"], job["reserved_hosts"])
    store.transition(job["id"], ("RESERVED",), "STARTING", launch_intent=True)
    store.recover()
    assert store.get(job["id"])["status"] == "UNKNOWN"
    scheduler = Scheduler(config, store, transport)
    transport.statuses["h1"] = "external_busy"
    with pytest.raises(ConflictError):
        scheduler.release_unknown(job["id"], True, "checked")
    transport.statuses.clear()
    scheduler.release_unknown(job["id"], True, "checked remote processes")
    assert store.get(job["id"])["status"] == "FAILED"
    assert not store.reservations()


def test_api_help_submit_retry_logs_and_cancel(config):
    transport = FakeTransport()
    app = create_app(config, transport, start_scheduler=False)
    with TestClient(app) as client:
        help_text = client.get("/help").text
        assert "no authentication" in help_text and "C01-B03" in help_text
        payload = {
            "request_id": "api-1",
            "name": "job",
            "execution_host": "h1",
            "reserved_hosts": ["h1"],
            "script_content": "echo ok",
        }
        response = client.post("/api/v1/jobs", json=payload)
        assert response.status_code == 201 and response.json()["npu_count"] == 8
        job_id = response.json()["id"]
        assert client.post("/api/v1/jobs", json=payload).status_code == 200
        assert (
            client.post("/api/v1/jobs", json={**payload, "name": "other"}).status_code
            == 409
        )
        assert (
            client.get(f"/api/v1/jobs/{job_id}/logs?cursor=0").json()["next_cursor"]
            == 0
        )
        assert (
            client.post(f"/api/v1/jobs/{job_id}/cancel").json()["status"] == "CANCELLED"
        )
        assert client.get("/openapi.json").status_code == 200
        assert client.get("/").status_code == 200


def test_dashboard_uses_vben_operations_shell(config):
    app = create_app(config, FakeTransport(), start_scheduler=False)
    with TestClient(app) as client:
        response = client.get("/")

    assert response.status_code == 200
    for marker in (
        'data-app-shell="tilexr-scheduler"',
        '<html lang="zh-CN" data-theme="light">',
        'id="sidebar"',
        'id="metric-total-npu"',
        'id="resource-filter"',
        'id="job-search"',
        'id="task-dialog"',
    ):
        assert marker in response.text


def test_log_is_closed_while_completed_job_checks_for_idle(config):
    app = create_app(config, FakeTransport(), start_scheduler=False)
    with TestClient(app) as client:
        response = client.post(
            "/api/v1/jobs",
            json={
                "request_id": "checking-log",
                "name": "checking-log",
                "execution_host": "h1",
                "reserved_hosts": ["h1"],
                "script_content": "echo ok",
            },
        )
        job_id = response.json()["id"]
        store = app.state.store
        assert store.reserve(job_id, ("h1",))
        store.transition(job_id, ("RESERVED",), "STARTING", launch_intent=True)
        store.transition(job_id, ("STARTING",), "RUNNING")
        store.transition(job_id, ("RUNNING",), "CHECKING", exit_code=0)

        log = client.get(f"/api/v1/jobs/{job_id}/logs?cursor=0").json()
        assert log["closed"] is True
