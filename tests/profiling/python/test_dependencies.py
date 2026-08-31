import ast
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL_ROOT = REPO_ROOT / "tools" / "profiling"
TEST_ROOT = REPO_ROOT / "tests" / "profiling" / "python"
CONCRETE_PRODUCER_TERMS = (
    "collectives",
    "dispatch",
    "combine",
    "moonep",
    "ep_memory",
    "txdt",
    "txdp",
    "txgp",
)


class ProfilingDependencyTests(unittest.TestCase):
    def test_tool_contains_no_adapter_package_or_concrete_producer_terms(self):
        adapter_sources = list((TOOL_ROOT / "adapters").glob("*.py"))
        self.assertEqual(adapter_sources, [])

        for path in TOOL_ROOT.rglob("*.py"):
            contents = path.read_text(encoding="utf-8").lower()
            for term in CONCRETE_PRODUCER_TERMS:
                self.assertNotIn(term, contents, f"{path} contains producer term {term!r}")

    def test_tool_uses_only_standard_library_and_package_relative_imports(self):
        for path in TOOL_ROOT.rglob("*.py"):
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.ImportFrom) and node.level == 0:
                    module = node.module or ""
                    self.assertFalse(
                        module.startswith(("src", "tools.")),
                        f"{path} imports non-generic module {module!r}",
                    )

    def test_src_does_not_depend_on_tool_package(self):
        source_root = REPO_ROOT / "src"
        for path in source_root.rglob("*"):
            if path.suffix.lower() not in {".c", ".cc", ".cpp", ".h", ".hpp", ".py"}:
                continue
            contents = path.read_text(encoding="utf-8", errors="ignore").lower()
            self.assertNotIn("tools/profiling", contents, str(path))
            self.assertNotIn("tools.profiling", contents, str(path))

    def test_generic_fixtures_use_no_concrete_producer_names(self):
        current = Path(__file__).resolve()
        for path in TEST_ROOT.glob("*.py"):
            if path.resolve() == current:
                continue
            contents = path.read_text(encoding="utf-8").lower()
            for term in CONCRETE_PRODUCER_TERMS:
                self.assertNotIn(term, contents, f"{path} contains producer term {term!r}")


if __name__ == "__main__":
    unittest.main()
