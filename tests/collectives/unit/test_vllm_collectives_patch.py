#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
INTEGRATION_DIR = ROOT / "integrations" / "vllm_ascend"


def _install_fake_npu_module(monkeypatch, communicator_cls: type) -> None:
    if not hasattr(communicator_cls, "destroy"):
        communicator_cls.destroy = lambda self: None
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

        def destroy(self):
            pass

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
    monkeypatch.setattr(vllm_patch, "bind_group", lambda comm, prefix: FakeAdapter(comm.rank, comm.world_size, prefix))
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
    monkeypatch.setattr(vllm_patch, "bind_group", lambda comm, prefix: FakeAdapter(comm.rank, comm.world_size, prefix))
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


def test_native_error_never_replays_original(monkeypatch):
    import pytest

    class Communicator:
        def __init__(self):
            self.rank, self.world_size = 0, 2
            self.original_calls = 0

        def all_reduce(self, tensor):
            self.original_calls += 1
            return tensor

    class Adapter:
        def all_reduce(self, tensor):
            raise RuntimeError("native submission failed")

    _install_fake_npu_module(monkeypatch, Communicator)
    patch = _import_patch_module(monkeypatch)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")
    monkeypatch.setattr(patch, "bind_group", lambda comm, prefix: Adapter())
    patch.patch_npu_communicator()
    comm = Communicator()
    with pytest.raises(RuntimeError, match="native submission failed"):
        comm.all_reduce("tensor")
    assert comm.original_calls == 0
    assert comm._tilexr_collectives_route_counts['all_reduce'] == dict(tilexr=0, fallback=0, error=1)


def test_group_ownership_and_destroy_order(monkeypatch):
    events = []

    class Communicator:
        def __init__(self):
            self.rank, self.world_size = 0, 2

        def destroy(self):
            events.append("framework")

    class Adapter:
        def close(self):
            events.append("native")

    _install_fake_npu_module(monkeypatch, Communicator)
    patch = _import_patch_module(monkeypatch)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")
    monkeypatch.setattr(patch, "bind_group", lambda comm, prefix: Adapter())
    patch.patch_npu_communicator()
    first, second = Communicator(), Communicator()
    assert first._tilexr_collectives_adapter is not second._tilexr_collectives_adapter
    first.destroy()
    assert events == ["native", "framework"]


def test_init_error_propagates(monkeypatch):
    import pytest

    class Communicator:
        def __init__(self):
            self.rank, self.world_size = 0, 2

    _install_fake_npu_module(monkeypatch, Communicator)
    patch = _import_patch_module(monkeypatch)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")

    def fail(comm, prefix):
        raise RuntimeError("rank 1: missing native library")

    monkeypatch.setattr(patch, "bind_group", fail)
    patch.patch_npu_communicator()
    with pytest.raises(RuntimeError, match="rank 1: missing native library"):
        Communicator()


def test_group_domains_distinct_and_uid_broadcast(monkeypatch):
    monkeypatch.syspath_prepend(str(INTEGRATION_DIR))
    from tilexr_collectives import group_binding
    from itertools import count

    dist = types.SimpleNamespace(
        get_process_group_ranks=lambda group: [3, 5],
        get_rank=lambda group=None: 0 if group else 3,
        get_world_size=lambda group=None: 2 if group else 8,
        all_gather_object=lambda output, value, group: output.__setitem__(slice(None), [value, None]),
        broadcast_object_list=lambda values, src, group: None,
    )
    npu = types.SimpleNamespace(set_device=lambda device: None, device_count=lambda: 8)
    torch = types.ModuleType("torch")
    torch.npu = npu
    torch.distributed = dist
    monkeypatch.setitem(sys.modules, "torch", torch)
    monkeypatch.setitem(sys.modules, "torch.distributed", dist)
    monkeypatch.setattr(group_binding, "_root_sequence", count(1))

    class Runtime:
        def __init__(self, rank, size, prefix, initialize):
            assert initialize is False
        def unique_id(self, domain):
            return domain.to_bytes(128, 'little')
        def initialize(self, uid):
            self.uid = uid

    monkeypatch.setattr(group_binding, "TileXRCollectivesRuntime", Runtime)
    comm = types.SimpleNamespace(cpu_group=object(), device=3)
    first = group_binding.bind_group(comm, "/prefix")
    second = group_binding.bind_group(comm, "/prefix")
    assert first.domain != second.domain
    assert first.runtime is not second.runtime
    assert first.runtime.uid == first.domain.to_bytes(128, 'little')
    assert first.members == (3, 5)
    assert first.rank == 0 and first.world_size == 2 and first.runtime.device_index == 3


def test_readiness_failure_prevents_native_init(monkeypatch):
    import pytest
    monkeypatch.syspath_prepend(str(INTEGRATION_DIR))
    from tilexr_collectives import group_binding
    dist = types.SimpleNamespace(
        get_process_group_ranks=lambda group: [0, 1],
        get_rank=lambda group=None: 0,
        get_world_size=lambda group=None: 2,
        all_gather_object=lambda output, value, group: output.__setitem__(slice(None), [None, "rank 1 missing library"]),
    )
    torch = types.ModuleType("torch")
    torch.npu = types.SimpleNamespace(set_device=lambda device: None)
    torch.distributed = dist
    monkeypatch.setitem(sys.modules, "torch", torch)
    monkeypatch.setitem(sys.modules, "torch.distributed", dist)
    monkeypatch.setattr(group_binding, "TileXRCollectivesRuntime", lambda *a, **kw: types.SimpleNamespace())
    with pytest.raises(RuntimeError, match="rank 1 missing library"):
        group_binding.bind_group(types.SimpleNamespace(cpu_group=object(), device=0), "/prefix")


def test_runtime_unique_id_abi_and_close(monkeypatch):
    import ctypes
    import pytest
    monkeypatch.syspath_prepend(str(INTEGRATION_DIR))
    from tilexr_collectives import runtime as module
    calls = []

    class Function:
        def __init__(self, name):
            self.name = name
        def __call__(self, *args):
            calls.append(self.name)
            if self.name == "TileXRGetUniqueId":
                ctypes.memmove(args[0], b'\x00ID\x00' + bytes(124), 128)
            if self.name == "TileXRCommInitRank":
                assert isinstance(args[0], module.TileXRUniqueId)
                assert bytes(args[0])[:4] == b'\x00ID\x00'
                ctypes.cast(args[-1], ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.c_void_p(123)
            return 0

    library = types.SimpleNamespace(**{name: Function(name) for name in [
        'TileXRGetUniqueId', 'TileXRCommInitRank', 'TileXRCommInitRankLocal', 'TileXRCommDestroy',
        'TileXRAllGather', 'TileXRAllToAll', 'TileXRAllReduce', 'TileXRReduceScatter', 'TileXRBroadcast',
    ]})
    monkeypatch.setattr(module, '_resolve_library', lambda *args: '/fake.so')
    monkeypatch.setattr(module.ctypes, 'CDLL', lambda *args, **kwargs: library)
    rt = module.TileXRCollectivesRuntime(1, 2, '/prefix', initialize=False)
    assert calls == []
    uid = rt.unique_id(7)
    assert len(uid) == 128
    rt.initialize(uid)
    assert rt.comm_ptr == 123
    assert library.TileXRCommInitRank.argtypes[0] is module.TileXRUniqueId
    rt.close()
    rt.close()
    assert calls == ['TileXRGetUniqueId', 'TileXRCommInitRank', 'TileXRCommDestroy']
    with pytest.raises(module.TileXRCollectivesError, match='not initialized'):
        rt.all_reduce(1, 2, 4, module.TILEXR_DATA_TYPE_FP16, 0)
    assert 'TileXRAllReduce' not in calls


def test_destroy_failure_keeps_native_owner(monkeypatch):
    import ctypes
    import pytest
    monkeypatch.syspath_prepend(str(INTEGRATION_DIR))
    from tilexr_collectives.runtime import TileXRCollectivesRuntime, TileXRCollectivesError
    rt = TileXRCollectivesRuntime.__new__(TileXRCollectivesRuntime)
    rt.rank = 0
    rt._comm = ctypes.c_void_p(123)
    rt._closed = False
    rt._comm_lib = types.SimpleNamespace(TileXRCommDestroy=lambda comm: 9)
    with pytest.raises(TileXRCollectivesError):
        rt.close()
    assert rt.comm_ptr == 123 and rt._closed is False
    rt._comm_lib.TileXRCommDestroy = lambda comm: 0
    rt.close()
    assert rt._comm.value is None and rt._closed is True


def test_enabled_bootstrap_failure_stops_startup(monkeypatch):
    import runpy
    import pytest
    patch = _import_patch_module(monkeypatch)
    monkeypatch.setenv("VLLM_ASCEND_TILEXR_COLLECTIVES", "1")

    def fail():
        raise ImportError("framework mismatch")

    monkeypatch.setattr(patch, "patch_npu_communicator", fail)
    with pytest.raises(SystemExit, match="TileXR bootstrap failed.*framework mismatch"):
        runpy.run_path(str(INTEGRATION_DIR / "sitecustomize.py"))
