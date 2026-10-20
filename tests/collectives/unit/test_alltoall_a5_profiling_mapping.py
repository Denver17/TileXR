import unittest
from pathlib import Path

from tilexr_profiling.mapping import load_mapping

REPO_ROOT = Path(__file__).resolve().parents[3]


MAPPING_PATH = (
    REPO_ROOT
    / "src"
    / "collectives"
    / "kernels"
    / "tilexr_alltoall_a5.mapping.json"
)


class A5AllToAllProfilingMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mapping = load_mapping(MAPPING_PATH)
        cls.stages = {stage.name: stage for stage in cls.mapping.stages}

    def test_mapping_identity_and_critical_stage(self):
        self.assertEqual(self.mapping.operator, "AIV URMA Grouped AllToAll")
        critical = [stage for stage in self.mapping.stages if stage.critical]
        self.assertEqual([stage.name for stage in critical], ["kernel_total"])

    def test_grouped_stages_have_expected_hierarchy_and_tracks(self):
        expected = {
            "credit-wait": ("send_all", "send"),
            "send-put-signal": ("send_all", "send"),
            "send-quiet": ("send_all", "send"),
            "receive-wait": ("receive_loop", "receive"),
            "receive-copy": ("receive_loop", "receive"),
            "sdma-submit": ("receive-copy", "sdma-request"),
            "sdma-prepare": ("sdma-submit", "sdma-submit-detail"),
            "sdma-cache-clean": ("sdma-submit", "sdma-submit-detail"),
            "sdma-dsb": ("sdma-submit", "sdma-submit-detail"),
            "sdma-doorbell": ("sdma-submit", "sdma-submit-detail"),
            "sdma-wait": ("receive-copy", "sdma-request"),
        }
        for name, (parent_name, track) in expected.items():
            with self.subTest(stage=name):
                stage = self.stages[name]
                parent = self.mapping.stages[stage.parent_stage_id]
                self.assertEqual(parent.name, parent_name)
                self.assertEqual(stage.attributes.get("track"), track)

    def test_repeated_stages_cover_all_rank_instances(self):
        repeated = {
            "credit-wait",
            "send-put-signal",
            "send-quiet",
            "receive-wait",
            "receive-copy",
            "sdma-submit",
            "sdma-prepare",
            "sdma-cache-clean",
            "sdma-dsb",
            "sdma-doorbell",
            "sdma-wait",
        }
        for name in repeated:
            with self.subTest(stage=name):
                stage = self.stages[name]
                events = [
                    event
                    for event in self.mapping.events
                    if event.stage_id == stage.id
                ]
                begins = {
                    event.instance_id for event in events if event.edge == "begin"
                }
                ends = {
                    event.instance_id for event in events if event.edge == "end"
                }
                self.assertEqual(begins, set(range(128)))
                self.assertEqual(ends, begins)


if __name__ == "__main__":
    unittest.main()
