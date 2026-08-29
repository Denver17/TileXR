#!/usr/bin/env python3
"""Run one command concurrently on a JSON-configured SSH cluster."""

from __future__ import annotations

import argparse
import getpass
import importlib
import json
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Sequence


DEFAULT_CONFIG = Path(__file__).with_name("clusters.json")
MAX_JOBS = 64
DEFAULT_JOBS = MAX_JOBS
DEFAULT_CONNECT_TIMEOUT = 10
DEFAULT_PUBLIC_KEY = Path.home() / ".ssh" / "id_ed25519.pub"


class ConfigError(ValueError):
    """The cluster inventory is invalid."""


@dataclass(frozen=True)
class Host:
    name: str
    address: str


@dataclass(frozen=True)
class Cluster:
    name: str
    user: str
    hosts: tuple[Host, ...]


@dataclass(frozen=True)
class HostResult:
    host: Host
    returncode: int | None
    stdout: bytes
    stderr: bytes
    elapsed_seconds: float
    timed_out: bool = False
    launch_error: str | None = None

    @property
    def succeeded(self) -> bool:
        return (
            self.returncode == 0
            and not self.timed_out
            and self.launch_error is None
        )


RunProcess = Callable[..., subprocess.CompletedProcess[bytes]]


def _nonempty_string(value: Any, location: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ConfigError(f"{location} must be a non-empty string")
    return value


def _unique_casefolded(values: Sequence[str], location: str) -> None:
    seen: set[str] = set()
    for value in values:
        key = value.casefold()
        if key in seen:
            raise ConfigError(f"duplicate {location}: {value}")
        seen.add(key)


def load_clusters(path: Path) -> tuple[Cluster, ...]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigError(f"cannot read config {path}: {error}") from error

    if not isinstance(document, dict):
        raise ConfigError("config root must be an object")
    if document.get("version") != 1:
        raise ConfigError("config version must be 1")
    raw_clusters = document.get("clusters")
    if not isinstance(raw_clusters, dict) or not raw_clusters:
        raise ConfigError("clusters must be a non-empty object")

    cluster_names = [
        _nonempty_string(name, "cluster name") for name in raw_clusters
    ]
    _unique_casefolded(cluster_names, "cluster name")

    clusters: list[Cluster] = []
    for cluster_name, raw_cluster in raw_clusters.items():
        location = f"cluster {cluster_name!r}"
        if not isinstance(raw_cluster, dict):
            raise ConfigError(f"{location} must be an object")
        user = _nonempty_string(raw_cluster.get("user"), f"{location}.user")
        raw_hosts = raw_cluster.get("hosts")
        if not isinstance(raw_hosts, list) or not raw_hosts:
            raise ConfigError(f"{location}.hosts must be a non-empty array")

        hosts: list[Host] = []
        for index, raw_host in enumerate(raw_hosts):
            host_location = f"{location}.hosts[{index}]"
            if not isinstance(raw_host, dict):
                raise ConfigError(f"{host_location} must be an object")
            hosts.append(
                Host(
                    name=_nonempty_string(
                        raw_host.get("name"), f"{host_location}.name"
                    ),
                    address=_nonempty_string(
                        raw_host.get("address"), f"{host_location}.address"
                    ),
                )
            )
        _unique_casefolded([host.name for host in hosts], f"host name in {cluster_name}")
        clusters.append(Cluster(cluster_name, user, tuple(hosts)))

    return tuple(clusters)


def find_cluster(clusters: Sequence[Cluster], name: str) -> Cluster:
    key = name.casefold()
    for cluster in clusters:
        if cluster.name.casefold() == key:
            return cluster
    available = ", ".join(cluster.name for cluster in clusters)
    raise ConfigError(f"unknown cluster {name!r}; available: {available}")


def remote_command(arguments: Sequence[str]) -> str:
    normalized = command_arguments(arguments)
    if not normalized:
        raise ValueError("remote command is required")
    if len(normalized) == 1:
        return normalized[0]
    return shlex.join(normalized)


def command_arguments(arguments: Sequence[str]) -> list[str]:
    normalized = list(arguments)
    if normalized and normalized[0] == "--":
        normalized.pop(0)
    return normalized


def ssh_command(
    ssh_path: str,
    cluster: Cluster,
    host: Host,
    command: str,
    connect_timeout: int,
    send_input: bool = False,
) -> list[str]:
    arguments = [
        ssh_path,
        "-T",
    ]
    if not send_input:
        arguments.append("-n")
    return arguments + [
        "-o",
        "LogLevel=ERROR",
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=accept-new",
        "-o",
        "UpdateHostKeys=no",
        "-o",
        f"ConnectTimeout={connect_timeout}",
        f"{cluster.user}@{host.address}",
        command,
    ]


def run_host(
    ssh_path: str,
    cluster: Cluster,
    host: Host,
    command: str,
    input_bytes: bytes | None,
    connect_timeout: int,
    timeout: float | None,
    *,
    run_process: RunProcess = subprocess.run,
) -> HostResult:
    started = time.perf_counter()
    process_input = (
        {"stdin": subprocess.DEVNULL}
        if input_bytes is None
        else {"input": input_bytes}
    )
    try:
        completed = run_process(
            ssh_command(
                ssh_path,
                cluster,
                host,
                command,
                connect_timeout,
                send_input=input_bytes is not None,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            **process_input,
        )
        return HostResult(
            host=host,
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
            elapsed_seconds=time.perf_counter() - started,
        )
    except subprocess.TimeoutExpired as error:
        return HostResult(
            host=host,
            returncode=None,
            stdout=error.stdout or b"",
            stderr=error.stderr or b"",
            elapsed_seconds=time.perf_counter() - started,
            timed_out=True,
        )
    except OSError as error:
        return HostResult(
            host=host,
            returncode=None,
            stdout=b"",
            stderr=b"",
            elapsed_seconds=time.perf_counter() - started,
            launch_error=str(error),
        )


def run_cluster(
    ssh_path: str,
    cluster: Cluster,
    command: str,
    input_bytes: bytes | None,
    jobs: int,
    connect_timeout: int,
    timeout: float | None,
    *,
    run_process: RunProcess = subprocess.run,
) -> list[HostResult]:
    worker_count = min(jobs, len(cluster.hosts))
    results: list[HostResult | None] = [None] * len(cluster.hosts)
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        pending = {
            executor.submit(
                run_host,
                ssh_path,
                cluster,
                host,
                command,
                input_bytes,
                connect_timeout,
                timeout,
                run_process=run_process,
            ): index
            for index, host in enumerate(cluster.hosts)
        }
        for future in as_completed(pending):
            results[pending[future]] = future.result()
    return [result for result in results if result is not None]


def load_public_key(path: Path) -> str:
    try:
        key = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ConfigError(f"cannot read public key {path}: {error}") from error
    fields = key.split()
    if "\n" in key or "\r" in key or len(fields) < 2:
        raise ConfigError(f"public key {path} must contain one OpenSSH public key")
    key_type = fields[0]
    security_key_types = {
        "sk-ssh-ed25519@openssh.com",
        "sk-ecdsa-sha2-nistp256@openssh.com",
    }
    if (
        not key_type.startswith(("ssh-", "ecdsa-"))
        and key_type not in security_key_types
    ):
        raise ConfigError(f"public key {path} has an unsupported key type")
    return key


def public_key_install_command(public_key: str) -> str:
    quoted_key = shlex.quote(public_key)
    return (
        "umask 077\n"
        f"key={quoted_key}\n"
        'mkdir -p "$HOME/.ssh" || exit\n'
        'touch "$HOME/.ssh/authorized_keys" || exit\n'
        'chmod 700 "$HOME/.ssh" || exit\n'
        'chmod 600 "$HOME/.ssh/authorized_keys" || exit\n'
        'if grep -qxF -- "$key" "$HOME/.ssh/authorized_keys"; then\n'
        "  printf 'already-present\\n'\n"
        "else\n"
        '  printf \'%s\\n\' "$key" >> "$HOME/.ssh/authorized_keys" && '
        "printf 'installed\\n'\n"
        "fi"
    )


def prepare_known_hosts(
    ssh_path: str,
    cluster: Cluster,
    connect_timeout: int,
    timeout: float | None,
    *,
    run_process: RunProcess = subprocess.run,
) -> dict[str, HostResult]:
    failures: dict[str, HostResult] = {}
    for host in cluster.hosts:
        result = run_host(
            ssh_path,
            cluster,
            host,
            "true",
            None,
            connect_timeout,
            timeout,
            run_process=run_process,
        )
        authentication_only = (
            result.returncode == 255 and b"Permission denied" in result.stderr
        )
        if not result.succeeded and not authentication_only:
            failures[host.address] = result
    return failures


def install_key_host(
    paramiko: Any,
    known_hosts_path: Path,
    cluster: Cluster,
    host: Host,
    public_key: str,
    password: str,
    connect_timeout: int,
    timeout: float | None,
) -> HostResult:
    started = time.perf_counter()
    client = paramiko.SSHClient()
    try:
        client.load_host_keys(str(known_hosts_path))
        client.set_missing_host_key_policy(paramiko.RejectPolicy())
        client.connect(
            hostname=host.address,
            username=cluster.user,
            password=password,
            look_for_keys=False,
            allow_agent=False,
            timeout=connect_timeout,
            auth_timeout=connect_timeout,
            banner_timeout=connect_timeout,
        )
        _, stdout, stderr = client.exec_command(
            public_key_install_command(public_key), timeout=timeout
        )
        output = stdout.read()
        error = stderr.read()
        return HostResult(
            host=host,
            returncode=stdout.channel.recv_exit_status(),
            stdout=output,
            stderr=error,
            elapsed_seconds=time.perf_counter() - started,
        )
    except Exception as error:
        return HostResult(
            host=host,
            returncode=None,
            stdout=b"",
            stderr=b"",
            elapsed_seconds=time.perf_counter() - started,
            launch_error=str(error),
        )
    finally:
        client.close()


def install_key_cluster(
    paramiko: Any,
    ssh_path: str,
    known_hosts_path: Path,
    cluster: Cluster,
    public_key: str,
    password: str,
    jobs: int,
    connect_timeout: int,
    timeout: float | None,
    *,
    run_process: RunProcess = subprocess.run,
) -> list[HostResult]:
    preflight_failures = prepare_known_hosts(
        ssh_path,
        cluster,
        connect_timeout,
        timeout,
        run_process=run_process,
    )
    results: list[HostResult | None] = [None] * len(cluster.hosts)
    worker_count = min(jobs, len(cluster.hosts))
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        pending = {}
        for index, host in enumerate(cluster.hosts):
            if host.address in preflight_failures:
                results[index] = preflight_failures[host.address]
                continue
            future = executor.submit(
                install_key_host,
                paramiko,
                known_hosts_path,
                cluster,
                host,
                public_key,
                password,
                connect_timeout,
                timeout,
            )
            pending[future] = index
        for future in as_completed(pending):
            results[pending[future]] = future.result()
    return [result for result in results if result is not None]


def _write_bytes(stream: Any, data: bytes) -> None:
    if not data:
        return
    text = data.decode("utf-8", errors="replace")
    stream.write(text)
    if not text.endswith("\n"):
        stream.write("\n")


def print_report(results: Sequence[HostResult], stream: Any = sys.stdout) -> None:
    for result in results:
        if result.timed_out:
            status = "timeout"
        elif result.launch_error is not None:
            status = "launch-error"
        else:
            status = f"exit={result.returncode}"
        stream.write(
            f"[{result.host.name} {result.host.address} {status} "
            f"{result.elapsed_seconds:.2f}s]\n"
        )
        _write_bytes(stream, result.stdout)
        if result.stderr:
            stream.write("[stderr]\n")
            _write_bytes(stream, result.stderr)
        if result.launch_error is not None:
            stream.write(f"[error] {result.launch_error}\n")

    succeeded = sum(result.succeeded for result in results)
    stream.write(f"[summary] ok={succeeded} failed={len(results) - succeeded}\n")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def jobs_value(value: str) -> int:
    parsed = positive_int(value)
    if parsed > MAX_JOBS:
        raise argparse.ArgumentTypeError(f"must be at most {MAX_JOBS}")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tssh",
        description="Run one command concurrently on a configured SSH cluster.",
    )
    parser.add_argument("-C", "--cluster", required=True, help="cluster name")
    parser.add_argument(
        "-c", "--config", type=Path, default=DEFAULT_CONFIG, help="inventory JSON"
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=jobs_value,
        default=DEFAULT_JOBS,
        help="parallel jobs (1-64; default: all hosts, capped at 64)",
    )
    parser.add_argument(
        "-I", "--send-input", action="store_true", help="broadcast stdin to every host"
    )
    parser.add_argument(
        "--install-key",
        action="store_true",
        help="install a local public key using one prompted password",
    )
    parser.add_argument(
        "--public-key",
        type=Path,
        default=DEFAULT_PUBLIC_KEY,
        metavar="PATH",
        help="public key for --install-key",
    )
    parser.add_argument(
        "--connect-timeout",
        type=positive_int,
        default=DEFAULT_CONNECT_TIMEOUT,
        metavar="SECONDS",
    )
    parser.add_argument(
        "--timeout",
        type=positive_int,
        default=None,
        metavar="SECONDS",
        help="local timeout per SSH process",
    )
    parser.add_argument("--ssh", default="ssh.exe", metavar="PATH")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="remote command after --")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    options = parser.parse_args(argv)
    try:
        cluster = find_cluster(load_clusters(options.config), options.cluster)
    except ConfigError as error:
        parser.error(str(error))

    if options.install_key:
        if command_arguments(options.command):
            parser.error("--install-key does not accept a remote command")
        if options.send_input:
            parser.error("--install-key cannot be combined with --send-input")
        try:
            public_key = load_public_key(options.public_key)
            paramiko = importlib.import_module("paramiko")
        except ConfigError as error:
            parser.error(str(error))
        except ImportError:
            parser.error(
                "--install-key requires Paramiko; run: "
                "python -m pip install paramiko==5.0.0"
            )
        password = getpass.getpass(f"Password for {cluster.user}@{cluster.name}: ")
        if not password:
            parser.error("password must not be empty")
        results = install_key_cluster(
            paramiko,
            options.ssh,
            Path.home() / ".ssh" / "known_hosts",
            cluster,
            public_key,
            password,
            options.jobs,
            options.connect_timeout,
            options.timeout,
        )
        del password
        print_report(results)
        return 0 if all(result.succeeded for result in results) else 1

    if not options.command:
        parser.error("remote command is required after --")
    try:
        command = remote_command(options.command)
    except ValueError as error:
        parser.error(str(error))

    input_bytes = sys.stdin.buffer.read() if options.send_input else None
    results = run_cluster(
        options.ssh,
        cluster,
        command,
        input_bytes,
        options.jobs,
        options.connect_timeout,
        options.timeout,
    )
    print_report(results)
    return 0 if all(result.succeeded for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
