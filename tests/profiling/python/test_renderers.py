import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from tools.profiling.analysis import analyze_run
from tools.profiling.html_report import render_analysis_markdown, render_report_html
from tools.profiling.io import atomic_write_json, atomic_write_text
from tools.profiling.model import Diagnostic
from tools.profiling.pairing import load_profile_run
from tools.profiling.perfetto import render_perfetto

from _fixtures import mapping_document, overlapping_child_capture, record_block, stream_id, write_capture


MALICIOUS = "</script><script>alert(1)</script>"
MARKDOWN_LINK = "![x](https://attacker.example/pixel)"


def crossing_slice_pairs(events):
    intervals_by_track = {}
    for event in events:
        if event.get("ph") != "X":
            continue
        intervals_by_track.setdefault((event["pid"], event["tid"]), []).append(event)

    crossings = []
    for track, intervals in intervals_by_track.items():
        ordered = sorted(intervals, key=lambda event: (event["ts"], -event["dur"]))
        for index, first in enumerate(ordered):
            first_end = first["ts"] + first["dur"]
            for second in ordered[index + 1 :]:
                second_end = second["ts"] + second["dur"]
                if first["ts"] < second["ts"] < first_end < second_end:
                    crossings.append((track, first["name"], second["name"]))
    return crossings


class ProfilingRendererTests(unittest.TestCase):
    def load(self, usable=True):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        mapping = mapping_document()
        mapping["operator"] = MALICIOUS
        records = None if usable else [(1000, 1), (1100, 2), (1500, 3)]
        mapping_path, records_path = write_capture(
            root,
            mapping=mapping,
            blocks=[record_block(stream_id(0), records=records)],
        )
        run = load_profile_run(mapping_path, [records_path])
        run = replace(
            run,
            status="incomplete",
            diagnostics=run.diagnostics
            + (
                Diagnostic(
                    "incomplete",
                    "sample_incomplete",
                    MALICIOUS,
                    "sample-source",
                    {"reason": MALICIOUS},
                ),
            ),
            attributes={"note": MALICIOUS, "remote": MARKDOWN_LINK},
        )
        return run, analyze_run(run)

    def test_perfetto_contains_numeric_event_provenance_and_diagnostics(self):
        run, analysis = self.load()
        trace = render_perfetto(run, analysis)
        encoded = json.dumps(trace, allow_nan=False)

        process = next(event for event in trace["traceEvents"] if event.get("name") == "process_name")
        thread = next(event for event in trace["traceEvents"] if event.get("name") == "thread_name")
        total = next(
            event
            for event in trace["traceEvents"]
            if event.get("ph") == "X" and event.get("name") == "kernel_total"
        )
        diagnostic = next(
            event
            for event in trace["traceEvents"]
            if event.get("ph") == "i"
            and event.get("cat") == "diagnostic"
            and "code" in event.get("args", {})
        )
        self.assertIn("launch0/rank0/deviceunknown", process["args"]["name"])
        self.assertIn("stream0", thread["args"]["name"])
        self.assertEqual(total["ts"], 0.0)
        self.assertEqual(total["dur"], 2.0)
        self.assertEqual(total["args"]["begin_event_id"], 1)
        self.assertEqual(total["args"]["end_event_id"], 6)
        self.assertEqual(total["args"]["raw_start"], 1000)
        self.assertEqual(diagnostic["args"]["code"], "sample_incomplete")
        self.assertIn(MALICIOUS, encoded)

    def test_markdown_contains_capture_provenance_and_stage_metrics(self):
        run, analysis = self.load()
        markdown = render_analysis_markdown(run, analysis)

        for heading in (
            "# TileXR Profiling Analysis",
            "## Provenance",
            "## Latency",
            "## Bottlenecks",
            "## Stage Breakdown",
            "## Diagnostics",
        ):
            self.assertIn(heading, markdown)
        for value in (
            r"018f6f5e\-41a0\-7d31\-9ad1\-82d61fc0a001",
            r"AscendC\:\:GetSystemCycle",
            "Streams: expected",
            r"payload\_transfer",
            r"sample\_incomplete",
        ):
            self.assertIn(value, markdown)
        self.assertNotIn(MALICIOUS, markdown)
        self.assertIn(r"&lt;\/script&gt;", markdown)
        self.assertNotIn(MARKDOWN_LINK, markdown)
        self.assertIn(r"\!\[x\]\(https\:\/\/attacker\.example\/pixel\)", markdown)

    def test_html_is_self_contained_and_escapes_metadata(self):
        run, analysis = self.load()
        report = render_report_html(run, analysis)

        self.assertIn("<!doctype html>", report.lower())
        self.assertIn("Profiling Summary", report)
        self.assertIn("Usable streams", report)
        self.assertIn("Rank / Core Comparison", report)
        self.assertIn("Stage Breakdown", report)
        self.assertIn("perfetto_trace.json", report)
        self.assertIn("AscendC::GetSystemCycle", report)
        self.assertNotIn(MALICIOUS, report)
        self.assertIn("&lt;/script&gt;", report)
        self.assertNotIn('src="http', report)
        self.assertNotIn('href="http', report)

    def test_missing_critical_interval_renders_unavailable(self):
        run, analysis = self.load(usable=False)
        markdown = render_analysis_markdown(run, analysis)
        report = render_report_html(run, analysis)

        self.assertIn("unavailable", markdown)
        self.assertIn("unavailable", report)
        self.assertIn("no_usable_critical_interval", markdown)

        trace = render_perfetto(run, analysis)
        unmatched = next(
            event
            for event in trace["traceEvents"]
            if event.get("args", {}).get("code") == "unmatched_begin"
        )
        self.assertEqual(unmatched["args"]["details"]["raw_timestamp"], 1000)

    def test_perfetto_uses_one_thread_per_record_stream(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        mapping_path, records_path = write_capture(
            Path(temp.name),
            blocks=[record_block(stream_id(0, 0), 0), record_block(stream_id(0, 1), 0)],
        )
        run = load_profile_run(mapping_path, [records_path])
        trace = render_perfetto(run, analyze_run(run))

        threads = [event for event in trace["traceEvents"] if event.get("name") == "thread_name"]
        self.assertEqual(len(threads), 2)
        self.assertEqual({event["tid"] for event in threads}, {0, 1})

    def test_perfetto_aligns_same_rank_cores_on_system_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                blocks=[
                    record_block(stream_id(0, 0), records=[(1000, 1), (2000, 6)]),
                    record_block(stream_id(0, 1), records=[(1500, 1), (2500, 6)]),
                ],
            )
            run = load_profile_run(mapping_path, [records_path])
        trace = render_perfetto(run, analyze_run(run))

        kernels = sorted(
            (
                event
                for event in trace["traceEvents"]
                if event.get("name") == "kernel_total"
            ),
            key=lambda event: event["args"]["core"],
        )
        self.assertEqual([event["ts"] for event in kernels], [0.0, 0.5])
        self.assertEqual(
            kernels[1]["args"]["raw_start"] - kernels[0]["args"]["raw_start"],
            500,
        )

    def test_perfetto_preserves_parent_and_overlapping_instance_identity(self):
        mapping, records = overlapping_child_capture()
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                mapping=mapping,
                blocks=[record_block(stream_id(0), records=records)],
            )
            run = load_profile_run(mapping_path, [records_path])
        trace = render_perfetto(run, analyze_run(run))

        children = [
            event
            for event in trace["traceEvents"]
            if event.get("cat") == "copy" and event.get("args", {}).get("instance_id") is not None
        ]
        self.assertEqual(
            [event["name"] for event in children],
            ["payload_transfer[0]", "payload_transfer[1]"],
        )
        self.assertTrue(all(event["args"]["parent_stage_id"] == 0 for event in children))
        self.assertLess(children[1]["ts"], children[0]["ts"] + children[0]["dur"])
        self.assertEqual(len({event["tid"] for event in children}), 2)
        self.assertTrue(all(event["tid"] != 0 for event in children))
        self.assertEqual(crossing_slice_pairs(trace["traceEvents"]), [])

        thread_names = {
            event["tid"]: event["args"]["name"]
            for event in trace["traceEvents"]
            if event.get("name") == "thread_name"
        }
        self.assertEqual(len(thread_names), 3)
        self.assertIn("core0/stream0", thread_names[0])
        self.assertIn("core0/payload_transfer[0]", thread_names[children[0]["tid"]])
        self.assertIn("core0/payload_transfer[1]", thread_names[children[1]["tid"]])

    def test_perfetto_places_serial_stages_on_a_shared_mapping_track(self):
        mapping, records = overlapping_child_capture()
        mapping["stages"][1]["track"] = "send"
        mapping["stages"][2]["track"] = "send"
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                mapping=mapping,
                blocks=[record_block(stream_id(0), records=records)],
            )
            run = load_profile_run(mapping_path, [records_path])
        trace = render_perfetto(run, analyze_run(run))

        grouped = [
            event
            for event in trace["traceEvents"]
            if event.get("name", "").startswith(("peer_wait", "payload_transfer"))
        ]
        self.assertTrue(grouped)
        self.assertEqual(len({event["tid"] for event in grouped}), 1)
        grouped_tid = grouped[0]["tid"]
        thread_name = next(
            event["args"]["name"]
            for event in trace["traceEvents"]
            if event.get("name") == "thread_name" and event["tid"] == grouped_tid
        )
        self.assertIn("core0/send", thread_name)

    def test_atomic_writers_keep_previous_finite_file_on_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            json_path = root / "trace.json"
            text_path = root / "analysis.md"
            atomic_write_json(json_path, {"traceEvents": []})
            atomic_write_text(text_path, "complete\n")
            self.assertEqual(json.loads(json_path.read_text(encoding="utf-8")), {"traceEvents": []})
            self.assertEqual(text_path.read_text(encoding="utf-8"), "complete\n")
            self.assertEqual(list(root.glob(".*.tmp")), [])

            with self.assertRaises(ValueError):
                atomic_write_json(json_path, {"bad": float("nan")})
            self.assertEqual(json.loads(json_path.read_text(encoding="utf-8")), {"traceEvents": []})


if __name__ == "__main__":
    unittest.main()
