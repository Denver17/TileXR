"""Compiled core splits must survive packaging and drive the real result validator."""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import workflow
import run_summary
import test_combine_m5 as m5


class CoreConfigTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tilexr-core-config-")
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.repo = Path(__file__).resolve().parents[2]

    def probe(self, overrides, root=None):
        config = SimpleNamespace(root=root or self.repo, core_overrides=overrides)
        with redirect_stdout(io.StringIO()):
            return workflow.compile_core_config(config, self.work, self.work / "logs", workflow.execute)

    def package(self, cores):
        install = self.work / "install"
        install.mkdir(exist_ok=True)
        manifest = dict(server_dedup_core_config=cores, server_dedup_enabled=True,
            server_dedup_protocol=2, mode="host", checkpoint=None,
            server_dedup_combine_contract=workflow.combine_contract(None, cores["combine_send_core_num"]),
            server_dedup_contract=workflow.DISPATCH_ABI, server_dedup_profiling=False,
            architecture=workflow.platform.machine(), artifacts={})
        workflow.write_json(install / "manifest.json", manifest)
        return SimpleNamespace(install=install, mode="host", checkpoint=None, action="case-dry-run",
            chain=dict(run_mode="functional", dispatch_send_core_num=32, combine_send_core_num=32)), manifest

    def test_compiler_resolves_all_supported_splits_and_header_defaults(self):
        for dispatch, combine in ((16, 56), (24, 24), (32, 32), (40, 16), (48, 40), (24, 48)):
            cores = dict(dispatch_send_core_num=dispatch, combine_send_core_num=combine)
            with self.subTest(cores=cores):
                self.assertEqual(self.probe(cores), cores)
        # Header edits remain supported when build arguments are absent.
        root = self.work / "source"
        for relative in ("src/include", "src/ep/common", "tools/validation"):
            target = root / relative
            target.mkdir(parents=True)
            if relative != "tools/validation":
                for source in (self.repo / relative).glob("*.h"):
                    shutil.copy2(source, target / source.name)
        shutil.copy2(self.repo / "tools/validation/core_config.cpp", root / "tools/validation")
        for filename, macro in (("ep_memory_server_dedup_count_layout.h", workflow.CORE_MACROS["dispatch_send_core_num"]),
                                ("ep_memory_server_dedup_combine_config.h", workflow.CORE_MACROS["combine_send_core_num"])):
            path = root / "src/ep/common" / filename
            path.write_text(path.read_text().replace(f"#define {macro} 32U", f"#define {macro} 24U"))
        self.assertEqual(self.probe({}, root), dict(dispatch_send_core_num=24, combine_send_core_num=24))

    def test_manifest_controls_runtime_and_report_not_environment(self):
        config, manifest = self.package(dict(dispatch_send_core_num=40, combine_send_core_num=24))
        with mock.patch.dict(os.environ, TILEXR_VALIDATION_COMBINE_SEND_CORES="56"):
            workflow.verify_build(config)
        self.assertEqual(config.chain["combine_send_core_num"], 24)
        self.assertEqual(config.chain["dispatch_send_core_num"], 40)
        report = dict(status="PASS", chain_configuration=dict(config.chain, bs=128, loops=100, warmup=50))
        line = run_summary.render_result(report, "summary.json")
        self.assertIn("dispatch_cores(send/recv)=40/24", line)
        self.assertIn("combine_cores(send/recv)=24/40", line)
        manifest["server_dedup_combine_contract"]["send_core_num"] = 32
        workflow.write_json(config.install / "manifest.json", manifest)
        with self.assertRaisesRegex(ValueError, "构建契约"):
            workflow.verify_build(config)

    def test_old_or_invalid_package_cannot_silently_fall_back_to_32(self):
        config, manifest = self.package(dict(dispatch_send_core_num=24, combine_send_core_num=24))
        for cores in (None, {}, dict(dispatch_send_core_num=56, combine_send_core_num=24),
                      dict(dispatch_send_core_num=24, combine_send_core_num=True),
                      dict(dispatch_send_core_num=24, combine_send_core_num="24"),
                      dict(dispatch_send_core_num=24, combine_send_core_num=20)):
            with self.subTest(cores=cores):
                manifest["server_dedup_core_config"] = cores
                workflow.write_json(config.install / "manifest.json", manifest)
                with self.assertRaises(ValueError):
                    workflow.verify_build(config)

    def test_24_core_consumer_passes_real_validation_after_loading_package(self):
        fixture = m5.CombineM5Tests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        config, mappings, records = fixture.m5_records("combine", "performance", 100, 50)
        record = records[0]
        record["combine_control_publish_bytes"] = 26624
        with self.assertRaisesRegex(workflow.Failure, "control_publish_bytes"):
            workflow.validate_combine_return_record(record, mappings[0], config.chain)
        package, _ = self.package(dict(dispatch_send_core_num=24, combine_send_core_num=24))
        package.chain = config.chain
        # The package mode must match the selected performance case.
        workflow.verify_build(package)
        workflow.validate_combine_return_record(record, mappings[0], config.chain)
        record["combine_control_publish_bytes"] = 24576
        with self.assertRaisesRegex(workflow.Failure, "control_publish_bytes"):
            workflow.validate_combine_return_record(record, mappings[0], config.chain)

    def test_cmake_builds_consumer_with_requested_split_and_rejects_invalid_split(self):
        source = self.work / "cmake-source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(CoreConfig CXX)
include("{self.repo}/src/cmake/TileXREpCoreConfig.cmake")
add_executable(probe "{self.repo}/tools/validation/core_config.cpp")
target_include_directories(probe PRIVATE "{self.repo}/src/include" "{self.repo}/src/ep/common")
target_compile_definitions(probe PRIVATE ${{TILEXR_EP_CORE_DEFINITIONS}})
''')
        build = self.work / "cmake-build"
        args = ["cmake", "-S", str(source), "-B", str(build),
                "-DTILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM=24", "-DTILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=40"]
        subprocess.run(args, check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(build)], check=True, capture_output=True)
        self.assertIn('"combine_send_core_num":40', subprocess.check_output([str(build / "probe")], text=True))
        result = subprocess.run(args + ["-DTILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM=56"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
