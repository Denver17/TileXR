"""Published mapping examples must be accepted by the real v1 parser."""
import json
from pathlib import Path
import re
import unittest

from tilexr_profiling.mapping import load_mapping
from tilexr_profiling.model import ProfilingInputError


REPO_ROOT = Path(__file__).resolve().parents[3]


class DocumentationTests(unittest.TestCase):
    def test_format_reference_examples_parse_with_all_event_forms(self):
        reference = (REPO_ROOT / "tools/profiling/FORMAT.md").read_text(encoding="utf-8")
        examples = re.findall(r"```json\n(.*?)\n```", reference, re.DOTALL)
        self.assertTrue(examples, "format reference must include a concrete mapping")
        for example in examples:
            with self.subTest(example=example):
                mapping = load_mapping(json.loads(example))
                self.assertEqual(len(mapping.stages), 4)
                self.assertEqual(len(mapping.events), 11)
                self.assertEqual(mapping.stages[2].attributes["track"], "work")

    def test_add4_template_requires_explicit_frequency_then_parses(self):
        path = REPO_ROOT / "examples/profiling/profiled_add4.mapping.template.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        with self.assertRaises(ProfilingInputError):
            load_mapping(document)
        document["cycles_per_us"] = 1  # Synthetic test conversion only.
        mapping = load_mapping(document)
        self.assertEqual(mapping.operator, "Add4")
        self.assertEqual(sum(stage.critical for stage in mapping.stages), 1)
        self.assertEqual(len(mapping.events), 14)


if __name__ == "__main__":
    unittest.main()
