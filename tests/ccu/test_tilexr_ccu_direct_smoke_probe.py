#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PROBE_SOURCE = REPO_ROOT / "tests" / "ccu" / "ccu_tilexr_direct_smoke_probe.cpp"
COMM_DIR = REPO_ROOT / "src" / "comm"
INCLUDE_DIR = REPO_ROOT / "src" / "include"


PRIVATE_CCU_NEEDLES = [
    "#include <hcomm/",
    "#include \"hcomm/",
    "#include <hccl/",
    "#include \"hccl/",
    "#include <hccl.h>",
    "#include \"hccl.h\"",
    "pkg_inc/hcomm",
    "pkg_inc/hccl",
    "include/hccl",
    "libhcomm",
    "libhccl_v2",
    "libhccl_fwk",
    "libmc2_client",
    "HcclGetCcuTaskInfo",
    "HcomGetCcuTaskInfo",
    "HcclChannelAcquire",
    "HcclGetChannelForCcu",
    "CcuResBatchAllocator",
    "CcuResRepository",
    "CcuDeviceManager",
    "CcuKernelMgr",
    "CtxMgrImp",
    "GeneTaskParam",
    "GetMissionKey",
    "SetMissionId",
    "SetMissionKey",
    "SetInstrId",
    "SetCcuInstrInfo",
    "LoadInstruction",
    "AllocIns",
    "AllocCke",
    "AllocXn",
]


def cann_paths():
    ascend_home = os.environ.get("ASCEND_HOME_PATH") or os.environ.get("ASCEND_HOME")
    if not ascend_home:
        return None

    arch = os.environ.get("ARCH")
    if not arch:
        arch = "aarch64" if platform.machine() in ("aarch64", "arm64") else "x86_64"

    cann_root = Path(ascend_home) / f"{arch}-linux"
    include_dirs = [
        cann_root / "pkg_inc",
        cann_root / "pkg_inc" / "runtime",
        cann_root / "include",
    ]
    if not any((include_dir / "acl" / "acl.h").exists() for include_dir in include_dirs):
        return None

    lib_dir = cann_root / "lib64"
    if not (lib_dir / "libascendcl.so").exists():
        return None
    driver_root = Path(os.environ.get("ASCEND_DRIVER_PATH", "/usr/local/Ascend/driver"))
    driver_lib_dir = driver_root / "lib64" / "driver"
    return include_dirs, lib_dir, driver_lib_dir


def find_tile_comm():
    env_path = os.environ.get("TILEXR_TILE_COMM_LIB")
    candidates = []
    if env_path:
        candidates.append(Path(env_path))
    candidates.extend(
        [
            REPO_ROOT / "build" / "src" / "comm" / "libtile-comm.so",
            REPO_ROOT / "install" / "lib64" / "libtile-comm.so",
            REPO_ROOT / "install" / "lib" / "libtile-comm.so",
            REPO_ROOT / "install_direct_ccu_guard" / "lib64" / "libtile-comm.so",
        ]
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


class TileXRCcuDirectSmokeProbeTest(unittest.TestCase):
    def compile_probe(self):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        cann_config = cann_paths()
        if cann_config is None:
            self.skipTest("CANN ACL headers/libs are not configured for direct CCU smoke probe")
        tile_comm = find_tile_comm()
        if tile_comm is None:
            self.skipTest("libtile-comm.so is not built; build tile-comm before compiling direct smoke probe")

        include_dirs, cann_lib_dir, driver_lib_dir = cann_config
        temp_dir = tempfile.TemporaryDirectory()
        temp_path = Path(temp_dir.name)
        probe_bin = temp_path / "ccu_tilexr_direct_smoke_probe"
        compile_cmd = [
            compiler,
            "-std=c++14",
            "-DTILEXR_CCU_TESTING=1",
            "-I",
            str(INCLUDE_DIR),
            "-I",
            str(COMM_DIR),
        ]
        for include_dir in include_dirs:
            if include_dir.exists():
                compile_cmd.extend(["-I", str(include_dir)])
        compile_cmd.extend(
            [
                str(PROBE_SOURCE),
                "-L",
                str(tile_comm.parent),
                "-L",
                str(cann_lib_dir),
                "-L",
                str(driver_lib_dir),
                f"-Wl,-rpath-link,{tile_comm.parent}",
                f"-Wl,-rpath-link,{cann_lib_dir}",
                f"-Wl,-rpath-link,{driver_lib_dir}",
                "-ltile-comm",
                "-lascendcl",
                "-lruntime",
                "-ldl",
                "-pthread",
                "-o",
                str(probe_bin),
            ]
        )
        subprocess.run(
            compile_cmd,
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        return temp_dir, probe_bin, tile_comm.parent, cann_lib_dir, driver_lib_dir

    def test_source_exists_and_defines_default_safe_env_guards(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_ENABLE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_AFTER_RUN", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_TRACE_LIFECYCLE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_SUBMIT", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK", source)
        self.assertIn("TILEXR_CCU_PROBE_RANK_SIZE", source)
        self.assertIn("TILEXR_CCU_PROBE_DEVICE", source)
        self.assertIn("TILEXR_CCU_PROBE_COMM_DOMAIN", source)
        self.assertIn("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START", source)
        self.assertIn("TILEXR_CCU_PROBE_SQE_ARG_COUNT", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES", source)
        self.assertIn("TILEXR_CCU_DIRECT_BARRIER_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE", source)
        self.assertIn("TileXRCcuRepositoryMemoryAllocMode::RtHbm", source)
        self.assertIn('text == "rt_hbm"', source)
        self.assertIn("TILEXR_CCU_DIRECT_INSTALL_ORDER", source)
        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID", source)
        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID", source)
        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE", source)
        self.assertIn("InitCommForDirectCcuSmoke", source)
        self.assertIn("RunThreadModeSmoke", source)
        self.assertIn("DirectCcuSmokeContext", source)
        self.assertIn("TileXRCcuRuntimeSession", source)
        self.assertIn("TileXRSockExchange", source)
        self.assertIn("ShouldFastExitAfterPrepareFailure", source)
        self.assertIn("ShouldFastExitAfterRun", source)
        self.assertIn("TraceLifecycle", source)
        self.assertIn("RepositoryInstallWindowFromEnv", source)
        self.assertIn("RepositoryInstallDataLenModeFromEnv", source)
        self.assertIn("RepositoryMemoryAllocModeFromEnv", source)
        self.assertIn("InstallOrderFromEnv", source)
        self.assertIn("TileXRCommInitAll", source)
        self.assertIn("DefaultSyncInstructionCount", source)
        self.assertIn('std::string(value) == "sync_cke"', source)
        self.assertIn('std::string(value) == "sync_cke_set_wait"', source)
        self.assertIn('std::string(value) == "sync_cke_post_only"', source)
        self.assertIn('std::string(value) == "local_cke"', source)
        self.assertIn('std::string(value) == "local_cke_post_only"', source)
        self.assertIn('std::string(value) == "sync_xn_post_only"', source)
        self.assertIn('std::string(value) == "sync_xn_load_post_only"', source)
        self.assertIn("PrepareDirectCcuInstallAttempt", source)
        self.assertIn("PrepareDirectCcuMemoryCopyInstallAttempt", source)
        self.assertIn("ReadDirectCcuInstructionsForDebug", source)
        self.assertIn("TileXRDirectCcuSubmitPrepared", source)
        self.assertIn("TileXRDirectCcuDestroyPrepared", source)
        self.assertIn("tilexr_ccu_direct_smoke config", source)
        self.assertIn("barrierMode=", source)
        self.assertIn("repositoryInstallWindow=", source)
        self.assertIn("repositoryInstallDataLenMode=", source)
        self.assertIn("repositoryMemoryAllocMode=", source)
        self.assertIn("installOrder=", source)
        self.assertIn("resourceWindowTokenId=", source)
        self.assertIn("resourceWindowRawTokenId=", source)
        self.assertIn("resourceWindowTokenValue=", source)
        self.assertIn("tilexr_ccu_direct_smoke preparedTasks", source)
        self.assertIn("tilexr_ccu_direct_smoke submitTiming", source)
        self.assertIn("tilexr_ccu_direct_smoke p2pCcuCopy", source)
        self.assertIn("aclrtMemcpy", source)
        self.assertIn("ACL_MEMCPY_HOST_TO_DEVICE", source)
        self.assertIn("ACL_MEMCPY_DEVICE_TO_HOST", source)
        self.assertIn("passed=", source)
        self.assertIn("syncMs=", source)
        self.assertIn("std::this_thread::sleep_for", source)
        self.assertIn("std::vector<TileXRCommPtr>", source)
        self.assertIn("std::vector<std::thread>", source)

    def test_p2p_ccu_copy_mode_prepares_memory_copy_task(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("kP2pCcuCopyEnv", source)
        self.assertIn("RunP2pCcuCopy", source)
        self.assertIn("PrepareDirectCcuMemoryCopyInstallAttempt", source)
        self.assertIn("TileXRCcuMemoryCopyDirection::RemoteToLocal", source)
        self.assertIn("ACL_MEMCPY_HOST_TO_DEVICE", source)
        self.assertIn("ACL_MEMCPY_DEVICE_TO_HOST", source)
        self.assertIn("p2pCcuCopy", source)

    def test_thread_mode_path_uses_single_process_init_and_never_rank_ipc_init(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        thread_mode_pos = source.index("RunThreadModeSmoke")
        thread_mode_body = source[thread_mode_pos: source.index("int main()")]
        prepared_body = source[source.index("RunPreparedSmokeForRank"): thread_mode_pos]
        self.assertIn("TileXRCommInitAll", thread_mode_body)
        self.assertIn("RunPreparedSmokeForRank", thread_mode_body)
        self.assertIn("PrepareDirectCcuInstallAttempt", prepared_body)
        self.assertIn("TileXRDirectCcuSubmitPrepared", prepared_body)
        self.assertNotIn("TileXRCommInitRankWithDomain", thread_mode_body)
        self.assertNotIn("TILEXR_COMM_ID", thread_mode_body)

    def test_thread_mode_worker_sets_device_before_direct_ccu_submit(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        thread_mode_body = source[source.index("int RunThreadModeSmoke"): source.index("int main()")]

        worker_body = thread_mode_body[
            thread_mode_body.index("rankThreads.emplace_back"):
            thread_mode_body.index("for (auto& rankThread", thread_mode_body.index("rankThreads.emplace_back"))
        ]
        self.assertIn("aclrtSetDevice(devices[rank])", worker_body)
        self.assertLess(
            worker_body.index("aclrtSetDevice(devices[rank])"),
            worker_body.index("RunPreparedSmokeForRank"),
        )

    def test_process_mode_can_opt_into_direct_ccu_only_init_to_bypass_peer_ipc(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        init_helper_body = source[
            source.index("int InitCommForDirectCcuSmoke"):
            source.index("TileXRDirectCcuPrepareOptions MakePrepareOptions")
        ]
        main_source = source[source.index("int main()"):]

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT", source)
        self.assertIn("TileXRSockExchange", init_helper_body)
        self.assertIn("TileXRCcuBackendOptions", init_helper_body)
        self.assertIn("context->session.Init(options)", init_helper_body)
        self.assertIn("EnvFlag(kDirectCcuOnlyInitEnv)", init_helper_body)
        self.assertIn("InitCommForDirectCcuSmoke", main_source)

    def test_process_mode_fast_exit_skips_comm_destroy_after_prepare_failure(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        main_source = source[source.index("int main()"):]

        final_ret_pos = main_source.index("int finalRet = RunPreparedSmokeForRank")
        fast_exit_pos = main_source.index("ShouldFastExitAfterPrepareFailure(finalRet)")
        shutdown_pos = main_source.index("context.session.Shutdown()", final_ret_pos)
        reset_pos = main_source.index("aclrtResetDevice(device)", final_ret_pos)
        finalize_pos = main_source.index("aclFinalize()", final_ret_pos)

        self.assertLess(final_ret_pos, fast_exit_pos)
        self.assertLess(fast_exit_pos, shutdown_pos)
        self.assertLess(fast_exit_pos, reset_pos)
        self.assertLess(fast_exit_pos, finalize_pos)
        self.assertIn("std::fflush(stdout)", main_source)
        self.assertIn("std::fflush(stderr)", main_source)
        self.assertIn("std::_Exit(finalRet)", main_source)
        self.assertIn("tilexr_ccu_direct_smoke fastExitOnPrepareFailure=1", source)

    def test_fast_exit_reason_distinguishes_runtime_failures_from_prepare_failure(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        main_source = source[source.index("int main()"):]

        self.assertIn("FastExitReasonForReturnCode", source)
        self.assertIn("direct CCU collective completion timed out; skipping cleanup", source)
        self.assertIn("direct CCU P2P CCU-copy check failed; skipping cleanup", source)
        self.assertIn("FastExitReasonForReturnCode(finalRet)", main_source)
        self.assertNotIn(
            'reason="prepare failed; skipping cleanup to preserve diagnostic status"',
            main_source,
        )

    def test_process_mode_can_fast_exit_after_run_to_isolate_cleanup_hangs(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        main_source = source[source.index("int main()"):]

        final_ret_pos = main_source.index("int finalRet = RunPreparedSmokeForRank")
        fast_exit_pos = main_source.index("ShouldFastExitAfterRun()")
        shutdown_pos = main_source.index("context.session.Shutdown()", final_ret_pos)

        self.assertLess(final_ret_pos, fast_exit_pos)
        self.assertLess(fast_exit_pos, shutdown_pos)
        self.assertIn("tilexr_ccu_direct_smoke fastExitAfterRun=1", source)
        self.assertIn("TraceLifecycle(\"before TileXRDirectCcuDestroyPrepared\")", source)
        self.assertIn("TraceLifecycle(\"after TileXRDirectCcuDestroyPrepared\")", source)
        self.assertIn("TraceLifecycle(\"before aclrtSynchronizeStream\")", source)
        self.assertIn("TraceLifecycle(\"after aclrtSynchronizeStream\")", source)

    def test_submit_task_selector_can_isolate_prepared_task_hangs(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        prepared_body = source[
            source.index("int RunPreparedSmokeForRank"):
            source.index("int RunThreadModeSmoke")
        ]

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_SUBMIT_TASK_SELECTOR", source)
        self.assertIn("SubmitPreparedWithSelector", source)
        self.assertIn("TileXRDirectCcuGetPreparedTask", source)
        self.assertIn("TileXRDirectCcuSubmitPreparedTask", source)
        self.assertIn('selector == "first"', source)
        self.assertIn('selector == "second"', source)
        self.assertIn("submitTaskSelector=", source)
        self.assertIn("TileXRDirectCcuSubmitPrepared(prepared, stream, &submitReport)", prepared_body)
        self.assertIn("SubmitPreparedWithSelector(prepared, installReport.submitTaskCount, stream", prepared_body)

    def test_process_mode_submit_uses_collective_ready_gate(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        runner = (REPO_ROOT / "tests" / "ccu" / "run_tilexr_ccu_direct_smoke.sh").read_text(encoding="utf-8")
        prepared_body = source[
            source.index("int RunPreparedSmokeForRank"):
            source.index("int RunThreadModeSmoke")
        ]

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_READY_DIR", source)
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_READY_TIMEOUT_MS", source)
        self.assertIn("WaitForCollectiveSubmitReadiness", source)
        self.assertIn("WriteSubmitReadiness", source)
        self.assertIn("collectiveSubmitReady", prepared_body)
        self.assertIn("collective submitReady gate did not pass", source)
        self.assertLess(
            prepared_body.index("WaitForCollectiveSubmitReadiness"),
            prepared_body.index("aclrtCreateStream"),
        )
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_READY_DIR=${ready_dir}", runner)
        self.assertIn("rm -rf \"${ready_dir}\"", runner)

    def test_process_mode_submit_waits_for_all_ranks_before_cleanup(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        runner = (REPO_ROOT / "tests" / "ccu" / "run_tilexr_ccu_direct_smoke.sh").read_text(encoding="utf-8")
        prepared_body = source[
            source.index("int RunPreparedSmokeForRank"):
            source.index("int RunThreadModeSmoke")
        ]

        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DONE_DIR", source)
        self.assertIn("WaitForCollectiveSubmitDone", source)
        self.assertIn("WaitForCollectiveSubmitDone(rank, rankSize, finalRet)", prepared_body)
        self.assertIn("collectiveSubmitDone", source)
        self.assertLess(
            prepared_body.index("aclrtSynchronizeStream"),
            prepared_body.index("WaitForCollectiveSubmitDone"),
        )
        self.assertLess(
            prepared_body.index("WaitForCollectiveSubmitDone"),
            prepared_body.index("TileXRDirectCcuDestroyPrepared"),
        )
        self.assertIn("TILEXR_CCU_DIRECT_SMOKE_DONE_DIR=${done_dir}", runner)
        self.assertIn("rm -rf \"${ready_dir}\" \"${done_dir}\"", runner)

    def test_probe_wires_gsa_and_split_cke_env_into_prepare_options_and_config_trace(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "TILEXR_CCU_PROBE_GSA_START",
            "TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START",
            "TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT",
            "TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START",
            "TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT",
            "options.localWaitCkeStartId",
            "options.localWaitCkeCount",
            "options.remoteNotifyCkeStartId",
            "options.remoteNotifyCkeCount",
            "options.repositoryInstallOptions.window",
            "options.repositoryInstallOptions.dataLenMode",
            "options.repositoryMemoryAllocMode",
            "options.installOrder",
            "options.sqeArgCount",
            "options.missionInstructionStartId",
            "options.gsaStartId",
            "sqeArgCount=",
            "gsaStartId=",
            "localWaitCkeStartId=",
            "localWaitCkeCount=",
            "remoteNotifyCkeStartId=",
            "remoteNotifyCkeCount=",
            "repositoryInstallWindow=",
            "repositoryInstallDataLenMode=",
            "repositoryMemoryAllocMode=",
            "installOrder=",
            "missionInstructionStartId=",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)

    def test_probe_prepared_task_trace_prints_full_runtime_args(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        prepared_body = source[
            source.index("void PrintPreparedTasks"):
            source.index("int RunPreparedSmokeForRank")
        ]

        self.assertIn("TILEXR_DIRECT_CCU_SQE_ARGS_LEN", prepared_body)
        self.assertIn("arg < TILEXR_DIRECT_CCU_SQE_ARGS_LEN", prepared_body)
        self.assertIn('<< ".arg" << arg << "=0x"', prepared_body)
        self.assertIn("task.args[arg]", prepared_body)
        self.assertNotIn(".arg0=0x", prepared_body)

    def test_probe_defaults_to_lower_layer_first_install_order(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        install_order_body = source[
            source.index("TileXR::TileXRCcuInstallOrder InstallOrderFromEnv()"):
            source.index("uint32_t DefaultSyncInstructionCount")
        ]

        self.assertIn("TileXRCcuInstallOrder::InstallLowerLayerFirst", install_order_body)
        self.assertIn("repository_first", install_order_body)
        self.assertIn("TileXRCcuInstallOrder::RepositoryFirst", install_order_body)
        self.assertLess(
            install_order_body.index("TileXRCcuInstallOrder::InstallLowerLayerFirst"),
            install_order_body.index("TileXRCcuInstallOrder::RepositoryFirst"),
        )

    def test_probe_default_sync_instruction_count_includes_hcomm_style_task1_prelude(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        default_body = source[
            source.index("uint32_t DefaultSyncInstructionCount"):
            source.index("const char* FirstEnv")
        ]

        self.assertIn("kHcommStyleTask1PreludeInstructionCount", source)
        self.assertIn(
            "return kHcommStyleTask1PreludeInstructionCount + syncResourceCount;",
            default_body,
        )
        self.assertIn(
            "return kHcommStyleTask1PreludeInstructionCount + syncResourceCount * 2U;",
            default_body,
        )

    def test_thread_mode_rank_specific_resource_env_overrides_common_prepare_options(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        prepare_options_body = source[
            source.index("TileXRDirectCcuPrepareOptions MakePrepareOptions"):
            source.index("void PrintInstallReport")
        ]

        self.assertIn("RankEnvInt", prepare_options_body)
        for suffix in [
            "XN_START",
            "LOCAL_WAIT_CKE_START",
            "LOCAL_WAIT_CKE_COUNT",
            "REMOTE_NOTIFY_CKE_START",
            "REMOTE_NOTIFY_CKE_COUNT",
        ]:
            with self.subTest(suffix=suffix):
                self.assertIn(f"TILEXR_CCU_PROBE_RANK\", rank, \"_{suffix}", source)
        self.assertIn(
            'RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_XN_START", "TILEXR_CCU_PROBE_XN_START", 1)',
            prepare_options_body,
        )
        self.assertIn(
            'RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_LOCAL_WAIT_CKE_START", kLocalWaitCkeStartEnv, 0)',
            prepare_options_body,
        )

    def test_default_skip_happens_before_acl_comm_prepare_or_submit(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")
        main_source = source[source.index("int main()"):]

        guard_pos = main_source.index("if (!EnvFlag(kEnableEnv))")
        for needle in [
            "aclInit(",
            "InitCommForDirectCcuSmoke",
            "RunPreparedSmokeForRank",
        ]:
            with self.subTest(needle=needle):
                self.assertLess(guard_pos, main_source.index(needle))

    def test_probe_keeps_hcomm_hccl_and_runtime_launch_out_of_source(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        for needle in PRIVATE_CCU_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, source)
        self.assertNotIn("runtime/kernel.h", source)
        self.assertNotIn("rtCCULaunch", source)

    def test_probe_compiles_and_default_run_skips_without_touching_hardware(self):
        temp_dir, probe_bin, tile_comm_dir, cann_lib_dir, driver_lib_dir = self.compile_probe()
        try:
            env = os.environ.copy()
            env.pop("TILEXR_CCU_DIRECT_SMOKE_ENABLE", None)
            env["LD_LIBRARY_PATH"] = (
                str(tile_comm_dir)
                + os.pathsep
                + str(cann_lib_dir)
                + os.pathsep
                + str(driver_lib_dir)
                + os.pathsep
                + env.get("LD_LIBRARY_PATH", "")
            )
            result = subprocess.run(
                [str(probe_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )
        finally:
            temp_dir.cleanup()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_smoke skipped", result.stdout)
        self.assertNotIn("prepare ret=", result.stdout)
        self.assertNotIn("submit ret=", result.stdout)

    def test_optional_probe_runtime_when_enabled(self):
        if os.environ.get("TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE") != "1":
            self.skipTest("set TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1 to run direct CCU smoke on hardware")
        rank_size = (
            os.environ.get("TILEXR_CCU_PROBE_RANK_SIZE")
            or os.environ.get("PMI_SIZE")
            or os.environ.get("OMPI_COMM_WORLD_SIZE")
            or os.environ.get("MV2_COMM_WORLD_SIZE")
            or os.environ.get("RANK_SIZE")
        )
        if rank_size is None or int(rank_size) <= 1:
            self.skipTest("direct CCU prepare smoke requires a real multi-rank TileXRComm launch")
        temp_dir, probe_bin, tile_comm_dir, cann_lib_dir, driver_lib_dir = self.compile_probe()
        try:
            env = os.environ.copy()
            env["TILEXR_CCU_DIRECT_SMOKE_ENABLE"] = "1"
            env["LD_LIBRARY_PATH"] = (
                str(tile_comm_dir)
                + os.pathsep
                + str(cann_lib_dir)
                + os.pathsep
                + str(driver_lib_dir)
                + os.pathsep
                + env.get("LD_LIBRARY_PATH", "")
            )
            result = subprocess.run(
                [str(probe_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )
        finally:
            temp_dir.cleanup()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_direct_smoke prepare ret=0", result.stdout)
        self.assertIn("submitReady=", result.stdout)


if __name__ == "__main__":
    unittest.main()
