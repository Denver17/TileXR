#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tests" / "ccu" / "run_tilexr_ccu_direct_smoke.sh"
EVENING_RUNNER = REPO_ROOT / "tests" / "ccu" / "run_tilexr_ccu_direct_evening_smoke.sh"
BUSY_GUARD = REPO_ROOT / "tests" / "ccu" / "ccu_npu_smi_busy_guard.py"


class TileXRCcuDirectSmokeRunnerTest(unittest.TestCase):
    def test_runner_is_default_safe_and_documents_hardware_gate(self):
        source = RUNNER.read_text(encoding="utf-8")

        self.assertIn("TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_ENABLE=1", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=1", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_SUBMIT", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_EXPECT_BARRIER_WAIT", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_MIN_SYNC_MS", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES", source)
        self.assertIn("TILEXR_CCU_DIRECT_BARRIER_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_INSTALL_ORDER", source)
        self.assertIn("TILEXR_CCU_DIRECT_INSTALL_DIE_ID", source)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_SQE_ARG_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_INSTRUCTION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_BINDINGS_PER_RESOURCE", source)
        self.assertIn("TILEXR_CCU_PROBE_CHANNEL_START", source)
        self.assertIn("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START", source)
        self.assertIn("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START", source)
        self.assertIn("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK0_XN_START", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK1_XN_START", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START", source)
        for suffix in [
            "EID",
            "TPN",
            "DOORBELL_VA",
            "DOORBELL_TOKEN_ID",
            "DOORBELL_TOKEN_VALUE",
            "SQ_DEPTH",
        ]:
            with self.subTest(endpoint_suffix=suffix):
                self.assertIn(f"    {suffix}", source)
        self.assertIn('endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}"', source)
        self.assertIn('rank0_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK0"', source)
        self.assertIn('rank1_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK1"', source)
        self.assertIn('rank0_env+=("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank0_endpoint_value}")', source)
        self.assertIn('rank1_env+=("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank1_endpoint_value}")', source)
        self.assertIn("rank0_env", source)
        self.assertIn("rank1_env", source)
        self.assertIn("TILEXR_COMM_ID", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK_SIZE=2", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK=0", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK=1", source)
        self.assertIn('if [ "${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE:-0}" = "1" ]', source)
        self.assertIn('"${probe_bin}" > "${thread_log}" 2>&1', source)
        self.assertIn("ccu_thread.log", source)
        self.assertIn("ccu_rank0.log", source)
        self.assertIn("ccu_rank1.log", source)
        self.assertIn("installSucceeded=1", source)
        self.assertIn("submitReady=1", source)
        self.assertLess(source.index("installSucceeded=1"), source.index("submitReady=1"))
        self.assertIn("${repo_root}/install/lib64/libtile-comm.so", source)
        self.assertIn(
            'timeout "${timeout_s}s" env "${common_env[@]}" "${rank0_env[@]}" TILEXR_CCU_PROBE_RANK=0',
            source,
        )
        self.assertIn(
            'timeout "${timeout_s}s" env "${common_env[@]}" "${rank1_env[@]}" TILEXR_CCU_PROBE_RANK=1',
            source,
        )
        self.assertNotIn('bash -c "wait', source)
        self.assertIn("npu-smi rc=", source)
        self.assertIn("TILEXR_CCU_SMOKE_ALLOW_BUSY_NPU", source)
        self.assertIn("TILEXR_CCU_SMOKE_ALLOW_UNHEALTHY_NPU", source)
        self.assertIn("--allow-unhealthy", source)
        self.assertIn("TILEXR_CCU_SMOKE_REQUIRE_NPU_SMI", source)
        self.assertIn("ccu_npu_smi_busy_guard.py", source)
        self.assertIn("tilexr_ccu_direct_smoke_runner summary", source)
        self.assertIn("rank0Status=", source)
        self.assertIn("rank1Status=", source)
        self.assertIn("rank0Log=", source)
        self.assertIn("rank1Log=", source)
        self.assertIn("submitTiming", source)
        self.assertIn("syncMs=", source)
        self.assertIn("p2pCcuCopy", source)
        self.assertIn("passed=1", source)
        self.assertIn('"TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE=${TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE:-1}"', source)

        gate = source.index('if [ "${TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE:-0}" != "1" ]')
        for needle in [
            "npu-smi info",
            '"${probe_bin}"',
            "TILEXR_CCU_DIRECT_SMOKE_ENABLE=1",
            "TILEXR_CCU_PROBE_RANK=0",
            "TILEXR_CCU_PROBE_RANK=1",
        ]:
            with self.subTest(needle=needle):
                self.assertLess(gate, source.index(needle))

    def test_runner_p2p_mode_applies_direct_ccu_resource_defaults(self):
        source = RUNNER.read_text(encoding="utf-8")

        self.assertIn("apply_p2p_ccu_copy_defaults", source)
        self.assertIn('TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT="${TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT:-1}"', source)
        self.assertIn('TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_ACTIVE_RANK="${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_ACTIVE_RANK:-0}"', source)
        self.assertIn('TILEXR_CCU_PROBE_GSA_START="${TILEXR_CCU_PROBE_GSA_START:-510}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START="${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START:-2361}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START="${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START:-2361}"', source)
        self.assertIn('TILEXR_CCU_PROBE_REMOTE_XN_COUNT="${TILEXR_CCU_PROBE_REMOTE_XN_COUNT:-8}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START="${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START:-332}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START="${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START:-332}"', source)
        self.assertIn('TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT="${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT:-8}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START="${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START:-364}"', source)
        self.assertIn('TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START="${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START:-364}"', source)
        self.assertIn('TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT="${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT:-8}"', source)
        self.assertIn('TILEXR_CCU_PROBE_CHANNEL_START="${TILEXR_CCU_PROBE_CHANNEL_START:-2}"', source)
        self.assertIn('TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE="${TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE:-ra_ctx}"', source)
        self.assertIn('common_env+=("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE=${TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE}")', source)
        self.assertIn("p2p_passed_count=0", source)
        self.assertIn('grep -q "tilexr_ccu_direct_smoke p2pCcuCopy skipped"', source)
        self.assertIn("direct CCU P2P CCU-copy produced no passing receiver result", source)

    def test_runner_default_run_skips_without_hardware(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env.pop("TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE", None)
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("skipped", result.stdout)
        self.assertNotIn("TILEXR_CCU_PROBE_RANK=0", result.stdout)
        self.assertNotIn("ccu_rank0.log", result.stdout)

    def test_runner_defaults_prepare_failure_fast_exit_and_allows_opt_out(self):
        source = RUNNER.read_text(encoding="utf-8")

        self.assertIn(
            '"TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE=${TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE:-1}"',
            source,
        )
        self.assertNotIn("TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE=0", source)

    def test_runner_dry_run_shows_rank_specific_endpoint_overrides_common(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE"] = "1"
            env["TILEXR_CCU_DIRECT_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN"] = "99"
            env["TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN_RANK0"] = "100"
            env["TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN_RANK1"] = "200"
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_smoke_runner dryRun=1", result.stdout)
        self.assertIn("dryRun rank0 TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN=100", result.stdout)
        self.assertIn("dryRun rank1 TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN=200", result.stdout)
        self.assertNotIn("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN=99", result.stdout)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)
        self.assertNotIn("ccu_rank0.log", result.stdout)

    def test_runner_dry_run_shows_rank_specific_resource_window_token_overrides_common(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE"] = "1"
            env["TILEXR_CCU_DIRECT_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID"] = "0x1111"
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK0"] = "0x2222"
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK1"] = "0x3333"
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID_RANK1"] = "0x4444"
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_RANK1"] = "0x5555"
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_smoke_runner dryRun=1", result.stdout)
        self.assertIn("dryRun rank0 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID=0x2222", result.stdout)
        self.assertIn("dryRun rank1 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID=0x3333", result.stdout)
        self.assertIn("dryRun rank1 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID=0x4444", result.stdout)
        self.assertIn("dryRun rank1 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE=0x5555", result.stdout)
        self.assertNotIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID=0x1111", result.stdout)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)
        self.assertNotIn("ccu_rank0.log", result.stdout)

    def test_runner_dry_run_shows_repository_install_diagnostic_variants(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE"] = "1"
            env["TILEXR_CCU_DIRECT_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW"] = "full_repository"
            env["TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE"] = "descriptor_bytes"
            env["TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE"] = "acl_hccl_module"
            env["TILEXR_CCU_DIRECT_INSTALL_ORDER"] = "lower_layer_first"
            env["TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START"] = "489"
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW=full_repository", result.stdout)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE=descriptor_bytes", result.stdout)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE=acl_hccl_module", result.stdout)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_INSTALL_ORDER=lower_layer_first", result.stdout)
        self.assertIn("dryRun TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START=489", result.stdout)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)
        self.assertNotIn("ccu_rank0.log", result.stdout)

    def test_runner_dry_run_defaults_to_lower_layer_first_install_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE"] = "1"
            env["TILEXR_CCU_DIRECT_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env.pop("TILEXR_CCU_DIRECT_INSTALL_ORDER", None)
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_INSTALL_ORDER=lower_layer_first", result.stdout)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)

    def test_runner_dry_run_defaults_sync_xn_post_only_window_for_hcomm_style_task1_prelude(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE"] = "1"
            env["TILEXR_CCU_DIRECT_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_BARRIER_MODE"] = "sync_xn_post_only"
            env["TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT"] = "1"
            env["TILEXR_CCU_PROBE_INSTRUCTION_START"] = "475"
            env["TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START"] = "489"
            env.pop("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT", None)
            result = subprocess.run(
                ["bash", str(RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_BARRIER_MODE=sync_xn_post_only", result.stdout)
        self.assertIn("dryRun derived task0.instStartId=489 task0.instCnt=13", result.stdout)
        self.assertIn("dryRun derived task1.instStartId=502 task1.instCnt=6", result.stdout)
        self.assertIn(
            "dryRun derived repositoryStartId=475 repositoryCount=33 missionInstructionStartId=489 "
            "missionInstructionCount=19",
            result.stdout,
        )
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)

    def test_npu_busy_guard_rejects_selected_device_processes(self):
        sample = """
+------------------+---------------+--------------+------------------+
| NPU     Chip     | Process id    | Process name | Process memory   |
+==================+===============+==============+==================+
| 0       0        | 31415         | python3.10   | 1024             |
| 2       0        | 27182         | train.py     | 2048             |
+------------------+---------------+--------------+------------------+
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "npu-smi.log"
            log_path.write_text(sample, encoding="utf-8")
            busy = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "0,1"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
            idle = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "1"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )

        self.assertEqual(1, busy.returncode, busy.stdout + busy.stderr)
        self.assertIn("device=0 pid=31415 process=python3.10", busy.stdout)
        self.assertEqual(0, idle.returncode, idle.stdout + idle.stderr)
        self.assertIn("no selected NPU processes", idle.stdout)

    def test_npu_busy_guard_ignores_device_status_table(self):
        sample = """
+------+-------------+--------+-------------+
| NPU  | Name        | Health | Power(W)    |
+======+=============+========+=============+
| 0    | Ascend950PR | OK     | 95          |
| 1    | Ascend950PR | OK     | 93          |
+------+-------------+--------+-------------+
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "npu-smi.log"
            log_path.write_text(sample, encoding="utf-8")
            result = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "0,1"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("no selected NPU processes", result.stdout)

    def test_npu_busy_guard_rejects_selected_alarm_devices(self):
        sample = """
+------+-------------+--------+-------------+
| NPU  | Name        | Health | Power(W)    |
+======+=============+========+=============+
| 0    | Ascend950PR | Alarm  | 95          |
| 1    | Ascend950PR | OK     | 93          |
| 2    | Ascend950PR | Alarm  | 94          |
+------+-------------+--------+-------------+
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "npu-smi.log"
            log_path.write_text(sample, encoding="utf-8")
            unhealthy = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "0,1"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
            healthy = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "1"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )

        self.assertEqual(1, unhealthy.returncode, unhealthy.stdout + unhealthy.stderr)
        self.assertIn("unhealthy selected NPU device=0 health=Alarm", unhealthy.stdout)
        self.assertNotIn("device=2", unhealthy.stdout)
        self.assertEqual(0, healthy.returncode, healthy.stdout + healthy.stderr)
        self.assertIn("selected NPU devices healthy", healthy.stdout)

    def test_npu_busy_guard_can_allow_alarm_without_allowing_busy_processes(self):
        sample = """
+------+-------------+--------+-------------+
| NPU  | Name        | Health | Power(W)    |
+======+=============+========+=============+
| 0    | Ascend950PR | Alarm  | 95          |
| 1    | Ascend950PR | OK     | 93          |
| 2    | Ascend950PR | Alarm  | 94          |
+------+-------------+--------+-------------+
| NPU     Chip     | Process id    | Process name | Process memory   |
+==================+===============+==============+==================+
| 0       0        | 31415         | python3.10   | 1024             |
+------+-------------+--------+-------------+
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "npu-smi.log"
            log_path.write_text(sample, encoding="utf-8")
            allowed_health = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "2", "--allow-unhealthy"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
            busy_still_blocked = subprocess.run(
                ["python", str(BUSY_GUARD), "--log", str(log_path), "--devices", "0", "--allow-unhealthy"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )

        self.assertEqual(0, allowed_health.returncode, allowed_health.stdout + allowed_health.stderr)
        self.assertIn("unhealthy selected NPU devices allowed", allowed_health.stdout)
        self.assertNotIn("selected NPU devices healthy", allowed_health.stdout)
        self.assertEqual(1, busy_still_blocked.returncode, busy_still_blocked.stdout + busy_still_blocked.stderr)
        self.assertIn("device=0 pid=31415 process=python3.10", busy_still_blocked.stdout)

    def test_runner_requires_submit_for_barrier_wait_expectation(self):
        source = RUNNER.read_text(encoding="utf-8")
        barrier_gate = source.index('if [ "${TILEXR_CCU_DIRECT_SMOKE_EXPECT_BARRIER_WAIT:-0}" = "1" ]')
        submit_guard = source.index(
            'if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" != "1" ]; then',
            barrier_gate,
        )
        timing_parse = source.index("wait_sync_ms=", barrier_gate)

        self.assertLess(barrier_gate, submit_guard)
        self.assertLess(submit_guard, timing_parse)
        self.assertIn("direct CCU barrier wait check requires TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1", source)

    def test_evening_wrapper_defaults_to_process_mode_direct_ccu_only_prepare_first(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")

        self.assertIn('--dry-run', source)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN", source)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES", source)
        self.assertIn("source scripts/common_env.sh", source)
        self.assertIn("cmake --build build --target tile-comm", source)
        self.assertIn("TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1", source)
        self.assertIn("TILEXR_CCU_SMOKE_REQUIRE_NPU_SMI=1", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=0", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_SUBMIT", source)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT", source)
        self.assertIn('TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT:-1', source)
        self.assertIn("TILEXR_CCU_DIRECT_BARRIER_MODE=sync_cke", source)
        self.assertIn("TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE=hcomm_cap", source)
        self.assertIn("TILEXR_CCU_PROBE_SQE_ARG_COUNT", source)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_INSTRUCTION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK0_XN_START", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK1_XN_START", source)
        self.assertIn(":-475", source)
        self.assertIn(":-489", source)
        self.assertIn(":-143", source)
        self.assertIn(":-full_repository", source)
        self.assertIn(":-instruction_bytes", source)
        self.assertIn(":-acl", source)
        self.assertIn("tests/ccu/run_tilexr_ccu_direct_smoke.sh", source)
        self.assertIn('run_smoke_stage "prepare_${safe_prepare_mode}"', source)
        self.assertIn("prepare_has_submit_ready", source)
        self.assertIn("submitReady=1", source)
        self.assertIn("run_smoke_stage submit", source)
        self.assertIn("run_smoke_stage barrier", source)
        self.assertIn("run_smoke_stage p2p", source)
        self.assertLess(source.index('run_smoke_stage "prepare_${safe_prepare_mode}"'), source.index("run_smoke_stage submit"))
        self.assertLess(source.index("prepare_has_submit_ready"), source.index("run_smoke_stage submit"))

    def test_evening_wrapper_dry_run_short_circuits_before_common_env(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")

        dry_run_gate = source.index('if [ "${TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN:-0}" = "1" ]')
        common_env_source = source.index("source scripts/common_env.sh")
        cmake_build = source.index("cmake --build build --target tile-comm")

        self.assertLess(dry_run_gate, common_env_source)
        self.assertLess(dry_run_gate, cmake_build)

    def test_evening_wrapper_dry_run_does_not_invoke_fake_npu_smi(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            marker = temp_path / "npu-smi-called"
            fake_npu_smi = temp_path / "npu-smi"
            fake_npu_smi.write_text(
                f"#!/usr/bin/env bash\n"
                f"echo called >> {str(marker).replace(os.sep, '/')!r}\n"
                f"exit 0\n",
                encoding="utf-8",
            )
            fake_npu_smi.chmod(0o755)

            env = os.environ.copy()
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN"] = "1"
            env["PATH"] = str(temp_path) + os.pathsep + env.get("PATH", "")
            result = subprocess.run(
                ["bash", str(EVENING_RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_evening_smoke dryRun=1", result.stdout)
        self.assertFalse(marker.exists(), result.stdout + result.stderr)

    def test_evening_wrapper_retries_prepare_with_module3_allocator_before_submit(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")

        self.assertIn('TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES:-acl,acl_module3,rt_hbm', source)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES", source)
        self.assertIn('prepare_alloc_modes', source)
        self.assertIn('prepare_${safe_prepare_mode}', source)
        self.assertIn('TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${prepare_profile_alloc}"', source)
        self.assertIn('TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${prepare_profile_window}"', source)
        self.assertIn('TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${prepare_profile_data_len_mode}"', source)
        self.assertIn('TILEXR_CCU_DIRECT_INSTALL_ORDER="${prepare_profile_install_order}"', source)
        self.assertIn('selected_prepare_alloc_mode', source)
        self.assertIn('selected_prepare_window', source)
        self.assertIn('prepareStatus=', source)
        self.assertIn('prepare_status_summary', source)
        self.assertIn('selectedPrepare alloc=', source)
        self.assertIn('stopAfter=prepare reason="submitReady=1 missing for every prepare profile"', source)

        prepare_loop = source.index('for prepare_profile in "${prepare_profiles[@]}"')
        self.assertLess(
            source.index('TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${prepare_profile_alloc}"', prepare_loop),
            source.index("prepare_has_submit_ready", prepare_loop),
        )
        self.assertLess(source.index("selected_prepare_alloc_mode"), source.index("run_smoke_stage submit"))
        submit_stage = source.index("run_smoke_stage submit")
        self.assertLess(
            submit_stage,
            source.index('TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}"', submit_stage),
        )
        self.assertLess(
            submit_stage,
            source.index('TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${selected_prepare_window}"', submit_stage),
        )
        barrier_stage = source.index("run_smoke_stage barrier")
        self.assertLess(
            barrier_stage,
            source.index('TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}"', barrier_stage),
        )
        p2p_stage = source.index("run_smoke_stage p2p")
        self.assertLess(
            p2p_stage,
            source.index('TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}"', p2p_stage),
        )

    def test_evening_wrapper_dry_run_lists_prepare_profiles(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "full_acl:acl:full_repository:instruction_bytes:lower_layer_first,"
                "mission_desc:acl:mission:descriptor_bytes:repository_first"
            )
            result = subprocess.run(
                ["bash", str(EVENING_RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn(
            "TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES="
            "full_acl:acl:full_repository:instruction_bytes:lower_layer_first,"
            "mission_desc:acl:mission:descriptor_bytes:repository_first",
            result.stdout,
        )
        self.assertIn(
            "dryRun prepareProfile[0] name=full_acl alloc=acl window=full_repository "
            "dataLenMode=instruction_bytes installOrder=lower_layer_first",
            result.stdout,
        )
        self.assertIn(
            "dryRun prepareProfile[1] name=mission_desc alloc=acl window=mission "
            "dataLenMode=descriptor_bytes installOrder=repository_first",
            result.stdout,
        )

    def test_evening_wrapper_prepare_profiles_pass_repository_options_to_runner(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<LOG\n"
                "tilexr_ccu_direct_smoke config rank=0 alloc=${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE} window=${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW} dataLen=${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE} order=${TILEXR_CCU_DIRECT_INSTALL_ORDER}\n"
                "tilexr_ccu_direct_smoke prepare ret=6 submitReady=0 message=\"profile failed\"\n"
                "LOG\n"
                "exit 6\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "full_acl:acl:full_repository:instruction_bytes:lower_layer_first,"
                "mission_desc:acl:mission:descriptor_bytes:repository_first"
            )
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn(
            "prepareStatus=6 profile=full_acl alloc=acl window=full_repository "
            "dataLenMode=instruction_bytes installOrder=lower_layer_first",
            result.stdout,
        )
        self.assertIn(
            "prepareStatus=6 profile=mission_desc alloc=acl window=mission "
            "dataLenMode=descriptor_bytes installOrder=repository_first",
            result.stdout,
        )
        self.assertIn("config rank=0 alloc=acl window=full_repository dataLen=instruction_bytes order=lower_layer_first", result.stdout)
        self.assertIn("config rank=0 alloc=acl window=mission dataLen=descriptor_bytes order=repository_first", result.stdout)
        self.assertIn("prepareStatusSummary=full_acl:6:acl:full_repository:instruction_bytes:lower_layer_first:", result.stdout)
        self.assertIn("mission_desc:6:acl:mission:descriptor_bytes:repository_first:", result.stdout)

    def test_evening_wrapper_prints_prepare_matrix_summary_for_profile_failures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "case \"${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE}:${TILEXR_CCU_DIRECT_INSTALL_ORDER}\" in\n"
                "  instruction_bytes:lower_layer_first)\n"
                "    ret=328107; opret=0; lower='lowerLayerPreconditions{msidTokenCount=1 pfeCount=1 jettyCount=1 channelCount=1 xnClearCount=1 ckeClearCount=1 localXnInstalled=1 notifyCkeInstalled=1 channelBindingInstalled=1}' ;;\n"
                "  descriptor_bytes:repository_first)\n"
                "    ret=0; opret=81; lower='' ;;\n"
                "esac\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<LOG\n"
                "tilexr_ccu_direct_smoke config rank=0 window=${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW} dataLen=${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE} order=${TILEXR_CCU_DIRECT_INSTALL_ORDER}\n"
                "tilexr_ccu_direct_smoke prepare ret=6 installSucceeded=0 submitReady=0 message=\"${lower}; failed to install CCU repository instruction image: CCU custom channel call failed op=251 driverRet=${ret} opRet=${opret}\"\n"
                "LOG\n"
                "exit 6\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "full_acl:acl:full_repository:instruction_bytes:lower_layer_first,"
                "desc_repo_first:acl:full_repository:descriptor_bytes:repository_first"
            )
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn(
            "prepareMatrix profile=full_acl status=6 submitReady=0 op=251 driverRet=328107 opRet=0 "
            "lowerLayerPreconditions=1",
            result.stdout,
        )
        self.assertIn(
            "prepareMatrix profile=desc_repo_first status=6 submitReady=0 op=251 driverRet=0 opRet=81 "
            "lowerLayerPreconditions=0",
            result.stdout,
        )
        self.assertIn("window=full_repository dataLenMode=instruction_bytes installOrder=lower_layer_first", result.stdout)
        self.assertIn("window=full_repository dataLenMode=descriptor_bytes installOrder=repository_first", result.stdout)

    def test_evening_wrapper_profile_can_matrix_lower_layer_pfe_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<LOG\n"
                "tilexr_ccu_direct_smoke config stage submit=${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0} pfeOffsetSource=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE:-NA} pfePartition=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION:-NA}\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "LOG\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<LOG\n"
                "tilexr_ccu_direct_smoke config stage submit=${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0} pfeOffsetSource=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE:-NA} pfePartition=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION:-NA}\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "LOG\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare,submit"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "pfe_hcomm:acl:full_repository:instruction_bytes:lower_layer_first:hcomm_die:hcomm_fe_id"
            )
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn(
            "prepareStatus=0 profile=pfe_hcomm alloc=acl window=full_repository "
            "dataLenMode=instruction_bytes installOrder=lower_layer_first "
            "pfeOffsetSource=hcomm_die pfePartition=hcomm_fe_id",
            result.stdout,
        )
        self.assertIn(
            "prepareMatrix profile=pfe_hcomm status=0 submitReady=1",
            result.stdout,
        )
        self.assertIn("pfeOffsetSource=hcomm_die pfePartition=hcomm_fe_id", result.stdout)
        self.assertIn(
            "selectedPrepare alloc=acl window=full_repository dataLenMode=instruction_bytes "
            "installOrder=lower_layer_first pfeOffsetSource=hcomm_die pfePartition=hcomm_fe_id",
            result.stdout,
        )
        self.assertIn("stage=submit", result.stdout)
        self.assertIn("config stage submit=1 pfeOffsetSource=hcomm_die pfePartition=hcomm_fe_id", result.stdout)
        self.assertIn(
            "finalStatus prepare=pass submit=pass barrier=skipped p2p=skipped completionCandidate=0 failedStage=none selectedProfile=pfe_hcomm",
            result.stdout,
        )

    def test_evening_wrapper_prints_final_status_for_all_successful_stages(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submitTiming rank=0 preSubmitDelayMs=0 submitRet=0 syncRet=0 submitMs=1 syncMs=150\n"
                "tilexr_ccu_direct_smoke p2pCcuCopy rank=0 passed=1\n"
                "LOG\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submitTiming rank=1 preSubmitDelayMs=0 submitRet=0 syncRet=0 submitMs=1 syncMs=160\n"
                "tilexr_ccu_direct_smoke p2pCcuCopy rank=1 passed=1\n"
                "LOG\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare,submit,barrier,p2p"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "pfe_hcomm:acl:full_repository:instruction_bytes:lower_layer_first:hcomm_die:hcomm_fe_id"
            )
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )
            final_status = fake_repo / "work" / "final_status.log"
            final_status_exists = final_status.exists()
            final_status_text = final_status.read_text(encoding="utf-8") if final_status_exists else ""

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn(
            "finalStatus prepare=pass submit=pass barrier=pass p2p=pass completionCandidate=1 failedStage=none selectedProfile=pfe_hcomm",
            result.stdout,
        )
        self.assertTrue(final_status_exists, result.stdout + result.stderr)
        self.assertIn(
            "finalStatus prepare=pass submit=pass barrier=pass p2p=pass completionCandidate=1 failedStage=none selectedProfile=pfe_hcomm",
            final_status_text,
        )
        self.assertIn("selectedPrepare", result.stdout)
        self.assertIn("success workRoot=work", result.stdout)

    def test_evening_wrapper_five_field_profile_preserves_ambient_pfe_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<LOG\n"
                "tilexr_ccu_direct_smoke config pfeOffsetSource=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE:-NA} pfePartition=${TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION:-NA}\n"
                "tilexr_ccu_direct_smoke prepare ret=6 installSucceeded=0 submitReady=0 message=\"profile failed\"\n"
                "LOG\n"
                "exit 6\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES"] = (
                "five_field:acl:full_repository:instruction_bytes:lower_layer_first"
            )
            env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE"] = "hcomm_die"
            env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION"] = "hcomm"
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("prepareStatus=6 profile=five_field", result.stdout)
        self.assertIn("pfeOffsetSource=default pfePartition=default", result.stdout)
        self.assertIn("config pfeOffsetSource=hcomm_die pfePartition=hcomm", result.stdout)

    def test_evening_wrapper_summarizes_prepare_failure_logs(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")

        self.assertIn("summarize_stage_logs", source)
        self.assertIn("tilexr_ccu_direct_evening_smoke prepareLogSummary", source)
        self.assertIn("tilexr_ccu_direct_evening_smoke stageLogSummary", source)
        self.assertIn("tilexr_ccu_direct_smoke prepare", source)
        self.assertIn("tilexr_ccu_direct_smoke submit", source)
        self.assertIn("tilexr_ccu_direct_smoke submitTiming", source)
        self.assertIn("tilexr_ccu_direct_smoke p2pCcuCopy", source)
        self.assertIn("CCU custom channel call failed", source)
        self.assertIn("op=", source)
        self.assertIn("driverRet=", source)
        self.assertIn("opRet=", source)

        prepare_loop = source.index('for prepare_profile in "${prepare_profiles[@]}"')
        prepare_status = source.index("prepareStatus=", prepare_loop)
        summary_call = source.index("summarize_stage_logs", prepare_loop)
        self.assertLess(prepare_status, summary_call)

    def test_evening_wrapper_summarizes_submit_barrier_and_p2p_logs(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")

        submit_stage = source.index("run_smoke_stage submit")
        submit_summary = source.index("summarize_stage_logs", submit_stage)
        barrier_stage = source.index("run_smoke_stage barrier")
        barrier_summary = source.index("summarize_stage_logs", barrier_stage)
        p2p_stage = source.index("run_smoke_stage p2p")
        p2p_summary = source.index("summarize_stage_logs", p2p_stage)

        self.assertLess(submit_stage, submit_summary)
        self.assertLess(submit_summary, barrier_stage)
        self.assertLess(barrier_stage, barrier_summary)
        self.assertLess(barrier_summary, p2p_stage)
        self.assertLess(p2p_stage, p2p_summary)

    def test_evening_wrapper_extracts_prepare_failure_summary_from_logs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke config rank=0 installOrder=1\n"
                "tilexr_ccu_direct_smoke prepare ret=6 submitReady=0 message=\"failed to install CCU repository instruction image: CCU custom channel call failed op=251 driverRet=7 opRet=9\"\n"
                "tilexr_ccu_direct_smoke preparedTasks count=0\n"
                "LOG\n"
                "exit 6\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES"] = "acl"
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )
            final_status = fake_repo / "work" / "final_status.log"
            final_status_exists = final_status.exists()
            final_status_text = final_status.read_text(encoding="utf-8") if final_status_exists else ""

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("prepareLogSummary mode=acl status=6", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke prepare ret=6 submitReady=0", result.stdout)
        self.assertIn("CCU custom channel call failed op=251 driverRet=7 opRet=9", result.stdout)
        self.assertIn('stopAfter=prepare reason="submitReady=1 missing for every prepare profile"', result.stdout)
        self.assertIn(
            "finalStatus prepare=fail submit=skipped barrier=skipped p2p=skipped "
            "completionCandidate=0 failedStage=prepare",
            result.stdout,
        )
        self.assertTrue(final_status_exists, result.stdout + result.stderr)
        self.assertIn(
            "finalStatus prepare=fail submit=skipped barrier=skipped p2p=skipped "
            "completionCandidate=0 failedStage=prepare",
            final_status_text,
        )

    def test_evening_wrapper_extracts_submit_stage_summary_from_logs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submitTiming rank=0 preSubmitDelayMs=0 submitRet=0 syncRet=0 submitMs=1 syncMs=7\n"
                "LOG\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "tilexr_ccu_direct_smoke submit ret=0 submitted=1 taskCount=1 submittedTaskCount=1 message=\"ok\"\n"
                "tilexr_ccu_direct_smoke submitTiming rank=1 preSubmitDelayMs=0 submitRet=0 syncRet=0 submitMs=1 syncMs=8\n"
                "LOG\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare,submit"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES"] = "acl"
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("stageLogSummary stage=submit status=0", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke submit ret=0 submitted=1", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke submitTiming rank=1", result.stdout)

    def test_evening_wrapper_runs_p2p_ccu_copy_with_process_mode_direct_ccu_init(self):
        source = EVENING_RUNNER.read_text(encoding="utf-8")
        p2p_stage = source[source.index("if stage_enabled p2p;"):]
        p2p_stage = p2p_stage[:p2p_stage.index("print_final_status none")]

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY=1", p2p_stage)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY=1", p2p_stage)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=1", p2p_stage)
        self.assertNotIn("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=1", p2p_stage)
        self.assertNotIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=0", p2p_stage)

    def test_evening_wrapper_keeps_submit_failure_diagnostics_before_long_trace_tail(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "if [[ \"${TILEXR_CCU_SMOKE_WORK_DIR}\" != *submit* ]]; then\n"
                "  cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "LOG\n"
                "  cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "LOG\n"
                "  exit 0\n"
                "fi\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "tilexr_ccu_direct_smoke submit ret=-2 submitted=0 taskCount=1 submittedTaskCount=0 message=\"direct CCU submit failed task=0 ret=-2 rtRet=507000 dieId=1 missionId=6 instStartId=489 instCnt=156 key=0x59b0f03 argSize=13 args[0]=0xfeed\"\n"
                "LOG\n"
                "for i in $(seq 1 40); do\n"
                "  echo \"TileXRDirectCcuTrace program.sync[$i] decoded=SyncXn\" >> \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\"\n"
                "done\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "LOG\n"
                "exit 7\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare,submit"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES"] = "acl"
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )
            final_status = fake_repo / "work" / "final_status.log"
            final_status_exists = final_status.exists()
            final_status_text = final_status.read_text(encoding="utf-8") if final_status_exists else ""

        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        self.assertIn("stageLogSummary stage=submit status=7", result.stdout)
        self.assertIn("direct CCU submit failed task=0", result.stdout)
        self.assertIn("rtRet=507000", result.stdout)
        self.assertIn("args[0]=0xfeed", result.stdout)
        self.assertIn(
            "finalStatus prepare=pass submit=fail barrier=skipped p2p=skipped completionCandidate=0 failedStage=submit",
            result.stdout,
        )
        self.assertTrue(final_status_exists, result.stdout + result.stderr)
        self.assertIn(
            "finalStatus prepare=pass submit=fail barrier=skipped p2p=skipped completionCandidate=0 failedStage=submit",
            final_status_text,
        )

    def test_evening_wrapper_stage_summary_keeps_decoded_direct_trace_lines(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            fake_bin = temp_path / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            fake_timeout = fake_bin / "timeout"
            fake_timeout.write_text(
                "#!/usr/bin/env bash\n"
                "shift\n"
                "exec \"$@\"\n",
                encoding="utf-8",
            )
            fake_timeout.chmod(0o755)

            fake_repo = temp_path / "repo"
            fake_tests = fake_repo / "tests" / "ccu"
            fake_scripts = fake_repo / "scripts"
            fake_tests.mkdir(parents=True)
            fake_scripts.mkdir()
            (fake_scripts / "common_env.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
            fake_runner = fake_tests / "run_tilexr_ccu_direct_smoke.sh"
            fake_runner.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"${TILEXR_CCU_SMOKE_WORK_DIR}\"\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank0.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "TileXRDirectCcuTrace lowerLayerPfe[0] decoded=PfeCtx pfeOffset=18 startTaJettyId=1024\n"
                "TileXRDirectCcuTrace lowerLayerJettyCtx[0,0] decoded=LocalJettyCtx inferredSqDepth=64 wqeBasicBlockStartId=0\n"
                "TileXRDirectCcuTrace lowerLayerChannel[0] decoded=ChannelCtxV1 sourcePfeId=2 remoteCcuVa=0x12340000\n"
                "TileXRDirectCcuTrace remoteXnBinding[0] localXn=1961 remoteXn=2361 endpointRouteVerified=1\n"
                "TileXRDirectCcuTrace task[0] missionId=6 instStartId=489 instCnt=13 argSize=13\n"
                "TileXRDirectCcuTrace task[1] missionId=6 instStartId=502 instCnt=143 argSize=13\n"
                "TileXRDirectCcuTrace finalRuntimeTask[0] dieId=1 missionId=6 timeout=20 instStartId=489 instCnt=2 key=0x59b0f03 argSize=1 args[0]=0xabc000\n"
                "TileXRDirectCcuTrace customChannel.return op=251 driverRet=328107 opRet=0\n"
                "LOG\n"
                "cat > \"${TILEXR_CCU_SMOKE_WORK_DIR}/ccu_rank1.log\" <<'LOG'\n"
                "tilexr_ccu_direct_smoke prepare ret=0 installSucceeded=1 submitReady=1\n"
                "LOG\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_runner.chmod(0o755)
            fake_evening = fake_tests / "run_tilexr_ccu_direct_evening_smoke.sh"
            fake_evening.write_text(EVENING_RUNNER.read_text(encoding="utf-8"), encoding="utf-8")
            fake_evening.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(fake_bin) + os.pathsep + env.get("PATH", "")
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES"] = "prepare"
            env["TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES"] = "acl"
            env["TILEXR_CCU_EVENING_WORK_ROOT"] = "work"
            result = subprocess.run(
                ["bash", str(fake_evening)],
                cwd=fake_repo,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("decoded=PfeCtx", result.stdout)
        self.assertIn("decoded=LocalJettyCtx", result.stdout)
        self.assertIn("decoded=ChannelCtxV1", result.stdout)
        self.assertIn("remoteXnBinding[0]", result.stdout)
        self.assertIn("TileXRDirectCcuTrace task[0]", result.stdout)
        self.assertIn("TileXRDirectCcuTrace finalRuntimeTask[0]", result.stdout)
        self.assertIn("TileXRDirectCcuTrace customChannel.return op=251", result.stdout)

    def test_evening_wrapper_dry_run_does_not_touch_npu(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK0"] = "0x2222"
            env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK1"] = "0x3333"
            result = subprocess.run(
                ["bash", str(EVENING_RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_evening_smoke dryRun=1", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DRY_RUN=1", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES=prepare,submit,barrier,p2p", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES=acl,acl_module3,rt_hbm", result.stdout)
        self.assertIn(
            "dryRun prepareProfile[2] name=rt_hbm alloc=rt_hbm window=full_repository",
            result.stdout,
        )
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=1", result.stdout)
        self.assertIn("TILEXR_CCU_PROBE_INSTRUCTION_START=475", result.stdout)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START=489", result.stdout)
        self.assertIn("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT=143", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE=acl", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_INSTALL_ORDER=lower_layer_first", result.stdout)
        self.assertIn("dryRun TILEXR_CCU_DIRECT_INSTALL_ORDER=lower_layer_first", result.stdout)
        self.assertIn("TILEXR_CCU_PROBE_SQE_ARG_COUNT=13", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_BARRIER_MODE=sync_cke", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE=hcomm_cap", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK0=0x2222", result.stdout)
        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK1=0x3333", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke_runner dryRun=1", result.stdout)
        self.assertIn("dryRun rank0 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID=0x2222", result.stdout)
        self.assertIn("dryRun rank1 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID=0x3333", result.stdout)
        self.assertIn(
            "dryRun derived repositoryStartId=475 repositoryCount=170 missionInstructionStartId=489",
            result.stdout,
        )
        self.assertIn("dryRun derived task0.instStartId=489 task0.instCnt=13", result.stdout)
        self.assertIn("dryRun derived task1.instStartId=502 task1.instCnt=143", result.stdout)
        self.assertIn("dryRun derived SET_INSTRUCTION offsetStartIdx=475 dataLen=5440", result.stdout)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)

    def test_evening_wrapper_accepts_dry_run_argument_without_touching_npu(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            result = subprocess.run(
                ["bash", str(EVENING_RUNNER), "--dry-run"],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_evening_smoke dryRun=1", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke_runner dryRun=1", result.stdout)
        self.assertNotIn("cmake --build", result.stdout + result.stderr)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)

    def test_evening_wrapper_accepts_generic_smoke_dry_run_without_touching_npu(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["TILEXR_CCU_SMOKE_DRY_RUN"] = "1"
            env["TILEXR_CCU_SMOKE_WORK_DIR"] = temp_dir
            result = subprocess.run(
                ["bash", str(EVENING_RUNNER)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_evening_smoke dryRun=1", result.stdout)
        self.assertIn("tilexr_ccu_direct_smoke_runner dryRun=1", result.stdout)
        self.assertNotIn("stage=prepare", result.stdout + result.stderr)
        self.assertNotIn("npu-smi rc=", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
