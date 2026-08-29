#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
INTEGRATION_DIR = ROOT / "integrations" / "vllm_ascend"


def _install_fake_npu_module(monkeypatch, communicator_cls: type) -> None:
    module_names = [
        "vllm_ascend",
        "vllm_ascend.distributed",
        "vllm_ascend.distributed.device_communicators",
        "vllm_ascend.distributed.device_communicators.npu_communicator",
    ]
    for name in module_names:
        monkeypatch.setitem(sys.modules, name, types.ModuleType(name))

    npu_module = sys.modules["vllm_ascend.distributed.device_communicators.npu_communicator"]
    npu_module.NPUCommunicator = communicator_cls


def _import_patch_module(monkeypatch):
    monkeypatch.syspath_prepend(str(INTEGRATION_DIR))
    from tilexr_collectives import vllm_patch

    return vllm_patch


def test_smoke_launcher_accepts_preflight_sentinel_with_plugin_stdout(tmp_path) -> None:
    fake_python = tmp_path / "fake-python"
    fake_python.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "-c" ]]; then
  if [[ "${2:-}" == *"tilexr_python_preflight_ok"* ]]; then
    echo "INFO plugin stdout before sentinel"
    echo "tilexr_python_preflight_ok"
    exit 0
  fi
  if [[ "${2:-}" == *"sys.executable"* ]]; then
    printf '%s\n' "$0"
    exit 0
  fi
fi
if [[ "${1:-}" == *"smoke_collectives.py" ]]; then
  exit 0
fi
exit 0
""",
        encoding="utf-8",
    )
    fake_python.chmod(0o755)

    launcher = INTEGRATION_DIR / "run_tilexr_collectives_smoke.sh"
    result = subprocess.run(
        [
            "bash",
            str(launcher),
            "2",
            "16",
            "0",
            str(tmp_path / "install"),
            "allgather",
            "int32",
        ],
        env={
            "PATH": "/usr/bin:/bin",
            "TILEXR_VLLM_SMOKE_PYTHON": str(fake_python),
            "TILEXR_VLLM_SMOKE_TIMEOUT_SEC": "5",
        },
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS TileXR vllm collectives smoke" in result.stdout


def test_patch_routes_supported_collectives_to_tilexr_adapter(monkeypatch) -> None:
    calls = []

    class FakeNPUCommunicator:
        def __init__(self):
            self.rank = 1
            self.world_size = 2
            self.fallback_calls = []

        def all_reduce(self, input_):
            self.fallback_calls.append(("all_reduce", input_))
            return ("fallback", input_)

    class FakeAdapter:
        def __init__(self, rank: int, world_size: int, install_prefix: str):
            calls.append(("init", rank, world_size, install_prefix))

        def all_reduce(self, input_):
            calls.append(("all_reduce", input_))
            return ("tilexr", input_)

    _install_fake_npu_module(monkeypatch, FakeNPUCommunicator)
    vllm_patch = _import_patch_module(monkeypatch)
    monkeypatch.setattr(vllm_patch, "TileXRVllmCollectivesAdapter", FakeAdapter)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")

    assert vllm_patch.patch_npu_communicator(install_prefix="/tmp/tilexr-install")

    communicator = FakeNPUCommunicator()

    assert communicator.all_reduce("tensor") == ("tilexr", "tensor")
    assert communicator.fallback_calls == []
    assert calls == [
        ("init", 1, 2, "/tmp/tilexr-install"),
        ("all_reduce", "tensor"),
    ]
    assert communicator._tilexr_collectives_route_counts["all_reduce"]["tilexr"] == 1


def test_patch_falls_back_when_adapter_returns_none(monkeypatch) -> None:
    class FakeNPUCommunicator:
        def __init__(self):
            self.rank = 0
            self.world_size = 2

        def all_gather(self, input_, dim: int = -1):
            return ("fallback-gather", input_, dim)

    class FakeAdapter:
        def __init__(self, rank: int, world_size: int, install_prefix: str):
            self.last_error = RuntimeError("unsupported")

        def all_gather(self, input_, dim: int = -1):
            return None

    _install_fake_npu_module(monkeypatch, FakeNPUCommunicator)
    vllm_patch = _import_patch_module(monkeypatch)
    monkeypatch.setattr(vllm_patch, "TileXRVllmCollectivesAdapter", FakeAdapter)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")

    assert vllm_patch.patch_npu_communicator(install_prefix="/tmp/tilexr-install")

    communicator = FakeNPUCommunicator()

    assert communicator.all_gather("tensor", dim=0) == ("fallback-gather", "tensor", 0)
    assert communicator._tilexr_collectives_route_counts["all_gather"]["fallback"] == 1


def test_patch_is_disabled_without_feature_flag(monkeypatch) -> None:
    class FakeNPUCommunicator:
        def __init__(self):
            self.rank = 0
            self.world_size = 2

        def broadcast(self, tensor, src: int = 0):
            return ("fallback-broadcast", tensor, src)

    _install_fake_npu_module(monkeypatch, FakeNPUCommunicator)
    vllm_patch = _import_patch_module(monkeypatch)
    monkeypatch.delenv("VLLM_ASCEND_TILEXR_COLLECTIVES", raising=False)

    assert not vllm_patch.patch_npu_communicator(install_prefix="/tmp/tilexr-install")
    communicator = FakeNPUCommunicator()

    assert communicator.broadcast("tensor", src=1) == ("fallback-broadcast", "tensor", 1)
    assert not hasattr(communicator, "_tilexr_collectives_adapter")
