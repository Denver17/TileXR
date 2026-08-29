from __future__ import annotations

import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "scripts" / "run_moonep_meta_gate.py"


def _load_gate_module():
    assert MODULE_PATH.is_file(), "MoonEP meta cascade gate is not implemented"
    spec = importlib.util.spec_from_file_location("run_moonep_meta_gate", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_gate_matrix_covers_both_shapes_and_group_modes() -> None:
    gate = _load_gate_module()

    assert [
        (case.tokens_per_rank, case.topk, case.hidden_size, case.peer_mode)
        for case in gate.CASES
    ] == [
        (4096, 8, 7168, "group"),
        (4096, 8, 7168, "group_credit"),
        (8192, 16, 3584, "group"),
        (8192, 16, 3584, "group_credit"),
    ]


def test_replay_command_is_self_contained_and_keeps_required_profiling() -> None:
    gate = _load_gate_module()
    case = gate.CASES[0]
    command = gate.build_replay_command(
        ROOT,
        case,
        ROOT / "run" / "gate-case",
        ROOT / "run" / "gate-cache",
    )

    assert command == [
        "bash",
        str(ROOT / "scripts" / "run_moonep.sh"),
        "--mode",
        "benchmark",
        "--rank-size",
        "8",
        "--visible-devices",
        "0,1,2,3,4,5,6,7",
        "--warmup",
        "0",
        "--iterations",
        "1",
        "--model-replay",
        "--model-replay-from",
        "meta",
        "--model-replay-cache-dir",
        str(ROOT / "run" / "gate-cache"),
        "--model-replay-profile",
        "--model-replay-stage-summary-only",
        "--s",
        "4096",
        "--k",
        "8",
        "--hidden-size",
        "7168",
        "--ep",
        "8",
    ]


def test_gate_runs_all_cases_and_returns_failure_when_any_case_fails() -> None:
    gate = _load_gate_module()
    seen = []
    output = io.StringIO()

    def invoke(case, _command, environment):
        assert environment["TILEXR_MODEL_REPLAY_BASH"] == "/bin/false"
        seen.append((case.label, environment["TILEXR_MOONEP_DISPATCH_PEER_MODE"]))
        return 9 if case.peer_mode == "group_credit" and case.tokens_per_rank == 4096 else 0

    results = gate.run_case_matrix(
        ROOT,
        ROOT / "run" / "gate",
        ROOT / "run" / "cache",
        {"PATH": "test-path"},
        invoke=invoke,
        output=output,
    )

    assert len(seen) == 4
    assert [result.returncode for result in results] == [0, 9, 0, 0]
    rendered = gate.format_result_table(results)
    assert "S=4096 K=8 H=7168" in rendered
    assert "group_credit" in rendered
    assert "FAIL(9)" in rendered
    assert gate.gate_exit_code(results) == 1


def test_gate_returns_success_only_when_every_case_passes() -> None:
    gate = _load_gate_module()

    results = gate.run_case_matrix(
        ROOT,
        ROOT / "run" / "gate",
        ROOT / "run" / "cache",
        {},
        invoke=lambda _case, _command, _environment: 0,
        output=io.StringIO(),
    )

    assert gate.gate_exit_code(results) == 0
    assert gate.format_result_table(results).count("PASS") == 4


def test_gate_uses_only_workspace_local_cann_for_build_and_replay(
    monkeypatch, tmp_path: Path
) -> None:
    gate = _load_gate_module()
    source_root = tmp_path / "TileXR"
    local_cann = source_root / "env" / "cann" / "cann-9.1.0"
    local_cann.mkdir(parents=True)
    (local_cann / "set_env.sh").touch()
    inherited = {
        "ASCEND_HOME_PATH": "/system/cann",
        "TILEXR_CANN_HOME": "/stale/cann",
        "PATH": "inherited",
    }
    loaded = {
        "ASCEND_HOME_PATH": "/system/cann",
        "TILEXR_CANN_HOME": "/stale/cann",
        "PATH": "loaded",
    }

    def load_common_environment(_source_root, sanitized):
        assert "ASCEND_HOME_PATH" not in sanitized
        assert "TILEXR_CANN_HOME" not in sanitized
        return loaded

    monkeypatch.setattr(
        gate,
        "_load_common_environment",
        load_common_environment,
    )

    environment = gate._prepare_gate_environment(
        source_root,
        source_root / "install-moonep-meta-gate",
        inherited,
    )

    assert environment["TILEXR_CANN_HOME"] == str(local_cann)
    assert environment["ASCEND_HOME_PATH"] == str(local_cann)
    assert environment["PATH"] == "loaded"
    assert environment["TILEXR_INSTALL_PREFIX"] == str(
        source_root / "install-moonep-meta-gate"
    )


def test_gate_rejects_system_cann_when_workspace_install_is_missing(
    monkeypatch, tmp_path: Path
) -> None:
    gate = _load_gate_module()
    source_root = tmp_path / "TileXR"
    source_root.mkdir()
    system_cann = tmp_path / "system-cann"
    system_cann.mkdir()
    (system_cann / "set_env.sh").touch()
    monkeypatch.setattr(
        gate,
        "_load_common_environment",
        lambda _source_root, _inherited: {
            "ASCEND_HOME_PATH": str(system_cann),
            "TILEXR_CANN_HOME": str(system_cann.parent),
        },
    )

    with pytest.raises(RuntimeError, match="cann_download_install.sh"):
        gate._prepare_gate_environment(
            source_root,
            source_root / "install-moonep-meta-gate",
            {"ASCEND_HOME_PATH": str(system_cann)},
        )


def test_gate_accepts_external_cann_only_when_passed_explicitly(
    monkeypatch, tmp_path: Path
) -> None:
    gate = _load_gate_module()
    source_root = tmp_path / "TileXR"
    local_cann = source_root / "env" / "cann" / "cann-9.1.0"
    local_cann.mkdir(parents=True)
    (local_cann / "set_env.sh").touch()
    external_cann = tmp_path / "external-cann"
    external_cann.mkdir()
    external_set_env = external_cann / "set_env.sh"
    external_set_env.touch()
    sourced = []
    monkeypatch.setattr(
        gate,
        "_load_common_environment",
        lambda _source_root, _inherited: {
            "ASCEND_HOME_PATH": str(local_cann),
            "PATH": "common-path",
        },
    )

    def load_environment_script(script, environment):
        sourced.append((script, environment["PATH"]))
        return {**environment, "PATH": "external-path"}

    monkeypatch.setattr(
        gate,
        "_load_environment_script",
        load_environment_script,
        raising=False,
    )

    environment = gate._prepare_gate_environment(
        source_root,
        source_root / "install-moonep-meta-gate",
        {"ASCEND_HOME_PATH": "/inherited/cann"},
        cann_path=external_cann,
    )

    assert sourced == [(external_set_env, "common-path")]
    assert environment["ASCEND_HOME_PATH"] == str(external_cann)
    assert environment["TILEXR_CANN_HOME"] == str(external_cann)
    assert environment["PATH"] == "external-path"


def test_gate_rejects_invalid_explicit_cann_path(tmp_path: Path) -> None:
    gate = _load_gate_module()
    invalid_cann = tmp_path / "not-a-toolkit"
    invalid_cann.mkdir()

    with pytest.raises(RuntimeError, match="set_env.sh"):
        gate._prepare_gate_environment(
            tmp_path / "TileXR",
            tmp_path / "install-moonep-meta-gate",
            {},
            cann_path=invalid_cann,
        )


def test_cli_accepts_one_cann_option_and_rejects_skip_build_combination(
    tmp_path: Path,
) -> None:
    gate = _load_gate_module()
    parser = gate._parser()

    arguments = parser.parse_args(["--cann", str(tmp_path)])
    assert arguments.cann == tmp_path
    assert arguments.skip_build is False
    with pytest.raises(SystemExit):
        parser.parse_args(["--cann", str(tmp_path), "--skip-build"])


def test_main_forwards_explicit_cann_path(monkeypatch, tmp_path: Path) -> None:
    gate = _load_gate_module()
    observed = {}

    def prepare_environment(*_args, cann_path=None):
        observed["cann_path"] = cann_path
        raise RuntimeError("stop before build")

    monkeypatch.setattr(gate, "_prepare_gate_environment", prepare_environment)

    assert gate.main(["--cann", str(tmp_path)]) == 1
    assert observed["cann_path"] == tmp_path


def test_main_reports_missing_workspace_cann_without_traceback(
    monkeypatch, capsys
) -> None:
    gate = _load_gate_module()

    def missing_cann(*_args, **_kwargs):
        raise RuntimeError("run 'bash scripts/cann_download_install.sh' first")

    monkeypatch.setattr(gate, "_prepare_gate_environment", missing_cann)

    assert gate.main([]) == 1
    assert "cann_download_install.sh" in capsys.readouterr().err


def test_replay_subprocess_runs_from_tilexr_root(monkeypatch) -> None:
    gate = _load_gate_module()
    observed = {}

    def run(_command, **kwargs):
        observed.update(kwargs)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(gate.subprocess, "run", run)
    returncode = gate._invoke_subprocess(
        gate.CASES[0],
        ["bash", str(ROOT / "scripts" / "run_moonep.sh")],
        {"TILEXR_HOME": str(ROOT)},
    )

    assert returncode == 0
    assert observed["cwd"] == str(ROOT)
