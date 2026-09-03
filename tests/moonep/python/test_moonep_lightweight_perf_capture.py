from __future__ import annotations

import json
from types import SimpleNamespace

from integrations.moonep_torch.tilexr_moonep.torch_api import TileXRMoonEPBuffer
from tools.moonep.model_flow import model_operator_order


class _Event:
    next_index = 0

    def __init__(self, *, enable_timing: bool) -> None:
        assert enable_timing is True
        self.index = _Event.next_index
        _Event.next_index += 1

    def record(self, _stream) -> None:
        return None

    def elapsed_time(self, end) -> float:
        assert end.index == self.index + 1
        return 0.25


def test_lightweight_capture_skips_warmup_and_writes_55_ordered_events(
    tmp_path, monkeypatch
) -> None:
    monkeypatch.setenv("TILEXR_MOONEP_PERF_CAPTURE_DIR", str(tmp_path))
    monkeypatch.setenv("TILEXR_MOONEP_PERF_CAPTURE_ID", "performance-1")
    monkeypatch.setenv("TILEXR_MOONEP_PERF_CAPTURE_SKIP_OPERATORS", "2")
    monkeypatch.setenv("TILEXR_MOONEP_PERF_CAPTURE_OPERATORS", "55")
    buffer = object.__new__(TileXRMoonEPBuffer)
    buffer.context = SimpleNamespace(global_rank=3)
    buffer._torch = SimpleNamespace(
        npu=SimpleNamespace(Event=_Event, current_stream=lambda: object())
    )
    buffer._initialize_perf_capture()

    for stage in ("planning", "dispatch_forward"):
        token = buffer._begin_perf_capture(stage)
        buffer._end_perf_capture(token)
    for item in model_operator_order():
        token = buffer._begin_perf_capture(str(item["stage"]))
        buffer._end_perf_capture(token)
    buffer._write_perf_capture()

    payload = json.loads(
        (tmp_path / "rank3_performance.json").read_text(encoding="utf-8")
    )
    assert payload["capture_id"] == "performance-1"
    assert payload["rank"] == 3
    assert [item["stage"] for item in payload["operators"]] == [
        item["stage"] for item in model_operator_order()
    ]
    assert payload["operators"][0]["source_operator"] == 2
    assert payload["operators"][0]["latency_us"] == 250.0
