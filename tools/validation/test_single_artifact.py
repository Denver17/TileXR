"""Production build/sync/run share one artifact; manifest controls mode admission."""

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import workflow


class SingleArtifactTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tilexr-single-artifact-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / "hostfile").write_text("127.0.0.1\n192.0.2.2\n")
        environment = dict(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_BUILD_MODE="production",
                           TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_M5_CHECKPOINT,
                           TILEXR_INTERNAL_ACCEPTANCE_STAGE=workflow.COMBINE_M5_CHECKPOINT,
                           TILEXR_COMM_ID="192.0.2.1:10177",
                           TILEXR_DEMO_BARRIER_ADDR="192.0.2.1:10274")
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)

    def install_fixture(self, profiling):
        with mock.patch.dict(os.environ, TILEXR_VALIDATION_BREAKDOWN=str(int(profiling))):
            config = workflow.Config("build")
        kernel = config.install / "lib/kernel.o"
        kernel.parent.mkdir(parents=True, exist_ok=True)
        kernel.write_text("profile=" + str(profiling))
        manifest = dict(build_id="build-" + str(profiling), mode=config.mode,
                        server_dedup_core_config=dict(dispatch_send_core_num=32, combine_send_core_num=32),
                        checkpoint=config.checkpoint, architecture=workflow.platform.machine(),
                        server_dedup_profiling=profiling,
                        server_dedup_contract=workflow.DISPATCH_ABI,
                        server_dedup_combine_contract=workflow.combine_contract(config.checkpoint),
                        server_dedup_profile_schema=workflow.measurements.RECORDS_SCHEMA,
                        artifacts=workflow.artifact_files(config.install), tests=[])
        workflow.write_json(config.install / "manifest.json", manifest)
        return config, manifest

    def test_sync_ignores_shell_profiling_and_distributes_current_manifest(self):
        for profiling in (True, False):
            config, manifest = self.install_fixture(profiling)
            for stale_shell_value in ("0", "1"):
                with self.subTest(profiling=profiling, shell=stale_shell_value), \
                        mock.patch.dict(os.environ, TILEXR_VALIDATION_BREAKDOWN=stale_shell_value):
                    sync_config = workflow.Config("sync")
                    self.assertEqual(sync_config.install, config.install)
                    with mock.patch.object(workflow, "require_tools"), \
                            mock.patch.object(workflow, "execute", return_value="VERIFY_PASS") as execute, \
                            redirect_stdout(io.StringIO()):
                        workflow.sync(sync_config)
                    receipt = workflow.read_json(config.logs / "sync.json")
                    self.assertEqual(receipt["build_id"], manifest["build_id"])
                    self.assertEqual(receipt["install"], str(config.install.relative_to(self.root)))
                    transfer = next(call.args[0] for call in execute.call_args_list if call.args[0][0] == "rsync")
                    files = Path(next(arg.split("=", 1)[1] for arg in transfer if arg.startswith("--files-from=")))
                    installations = [line for line in files.read_text().splitlines() if line.startswith("install/")]
                    self.assertEqual(installations, [receipt["install"] + "/"])
                    remote = workflow.Config("verify")
                    self.assertEqual(remote.install, config.install)
                    self.assertEqual(workflow.verify_build(remote, check_run_mode=False), manifest)

    def test_all_operators_reject_wrong_mode_before_launch(self):
        for profiling in (False, True):
            build_config, manifest = self.install_fixture(profiling)
            for operator in ("dispatch", "combine", "both"):
                for mode in ("functional", "performance", "breakdown"):
                    with self.subTest(profiling=profiling, operator=operator, mode=mode), \
                            mock.patch.dict(os.environ, TILEXR_VALIDATION_OPERATOR_MODE=operator,
                                            TILEXR_VALIDATION_RUN_MODE=mode):
                        config = workflow.Config("run")
                        self.assertEqual(config.install, build_config.install)
                        if profiling == (mode == "breakdown"):
                            self.assertEqual(workflow.verify_build(config), manifest)
                        else:
                            with mock.patch.object(workflow, "execute") as launch, \
                                    self.assertRaisesRegex(ValueError, "设备打点开关与运行模式不一致"):
                                workflow.run(config)
                            launch.assert_not_called()

    def test_rebuild_uses_one_directory_and_invalidates_old_evidence(self):
        paths = set()
        for profiling in (False, True, False):
            config, _ = self.install_fixture(profiling)
            work = workflow.validation_build_directory(config)
            paths.add((work, config.install))
            work.mkdir(parents=True, exist_ok=True)
            (work / "old-object.o").write_text("old flags")
            workflow.write_json(config.logs / "sync.json", dict(build_id="old"))
            workflow.clean_build_directory(config)
            self.assertFalse((config.install / "manifest.json").exists())
            self.assertFalse((config.logs / "sync.json").exists())
            self.assertFalse((work / "old-object.o").exists())
            with self.assertRaises(FileNotFoundError):
                workflow.verify_build(workflow.Config("sync"), check_run_mode=False)
        self.assertEqual(len(paths), 1)
        self.assertEqual(len(list((self.root / "build").iterdir())), 1)
        self.assertEqual(len(list((self.root / "install").iterdir())), 1)


if __name__ == "__main__":
    unittest.main()
