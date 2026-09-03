#!/usr/bin/env python3

import os
import pathlib
import shutil
import shlex
import stat
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTROL = ROOT / "scripts" / "ci" / "control"


def extract_shell_function(text, name):
    marker = "{}() {{".format(name)
    start = text.index(marker)
    end = text.index("\n}\n", start) + len("\n}\n")
    return text[start:end]

class ControlSourceContractTests(unittest.TestCase):
    def read(self, relative_path):
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_control_shell_entrypoints_are_executable(self):
        for name in [
            "build_blue.sh",
            "run_hardware.sh",
            "job_completed.sh",
        ]:
            mode = (CONTROL / name).stat().st_mode
            self.assertTrue(mode & stat.S_IXUSR, name)

    def test_controller_omits_removed_output_and_queue_protocols(self):
        text = self.read("scripts/ci/control/gate.py")
        for token in [
            "StepSummaryBoundary",
            "_prepare_controller_step_summary",
            "append_authoritative_step_summary",
            "collect_artifacts.sh",
            "manifest.txt",
            "--github-token-fd",
            "fetch_current_merge_sha",
            "def npu_lock(",
            "fcntl.flock",
            "urllib.request",
        ]:
            self.assertNotIn(token, text)
        self.assertFalse((CONTROL / "collect_artifacts.sh").exists())

    def test_host_checks_do_not_create_artifact_evidence(self):
        text = self.read("scripts/ci/host_checks.sh")
        for token in [
            ".ci-artifacts",
            "cases.tsv",
            "summary.md",
            "--output-junit",
            "copy_ctest_xml",
            "validate_case_evidence",
        ]:
            self.assertNotIn(token, text)

    def test_build_manifest_enables_and_runs_all_non_hardware_coverage(self):
        text = self.read("scripts/ci/control/build_blue.sh")
        for token in [
            "/home/tilexr-ci/toolchains/cann/9.1.0",
            "-DTILEXR_BUILD_COLLECTIVES=ON",
            "-DTILEXR_BUILD_EP=ON",
            "-DTILEXR_BUILD_CHECKER=ON",
            "-DTILEXR_BUILD_TESTS=ON",
            "-DBUILD_TESTING=ON",
            '(cd "$1" && ctest --output-on-failure)',
            "test_tilexr_log",
            "test_tilexr_log_spdlog_compile",
            "test_tilexr_udma_transport_layout",
            "test_tilexr_udma_registry",
            "test_tilexr_sdma_metadata",
            "test_tilexr_sdma_api_invalid",
            "test_tilexr_sdma_transport_disabled",
            "test_tilexr_sdma_header_compile",
            "test_tilexr_ep_layout",
            "test_tilexr_ep_host_validation",
            "libascend_hal.so",
            "readelf",
            "RPATH|RUNPATH",
            "devlib",
        ]:
            self.assertIn(token, text)
        self.assertNotIn("--output-junit", text)
        self.assertNotIn("run_case sdma-disabled-comm", text)

    def test_top_level_only_adds_python_ci_tests_when_explicitly_enabled(self):
        top_level = self.read("CMakeLists.txt")
        ci_cmake = self.read("tests/ci/CMakeLists.txt")

        self.assertIn(
            "if(TILEXR_BUILD_TESTS)\n"
            "    add_subdirectory(tests/ci)\n"
            "endif()",
            top_level,
        )
        self.assertNotIn(
            "if(BUILD_TESTING OR TILEXR_BUILD_TESTS)\n"
            "    add_subdirectory(tests/data_as_flag)\n"
            "    add_subdirectory(tests/ci)",
            top_level,
        )
        self.assertIn(
            "find_package(Python3 COMPONENTS Interpreter REQUIRED)", ci_cmake
        )

    def test_live_runner_requires_python_pidfd_support(self):
        text = self.read("scripts/ci/provision/verify.sh")
        for token in [
            "sys.version_info < (3, 9)",
            'hasattr(os, "pidfd_open")',
            'hasattr(signal, "pidfd_send_signal")',
        ]:
            self.assertIn(token, text)

    def test_sealed_controller_v4_is_consistent_across_provisioning(self):
        self.assertEqual("v4\n", self.read("scripts/ci/control/VERSION"))
        common = self.read("scripts/ci/provision/common.sh")
        control = self.read("scripts/ci/provision/control.sh")
        verify = self.read("scripts/ci/provision/verify.sh")
        workflow = self.read(".github/workflows/npu-ci.yml")

        self.assertIn("CONTROL_VERSION=v4", common)
        self.assertIn(
            'CONTROL_HOME="${CI_HOME}/control/${CONTROL_VERSION}"', common
        )
        self.assertIn('stage="${control_parent}/.${CONTROL_VERSION}.new"', control)
        self.assertIn(
            '"$(< "${control_source}/VERSION")" != "${CONTROL_VERSION}"',
            control,
        )
        self.assertIn(
            '"$(< "${CONTROL_HOME}/VERSION")" == "${CONTROL_VERSION}"',
            verify,
        )
        self.assertNotIn("/home/tilexr-ci/control/current/VERSION", workflow)
        self.assertIn(
            "exec python3 /home/tilexr-ci/control/current/gate.py", workflow
        )

    def test_controller_supports_verified_stage_only_rollout(self):
        control = self.read("scripts/ci/provision/control.sh")
        for token in [
            "--stage-only",
            "STAGE_ONLY=1",
            'validate_control_package "${CONTROL_HOME}"',
            'if [[ "${STAGE_ONLY}" == 1 ]]; then',
            'echo "Staged sealed controller ${CONTROL_VERSION}; current was not changed"',
        ]:
            self.assertIn(token, control)
        self.assertLess(
            control.index('if [[ "${STAGE_ONLY}" == 1 ]]; then'),
            control.index('run ln -s "${CONTROL_HOME}" "${current_stage}"'),
        )

    def test_runner_uses_the_configured_local_github_proxy(self):
        common = self.read("scripts/ci/provision/common.sh")
        runner = self.read("scripts/ci/provision/runner.sh")
        verify = self.read("scripts/ci/provision/verify.sh")

        self.assertIn("GITHUB_PROXY=http://127.0.0.1:3128", common)
        self.assertIn("RUNNER_NO_PROXY=localhost,127.0.0.1", common)
        self.assertIn('env "${proxy_env[@]}" curl', runner)
        self.assertIn('printf \'%s\\n\' "${runner_env_entries[@]}"', runner)
        self.assertIn('for entry in "${runner_env_entries[@]}"; do', verify)

    def test_existing_cann_install_still_revalidates_blue(self):
        text = self.read("scripts/ci/provision/cann.sh")
        existing_tree = text.index(
            'if [[ "${DRY_RUN}" != 1 && ( -e "${CANN_HOME}" || -L "${CANN_HOME}" ) ]]'
        )
        live_host_check = text.index("    check_blue_host\n")
        self.assertLess(live_host_check, existing_tree)

    def test_provisioning_enforces_the_documented_minimum_driver_version(self):
        common = self.read("scripts/ci/provision/common.sh")
        cann = self.read("scripts/ci/provision/cann.sh")
        verify = self.read("scripts/ci/provision/verify.sh")

        self.assertIn("version_at_least()", common)
        for text in [cann, verify]:
            self.assertIn('version_at_least "${driver_version}" 25.1.rc1', text)
            self.assertNotIn('"${driver_version}" != 25.1.rc1', text)
        self.assertNotIn("grep -Fx Version=25.1.rc1", cann)

    def test_cann_paths_use_installer_required_permissions(self):
        common = self.read("scripts/ci/provision/common.sh")
        account = self.read("scripts/ci/provision/account.sh")
        cann = self.read("scripts/ci/provision/cann.sh")

        self.assertEqual(3, common.count(" 755"))
        continuation = "\\" + "\n"
        self.assertIn('-m 0755 ' + continuation + '    "${CI_HOME}"', account)
        self.assertIn(
            '-m 0755 '
            + continuation
            + '    "${toolchains_home}" "${toolchain_parent}"',
            account,
        )
        self.assertIn('mkdir -m 0755 -- "${CANN_HOME}"', cann)
        self.assertIn("u+rwX,g+rX,o+rX,go-w", cann)

    def test_account_keeps_runner_installation_admin_owned(self):
        account = self.read("scripts/ci/provision/account.sh")
        continuation = "\\" + "\n"

        self.assertIn(
            '-o root -g "${CI_PRIMARY_GROUP}" -m 0750 '
            + continuation
            + '    "${RUNNER_HOME}"',
            account,
        )
        self.assertIn(
            '-o "${CI_USER}" -g "${CI_PRIMARY_GROUP}" -m 0750 '
            + continuation
            + '    "${runner_work}"',
            account,
        )
        self.assertNotIn(
            'run install -d -o "${CI_USER}" -g "${CI_PRIMARY_GROUP}" -m 0750 '
            + continuation
            + '    "${RUNNER_HOME}" "${runner_work}"',
            account,
        )

    def test_cann_installer_uses_home_backed_tmpdir(self):
        cann = self.read("scripts/ci/provision/cann.sh")
        tmpdir = 'TMPDIR="${stage}/tmp"'
        installer = 'bash "${stage}/scripts/cann_download_install.sh"'

        self.assertIn('"${stage}/scripts" "${stage}/tmp"', cann)
        self.assertIn(tmpdir, cann)
        self.assertLess(cann.index(tmpdir), cann.index(installer))

    def test_control_archive_uses_an_explicit_real_git_directory(self):
        control = self.read("scripts/ci/provision/control.sh")
        self.assertIn('[[ -d "${git_dir}" && ! -L "${git_dir}" ]]', control)
        self.assertIn(
            'git --git-dir="${git_dir}" archive',
            control,
        )
        self.assertIn("archive --format=tar HEAD scripts/ci/control", control)
        self.assertNotIn("safe.directory", control)

    def test_runner_has_no_preloaded_release_asset_fallback(self):
        runner = self.read("scripts/ci/provision/runner.sh")
        self.assertNotIn("TILEXR_CI_RUNNER_ASSET", runner)
        self.assertNotIn("preloaded_asset", runner)

    def test_runner_tools_execute_from_runner_root_and_accept_bom_config(self):
        runner = self.read("scripts/ci/provision/runner.sh")
        self.assertIn('encoding="utf-8-sig"', runner)
        self.assertIn("run_in_runner_home()", runner)
        self.assertIn("run_as_ci_in_runner_home()", runner)
        token_call = runner.index(
            'run_with_runner_registration_token "${registration_token}"'
        )
        self.assertIn("run_as_ci_in_runner_home", runner[token_call:token_call + 200])
        self.assertIn(
            'run run_in_runner_home "${RUNNER_HOME}/svc.sh" install', runner
        )
        self.assertIn(
            'run run_in_runner_home "${RUNNER_HOME}/svc.sh" start', runner
        )

    def test_runner_converges_selinux_entrypoint_and_requires_active_service(self):
        runner = self.read("scripts/ci/provision/runner.sh")
        verify = self.read("scripts/ci/provision/verify.sh")

        self.assertIn('chcon -t bin_t "${RUNNER_HOME}/runsvc.sh"', runner)
        self.assertIn('systemctl is-active --quiet -- "${service_name}"', runner)
        self.assertIn("runner service failed to become active", runner)
        self.assertIn("runner service entrypoint has an unexpected SELinux type", verify)
        self.assertIn(
            "git ls-remote https://github.com/LingquLab/TileXR.git HEAD", verify
        )

    def test_verify_accepts_locked_password_states_across_supported_hosts(self):
        verify = self.read("scripts/ci/provision/verify.sh")
        self.assertIn(
            '"${password_state}" == L || "${password_state}" == LK', verify
        )

    def test_runner_forces_git_http11_for_jobs_and_verification(self):
        runner = self.read("scripts/ci/provision/runner.sh")
        verify = self.read("scripts/ci/provision/verify.sh")
        for entry in [
            "GIT_CONFIG_COUNT=1",
            "GIT_CONFIG_KEY_0=http.version",
            "GIT_CONFIG_VALUE_0=HTTP/1.1",
        ]:
            self.assertIn(entry, runner)
            self.assertIn(entry, verify)
        self.assertIn('"${runner_env_entries[@]}"', runner)

    def test_manifests_never_source_pull_request_code_into_trusted_shell(self):
        for relative in [
            "scripts/ci/control/build_blue.sh",
            "scripts/ci/control/run_hardware.sh",
        ]:
            text = self.read(relative)
            self.assertNotIn('source "${SOURCE_DIR}/', text)
            sealed_source = (
                "source /home/tilexr-ci/toolchains/cann/9.1.0/cann/set_env.sh"
            )
            self.assertIn(sealed_source, text)
            self.assertLess(text.index(sealed_source), text.index("run_case()"))
            self.assertIn("trusted-environment", text)

    def test_manifests_prioritize_freshly_built_libraries(self):
        for relative in [
            "scripts/ci/control/build_blue.sh",
            "scripts/ci/control/run_hardware.sh",
        ]:
            text = self.read(relative)
            assignment = (
                'SOURCE_LIBRARY_PATH="${SOURCE_DIR}/install/lib64:'
                '${SOURCE_DIR}/install/lib"'
            )
            self.assertIn(assignment, text)
            export_line = next(
                line for line in text.splitlines()
                if line.startswith("export LD_LIBRARY_PATH=")
            )
            self.assertLess(
                export_line.index("${SOURCE_LIBRARY_PATH}"),
                export_line.index("${ASCEND_DRIVER_PATH}"),
            )
            self.assertIn("for test_install in comm udma sdma ep memory", text)
            self.assertIn('${SOURCE_DIR}/tests/${test_install}/install/lib64', text)
            self.assertIn('${SOURCE_DIR}/tests/${test_install}/install/lib', text)

    def test_build_manifest_requires_every_hardware_binary(self):
        text = self.read("scripts/ci/control/build_blue.sh")
        for token in [
            "test_tilexr_udma",
            "tilexr_memory_demo",
            "tilexr_ep_dispatch_demo",
            "tilexr_sdma_demo",
            "test_tilexr_collectives_correctness",
            "tilexr_collective_perf",
        ]:
            self.assertIn(token, text)
        self.assertIn("test -x", text)

    def test_build_manifest_validates_installed_headers_libraries_and_checker(self):
        text = self.read("scripts/ci/control/build_blue.sh")
        for token in [
            "tilexr_api.h",
            "tilexr_types.h",
            "comm_args.h",
            "tilexr_sync.h",
            "tilexr_udma.h",
            "tilexr_sdma.h",
            "tilexr_ep.h",
            "tilexr_collectives.h",
            "tilexr_collectives_perf.h",
            "libtile-comm.so",
            "libtilexr-ep.so",
            "libtilexr-collectives.so",
            "install/bin/tilexr_checker",
            "libtilexr-checker-core.a",
            "validate_dynamic_output",
            "ldd-${label}.txt",
            "readelf-${label}.txt",
            "top-level-reinstall",
        ]:
            self.assertIn(token, text)
        self.assertNotIn("build-ci/tools/checker/tilexr_checker", text)
        checker_cmake = self.read("tools/checker/CMakeLists.txt")
        for token in [
            "install(TARGETS",
            "tilexr_checker",
            "tilexr-checker-core",
            "${CMAKE_INSTALL_BINDIR}",
            "${CMAKE_INSTALL_LIBDIR}",
        ]:
            self.assertIn(token, checker_cmake)

    def test_hardware_manifest_has_required_8_card_coverage(self):
        text = self.read("scripts/ci/control/run_hardware.sh")
        for token in [
            "TILEXR_TEST_DEVICES=0,1,2,3,4,5,6,7",
            "TILEXR_AVAILABLE_NPUS=8",
            "TILEXR_SKIP_IF_INSUFFICIENT_NPUS=0",
            "TILEXR_COLLECTIVES_RUN_TIMEOUT_SEC=600",
            "test_tilexr_udma",
            "run_tilexr_memory_demo.sh",
            "allgather allreduce reducescatter",
            "broadcast --root 0",
            "broadcast --root 7",
            "64 4096 1048576",
            "--check 1",
            "test_tilexr_sdma_disabled_comm",
            "npu-smi info -t health -i",
            "/usr/bin/timeout --signal=TERM --kill-after=2 10",
            "Health",
            "OK",
        ]:
            self.assertIn(token, text)

    def test_910b_hardware_manifest_excludes_a5_only_ep_memory(self):
        text = self.read("scripts/ci/control/run_hardware.sh")
        self.assertNotIn("run_tilexr_ep_dispatch_demo.sh", text)
        self.assertIn("EP Dispatch/Combine Memory: out of scope on 910B3", text)

    def test_every_multirank_launch_has_external_ten_minute_timeout(self):
        text = self.read("scripts/ci/control/run_hardware.sh")
        timeout = "/usr/bin/timeout --signal=TERM --kill-after=10 600"
        # UDMA MPI, memory, correctness loop, two broadcasts, and perf loop.
        self.assertGreaterEqual(text.count(timeout), 7)
        for marker in [
            "mpirun -n 8",
            "run_tilexr_memory_demo.sh",
            "run_collectives_correctness.sh",
            "run_collective_perf.sh",
        ]:
            position = text.find(marker)
            self.assertNotEqual(-1, position)
            prefix = text[max(0, position - 500) : position]
            self.assertIn(timeout, prefix)

    def test_hardware_manifest_records_stable_cases_and_scope(self):
        text = self.read("scripts/ci/control/run_hardware.sh")
        for token in [
            "run_case()",
            "cases.tsv",
            "udma-single-rank-fallback",
            "udma-eight-rank-fallback",
            "memory-eight-rank",
            "collectives-correctness-${op}",
            "collectives-correctness-broadcast-root-0",
            "collectives-correctness-broadcast-root-7",
            "collectives-perf-${op}",
            "sdma-device-${device}",
        ]:
            self.assertIn(token, text)
        self.assertNotIn("run_tilexr_udma_demo.sh", text)
        self.assertIn("UDMA data plane: out of scope on 910B3", text)
        self.assertIn("EP Dispatch/Combine Memory: out of scope on 910B3", text)
        self.assertIn("Multi-host validation: out of scope for this gate", text)
        self.assertIn("vLLM model inference: out of scope for this gate", text)
        self.assertIn("Performance regression thresholds: out of scope for this gate", text)

    def test_hardware_manifest_captures_bounded_state_topology_and_cann_evidence(self):
        hardware = self.read("scripts/ci/control/run_hardware.sh")
        build = self.read("scripts/ci/control/build_blue.sh")
        for token in [
            "npu-state-before.txt",
            "npu-state-after.txt",
            "npu-topology.txt",
            "trap finalize_hardware EXIT",
            "/usr/bin/timeout --signal=TERM --kill-after=2 10",
        ]:
            self.assertIn(token, hardware)
        for token in [
            "version-cann.txt",
            "${ASCEND_HOME_PATH}/${ARCH}-linux/ascend_toolkit_install.info",
            "ascend_toolkit_install.info",
            "ascend_ops_install.info",
            "Ascend-cann-toolkit",
            "Ascend-cann-910b-ops",
            "version=9.1.0",
        ]:
            self.assertIn(token, build)
        self.assertNotIn("9\\.1([.]0)?", build)

    def test_build_manifest_resets_cases_and_records_each_safe_test(self):
        text = self.read("scripts/ci/control/build_blue.sh")
        self.assertIn(': > "${CASES_FILE}"', text)
        self.assertIn("run_case()", text)
        self.assertGreaterEqual(text.count("run_case "), 11)
        self.assertIn("printf '%s\\t%s\\t%s\\t%s\\n'", text)

    def test_build_manifest_captures_configuration_and_build_logs(self):
        text = self.read("scripts/ci/control/build_blue.sh")
        self.assertIn("run_logged_step()", text)
        for name in [
            "top-level-configure",
            "top-level-build",
            "comm-build",
            "udma-build",
            "sdma-build",
            "ep-build",
            "memory-configure",
            "memory-build",
        ]:
            self.assertIn('run_logged_step "{}"'.format(name), text)

    def test_job_hook_uses_fixed_workspace_and_absolute_tools(self):
        text = self.read("scripts/ci/control/job_completed.sh")
        self.assertIn("/home/tilexr-ci/actions-runner/_work/TileXR/TileXR", text)
        self.assertIn("/home/tilexr-ci/actions-runner/_work", text)
        self.assertIn("/usr/bin/find", text)
        self.assertIn("/usr/bin/rm", text)
        self.assertNotIn("TILEXR_CI_WORKSPACE", text)

    def test_job_hook_requires_exact_canonical_fixed_paths(self):
        text = self.read("scripts/ci/control/job_completed.sh")
        self.assertTrue(text.startswith("#!/bin/bash\n"))
        self.assertIn(
            '[[ "${RUNNER_WORK_ROOT_REAL}" != "${RUNNER_WORK_ROOT}" ]]', text
        )
        self.assertIn(
            '[[ "${WORKSPACE_PARENT_REAL}" != "${WORKSPACE_PARENT}" ]]', text
        )
        self.assertIn('[[ "${WORKSPACE_REAL}" != "${WORKSPACE}" ]]', text)

    def test_host_checks_run_complete_fast_coverage(self):
        text = self.read("scripts/ci/host_checks.sh")
        for token in [
            "set -euo pipefail",
            "TILEXR_CI_BUILD_ROOT",
            'git -C "${ROOT_DIR}" ls-files -z --',
            'bash -n "${tracked_shell_files[@]}"',
            'cmake -S "${ROOT_DIR}/tests/ci"',
            "ctest --output-on-failure",
            'cmake -S "${ROOT_DIR}/tests/comm"',
            "test_tilexr_log",
            "test_tilexr_log_spdlog_compile",
            'cmake -S "${ROOT_DIR}/tests/ep"',
            "-DBUILD_TILEXR_EP_DEMO=OFF",
            'cmake -S "${ROOT_DIR}/tests/data_as_flag"',
            "test_vllm_collectives_patch.py",
            "test_collective_profile_report.py",
            "CASE_NAMES=()",
            "run_case()",
            "trap finalize_host_checks EXIT",
            "GITHUB_STEP_SUMMARY",
            "TILEXR_CI_PR_NUMBER",
            "TILEXR_CI_HEAD_SHA",
            "TILEXR_CI_BASE_SHA",
            "TILEXR_CI_EXPECTED_MERGE_SHA",
            "NPU-only coverage is not run by Host Checks",
        ]:
            self.assertIn(token, text)
        for suite in [
            "shell-syntax",
            "ci-ctest",
            "comm-host",
            "ep-host",
            "data-as-flag",
            "collectives-vllm-patch",
            "collectives-profile-report",
        ]:
            self.assertIn("run_and_accumulate_case {} ".format(suite), text)
        self.assertGreaterEqual(text.count(":-local}"), 4)

    def test_host_check_generated_root_is_ignored_at_repository_root(self):
        rules = self.read(".gitignore").splitlines()
        self.assertIn("/.ci-build/", rules)
        self.assertNotIn("/.ci-artifacts/", rules)


class ProvisioningHelperBehaviorTests(unittest.TestCase):
    def test_runner_dry_run_does_not_require_registration_token_input(self):
        result = subprocess.run(
            ["bash", str(ROOT / "scripts/ci/provision/runner.sh"), "--dry-run"],
            input="",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("registration-token", result.stdout)
        self.assertNotIn("registration token is required", result.stderr)

    def test_version_at_least_compares_release_and_rc_components(self):
        common = ROOT / "scripts/ci/provision/common.sh"
        harness = 'source "$1"; version_at_least "$2" 25.1.rc1'

        def supported(version):
            return subprocess.run(
                ["bash", "-c", harness, "version-check", str(common), version],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        for version in [
            "25.1.rc1",
            "25.1.rc1.b188",
            "25.1.rc2",
            "25.1.0",
            "25.5.0",
            "26.0.0",
        ]:
            with self.subTest(version=version):
                self.assertEqual(0, supported(version).returncode)
        for version in [
            "25.1.rc0",
            "25.0.99",
            "24.99.99",
            "25.1",
            "25.1.rc1.b",
            "25.1.RC1",
            "",
        ]:
            with self.subTest(version=version):
                self.assertNotEqual(0, supported(version).returncode)


class HardwareHelperBehaviorTests(unittest.TestCase):
    def hardware_function(self, name):
        text = (CONTROL / "run_hardware.sh").read_text(encoding="utf-8")
        return extract_shell_function(text, name)

    def test_required_no_skip_wrapper_rejects_skip_and_accepts_real_pass(self):
        hardware = (CONTROL / "run_hardware.sh").read_text(encoding="utf-8")
        self.assertIn("run_case sdma-disabled-comm run_required_no_skip", hardware)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            trusted = root / "trusted"
            trusted.mkdir()
            skipped = root / "skipped.sh"
            skipped.write_text(
                "#!/bin/bash\necho 'Skip test_tilexr_sdma_disabled_comm: aclInit failed'\nexit 0\n"
            )
            passed = root / "passed.sh"
            passed.write_text("#!/bin/bash\necho passed\nexit 0\n")
            skipped.chmod(0o755)
            passed.chmod(0o755)
            function = self.hardware_function("run_required_no_skip")

            def invoke(command):
                harness = "set -euo pipefail\nTRUSTED_ENV_ROOT={}\n{}\n".format(
                    shlex.quote(str(trusted)), function
                )
                harness += "run_required_no_skip 'Skip test_tilexr_sdma_disabled_comm:' {}\n".format(
                    shlex.quote(str(command))
                )
                return subprocess.run(
                    ["bash", "-c", harness],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertNotEqual(0, invoke(skipped).returncode)
            self.assertEqual(0, invoke(passed).returncode)

    def test_health_parser_uses_first_exact_top_level_field(self):
        hardware = (CONTROL / "run_hardware.sh").read_text(encoding="utf-8")
        self.assertIn("| parse_top_level_health)", hardware)
        function = self.hardware_function("parse_top_level_health")

        def parse(output):
            harness = "{}\nprintf '%s' {} | parse_top_level_health\n".format(
                function, shlex.quote(output)
            )
            return subprocess.run(
                ["bash", "-c", harness],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        ok = "Health : OK\nMCU Health : Warning\n"
        misleading = "Health : Warning\nMCU Health : OK\n"
        self.assertEqual("OK", parse(ok).stdout.strip())
        self.assertEqual("Warning", parse(misleading).stdout.strip())

    def test_exit_evidence_failure_preserves_primary_and_fails_success(self):
        function = self.hardware_function("finalize_hardware")
        with tempfile.TemporaryDirectory() as temporary:
            prefix = "ARTIFACT_DIR={}\n{}\n".format(
                shlex.quote(temporary), function
            )
            prefix += "capture_npu_evidence() { return 9; }\n"
            failed = subprocess.run(["bash", "-c", prefix + "false; finalize_hardware"])
            passed = subprocess.run(["bash", "-c", prefix + "true; finalize_hardware"])
            self.assertEqual(1, failed.returncode)
            self.assertEqual(9, passed.returncode)

    def test_run_directory_rejects_file_and_symlink_without_running_command(self):
        hardware = (CONTROL / "run_hardware.sh").read_text(encoding="utf-8")
        prepare = extract_shell_function(hardware, "prepare_ci_run_root")
        run_in_dir = extract_shell_function(hardware, "run_in_dir")
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary).resolve()
            marker = source / "command-ran"

            def invoke(setup, directory, after_prepare=""):
                harness = "set -euo pipefail\nSOURCE_DIR={}\n{}\n{}\n".format(
                    shlex.quote(str(source)), prepare, run_in_dir
                )
                harness += setup + "\n"
                harness += "prepare_ci_run_root\n"
                harness += after_prepare + "\n"
                harness += "run_in_dir {} /usr/bin/touch {}\n".format(
                    shlex.quote(str(directory)), shlex.quote(str(marker))
                )
                return subprocess.run(
                    ["bash", "-c", harness],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            ci_root = source / ".ci-run"
            symlink_setup = "mkdir -p {0}; ln -s {0} {1}".format(
                shlex.quote(str(source / "outside")), shlex.quote(str(ci_root))
            )
            self.assertNotEqual(0, invoke(symlink_setup, ci_root / "case").returncode)
            self.assertFalse(marker.exists())

            file_case = ci_root / "case-file"
            file_setup = "touch {}".format(shlex.quote(str(file_case)))
            self.assertNotEqual(
                0,
                invoke(
                    "mkdir -p {}".format(shlex.quote(str(ci_root))),
                    file_case,
                    file_setup,
                ).returncode,
            )
            self.assertFalse(marker.exists())

            linked_case = ci_root / "case-link"
            link_setup = "mkdir -p {0}; ln -s {0} {1}".format(
                shlex.quote(str(source / "outside-case")),
                shlex.quote(str(linked_case)),
            )
            self.assertNotEqual(
                0,
                invoke(
                    "mkdir -p {}".format(shlex.quote(str(ci_root))),
                    linked_case,
                    link_setup,
                ).returncode,
            )
            self.assertFalse(marker.exists())

class HostChecksBehaviorTests(unittest.TestCase):
    def materialize_host_checks(self, root):
        destination = root / "scripts" / "ci" / "host_checks.sh"
        destination.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/ci/host_checks.sh", destination)
        destination.chmod(0o755)
        return destination

    def run_host_checks(self, script, environment=None, intercept_rm=False):
        root = script.parents[2]
        fake_bin = root / "fake-bin"
        fake_bin.mkdir(exist_ok=True)
        fake_git = fake_bin / "git"
        fake_git.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
        fake_git.chmod(0o755)

        rm_log = root / "rm-invocations"
        if intercept_rm:
            fake_rm = fake_bin / "rm"
            fake_rm.write_text(
                "#!/bin/bash\n"
                "printf '%s\\n' \"$*\" >> \"${TILEXR_TEST_RM_LOG:?}\"\n"
                "exit 97\n",
                encoding="utf-8",
            )
            fake_rm.chmod(0o755)

        command_environment = dict(os.environ)
        command_environment.pop("TILEXR_CI_BUILD_ROOT", None)
        command_environment.pop("RUNNER_TEMP", None)
        command_environment["PATH"] = "{}:{}".format(
            fake_bin, command_environment["PATH"]
        )
        if intercept_rm:
            command_environment["TILEXR_TEST_RM_LOG"] = str(rm_log)
        if environment:
            command_environment.update(environment)
        result = subprocess.run(
            ["bash", str(script)],
            cwd=str(root),
            env=command_environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return result, rm_log

    def test_run_case_stops_at_and_reports_an_intermediate_failure(self):
        source = (ROOT / "scripts/ci/host_checks.sh").read_text(encoding="utf-8")
        function = extract_shell_function(source, "run_case")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            after_failure = root / "after-failure"
            harness = (
                "set -euo pipefail\n"
                "CASE_NAMES=()\nCASE_RESULTS=()\nCASE_STATUSES=()\nCASE_DURATIONS=()\n"
                + function
            )
            harness += "suite() {{ false; touch {}; }}\n".format(
                shlex.quote(str(after_failure))
            )
            harness += "run_case sample suite\n"
            result = subprocess.run(
                ["bash", "-c", harness],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertNotEqual(0, result.returncode)
            self.assertFalse(after_failure.exists())
            self.assertIn("[host-check] sample: FAIL (exit=1", result.stdout)


    def test_build_root_rejects_arbitrary_and_non_absolute_paths_before_rm(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            cases = [
                "",
                "relative/tilexr-host-build",
                str(root / "scripts"),
                "/tmp",
                "/home",
            ]
            for build_root in cases:
                with self.subTest(build_root=build_root):
                    rm_log = root / "rm-invocations"
                    if rm_log.exists():
                        rm_log.unlink()
                    result, rm_log = self.run_host_checks(
                        script,
                        {"TILEXR_CI_BUILD_ROOT": build_root},
                        intercept_rm=True,
                    )
                    self.assertNotEqual(0, result.returncode)
                    self.assertFalse(rm_log.exists(), result.stderr)

    def test_build_root_rejects_symlinked_allowed_parent_before_rm(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            real_runner_temp = root / "real-runner-temp"
            real_runner_temp.mkdir()
            runner_temp_link = root / "runner-temp-link"
            runner_temp_link.symlink_to(real_runner_temp, target_is_directory=True)
            build_root = runner_temp_link / "tilexr-host-build"

            result, rm_log = self.run_host_checks(
                script,
                {
                    "RUNNER_TEMP": str(runner_temp_link),
                    "TILEXR_CI_BUILD_ROOT": str(build_root),
                },
                intercept_rm=True,
            )

            self.assertNotEqual(0, result.returncode)
            self.assertFalse(rm_log.exists(), result.stderr)

    def test_build_root_rejects_symlinked_repo_namespace_before_rm(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            outside = root / "outside"
            outside.mkdir()
            marker = outside / "tilexr-host-default" / "keep"
            marker.parent.mkdir()
            marker.write_text("keep", encoding="utf-8")
            (root / ".ci-build").symlink_to(outside, target_is_directory=True)

            result, rm_log = self.run_host_checks(
                script, intercept_rm=True
            )

            self.assertNotEqual(0, result.returncode)
            self.assertFalse(rm_log.exists(), result.stderr)
            self.assertEqual("keep", marker.read_text(encoding="utf-8"))

    def test_build_root_rejects_a_symlink_candidate_without_unlinking_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            runner_temp = root / "runner-temp"
            outside = root / "outside"
            runner_temp.mkdir()
            outside.mkdir()
            marker = outside / "keep"
            marker.write_text("keep", encoding="utf-8")
            build_root = runner_temp / "tilexr-host-linked"
            build_root.symlink_to(outside, target_is_directory=True)

            result, _ = self.run_host_checks(
                script,
                {
                    "RUNNER_TEMP": str(runner_temp),
                    "TILEXR_CI_BUILD_ROOT": str(build_root),
                },
            )

            self.assertNotEqual(0, result.returncode)
            self.assertTrue(build_root.is_symlink())
            self.assertEqual("keep", marker.read_text(encoding="utf-8"))

    def test_build_root_accepts_default_and_owned_direct_child_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            runner_temp = root / "runner-temp"
            runner_temp.mkdir()
            build_roots = [
                root / ".ci-build" / "tilexr-host-default",
                root / ".ci-build" / ".tilexr-host-local",
                runner_temp / "tilexr-host-runner",
            ]
            for index, build_root in enumerate(build_roots):
                with self.subTest(build_root=build_root):
                    build_root.mkdir(parents=True, exist_ok=True)
                    marker = build_root / "remove-me"
                    marker.write_text("remove", encoding="utf-8")
                    environment = {"RUNNER_TEMP": str(runner_temp)}
                    if index > 0:
                        environment["TILEXR_CI_BUILD_ROOT"] = str(build_root)
                    result, _ = self.run_host_checks(script, environment)
                    self.assertNotEqual(0, result.returncode)
                    self.assertFalse(marker.exists())
                    self.assertTrue(build_root.is_dir())

    def test_host_checks_runs_all_cases_after_an_intermediate_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            script = self.materialize_host_checks(root)
            fake_bin = root / "fake-bin"
            fake_bin.mkdir()
            tracked_shell = root / "tracked.sh"
            tracked_shell.write_text("#!/bin/bash\ntrue\n", encoding="utf-8")
            later_case = root / "later-case-ran"
            build_root = root / ".ci-build" / "tilexr-host-default"
            comm_bin = build_root / "comm-install" / "bin"

            fake_git = fake_bin / "git"
            fake_git.write_text(
                "#!/bin/bash\nprintf 'tracked.sh\\0'\n", encoding="utf-8"
            )
            fake_git.chmod(0o755)

            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text(
                "#!/bin/bash\n"
                "set -euo pipefail\n"
                "build_dir=''\n"
                "while [[ $# -gt 0 ]]; do\n"
                "  if [[ \"$1\" == '-B' ]]; then\n"
                "    shift\n"
                "    build_dir=\"$1\"\n"
                "  fi\n"
                "  shift\n"
                "done\n"
                "if [[ -n \"$build_dir\" ]]; then\n"
                "  mkdir -p \"$build_dir/Testing/tag\"\n"
                "  printf '<testsuite/>\\n' > \"$build_dir/Testing/tag/Test.xml\"\n"
                "fi\n"
                "mkdir -p \"${TILEXR_TEST_COMM_BIN:?}\"\n"
                "for name in test_tilexr_log test_tilexr_log_spdlog_compile; do\n"
                "  printf '#!/bin/bash\\nexit 0\\n' > "
                "\"${TILEXR_TEST_COMM_BIN}/$name\"\n"
                "  chmod 755 \"${TILEXR_TEST_COMM_BIN}/$name\"\n"
                "done\n",
                encoding="utf-8",
            )
            fake_cmake.chmod(0o755)

            fake_ctest = fake_bin / "ctest"
            fake_ctest.write_text(
                "#!/bin/bash\n"
                "if [[ \"$PWD\" == */ep ]]; then exit 23; fi\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_ctest.chmod(0o755)

            fake_python = fake_bin / "python3"
            fake_python.write_text(
                "#!/bin/bash\n"
                "set -euo pipefail\n"
                "if [[ \"${1:-}\" == '-m' && \"${2:-}\" == 'pytest' ]]; then\n"
                "  exit 0\n"
                "fi\n"
                "if [[ \"${1:-}\" == *test_collective_profile_report.py ]]; then\n"
                "  touch \"${TILEXR_TEST_LATER_CASE:?}\"\n"
                "  exit 31\n"
                "fi\n"
                "exec \"${TILEXR_TEST_REAL_PYTHON:?}\" \"$@\"\n",
                encoding="utf-8",
            )
            fake_python.chmod(0o755)

            environment = dict(os.environ)
            environment.pop("TILEXR_CI_BUILD_ROOT", None)
            environment.pop("RUNNER_TEMP", None)
            environment["PATH"] = "{}:{}".format(
                fake_bin, environment["PATH"]
            )
            environment["TILEXR_TEST_COMM_BIN"] = str(comm_bin)
            environment["TILEXR_TEST_LATER_CASE"] = str(later_case)
            environment["TILEXR_TEST_REAL_PYTHON"] = shutil.which("python3")
            result = subprocess.run(
                ["/bin/bash", str(script)],
                cwd=str(root),
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertNotEqual(0, result.returncode)
            self.assertTrue(later_case.is_file())

            self.assertNotEqual(0, result.returncode)
            self.assertTrue(later_case.is_file())
            case_lines = [
                line for line in result.stdout.splitlines()
                if line.startswith("[host-check] ")
            ]
            self.assertEqual(7, len(case_lines))
            expected = [
                "shell-syntax",
                "ci-ctest",
                "comm-host",
                "ep-host",
                "data-as-flag",
                "collectives-vllm-patch",
                "collectives-profile-report",
            ]
            self.assertEqual(
                expected,
                [line.split(":", 1)[0][len("[host-check] "):] for line in case_lines],
            )
            self.assertIn("[host-check] ep-host: FAIL (exit=23", result.stdout)
            self.assertIn(
                "[host-check] collectives-profile-report: FAIL (exit=31",
                result.stdout,
            )
            self.assertIn("Host cases: 7 total, 5 passed, 2 failed", result.stdout)
            self.assertIn(
                "Failed cases: ep-host, collectives-profile-report",
                result.stdout,
            )


class BuildHelperBehaviorTests(unittest.TestCase):
    def test_cann_metadata_requires_exact_package_and_version(self):
        build = (CONTROL / "build_blue.sh").read_text(encoding="utf-8")
        function = extract_shell_function(build, "validate_cann_metadata_file")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)

            def validate(contents, package):
                metadata = root / "metadata.info"
                metadata.write_text(contents)
                harness = "{}\nvalidate_cann_metadata_file {} {}\n".format(
                    function, shlex.quote(str(metadata)), shlex.quote(package)
                )
                return subprocess.run(
                    ["bash", "-c", harness],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            valid = "package_name=Ascend-cann-toolkit\nversion=9.1.0\n"
            wrong_version = "package_name=Ascend-cann-toolkit\nversion=9.10.0\n"
            wrong_package = "package_name=Ascend-cann-910b-ops\nversion=9.1.0\n"
            conflicting = (
                "package_name=Ascend-cann-toolkit\n"
                "version=9.1.0\nversion=9.10.0\n"
            )
            self.assertEqual(0, validate(valid, "Ascend-cann-toolkit").returncode)
            self.assertNotEqual(
                0, validate(wrong_version, "Ascend-cann-toolkit").returncode
            )
            self.assertNotEqual(
                0, validate(wrong_package, "Ascend-cann-toolkit").returncode
            )
            self.assertNotEqual(
                0, validate(conflicting, "Ascend-cann-toolkit").returncode
            )

class JobHookBehaviorTests(unittest.TestCase):
    def materialize_hook(self, root, runner_work_root):
        runner_work_root = runner_work_root.resolve()
        source = (CONTROL / "job_completed.sh").read_text(encoding="utf-8")
        source = source.replace(
            "/home/tilexr-ci/actions-runner/_work", str(runner_work_root)
        )
        if not pathlib.Path("/usr/bin/rm").exists():
            source = source.replace("/usr/bin/rm", shutil.which("rm") or "/bin/rm")
        hook = root / "job_completed-test.sh"
        hook.write_text(source, encoding="utf-8")
        hook.chmod(hook.stat().st_mode | stat.S_IXUSR)
        return hook

    def test_job_hook_cleans_children_without_following_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runner_root = root / "runner" / "_work"
            workspace = runner_root / "TileXR" / "TileXR"
            external = root / "external"
            artifacts = root / "artifacts"
            (workspace / "nested").mkdir(parents=True)
            external.mkdir()
            artifacts.mkdir()
            (workspace / "nested" / "file").write_text("delete")
            (workspace / "outside-link").symlink_to(external, target_is_directory=True)
            (external / "keep").write_text("keep")
            (artifacts / "keep.xml").write_text("keep")
            hook = self.materialize_hook(root, runner_root)

            result = subprocess.run(
                [str(hook)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue(workspace.is_dir())
            self.assertEqual([], list(workspace.iterdir()))
            self.assertEqual("keep", (external / "keep").read_text())
            self.assertEqual("keep", (artifacts / "keep.xml").read_text())

    def test_job_hook_refuses_a_symlink_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runner_root = root / "runner" / "_work"
            workspace_parent = runner_root / "TileXR"
            target = root / "target"
            workspace_parent.mkdir(parents=True)
            target.mkdir()
            (target / "keep").write_text("keep")
            (workspace_parent / "TileXR").symlink_to(target, target_is_directory=True)
            hook = self.materialize_hook(root, runner_root)

            result = subprocess.run(
                [str(hook)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("keep", (target / "keep").read_text())

    def test_job_hook_refuses_a_dangling_symlink_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runner_root = root / "runner" / "_work"
            workspace_parent = runner_root / "TileXR"
            workspace_parent.mkdir(parents=True)
            (workspace_parent / "TileXR").symlink_to(root / "missing")
            hook = self.materialize_hook(root, runner_root)

            result = subprocess.run(
                [str(hook)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertNotEqual(0, result.returncode)

    def test_job_hook_refuses_an_intermediate_symlink_inside_work_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runner_root = root / "runner" / "_work"
            redirected = runner_root / "redirected" / "TileXR"
            redirected.mkdir(parents=True)
            (redirected / "keep").write_text("keep")
            (runner_root / "TileXR").symlink_to(
                runner_root / "redirected", target_is_directory=True
            )
            hook = self.materialize_hook(root, runner_root)

            result = subprocess.run(
                [str(hook)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("keep", (redirected / "keep").read_text())

    def test_job_hook_refuses_a_dangling_intermediate_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runner_root = root / "runner" / "_work"
            runner_root.mkdir(parents=True)
            (runner_root / "TileXR").symlink_to(
                runner_root / "missing", target_is_directory=True
            )
            hook = self.materialize_hook(root, runner_root)

            result = subprocess.run(
                [str(hook)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertNotEqual(0, result.returncode)
if __name__ == "__main__":
    unittest.main()
