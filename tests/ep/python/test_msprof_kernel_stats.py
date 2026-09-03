#!/usr/bin/env python

import csv
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path


TILEXR_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TILEXR_ROOT / "tools" / "ep"))

import msprof_kernel_stats as stats  # noqa: E402


class MsprofKernelStatsTest(unittest.TestCase):
    def test_matches_exact_and_static_hash_kernel_names(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        self.assertTrue(stats.kernel_name_matches(canonical, canonical))
        self.assertTrue(stats.kernel_name_matches(
            "tilexr_ep_dispatch_memory_static_0123456789abcdef", canonical))
        self.assertTrue(stats.kernel_name_matches(
            "tilexr_ep_dispatch_memory_static_ABCDEF0123456789", canonical))

    def test_rejects_unrelated_or_malformed_static_names(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        for actual in (
            "tilexr_ep_dispatch_memory_helper",
            "tilexr_ep_dispatch_memory_static_0123456789abcde",
            "tilexr_ep_dispatch_memory_static_0123456789abcdef0",
            "tilexr_ep_dispatch_memory_static_0123456789abcdeg",
            "tilexr_ep_combine_memory_static_0123456789abcdef",
        ):
            self.assertFalse(stats.kernel_name_matches(actual, canonical), actual)

    def test_extracts_last_50_across_static_hash_variants(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        static_names = (
            "tilexr_ep_dispatch_memory_static_0123456789abcdef",
            "tilexr_ep_dispatch_memory_static_fedcba9876543210",
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "op_summary.csv"
            with path.open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=(
                    "Op Name", "Task Start Time(us)", "Task Duration(us)"))
                writer.writeheader()
                for index in range(60):
                    writer.writerow({
                        "Op Name": canonical if index == 0 else static_names[index % 2],
                        "Task Start Time(us)": index,
                        "Task Duration(us)": index + 1,
                    })
                writer.writerow({
                    "Op Name": "tilexr_ep_dispatch_memory_static_not-a-hash",
                    "Task Start Time(us)": 1000,
                    "Task Duration(us)": 1000,
                })

            series, matched, matched_names = stats.extract_csv_series(path, canonical)

        self.assertEqual(matched, 60)
        self.assertEqual(series, [float(value) for value in range(11, 61)])
        self.assertEqual(set(matched_names), {canonical, *static_names})

    def test_server_summary_averages_last_50_samples_from_all_eight_cards(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for card_index in range(8):
                path = Path(directory) / f"card_{card_index}.csv"
                paths.append(path)
                with path.open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=(
                        "Op Name", "Task Start Time(us)", "Task Duration(us)"))
                    writer.writeheader()
                    for sample_index in range(60):
                        writer.writerow({
                            "Op Name": canonical,
                            "Task Start Time(us)": sample_index,
                            "Task Duration(us)": (
                                10000 if sample_index < 10 else card_index + 1
                            ),
                        })

            result = stats.build_server_samples_result(paths, canonical, "server_0")

        self.assertEqual(result["aggregation"], stats.SERVER_SAMPLES)
        self.assertEqual(result["input_count"], 8)
        self.assertEqual(result["samples_per_input"], 50)
        self.assertEqual(result["total_sample_count"], 400)
        self.assertEqual(result["average_us"], 4.5)

    def test_discovers_sqlite_fallback_and_converts_ns_to_us(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        with tempfile.TemporaryDirectory() as directory:
            profile_root = Path(directory)
            database_dir = (
                profile_root / "PROF_000001_test" / "device_0" / "sqlite"
            )
            database_dir.mkdir(parents=True)
            path = database_dir / "ai_core_op_summary.db"
            connection = sqlite3.connect(path)
            connection.execute(
                "CREATE TABLE ge_summary (task_id INTEGER, stream_id INTEGER, "
                "index_id INTEGER, model_id INTEGER, batch_id INTEGER, op_name TEXT)"
            )
            connection.execute(
                "CREATE TABLE task_time (task_id INTEGER, stream_id INTEGER, "
                "index_id INTEGER, model_id INTEGER, batch_id INTEGER, "
                "start_time REAL, duration_time REAL)"
            )
            for index in range(60):
                key = (index, 1, -1, 4294967295, 0)
                connection.execute(
                    "INSERT INTO ge_summary VALUES (?, ?, ?, ?, ?, ?)",
                    (*key, canonical),
                )
                connection.execute(
                    "INSERT INTO task_time VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (*key, index, (index + 1) * 1000),
                )
            connection.execute(
                "INSERT INTO ge_summary VALUES (60, 1, -1, 4294967295, 0, ?)",
                (canonical,),
            )
            connection.execute(
                "INSERT INTO task_time VALUES "
                "(60, 1, -1, 4294967295, 0, -1, -1)"
            )
            connection.commit()
            connection.close()

            input_paths = stats.expand_inputs([profile_root])
            series, details = stats.series_from_input(input_paths[0], canonical)

        self.assertEqual(input_paths, [path])
        self.assertEqual(series, [float(value) for value in range(11, 61)])
        self.assertEqual(details["source_type"], "ai_core_op_summary_db")
        self.assertEqual(details["matched_samples"], 60)

    def test_cluster_summary_uses_eight_server_averages(self):
        canonical = "tilexr_ep_dispatch_memory_kernel"
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for server_index in range(8):
                path = Path(directory) / f"server_{server_index}.json"
                paths.append(path)
                path.write_text(json.dumps({
                    "schema": stats.SCHEMA,
                    "aggregation": stats.SERVER_SAMPLES,
                    "label": f"server_{server_index}",
                    "kernel": canonical,
                    "input_count": 8,
                    "samples_per_input": 50,
                    "total_sample_count": 400,
                    "average_us": (server_index + 1) * 10,
                }), encoding="utf-8")

            result = stats.build_server_averages_result(
                paths, canonical, "cluster"
            )

        self.assertEqual(result["aggregation"], stats.SERVER_AVERAGES)
        self.assertEqual(result["server_count"], 8)
        self.assertEqual(result["minimum_us"], 10.0)
        self.assertEqual(result["maximum_us"], 80.0)
        self.assertEqual(result["average_us"], 45.0)
        self.assertEqual(result["server_average_us"], [
            10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0,
        ])


if __name__ == "__main__":
    unittest.main()
