from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path

from tools.tssh.tssh import ConfigError, load_clusters

DEFAULT_INVENTORY = Path(__file__).parents[1] / "tssh" / "clusters.json"


@dataclass(frozen=True)
class HostConfig:
    name: str
    address: str
    user: str
    pool: str
    device_count: int


@dataclass(frozen=True)
class SchedulerConfig:
    hosts: tuple[HostConfig, ...]
    state_dir: Path
    bind: str = "0.0.0.0"
    port: int = 12345
    public_url: str | None = None
    poll_interval_seconds: float = 5.0
    idle_release_seconds: float = 60.0
    probe_timeout_seconds: float = 15.0
    connect_timeout_seconds: int = 10
    max_parallel_ssh: int = 16
    max_log_bytes: int = 16 * 1024 * 1024
    ssh_path: str = "ssh"

    def host_map(self) -> dict[str, HostConfig]:
        return {host.name.casefold(): host for host in self.hosts}


def load_config(path: Path | None = None) -> SchedulerConfig:
    raw: dict[str, object] = {}
    if path is not None:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
        if not isinstance(value, dict):
            raise ConfigError("scheduler config root must be an object")
        raw = value

    inventory = Path(str(raw.get("inventory", DEFAULT_INVENTORY))).resolve()
    selected = raw.get("clusters", ["c01-b03", "c01-b08"])
    if (
        not isinstance(selected, list)
        or not selected
        or not all(isinstance(x, str) for x in selected)
    ):
        raise ConfigError("clusters must be a non-empty string array")
    device_count = int(raw.get("devices_per_host", 8))
    if device_count <= 0:
        raise ConfigError("devices_per_host must be positive")

    clusters = {
        cluster.name.casefold(): cluster for cluster in load_clusters(inventory)
    }
    hosts: list[HostConfig] = []
    names: set[str] = set()
    addresses: set[str] = set()
    for requested in selected:
        cluster = clusters.get(requested.casefold())
        if cluster is None:
            raise ConfigError(f"unknown configured cluster {requested!r}")
        for host in cluster.hosts:
            key = host.name.casefold()
            if key in names or host.address.casefold() in addresses:
                raise ConfigError(f"duplicate scheduler host identity: {host.name}")
            names.add(key)
            addresses.add(host.address.casefold())
            hosts.append(
                HostConfig(
                    host.name, host.address, cluster.user, cluster.name, device_count
                )
            )

    state_dir = Path(str(raw.get("state_dir", "./npu-scheduler-state"))).resolve()
    port = int(raw.get("port", 12345))
    if port <= 0 or port > 65535:
        raise ConfigError("port must be in [1, 65535]")
    max_parallel = int(raw.get("max_parallel_ssh", 16))
    if max_parallel <= 0:
        raise ConfigError("max_parallel_ssh must be positive")
    idle_release_seconds = float(raw.get("idle_release_seconds", 60.0))
    if not math.isfinite(idle_release_seconds) or idle_release_seconds <= 0:
        raise ConfigError("idle_release_seconds must be finite and positive")
    return SchedulerConfig(
        hosts=tuple(hosts),
        state_dir=state_dir,
        bind=str(raw.get("bind", "0.0.0.0")),
        port=port,
        public_url=str(raw["public_url"]).rstrip("/")
        if raw.get("public_url")
        else None,
        poll_interval_seconds=float(raw.get("poll_interval_seconds", 5.0)),
        idle_release_seconds=idle_release_seconds,
        probe_timeout_seconds=float(raw.get("probe_timeout_seconds", 15.0)),
        connect_timeout_seconds=int(raw.get("connect_timeout_seconds", 10)),
        max_parallel_ssh=max_parallel,
        max_log_bytes=int(raw.get("max_log_bytes", 16 * 1024 * 1024)),
        ssh_path=str(raw.get("ssh_path", "ssh")),
    )
