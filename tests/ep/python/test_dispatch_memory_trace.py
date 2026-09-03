#!/usr/bin/env python

import argparse
import copy
import json
import sys
import unittest
from pathlib import Path


TILEXR_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TILEXR_ROOT / "tools" / "ep"))

import dispatch_memory_trace as dispatch_trace  # noqa: E402


trace = dispatch_trace.trace


class DispatchMemoryTraceTest(unittest.TestCase):
    def setUp(self):
        self.provenance = {
            "world_size": 64,
            "bs": 96,
            "h": 7168,
            "k": 6,
            "experts": 384,
            "quant_mode": 0,
            "comm_quant_mode": 0,
            "active_mask_type": 0,
            "expert_token_nums_type": 1,
            "dtype": "bf16",
        }

    def make_profiles(self):
        profiles = {}
        for rank in range(64):
            for core in range(4):
                base = 1000000 + rank * 10000 + core * 100
                points = [base + index for index in range(len(trace.TIME_POINT_NAMES))]
                points[-1] = base + 100 + rank
                diagnostics = {name: 0 for name in trace.DIAGNOSTIC_NAMES}
                diagnostics["alltoall_core_count"] = 2
                diagnostics["send_group_count"] = 2
                core_role = "sender" if core < 2 else "count_cumsum"
                if core_role == "sender":
                    points[4] = points[3]
                record = {
                    "block_dim": 4,
                    "generation": 120,
                    "cycles_per_us": 1000,
                    "points": tuple(points),
                    "metrics": tuple(range(len(trace.METRIC_NAMES))),
                    "diagnostics": diagnostics,
                    "provenance": dict(self.provenance),
                    "core_role": core_role,
                }
                profiles[("run", 99, rank, core)] = record
        return profiles

    def test_selects_fastest_p50_and_slowest_and_builds_chrome_trace(self):
        rank_records, generation = trace.validate_iteration(
            self.make_profiles(), "run", 99, 64, self.provenance)
        selected, rank_order = trace.select_ranks(rank_records)
        self.assertEqual([item[1][1] for item in selected], [0, 31, 63])

        role, (elapsed_us, rank) = selected[-1]
        output = trace.build_trace(
            role, elapsed_us, rank, rank_records[rank], "run", 99,
            generation, rank_order, self.provenance)
        encoded = json.dumps(output)
        self.assertNotIn("displayTimeUnit", encoded)
        event_names = {event.get("name") for event in output["traceEvents"]}
        self.assertNotIn("ep_combine_memory", event_names)
        self.assertNotIn("ep_dispatch_memory", event_names)
        thread_names = [
            event["args"]["name"] for event in output["traceEvents"]
            if event.get("name") == "thread_name"
        ]
        self.assertTrue(any("sender" in name for name in thread_names))
        self.assertTrue(any("count_cumsum" in name for name in thread_names))
        phase_names = [
            event["name"] for event in output["traceEvents"]
            if event.get("cat") == "phase"
        ]
        self.assertEqual(
            phase_names.count("transfer_group0_end -> transfer_end"), 4)
        self.assertEqual(phase_names.count("transfer_end -> cumsum_end"), 2)

    def test_single_group_hides_group1_phase(self):
        profiles = self.make_profiles()
        for record in profiles.values():
            record["diagnostics"]["send_group_count"] = 1
            points = list(record["points"])
            points[2] = points[3]
            record["points"] = tuple(points)
        rank_records, generation = trace.validate_iteration(
            profiles, "run", 99, 64, self.provenance)
        selected, rank_order = trace.select_ranks(rank_records)
        role, (elapsed_us, rank) = selected[0]
        output = trace.build_trace(
            role, elapsed_us, rank, rank_records[rank], "run", 99,
            generation, rank_order, self.provenance)
        phase_names = {
            event["name"] for event in output["traceEvents"]
            if event.get("cat") == "phase"
        }
        self.assertNotIn("transfer_group0_end -> transfer_end", phase_names)

    def test_rejects_core_role_boundary_mismatch(self):
        profiles = self.make_profiles()
        broken = copy.deepcopy(profiles[("run", 99, 0, 0)])
        broken["core_role"] = "count_cumsum"
        profiles[("run", 99, 0, 0)] = broken
        with self.assertRaisesRegex(ValueError, "role boundary mismatch"):
            trace.validate_iteration(profiles, "run", 99, 64, self.provenance)

    def test_rejects_nonzero_sender_cumsum_phase(self):
        profiles = self.make_profiles()
        broken = copy.deepcopy(profiles[("run", 99, 0, 0)])
        points = list(broken["points"])
        points[4] = points[3] + 1
        broken["points"] = tuple(points)
        profiles[("run", 99, 0, 0)] = broken
        with self.assertRaisesRegex(ValueError, "phase 3 is not zero-duration"):
            trace.validate_iteration(profiles, "run", 99, 64, self.provenance)

    def test_rejects_inconsistent_send_group_count(self):
        profiles = self.make_profiles()
        broken = copy.deepcopy(profiles[("run", 99, 0, 1)])
        broken["diagnostics"]["send_group_count"] = 1
        profiles[("run", 99, 0, 1)] = broken
        with self.assertRaisesRegex(ValueError, "inconsistent send group count"):
            trace.validate_iteration(profiles, "run", 99, 64, self.provenance)

    def test_rejects_unsafe_output_prefix(self):
        self.assertEqual(trace.safe_output_component("dispatch.trace-1"),
                         "dispatch.trace-1")
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "letters, digits"):
            trace.safe_output_component("../outside")


if __name__ == "__main__":
    unittest.main()
