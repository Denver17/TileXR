import html
import re
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from tilexr_profiling.analysis import analyze_run
from tilexr_profiling.html_report import render_analysis_markdown, render_report_html
from tilexr_profiling.model import ProfilingInputError
from tilexr_profiling.pairing import load_profile_run
from tilexr_profiling.perfetto import render_perfetto
from tilexr_profiling.provenance import build_provenance, provenance_items, validate_attributes

from _fixtures import CAPTURE_ID, record_block, stream_id, write_capture


class ProvenanceTests(unittest.TestCase):
    def load(self, blocks=None):
        with tempfile.TemporaryDirectory() as tmp:
            mapping, records = write_capture(Path(tmp), blocks=blocks)
            return load_profile_run(mapping, [records])

    def test_three_outputs_share_environment_facts_and_statistical_contract(self):
        run = replace(self.load(), attributes={"sdk_version": "sdk-v7", "kernel_version": "kernel-v9",
                      "soc": "Ascend950", "cann_version": "9.1.0", "driver_version": "25.1.rc1",
                      "backend": "memory", "native_status": "native"})
        analysis = analyze_run(run)
        provenance = render_perfetto(run, analysis)["metadata"]["provenance"]
        self.assertEqual(provenance, build_provenance(run))
        self.assertEqual(provenance["capture_id"], CAPTURE_ID)
        self.assertEqual(provenance["timing"]["cycles_per_us"], 1000.0)
        self.assertEqual(provenance["timing"]["conversion_source"], "mapping.cycles_per_us")
        markdown = render_analysis_markdown(run, analysis)
        plain_markdown = html.unescape(re.sub(r"\\([!\"#$%&'()*+,\-./:;<=>?@\[\]^_`{|}~\\])", r"\1", markdown))
        report = html.unescape(render_report_html(run, analysis))
        for value in run.attributes.values():
            self.assertIn(value, plain_markdown)
            self.assertIn(value, report)
        for name in ("inclusive", "union", "exclusive", "wait_sync_pressure"):
            self.assertIn(name.replace("_", r"\_"), markdown)
            self.assertIn(provenance["statistics"][name], report)

    def test_unknown_hardware_and_provided_input_completeness_boundary(self):
        run = self.load([record_block(stream_id(0))])
        provenance = build_provenance(run)
        self.assertEqual(run.status, "complete")
        self.assertEqual(provenance["reported_environment"]["soc"], "unknown")
        self.assertEqual(provenance["completeness"]["scope"], "provided_inputs_only")
        self.assertIn("unprovided rank files", provenance["completeness"]["boundary"])
        self.assertNotEqual(run.clock_domain, run.display_origin)

    def test_rejects_protocol_fact_override_at_api_and_renderer_boundaries(self):
        for key in ("capture_id", "operator", "cycles_per_us", "clock_domain", "sources", "statistics"):
            with self.subTest(key=key):
                with self.assertRaises(ProfilingInputError):
                    validate_attributes({key: "fake"})
                run = replace(self.load(), attributes={key: "fake"})
                with self.assertRaises(ProfilingInputError):
                    render_perfetto(run, analyze_run(run))

    def test_finite_json_size_depth_and_type_limits(self):
        nested = "leaf"
        for _ in range(10):
            nested = {"nested": nested}
        for value in (float("nan"), float("inf"), object(), "x" * 65537, nested,
                      list(range(5000)), 1 << 300):
            with self.subTest(value_type=type(value)):
                with self.assertRaises(ProfilingInputError):
                    validate_attributes({"input": value})
        with self.assertRaises(ProfilingInputError):
            validate_attributes({"a" * 257: 0})
        self.assertEqual(validate_attributes({"nested": {"value": [None, True, 1, 0.5]}}),
                         {"nested": {"value": [None, True, 1, 0.5]}})

    def test_nested_caller_keys_and_empty_objects_keep_their_structure(self):
        attributes = {"a.b": 1, "a": {"b": 2}, "empty": {}}
        run = replace(self.load(), attributes=attributes)
        rows = dict(provenance_items(build_provenance(run)))
        self.assertEqual(rows["attributes"], attributes)

    def test_malicious_caller_values_remain_inert_in_all_formats(self):
        value = '</td><script src="https://evil.example/x"></script> ![x](https://evil.example/pixel)'
        run = replace(self.load(), attributes={"kernel_version": value})
        analysis = analyze_run(run)
        self.assertEqual(render_perfetto(run, analysis)["metadata"]["provenance"]["attributes"]["kernel_version"], value)
        markdown = render_analysis_markdown(run, analysis)
        report = render_report_html(run, analysis)
        self.assertNotIn("<script", markdown)
        self.assertNotIn("<script", report)
        self.assertNotIn("![x](https", markdown)
        self.assertNotIn('src="https', report)


if __name__ == "__main__":
    unittest.main()
