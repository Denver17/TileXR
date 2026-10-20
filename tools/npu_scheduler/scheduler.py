from __future__ import annotations

import logging
import threading
import time
from collections.abc import Callable

from .config import HostConfig, SchedulerConfig
from .store import ConflictError, JobStore
from .transport import LogFile, Observation, SSHTransport

LOGGER = logging.getLogger(__name__)


class Scheduler:
    def __init__(
        self,
        config: SchedulerConfig,
        store: JobStore,
        transport: SSHTransport | None = None,
        clock: Callable[[], float] | None = None,
    ):
        self.config, self.store = config, store
        self.transport = transport or SSHTransport(config)
        self.hosts = {host.name: host for host in config.hosts}
        self.observations: dict[str, Observation] = {}
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None
        self._running: set[str] = set()
        self._lock = threading.Lock()
        self._stopping = False
        self._clock = clock if clock is not None else time.monotonic
        self._idle_since: dict[str, float] = {}

    def start(self) -> None:
        self.store.recover()
        self._thread = threading.Thread(
            target=self._loop, name="npu-scheduler", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        self._stopping = True
        self._stop.set()
        self._wake.set()
        if self._thread:
            self._thread.join(timeout=10)
        for job_id in tuple(self._running):
            job = self.store.get(job_id)
            if job["status"] in ("STARTING", "RUNNING"):
                self.store.transition(
                    job_id,
                    (str(job["status"]),),
                    "UNKNOWN",
                    "service_stopped",
                    "Service stopped before remote completion was resolved",
                )

    def wake(self) -> None:
        self._wake.set()

    def tick(self) -> None:
        queued = self.store.queued()
        reservations = self.store.reservations()
        earlier_waiting: list[set[str]] = []
        for job in queued:
            requested = set(job["reserved_hosts"])
            blocking_queued = (
                set().union(*(value for value in earlier_waiting)) & requested
                if earlier_waiting
                else set()
            )
            blocking_active = requested & set(reservations)
            if blocking_queued or blocking_active:
                blockers = sorted(blocking_queued | blocking_active)
                self.store.transition(
                    job["id"],
                    ("QUEUED",),
                    "QUEUED",
                    "hosts_reserved",
                    f"Waiting for hosts: {', '.join(blockers)}",
                )
                earlier_waiting.append(requested)
                continue
            hosts = tuple(self.hosts[name] for name in job["reserved_hosts"])
            observations = self.transport.probe_many(hosts)
            self.observations.update(observations)
            unavailable = _unavailable_hosts(hosts, observations)
            if unavailable:
                self.store.transition(
                    job["id"],
                    ("QUEUED",),
                    "QUEUED",
                    "external_or_unknown",
                    f"Hosts not idle: {', '.join(sorted(unavailable))}",
                )
                earlier_waiting.append(requested)
                continue
            if not self.store.reserve(str(job["id"]), job["reserved_hosts"]):
                earlier_waiting.append(requested)
                continue
            reservations.update({name: str(job["id"]) for name in requested})
            final = self.transport.probe_many(hosts)
            self.observations.update(final)
            failed = _unavailable_hosts(hosts, final)
            if failed:
                self.store.transition(
                    job["id"],
                    ("RESERVED",),
                    "QUEUED",
                    "final_probe_failed",
                    f"Final idle check failed: {', '.join(sorted(failed))}",
                    release=True,
                )
                for name in requested:
                    reservations.pop(name, None)
                earlier_waiting.append(requested)
                continue
            if self.store.transition(
                job["id"], ("RESERVED",), "STARTING", launch_intent=True
            ):
                self._start_job(self.store.get(str(job["id"])))

    def poll(self) -> None:
        observations = self.transport.probe_many(tuple(self.hosts.values()))
        self.observations.update(observations)
        self._reconcile_exited_jobs(observations)
        self.tick()

    def _reconcile_exited_jobs(
        self, observations: dict[str, Observation]
    ) -> None:
        reservations = self.store.reservations()
        reserved_job_ids = set(reservations.values())
        if not reserved_job_ids:
            self._idle_since.clear()
            return
        jobs = self.store.list(
            ("CHECKING", "UNKNOWN"), limit=len(reserved_job_ids)
        )
        tracked: set[str] = set()
        now = self._clock()
        for job in jobs:
            job_id = str(job["id"])
            if job_id not in reserved_job_ids:
                continue
            tracked.add(job_id)
            hosts = tuple(self.hosts[name] for name in job["reserved_hosts"])
            if _unavailable_hosts(hosts, observations):
                self._idle_since.pop(job_id, None)
                continue
            idle_since = self._idle_since.setdefault(job_id, now)
            if now - idle_since < self.config.idle_release_seconds:
                continue
            if job["status"] == "CHECKING":
                succeeded = job["exit_code"] == 0
                self.store.transition(
                    job_id,
                    ("CHECKING",),
                    "SUCCEEDED" if succeeded else "FAILED",
                    None if succeeded else "script_failed",
                    exit_code=job["exit_code"],
                    release=True,
                )
            else:
                self.store.transition(
                    job_id,
                    ("UNKNOWN",),
                    "FAILED",
                    "auto_released_after_idle",
                    "Execution outcome remained unknown; reservation automatically "
                    f"released after all hosts were idle for at least "
                    f"{self.config.idle_release_seconds:g} seconds",
                    release=True,
                )
            self._idle_since.pop(job_id, None)
        for job_id in set(self._idle_since) - tracked:
            self._idle_since.pop(job_id, None)

    def resource_snapshot(self, refresh: bool = False) -> list[dict[str, object]]:
        if refresh:
            self.observations.update(
                self.transport.probe_many(tuple(self.hosts.values()))
            )
        reservations = self.store.reservations()
        result = []
        for host in self.config.hosts:
            observation = self.observations.get(host.name)
            observed = observation.status if observation else "unknown"
            result.append(
                {
                    "name": host.name,
                    "address": host.address,
                    "pool": host.pool,
                    "device_count": host.device_count,
                    "npu_count": host.device_count,
                    "reservation_job_id": reservations.get(host.name),
                    "status": "reserved" if host.name in reservations else observed,
                    "observed_status": observed,
                    "observed_at": observation.observed_at if observation else None,
                    "covered_devices": list(observation.covered_devices)
                    if observation
                    else [],
                    "processes": [vars(value) for value in observation.processes]
                    if observation
                    else [],
                    "probe_error": observation.error
                    if observation
                    else "not observed yet",
                }
            )
        return result

    def release_unknown(
        self, job_id: str, confirmed_stopped: bool, reason: str
    ) -> None:
        job = self.store.get(job_id)
        if job["status"] != "UNKNOWN":
            raise ConflictError("only UNKNOWN jobs can be manually released")
        if not confirmed_stopped or not reason.strip():
            raise ValueError(
                "confirmed_stopped=true and a nonempty reason are required"
            )
        hosts = tuple(self.hosts[name] for name in job["reserved_hosts"])
        observations = self.transport.probe_many(hosts)
        self.observations.update(observations)
        if _unavailable_hosts(hosts, observations):
            raise ConflictError("all reserved hosts must be freshly observed idle")
        self.store.transition(
            job_id,
            ("UNKNOWN",),
            "FAILED",
            "manually_released",
            reason.strip(),
            release=True,
        )
        self.wake()

    def _start_job(self, job: dict[str, object]) -> None:
        job_id = str(job["id"])
        with self._lock:
            if job_id in self._running:
                return
            self._running.add(job_id)
        threading.Thread(
            target=self._run_job, args=(job,), name=f"job-{job_id}", daemon=True
        ).start()

    def _run_job(self, job: dict[str, object]) -> None:
        job_id = str(job["id"])
        try:
            self.store.transition(job_id, ("STARTING",), "RUNNING")
            log = LogFile(
                self.config.state_dir / "logs" / f"{job_id}.log",
                self.config.max_log_bytes,
            )
            result = self.transport.run(
                self.hosts[str(job["execution_host"])],
                job.get("script_path"),
                job.get("script_content"),
                job.get("working_directory"),
                log,
            )
            if self._stopping:
                return
            if not result.known:
                self.store.transition(
                    job_id, ("RUNNING",), "UNKNOWN", "ssh_outcome_unknown", result.error
                )
                return
            self.store.transition(
                job_id, ("RUNNING",), "CHECKING", exit_code=result.exit_code
            )
        except Exception as error:  # noqa: BLE001 - execution boundary must preserve reservation
            try:
                current = self.store.get(job_id)
                if current["status"] in ("STARTING", "RUNNING", "CHECKING"):
                    self.store.transition(
                        job_id,
                        (str(current["status"]),),
                        "UNKNOWN",
                        "execution_internal_error",
                        str(error),
                    )
            except Exception:
                LOGGER.exception("could not persist UNKNOWN state for job %s", job_id)
        finally:
            with self._lock:
                self._running.discard(job_id)
            self.wake()

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self.poll()
            except Exception:
                LOGGER.exception("scheduler iteration failed")
            self._wake.wait(self.config.poll_interval_seconds)
            self._wake.clear()


def _unavailable_hosts(
    hosts: tuple[HostConfig, ...], observations: dict[str, Observation]
) -> list[str]:
    return [
        host.name
        for host in hosts
        if host.name not in observations or not observations[host.name].idle
    ]
