from __future__ import annotations

import json
import threading

import pytest

from tools.npu_scheduler.config import ConfigError, HostConfig, SchedulerConfig, load_config
from tools.npu_scheduler.lock import ServiceLock
from tools.npu_scheduler.transport import (
    LogFile,
    Observation,
    SSHTransport,
    _shell_quote,
)


def test_checked_in_inventory_selects_128p_capacity(tmp_path):
    config_path = tmp_path / "config.json"
    config_path.write_text(
        json.dumps({"state_dir": str(tmp_path / "state")}), encoding="utf-8"
    )
    config = load_config(config_path)
    assert len(config.hosts) == 16
    assert sum(host.device_count for host in config.hosts) == 128
    assert config.hosts[0].name == "C01-B03-1"
    assert config.hosts[-1].name == "C01-B08-8"
    assert all(host.user == "root" for host in config.hosts)
    assert config.idle_release_seconds == 60.0


@pytest.mark.parametrize("value", (0, "nan", "inf"))
def test_idle_release_seconds_must_be_finite_and_positive(tmp_path, value):
    config_path = tmp_path / "config.json"
    config_path.write_text(
        json.dumps(
            {
                "state_dir": str(tmp_path / "state"),
                "idle_release_seconds": value,
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ConfigError, match="idle_release_seconds"):
        load_config(config_path)


def test_service_lock_rejects_second_owner(tmp_path):
    first, second = ServiceLock(tmp_path / "lock"), ServiceLock(tmp_path / "lock")
    first.acquire()
    try:
        with pytest.raises(RuntimeError):
            second.acquire()
    finally:
        first.release()
    second.acquire()
    second.release()


def test_log_file_is_bounded_and_incremental(tmp_path):
    log = LogFile(tmp_path / "job.log", 6)
    log.append(b"abc")
    log.append(b"defghi")
    first, cursor, truncated = log.read(0, 2)
    second, cursor, _ = log.read(cursor, 20)
    assert first == "ab" and second == "cdef"
    assert cursor == 6 and truncated
    reopened = LogFile(tmp_path / "job.log", 6)
    assert reopened.read(0, 20)[2]


def test_shell_quote_preserves_single_quotes():
    assert _shell_quote("a'b") == "'a'\"'\"'b'"


def test_real_bash_wrapper_handles_output_without_newline(tmp_path):
    host = HostConfig("local", "unused", "root", "pool", 1)
    config = SchedulerConfig((host,), tmp_path)

    class LocalTransport(SSHTransport):
        def _command(self, host, remote):
            return ["bash", "-lc", remote]

    transport = LocalTransport(config)
    log = LogFile(tmp_path / "local.log", 1024)
    result = transport.run(host, None, "printf done; exit 7", None, log)
    assert result.known and result.exit_code == 7
    assert log.read(0, 1024)[0] == "done\n"
    invalid = transport.run(
        host, None, "exit 0", "/does/not/exist", LogFile(tmp_path / "bad.log", 1024)
    )
    assert invalid.known and invalid.exit_code == 125


def test_real_bash_wrapper_isolates_user_script_stdin(tmp_path):
    host = HostConfig("local", "unused", "root", "pool", 1)
    config = SchedulerConfig((host,), tmp_path)

    class LocalTransport(SSHTransport):
        def _command(self, host, remote):
            return ["bash", "-lc", remote]

    transport = LocalTransport(config)
    log = LogFile(tmp_path / "stdin.log", 1024)
    result = transport.run(
        host, None, "cat >/dev/null; printf stdin-isolated", None, log
    )

    assert result.known and result.exit_code == 0
    assert log.read(0, 1024)[0] == "stdin-isolated\n"


def test_concurrent_probes_for_one_host_are_coalesced(tmp_path):
    host = HostConfig("local", "unused", "root", "pool", 1)
    config = SchedulerConfig((host,), tmp_path, max_parallel_ssh=1)
    started, release = threading.Event(), threading.Event()

    class CountingTransport(SSHTransport):
        calls = 0

        def probe(self, selected):
            self.calls += 1
            started.set()
            release.wait(2)
            return Observation(selected.name, "idle", 1.0, 1, (0,), ())

    transport = CountingTransport(config)
    results = []
    first = threading.Thread(
        target=lambda: results.append(transport.probe_many((host,)))
    )
    second = threading.Thread(
        target=lambda: results.append(transport.probe_many((host,)))
    )
    first.start()
    started.wait(1)
    second.start()
    release.set()
    first.join()
    second.join()
    assert transport.calls == 1 and len(results) == 2
