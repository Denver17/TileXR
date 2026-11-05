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
import current_run_fixture as m5


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
            server_dedup_combine_contract=workflow.combine_contract(cores["combine_send_core_num"]),
            server_dedup_contract=workflow.DISPATCH_ABI, server_dedup_profiling=False,
            architecture=workflow.platform.machine(), artifacts={})
        workflow.write_json(install / "manifest.json", manifest)
        return SimpleNamespace(install=install, mode="host", checkpoint=None, action="case-dry-run",
            chain=dict(run_mode="functional", dispatch_send_core_num=32, combine_send_core_num=32)), manifest

    def test_compiler_resolves_all_supported_splits_and_header_defaults(self):
        for dispatch, combine in (*((n, n) for n in range(16, 49, 4)), (28, 52), (44, 56)):
            cores = dict(dispatch_send_core_num=dispatch, combine_send_core_num=combine, dispatch_count_core_num=32)
            with self.subTest(cores=cores):
                self.assertEqual(self.probe(cores), cores)
        for count in (1, 16, 31, 48, 64):
            cores = dict(dispatch_send_core_num=32, combine_send_core_num=32, dispatch_count_core_num=count)
            with self.subTest(count=count):
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
        self.assertEqual(self.probe({}, root), dict(dispatch_send_core_num=24, combine_send_core_num=24, dispatch_count_core_num=32))

    def test_manifest_controls_runtime_and_report_not_environment(self):
        config, manifest = self.package(dict(dispatch_send_core_num=44, combine_send_core_num=28, dispatch_count_core_num=16))
        with mock.patch.dict(os.environ, TILEXR_VALIDATION_COMBINE_SEND_CORES="56"):
            workflow.verify_build(config)
        self.assertEqual(config.chain["combine_send_core_num"], 28)
        self.assertEqual(config.chain["dispatch_send_core_num"], 44)
        self.assertEqual(config.chain["dispatch_count_core_num"], 16)
        report = dict(status="PASS", chain_configuration=dict(config.chain, bs=128, loops=100, warmup=50))
        line = run_summary.render_result(report, "summary.json")
        self.assertIn("dispatch_cores(send/recv)=44/20", line)
        self.assertIn("combine_cores(send/recv)=28/36", line)
        self.assertIn("dispatch_count_cores=16(cores=48..63)", line)
        manifest["server_dedup_combine_contract"]["send_core_num"] = 32
        workflow.write_json(config.install / "manifest.json", manifest)
        with self.assertRaisesRegex(ValueError, "构建契约"):
            workflow.verify_build(config)

    def test_old_or_invalid_package_cannot_silently_fall_back_to_32(self):
        config, manifest = self.package(dict(dispatch_send_core_num=24, combine_send_core_num=24, dispatch_count_core_num=32))
        for cores in (None, {}, dict(dispatch_send_core_num=56, combine_send_core_num=24),
                      dict(dispatch_send_core_num=24, combine_send_core_num=True),
                      dict(dispatch_send_core_num=24, combine_send_core_num="24"),
                      dict(dispatch_send_core_num=52, combine_send_core_num=28),
                      dict(dispatch_send_core_num=12, combine_send_core_num=28),
                      dict(dispatch_send_core_num=28, combine_send_core_num=60),
                      dict(dispatch_send_core_num=28, combine_send_core_num=30)):
            with self.subTest(cores=cores):
                if cores and "dispatch_count_core_num" not in cores and len(cores) == 2:
                    cores = dict(cores, dispatch_count_core_num=32)
                manifest["server_dedup_core_config"] = cores
                workflow.write_json(config.install / "manifest.json", manifest)
                with self.assertRaises(ValueError):
                    workflow.verify_build(config)

    def test_24_core_consumer_passes_real_validation_after_loading_package(self):
        fixture = m5.CurrentRunFixture()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        config, mappings, records = fixture.full_records("combine", "performance", 100, 50)
        record = records[0]
        record["combine_control_publish_bytes"] = 26624
        with self.assertRaisesRegex(workflow.Failure, "control_publish_bytes"):
            workflow.validate_combine_return_record(record, mappings[0], config.chain)
        package, _ = self.package(dict(dispatch_send_core_num=24, combine_send_core_num=24, dispatch_count_core_num=32))
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
                "-DTILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM=16",
                "-DTILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM=28", "-DTILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=52"]
        subprocess.run(args, check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(build)], check=True, capture_output=True)
        self.assertIn('"combine_send_core_num":52', subprocess.check_output([str(build / "probe")], text=True))
        self.assertIn('"dispatch_count_core_num":16', subprocess.check_output([str(build / "probe")], text=True))
        for invalid in ("-DTILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM=56",
                        "-DTILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM=0",
                        "-DTILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM=65"):
            result = subprocess.run(args + [invalid], capture_output=True)
            self.assertNotEqual(result.returncode, 0)

    def test_invalid_count_configuration_is_rejected(self):
        with self.assertRaises(ValueError):
            workflow.validate_core_config(dict(dispatch_send_core_num=32, combine_send_core_num=32))
        for count in (0, 65, True, "16"):
            with self.subTest(count=count), self.assertRaises(ValueError):
                workflow.validate_core_config(dict(dispatch_send_core_num=32, combine_send_core_num=32,
                                                    dispatch_count_core_num=count))


if __name__ == "__main__":
    unittest.main()
