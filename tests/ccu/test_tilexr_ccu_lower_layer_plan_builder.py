#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import shutil
import json
import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILDER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_plan_builder.h"
BUILDER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_plan_builder.cpp"
DIRECT_RUNTIME_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_runtime.h"
DIRECT_RUNTIME_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_runtime.cpp"
TOPOLOGY_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_topology.cpp"
PAYLOAD_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_payloads.cpp"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
ALLOCATOR_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_resource_allocator.cpp"
PRODUCER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
COMM_HEADER_FILE = REPO_ROOT / "src" / "comm" / "tilexr_comm.h"
COMM_SOURCE_FILE = REPO_ROOT / "src" / "comm" / "tilexr_comm.cpp"
CCU_BACKEND_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_backend.h"
CCU_BACKEND_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_backend.cpp"
CCU_RUNTIME_SESSION_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime_session.h"
CCU_RUNTIME_SESSION_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime_session.cpp"
CCU_PLANNER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_collective_planner.h"
CCU_PLANNER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_collective_planner.cpp"
CCU_EXECUTOR_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_executor.h"
CCU_EXECUTOR_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_executor.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuLowerLayerPlanBuilderTest(unittest.TestCase):
    def setUp(self):
        runtime_integration_prefixes = (
            "test_plan_builder_surface_",
            "test_tilexr_comm_",
            "test_direct_ccu_runtime_",
            "test_remote_xn_",
            "test_remote_notify_",
            "test_peer_xn_",
        )
        if (not CCU_BACKEND_SOURCE.exists() and
                self._testMethodName.startswith(runtime_integration_prefixes)):
            self.skipTest("direct CCU runtime integration is introduced by the next stack layer")

    def compile_and_run(self, code: str, env=None, extra_sources=None, extra_link_flags=None):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        extra_sources = extra_sources or []
        if DIRECT_RUNTIME_SOURCE in extra_sources and TOPOLOGY_SOURCE not in extra_sources:
            extra_sources = [*extra_sources, TOPOLOGY_SOURCE]
        extra_link_flags = extra_link_flags or []
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "lower_layer_plan_builder_test.cpp"
            test_bin = temp_path / "lower_layer_plan_builder_test"
            test_cpp.write_text(code, encoding="utf-8")
            compile_cmd = [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(test_cpp),
                str(BUILDER_SOURCE),
                str(PAYLOAD_SOURCE),
                str(SPECS_SOURCE),
                str(ALLOCATOR_SOURCE),
                str(PRODUCER_SOURCE),
                str(BARRIER_SOURCE),
                str(MICROCODE_SOURCE),
                *[str(source) for source in extra_sources],
                "-o",
                str(test_bin),
                *extra_link_flags,
            ]
            try:
                subprocess.run(
                    compile_cmd,
                    cwd=REPO_ROOT,
                    check=True,
                    text=True,
                    capture_output=True,
                )
            except subprocess.CalledProcessError as exc:
                self.fail(exc.stdout + exc.stderr)
            return subprocess.run(
                [str(test_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env)

    def compile_only(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "comm_lower_layer_plan_api_test.cpp"
            test_obj = temp_path / "comm_lower_layer_plan_api_test.o"
            test_cpp.write_text(code, encoding="utf-8")
            return subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-I",
                    str(INCLUDE_DIR),
                    "-I",
                    str(COMM_DIR),
                    "-c",
                    str(test_cpp),
                    "-o",
                    str(test_obj),
                ],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )

    def test_topology_resolver_selects_peer_specific_hccs_eids(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_topology.h"

            #include <cstdlib>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                const char* rootPath = std::getenv("TILEXR_TEST_CCU_ROOT_INFO");
                std::vector<TileXRCcuPeerEidRoute> routes;
                std::string message;
                const int ret = TileXRCcuResolvePeerEidRoutes(
                    rootPath == nullptr ? "" : rootPath,
                    0,
                    {1, 2, 3},
                    &routes,
                    &message);
                if (ret != TILEXR_SUCCESS || routes.size() != 3) {
                    std::cerr << "resolve failed ret=" << ret << " message=" << message << "\n";
                    return 1;
                }
                if (routes[0].localPort != "0/8" || routes[0].localEid[5] != 0x08 ||
                    routes[1].localPort != "0/0" || routes[1].localEid[5] != 0x00 ||
                    routes[2].localPort != "0/7" || routes[2].localEid[5] != 0x07) {
                    std::cerr << "peer-specific EID mapping mismatch\n";
                    return 2;
                }
                return 0;
            }
            ''')
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            topo_path = temp_path / "topology.json"
            root_path = temp_path / "rootinfo.json"
            topo_path.write_text(json.dumps({
                "edge_list": [
                    {"local_a": 0, "local_a_ports": ["0/8"], "local_b": 1, "local_b_ports": ["0/7"]},
                    {"local_a": 0, "local_a_ports": ["0/0"], "local_b": 2, "local_b_ports": ["0/0"]},
                    {"local_a": 0, "local_a_ports": ["0/7"], "local_b": 3, "local_b_ports": ["0/7"]},
                ]
            }), encoding="utf-8")
            root_path.write_text(json.dumps({
                "topo_file_path": str(topo_path),
                "rank_list": [
                    {"device_id": device, "local_id": device, "level_list": [{"rank_addr_list": addresses}]}
                    for device, addresses in [
                        (0, [
                            {"addr": "000000000000030000100000df160100", "ports": ["0/0"]},
                            {"addr": "000000000008030000100000df160900", "ports": ["0/8"]},
                            {"addr": "000000000007030000100000df160800", "ports": ["0/7"]},
                        ]),
                        (1, []),
                        (2, []),
                        (3, []),
                    ]
                ]
            }), encoding="utf-8")
            env = os.environ.copy()
            env["TILEXR_TEST_CCU_ROOT_INFO"] = str(root_path)
            result = self.compile_and_run(code, env=env, extra_sources=[TOPOLOGY_SOURCE])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_builds_lower_layer_install_plan_from_tilexr_owned_specs(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdint>
            #include <iostream>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            int main()
            {
                TileXRCcuLowerLayerPlanSpec spec;
                spec.msidToken.dieId = 1;
                spec.msidToken.msId = 0x55;
                spec.msidToken.tokenId = 0x45678;
                spec.msidToken.tokenValue = 0;
                spec.msidToken.valid = true;

                spec.pfe.dieId = 1;
                spec.pfe.pfeOffset = 3;
                spec.pfe.startJettyId = 0x120;
                spec.pfe.startLocalJettyCtxId = 0x21;

                TileXRCcuLowerLayerJettySpec jetty0;
                jetty0.dieId = 1;
                jetty0.pfeId = 3;
                jetty0.startJettyCtxId = 0x21;
                jetty0.doorbellVa = 0x1122334455667788ULL;
                jetty0.doorbellTokenId = 0x45678;
                jetty0.doorbellTokenValue = 0;
                jetty0.sqDepth = 16;
                jetty0.wqeBasicBlockStartId = 0x40;
                spec.jettys.push_back(jetty0);

                TileXRCcuLowerLayerJettySpec jetty1 = jetty0;
                jetty1.startJettyCtxId = 0x22;
                jetty1.doorbellVa = 0x2122334455667788ULL;
                jetty1.wqeBasicBlockStartId = 0x44;
                spec.jettys.push_back(jetty1);

                TileXRCcuLowerLayerChannelSpec channel;
                channel.dieId = 1;
                channel.channelId = 5;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    channel.remoteEid[i] = static_cast<uint8_t>(0x20 + i);
                }
                channel.tpn = 0x123456;
                channel.sourcePfeId = 3;
                channel.startJettyId = 0x120;
                channel.memoryTokenId = 0xabcde;
                channel.memoryTokenValue = 0;
                channel.remoteCcuVa = 0x0001234567800000ULL;
                spec.channels.push_back(channel);

                spec.xnClear.dieId = 1;
                spec.xnClear.startXnId = 0x1f0;
                spec.xnClear.count = 3;
                spec.xnClear.valid = true;

                spec.ckeClear.dieId = 1;
                spec.ckeClear.startCkeId = 0x180;
                spec.ckeClear.count = 2;
                spec.ckeClear.valid = true;

                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerInstallPlan(spec, &plan, &report) != TILEXR_SUCCESS) {
                    std::cerr << "plan build failed: " << report.message << "\n";
                    return 1;
                }
                if (report.msidTokenCount != 1 || report.pfeCount != 1 ||
                    report.jettyCount != 1 || report.localJettyCtxCount != 2 ||
                    report.channelCount != 1 || report.ckeClearCount != 1) {
                    std::cerr << "unexpected plan report counts\n";
                    return 2;
                }
                if (plan.msidTokens.size() != 1 || plan.pfes.size() != 1 ||
                    plan.jettys.size() != 1 || plan.jettys[0].ctxs.size() != 2 ||
                    plan.channels.size() != 1 || plan.xnClears.size() != 1 ||
                    plan.ckeClears.size() != 1) {
                    std::cerr << "unexpected install plan shape\n";
                    return 3;
                }
                if (plan.msidTokens[0].tokenValue != 0 ||
                    plan.msidTokens[0].tokenId != 0x45678U ||
                    plan.xnClears[0].startXnId != 0x1f0 ||
                    plan.xnClears[0].count != 3 ||
                    plan.ckeClears[0].startCkeId != 0x180 ||
                    plan.ckeClears[0].count != 2) {
                    std::cerr << "scalar plan fields mismatch\n";
                    return 4;
                }
                if (Read16(plan.pfes[0].ctx.raw, 0) != 0x120 ||
                    Read16(plan.pfes[0].ctx.raw, 2) != static_cast<uint16_t>(1U | (0x21U << 7U))) {
                    std::cerr << "pfe payload mismatch\n";
                    return 5;
                }
                if (Read16(plan.jettys[0].ctxs[0].raw, 0) != 0x7788 ||
                    Read16(plan.jettys[0].ctxs[1].raw, 0) != 0x7788 ||
                    Read16(plan.jettys[0].ctxs[0].raw, 8) != 0x7873) {
                    std::cerr << "jetty payload mismatch\n";
                    return 6;
                }
                if (plan.channels[0].ctx.raw[0] != 0x20 ||
                    plan.channels[0].ctx.raw[15] != 0x2f ||
                    Read16(plan.channels[0].ctx.raw, 16) != 0x3456) {
                    std::cerr << "channel payload mismatch\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_can_use_hcomm_compatible_wqe_stride_for_direct_ccu_experiment(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 2};
                allocation.localXn = {1, 0x120, 2};
                allocation.remoteXn = {1, 0x240, 2};
                allocation.notifyCke = {1, 0x330, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remote0.remoteNotifyCke = 0x360;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remote1.remoteNotifyCke = 0x361;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.routes.size() != 2 ||
                    snapshot.routes[0].wqeBasicBlockStartId != 0 ||
                    snapshot.routes[1].wqeBasicBlockStartId != 256) {
                    std::cerr << "hcomm-compatible WQE stride not applied\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE"] = "hcomm_cap"

        result = self.compile_and_run(code, env=env)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_preserves_explicit_peer_rank_from_remote_buffer(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 7, 1};
                allocation.localXn = {1, 0x120, 1};
                allocation.remoteXn = {1, 0x240, 1};
                allocation.notifyCke = {1, 0x330, 1};

                TileXRCcuRemoteCcuBufferInfo remote;
                remote.peerRank = 1;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers {remote};

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.routes.size() != 1 || snapshot.routes[0].peerRank != 1) {
                    std::cerr << "explicit peer rank from remote buffer was not preserved\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_can_use_hcomm_die_pfe_offset_for_direct_ccu_experiment(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;
                basic.caps.cap4 = 15U;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 2};
                allocation.localXn = {1, 0x120, 2};
                allocation.remoteXn = {1, 0x240, 2};
                allocation.notifyCke = {1, 0x330, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.pfeId != 2 || snapshot.pfeOffset != 18) {
                    std::cerr << "hcomm die pfe offset not applied: pfeId=" << snapshot.pfeId
                              << " pfeOffset=" << snapshot.pfeOffset << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE"] = "hcomm_die"

        result = self.compile_and_run(code, env=env)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_can_use_hcomm_ordered_pfe_partition_for_direct_ccu_experiment(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 3, 2};
                allocation.localXn = {1, 0x120, 2};
                allocation.remoteXn = {1, 0x240, 2};
                allocation.notifyCke = {1, 0x330, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.pfeId != 3 ||
                    snapshot.startLocalJettyCtxId != 0 ||
                    snapshot.startJettyId != 1024 ||
                    snapshot.pfeJettyCount != 128 ||
                    snapshot.routes.size() != 2) {
                    std::cerr << "hcomm ordered pfe partition not applied: pfeId=" << snapshot.pfeId
                              << " startLocalJettyCtxId=" << snapshot.startLocalJettyCtxId
                              << " startJettyId=" << snapshot.startJettyId
                              << " pfeJettyCount=" << snapshot.pfeJettyCount << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION"] = "hcomm"

        result = self.compile_and_run(code, env=env)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_can_use_hcomm_fe_id_pfe_partition_for_direct_ccu_experiment(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 3, 2};
                allocation.localXn = {1, 0x120, 2};
                allocation.remoteXn = {1, 0x240, 2};
                allocation.notifyCke = {1, 0x330, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.pfeId != 3 ||
                    snapshot.startLocalJettyCtxId != 69 ||
                    snapshot.startJettyId != 1093 ||
                    snapshot.pfeJettyCount != 23) {
                    std::cerr << "hcomm fe-id pfe partition not applied: pfeId=" << snapshot.pfeId
                              << " startLocalJettyCtxId=" << snapshot.startLocalJettyCtxId
                              << " startJettyId=" << snapshot.startJettyId
                              << " pfeJettyCount=" << snapshot.pfeJettyCount << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION"] = "hcomm_fe_id"

        result = self.compile_and_run(code, env=env)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_hcomm_pfe_window_keeps_base_and_maps_sparse_verified_jetty_context(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdlib>
            #include <iostream>

            using namespace TileXR;

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION", "hcomm", 1);
                TileXRCcuBasicInfo basic;
                basic.dieId = 0;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {0, 2, 1};
                allocation.localXn = {0, 1961, 1};
                allocation.remoteXn = {0, 2361, 1};
                allocation.notifyCke = {0, 332, 1};

                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0xf98000000000ULL;
                remote.memoryTokenId = 0x1100;
                remote.remoteEid[0] = 1;
                remote.tpn = 0x51;
                remote.doorbellVa = 0x3fffff85080ULL;
                remote.doorbellTokenId = 0x1103;
                remote.sqDepth = 8;
                remote.startJettyId = 1026;
                remote.endpointRouteVerified = true;

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, {remote}, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << report.message << "\n";
                    return 1;
                }
                if (snapshot.startJettyId != 1024 || snapshot.pfeJettyCount != 128 ||
                    snapshot.routes[0].wqeBasicBlockStartId != 64) {
                    std::cerr << "PFE window was narrowed\n";
                    return 2;
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(
                        snapshot, &plan, &report) != TILEXR_SUCCESS) {
                    std::cerr << report.message << "\n";
                    return 3;
                }
                if (plan.pfes.size() != 1 || plan.jettys.size() != 1 ||
                    plan.jettys[0].startJettyCtxId != 2 || plan.jettys[0].ctxs.size() != 1) {
                    std::cerr << "sparse jetty context mapping mismatch\n";
                    return 4;
                }
                return 0;
            }
            ''')

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_uses_local_pfe_id_from_ra_eid_func_id(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x120, 1};
                allocation.remoteXn = {1, 0x240, 1};
                allocation.notifyCke = {1, 0x330, 1};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                remote.localPfeId = 7;
                remote.localPfeIdValid = true;
                remoteCcuBuffers.push_back(remote);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.pfeId != 7 || snapshot.pfeOffset != 23) {
                    std::cerr << "local PFE id not applied: pfeId=" << snapshot.pfeId
                              << " pfeOffset=" << snapshot.pfeOffset << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE"] = "hcomm_die"

        result = self.compile_and_run(code, env=env)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_prefers_ra_eid_func_id_over_channel_id_in_source(self):
        source = BUILDER_SOURCE.read_text(encoding="utf-8")
        build_body = source[
            source.index("int TileXRCcuBuildLowerLayerTransportTemplate"):
            source.index("int TileXRCcuOverlayVerifiedEndpointRoutes")
        ]

        self.assertIn("remoteCcuBuffers.front().localPfeId", build_body)
        self.assertIn("allocation.channels.startId", build_body)
        self.assertLess(
            build_body.index("remoteCcuBuffers.front().localPfeId"),
            build_body.index("SelectLowerLayerPfeOffset"))

    def test_builds_lower_layer_install_plan_from_transport_snapshot(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdint>
            #include <iostream>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            int main()
            {
                TileXRCcuLowerLayerTransportSnapshot snapshot;
                snapshot.msidToken.dieId = 0;
                snapshot.msidToken.msId = 0x9;
                snapshot.msidToken.tokenId = 0x12345;
                snapshot.msidToken.tokenValue = 0;
                snapshot.msidToken.valid = true;
                snapshot.dieId = 0;
                snapshot.pfeOffset = 2;
                snapshot.pfeId = 2;
                snapshot.startJettyId = 0x80;
                snapshot.startLocalJettyCtxId = 0x10;
                snapshot.xnStartId = 0x1a0;
                snapshot.xnCount = 6;
                snapshot.ckeStartId = 0x220;
                snapshot.ckeCount = 2;

                TileXRCcuLowerLayerTransportRoute route0;
                route0.channelId = 7;
                route0.remoteXnId = 0x2a0;
                route0.remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    route0.remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                route0.tpn = 0x010203;
                route0.doorbellVa = 0x1111222233334444ULL;
                route0.doorbellTokenId = 0x12345;
                route0.doorbellTokenValue = 0;
                route0.sqDepth = 8;
                route0.wqeBasicBlockStartId = 0x30;
                route0.memoryTokenId = 0x23456;
                route0.memoryTokenValue = 0;
                route0.remoteCcuVa = 0x0000009234000000ULL;
                route0.peerRank = 7;
                route0.endpointRouteVerified = true;
                snapshot.routes.push_back(route0);

                TileXRCcuLowerLayerTransportRoute route1 = route0;
                route1.channelId = 8;
                route1.remoteXnId = 0x2a1;
                route1.remoteNotifyCke = 0x361;
                route1.remoteEid[0] = 0x50;
                route1.tpn = 0x010204;
                route1.doorbellVa = 0x5555666677778888ULL;
                route1.wqeBasicBlockStartId = 0x34;
                route1.memoryTokenId = 0x23457;
                route1.remoteCcuVa = 0x0000009334000000ULL;
                route1.peerRank = 9;
                route1.endpointRouteVerified = true;
                snapshot.routes.push_back(route1);

                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "transport snapshot build failed: " << report.message << "\n";
                    return 1;
                }
                if (plan.msidTokens.size() != 1 || plan.pfes.size() != 1 ||
                    plan.jettys.size() != 1 || plan.jettys[0].ctxs.size() != 2 ||
                    plan.channels.size() != 2 || plan.xnClears.size() != 2 ||
                    plan.ckeClears.size() != 2 || plan.remoteXnBindings.size() != 2) {
                    std::cerr << "unexpected transport-derived plan shape\n";
                    return 2;
                }
                if (report.channelCount != 2 || report.localJettyCtxCount != 2 ||
                    report.ckeClearCount != 2) {
                    std::cerr << "unexpected transport-derived report counts\n";
                    return 3;
                }
                if (Read16(plan.pfes[0].ctx.raw, 0) != 0x80 ||
                    Read16(plan.pfes[0].ctx.raw, 2) != static_cast<uint16_t>(1U | (0x10U << 7U))) {
                    std::cerr << "transport pfe payload mismatch\n";
                    return 4;
                }
                if (Read16(plan.jettys[0].ctxs[0].raw, 0) != 0x4444 ||
                    Read16(plan.jettys[0].ctxs[1].raw, 0) != 0x8888) {
                    std::cerr << "transport jetty payload mismatch\n";
                    return 5;
                }
                if (plan.channels[0].channelId != 7 || plan.channels[1].channelId != 8 ||
                    plan.channels[0].ctx.raw[0] != 0x40 || plan.channels[1].ctx.raw[0] != 0x50 ||
                    Read16(plan.channels[0].ctx.raw, 16) != 0x0203 ||
                    Read16(plan.channels[1].ctx.raw, 16) != 0x0204) {
                    std::cerr << "transport channel payload mismatch\n";
                    return 6;
                }
                if (Read16(plan.channels[0].ctx.raw, 18) != 0x0201 ||
                    Read16(plan.channels[0].ctx.raw, 20) != 0x0008 ||
                    Read16(plan.channels[1].ctx.raw, 18) != 0x1201 ||
                    Read16(plan.channels[1].ctx.raw, 20) != 0x0008) {
                    std::cerr << "transport channel jetty window mismatch\n";
                    return 9;
                }
                if (plan.xnClears[0].startXnId != 0x1a0 || plan.xnClears[0].count != 6 ||
                    plan.xnClears[1].startXnId != 0x2a0 || plan.xnClears[1].count != 2 ||
                    plan.ckeClears[0].startCkeId != 0x220 || plan.ckeClears[0].count != 2 ||
                    plan.ckeClears[1].startCkeId != 0x360 || plan.ckeClears[1].count != 2) {
                    std::cerr << "transport local/channel XN or CKE clear mismatch\n";
                    return 7;
                }
                if (!plan.remoteXnBindings[0].peerExchangeObserved ||
                    plan.remoteXnBindings[0].channelId != 7 ||
                    plan.remoteXnBindings[0].localXn != 0x1a0 ||
                    plan.remoteXnBindings[0].remoteXn != 0x2a0 ||
                    plan.remoteXnBindings[0].notifyCke != 0x360 ||
                    plan.remoteXnBindings[0].peerRank != 7 ||
                    plan.remoteXnBindings[0].localWaitCke != 0x220 ||
                    !plan.remoteXnBindings[0].endpointRouteVerified ||
                    plan.remoteXnBindings[1].channelId != 8 ||
                    plan.remoteXnBindings[1].localXn != 0x1a1 ||
                    plan.remoteXnBindings[1].remoteXn != 0x2a1 ||
                    plan.remoteXnBindings[1].notifyCke != 0x361 ||
                    plan.remoteXnBindings[1].peerRank != 9 ||
                    plan.remoteXnBindings[1].localWaitCke != 0x221 ||
                    !plan.remoteXnBindings[1].endpointRouteVerified) {
                    std::cerr << "transport remote XN proof mismatch\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_snapshot_pfe_jetty_count_reaches_pfe_ctx(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdint>
            #include <iostream>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            int main()
            {
                TileXRCcuLowerLayerTransportSnapshot snapshot;
                snapshot.msidToken.dieId = 1;
                snapshot.msidToken.msId = 0x9;
                snapshot.msidToken.tokenId = 0x12345;
                snapshot.msidToken.valid = true;
                snapshot.dieId = 1;
                snapshot.pfeOffset = 18;
                snapshot.pfeId = 2;
                snapshot.startJettyId = 1024;
                snapshot.pfeJettyCount = 23;
                snapshot.startLocalJettyCtxId = 0;
                snapshot.xnStartId = 0x1a0;
                snapshot.xnCount = 1;
                snapshot.ckeStartId = 0x220;
                snapshot.ckeCount = 1;

                TileXRCcuLowerLayerTransportRoute route;
                route.channelId = 7;
                route.remoteXnId = 0x2a0;
                route.remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    route.remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                route.tpn = 0x010203;
                route.doorbellVa = 0x1111222233334444ULL;
                route.doorbellTokenId = 0x12345;
                route.sqDepth = 8;
                route.memoryTokenId = 0x23456;
                route.remoteCcuVa = 0x0000009234000000ULL;
                snapshot.routes.push_back(route);

                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "transport snapshot build failed: " << report.message << "\n";
                    return 1;
                }
                if (plan.pfes.size() != 1 || plan.pfes[0].pfeOffset != 18) {
                    std::cerr << "pfe install shape mismatch\n";
                    return 2;
                }
                const uint16_t pfeWord = Read16(plan.pfes[0].ctx.raw, 2);
                if ((pfeWord & 0x7fU) != 22U) {
                    std::cerr << "pfe jetty count did not reach ctx: word=" << pfeWord << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_snapshot_installs_remote_xn_range_separately(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuLowerLayerTransportSnapshot snapshot;
                snapshot.msidToken.dieId = 1;
                snapshot.msidToken.msId = 0x45;
                snapshot.msidToken.tokenId = 0x1234;
                snapshot.msidToken.valid = true;
                snapshot.dieId = 1;
                snapshot.pfeOffset = 0x80;
                snapshot.pfeId = 2;
                snapshot.startJettyId = 0x400;
                snapshot.pfeJettyCount = 2;
                snapshot.startLocalJettyCtxId = 0;
                snapshot.xnStartId = 0x1a0;
                snapshot.xnCount = 2;
                snapshot.ckeStartId = 0x220;
                snapshot.ckeCount = 2;

                snapshot.routes.resize(2);
                snapshot.routes[0].channelId = 7;
                snapshot.routes[0].remoteXnId = 0x2a0;
                snapshot.routes[0].remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    snapshot.routes[0].remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                snapshot.routes[0].tpn = 0x010203;
                snapshot.routes[0].doorbellVa = 0x1111222233334444ULL;
                snapshot.routes[0].doorbellTokenId = 0x12345;
                snapshot.routes[0].sqDepth = 8;
                snapshot.routes[0].remoteCcuVa = 0x90000000ULL;
                snapshot.routes[0].memoryTokenId = 0x1234;

                snapshot.routes[1] = snapshot.routes[0];
                snapshot.routes[1].channelId = 8;
                snapshot.routes[1].remoteXnId = 0x2a1;
                snapshot.routes[1].remoteNotifyCke = 0x361;
                snapshot.routes[1].tpn = 0x010204;

                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "transport snapshot build failed: " << report.message << "\n";
                    return 1;
                }
                if (plan.xnClears.size() != 2) {
                    std::cerr << "expected local and channel remote XN clears, got " <<
                        plan.xnClears.size() << "\n";
                    return 2;
                }
                if (plan.xnClears[0].startXnId != 0x1a0 || plan.xnClears[0].count != 2) {
                    std::cerr << "local XN clear mismatch\n";
                    return 3;
                }
                if (plan.xnClears[1].startXnId != 0x2a0 || plan.xnClears[1].count != 2) {
                    std::cerr << "channel remote XN clear mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_snapshot_installs_remote_notify_cke_range_separately(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuLowerLayerTransportSnapshot snapshot;
                snapshot.msidToken.dieId = 1;
                snapshot.msidToken.msId = 0x45;
                snapshot.msidToken.tokenId = 0x1234;
                snapshot.msidToken.valid = true;
                snapshot.dieId = 1;
                snapshot.pfeOffset = 0x80;
                snapshot.pfeId = 2;
                snapshot.startJettyId = 0x400;
                snapshot.pfeJettyCount = 2;
                snapshot.startLocalJettyCtxId = 0;
                snapshot.xnStartId = 0x1a0;
                snapshot.xnCount = 2;
                snapshot.ckeStartId = 0x220;
                snapshot.ckeCount = 3;

                snapshot.routes.resize(2);
                snapshot.routes[0].channelId = 7;
                snapshot.routes[0].remoteXnId = 0x2a0;
                snapshot.routes[0].remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    snapshot.routes[0].remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                snapshot.routes[0].tpn = 0x010203;
                snapshot.routes[0].doorbellVa = 0x1111222233334444ULL;
                snapshot.routes[0].doorbellTokenId = 0x12345;
                snapshot.routes[0].sqDepth = 8;
                snapshot.routes[0].remoteCcuVa = 0x90000000ULL;
                snapshot.routes[0].memoryTokenId = 0x1234;
                snapshot.routes[1] = snapshot.routes[0];
                snapshot.routes[1].channelId = 8;
                snapshot.routes[1].remoteXnId = 0x2a1;
                snapshot.routes[1].remoteNotifyCke = 0x361;
                snapshot.routes[1].tpn = 0x010204;

                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "transport snapshot build failed: " << report.message << "\n";
                    return 1;
                }
                if (plan.ckeClears.size() != 2 || report.ckeClearCount != 2) {
                    std::cerr << "expected local and remote notify CKE clears, got " <<
                        plan.ckeClears.size() << " report=" << report.ckeClearCount << "\n";
                    return 2;
                }
                if (plan.ckeClears[0].startCkeId != 0x220 || plan.ckeClears[0].count != 3) {
                    std::cerr << "local CKE clear mismatch\n";
                    return 3;
                }
                if (plan.ckeClears[1].startCkeId != 0x360 || plan.ckeClears[1].count != 2) {
                    std::cerr << "remote notify CKE clear mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_builds_transport_template_from_basic_info_and_resource_allocation(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0;
                basic.msidToken.valid = true;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (5U << 16) | 255U;
                basic.caps.cap1 = (127U << 16) | 63U;
                basic.caps.cap2 = (31U << 16) | 15U;
                basic.caps.cap3 = (7U << 16) | 1U;
                basic.caps.cap4 = 9U;

                TileXRCcuSpecInfo specInfo;
                TileXRCcuSpecsReport specsReport;
                if (TileXRCcuDecodeBasicInfo(basic, &specInfo, &specsReport) != TILEXR_SUCCESS) {
                    std::cerr << "decode failed: " << specsReport.message << "\n";
                    return 1;
                }

                TileXRCcuResourceSpec resourceSpec;
                if (TileXRCcuBuildResourceSpec(specInfo, 6, 475, 1961, 332, 2, &resourceSpec, &specsReport) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "resource spec failed: " << specsReport.message << "\n";
                    return 2;
                }

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(resourceSpec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 3;
                }
                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 2;
                request.syncInstructionCount = 9;
                request.bindingsPerSyncResource = 1;
                TileXRCcuProducerPlan producerPlan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport allocatorReport;
                if (allocator.Allocate(request, &producerPlan, &allocation, &allocatorReport) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << allocatorReport.message << "\n";
                    return 4;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = specInfo.xnBaseAddr + allocation.remoteXn.startId * 8ULL;
                remote0.memoryTokenId = 0x23456;
                remote0.memoryTokenValue = 0x5678;
                remote0.remoteNotifyCke = 0x360;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = specInfo.xnBaseAddr + (allocation.remoteXn.startId + 1U) * 8ULL;
                remote1.memoryTokenId = 0x23457;
                remote1.memoryTokenValue = 0x5679;
                remote1.remoteNotifyCke = 0x361;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 5;
                }
                if (!snapshot.msidToken.valid || snapshot.msidToken.dieId != 1 ||
                    snapshot.msidToken.msId != 0x45 || snapshot.msidToken.tokenId != 0x1234 ||
                    snapshot.msidToken.tokenValue != 0) {
                    std::cerr << "msid token template mismatch\n";
                    return 6;
                }
                if (snapshot.dieId != 1 || snapshot.pfeOffset != allocation.channels.startId ||
                    snapshot.pfeId != allocation.channels.startId ||
                    snapshot.startJettyId != 1024 || snapshot.startLocalJettyCtxId != 0 ||
                    snapshot.xnStartId != allocation.localXn.startId ||
                    snapshot.xnCount != allocation.localXn.num ||
                    snapshot.ckeStartId != allocation.notifyCke.startId ||
                    snapshot.ckeCount != allocation.notifyCke.num) {
                    std::cerr << "template scalar mismatch\n";
                    return 7;
                }
                if (snapshot.routes.size() != 2 || snapshot.routes[0].channelId != allocation.channels.startId ||
                    snapshot.routes[1].channelId != allocation.channels.startId + 1U ||
                    snapshot.routes[0].remoteXnId != allocation.remoteXn.startId ||
                    snapshot.routes[1].remoteXnId != allocation.remoteXn.startId + 1U ||
                    snapshot.routes[0].remoteNotifyCke != 0x360 ||
                    snapshot.routes[1].remoteNotifyCke != 0x361 ||
                    snapshot.routes[0].wqeBasicBlockStartId != 0 ||
                    snapshot.routes[1].wqeBasicBlockStartId != 4 ||
                    snapshot.routes[0].remoteCcuVa != remoteCcuBuffers[0].remoteCcuVa ||
                    snapshot.routes[1].remoteCcuVa != remoteCcuBuffers[1].remoteCcuVa ||
                    snapshot.routes[0].memoryTokenId != 0x23456 ||
                    snapshot.routes[0].memoryTokenValue != 0x5678 ||
                    snapshot.routes[1].memoryTokenId != 0x23457 ||
                    snapshot.routes[1].memoryTokenValue != 0x5679) {
                    std::cerr << "route template mismatch\n";
                    return 8;
                }
                if (report.message != "ok" || report.channelCount != 2 || report.ckeClearCount != 1) {
                    std::cerr << "report mismatch\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_accepts_basic_info_without_basic_msid_token(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0;
                basic.msidToken.tokenValue = 0;
                basic.msidToken.valid = false;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x1a0, 1};
                allocation.remoteXn = {1, 0x2a0, 1};
                allocation.notifyCke = {1, 0x220, 1};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                remote.memoryTokenValue = 0x5678;
                remote.remoteNotifyCke = 0x360;
                remoteCcuBuffers.push_back(remote);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.msidToken.valid || snapshot.msidToken.tokenId != 0 ||
                    snapshot.msidToken.tokenValue != 0 || snapshot.msidToken.dieId != 1 ||
                    snapshot.msidToken.msId != 0x45) {
                    std::cerr << "basic-info token should remain absent in template\n";
                    return 2;
                }
                if (report.msidTokenCount != 0 || report.channelCount != 1 ||
                    snapshot.routes.size() != 1 || snapshot.routes[0].memoryTokenId != 0x23456) {
                    std::cerr << "template route/report mismatch\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_promotes_verified_endpoint_route_from_remote_buffer_info(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x1a0, 1};
                allocation.remoteXn = {1, 0x2a0, 1};
                allocation.notifyCke = {1, 0x220, 1};

                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                remote.memoryTokenValue = 0x5678;
                remote.remoteXnId = 0x2a7;
                remote.remoteNotifyCke = 0x361;
                for (uint32_t i = 0; i < remote.remoteEid.size(); ++i) {
                    remote.remoteEid[i] = static_cast<uint8_t>(0x80 + i);
                }
                remote.tpn = 0x010203;
                remote.doorbellVa = 0x1122334455667788ULL;
                remote.doorbellTokenId = 0x3456;
                remote.doorbellTokenValue = 0;
                remote.sqDepth = 64;
                remote.endpointRouteVerified = true;
                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers {remote};

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.routes.size() != 1 ||
                    !snapshot.routes[0].endpointRouteVerified ||
                    snapshot.routes[0].remoteEid[0] != 0x80 ||
                    snapshot.routes[0].tpn != remote.tpn ||
                    snapshot.routes[0].doorbellVa != remote.doorbellVa ||
                    snapshot.routes[0].doorbellTokenId != remote.doorbellTokenId ||
                    snapshot.routes[0].doorbellTokenValue != remote.doorbellTokenValue ||
                    snapshot.routes[0].sqDepth != remote.sqDepth ||
                    snapshot.routes[0].remoteXnId != remote.remoteXnId ||
                    snapshot.routes[0].remoteNotifyCke != remote.remoteNotifyCke) {
                    std::cerr << "verified endpoint route was not promoted from remote buffer info\n";
                    return 2;
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install plan build failed: " << report.message << "\n";
                    return 3;
                }
                if (plan.remoteXnBindings.size() != 1 ||
                    !plan.remoteXnBindings[0].endpointRouteVerified ||
                    plan.remoteXnBindings[0].remoteXn != remote.remoteXnId ||
                    plan.remoteXnBindings[0].notifyCke != remote.remoteNotifyCke) {
                    std::cerr << "verified endpoint proof was not propagated\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_rejects_incomplete_verified_endpoint_route_from_remote_buffer_info(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x1a0, 1};
                allocation.remoteXn = {1, 0x2a0, 1};
                allocation.notifyCke = {1, 0x220, 1};

                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                remote.memoryTokenValue = 0x5678;
                remote.remoteXnId = 0x2a7;
                remote.remoteNotifyCke = 0x361;
                remote.remoteEid[0] = 0x80;
                remote.tpn = 0x010203;
                remote.doorbellVa = 0x1122334455667788ULL;
                remote.doorbellTokenId = 0;
                remote.sqDepth = 64;
                remote.endpointRouteVerified = true;
                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers {remote};

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.routes.size() != 1 ||
                    snapshot.routes[0].endpointRouteVerified ||
                    snapshot.routes[0].remoteEid[0] != 0 ||
                    snapshot.routes[0].tpn != 0 ||
                    snapshot.routes[0].doorbellVa != 0 ||
                    snapshot.routes[0].sqDepth != 0) {
                    std::cerr << "incomplete endpoint route should fail closed\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_rejects_channel_allocation_smaller_than_routes(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x1a0, 2};
                allocation.remoteXn = {1, 0x2a0, 2};
                allocation.notifyCke = {1, 0x360, 2};
                allocation.localWaitCke = {1, 0x220, 2};
                allocation.remoteNotifyCke = {1, 0x360, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remote0.memoryTokenValue = 0x5678;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "template accepted an allocation with too few channels\n";
                    return 1;
                }
                if (report.message.find("channel allocation count") == std::string::npos) {
                    std::cerr << "weak channel allocation diagnostic: " << report.message << "\n";
                    return 2;
                }
                if (!snapshot.routes.empty()) {
                    std::cerr << "failed template should not retain routes\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_uses_local_wait_cke_for_clear_and_remote_notify_cke_for_routes(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 2};
                allocation.localXn = {1, 0x1a0, 2};
                allocation.remoteXn = {1, 0x2a0, 2};
                allocation.notifyCke = {1, 0x360, 2};
                allocation.localWaitCke = {1, 0x220, 2};
                allocation.remoteNotifyCke = {1, 0x360, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remote0.memoryTokenValue = 0x5678;
                remote0.remoteNotifyCke = 0;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remote1.memoryTokenValue = 0x5679;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.ckeStartId != 0x220 || snapshot.ckeCount != 2) {
                    std::cerr << "local wait CKE clear range mismatch\n";
                    return 2;
                }
                if (snapshot.routes.size() != 2 ||
                    snapshot.routes[0].remoteNotifyCke != 0x360 ||
                    snapshot.routes[1].remoteNotifyCke != 0x361) {
                    std::cerr << "remote notify CKE route fallback mismatch\n";
                    return 3;
                }
                for (uint32_t i = 0; i < snapshot.routes.size(); ++i) {
                    for (uint32_t eidIndex = 0; eidIndex < TILEXR_CCU_EID_BYTES; ++eidIndex) {
                        snapshot.routes[i].remoteEid[eidIndex] =
                            static_cast<uint8_t>(0x40 + i * 0x10 + eidIndex);
                    }
                    snapshot.routes[i].tpn = 0x010200 + i;
                    snapshot.routes[i].doorbellVa = 0x1111222233334444ULL + i * 0x1000ULL;
                    snapshot.routes[i].doorbellTokenId = 0x12345;
                    snapshot.routes[i].doorbellTokenValue = 0;
                    snapshot.routes[i].sqDepth = 8;
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install plan build failed: " << report.message << "\n";
                    return 4;
                }
                if (plan.remoteXnBindings.size() != 2 ||
                    plan.remoteXnBindings[0].notifyCke != 0x360 ||
                    plan.remoteXnBindings[0].localWaitCke != 0x220 ||
                    plan.remoteXnBindings[1].notifyCke != 0x361 ||
                    plan.remoteXnBindings[1].localWaitCke != 0x221) {
                    std::cerr << "split CKE remote XN proof mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_overlay_verified_endpoint_routes_updates_only_matching_endpoint_fields(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuLowerLayerTransportSnapshot snapshot;
                snapshot.routes.resize(2);
                snapshot.routes[0].channelId = 0x20;
                snapshot.routes[0].peerRank = 3;
                snapshot.routes[0].remoteXnId = 0x1a0;
                snapshot.routes[0].remoteNotifyCke = 0x360;
                snapshot.routes[0].wqeBasicBlockStartId = 0x12;
                snapshot.routes[0].memoryTokenId = 0x2001;
                snapshot.routes[0].remoteCcuVa = 0x90000000ULL;
                snapshot.routes[1].channelId = 0x21;
                snapshot.routes[1].peerRank = 4;
                snapshot.routes[1].remoteXnId = 0x1a1;
                snapshot.routes[1].remoteNotifyCke = 0x361;

                TileXRCcuLowerLayerTransportRoute verified;
                verified.channelId = 0x20;
                verified.peerRank = 99;
                verified.remoteXnId = 0x2b0;
                verified.remoteNotifyCke = 0x470;
                for (uint32_t i = 0; i < verified.remoteEid.size(); ++i) {
                    verified.remoteEid[i] = static_cast<uint8_t>(0xc0 + i);
                }
                verified.tpn = 0x010203;
                verified.doorbellVa = 0x1122334455667788ULL;
                verified.doorbellTokenId = 0x3456;
                verified.doorbellTokenValue = 0;
                verified.sqDepth = 64;
                verified.wqeBasicBlockStartId = 0x77;
                verified.memoryTokenId = 0x9999;
                verified.remoteCcuVa = 0xabcdefULL;
                verified.endpointRouteVerified = true;

                TileXRCcuLowerLayerPlanBuilderReport report;
                std::vector<TileXRCcuLowerLayerTransportRoute> routes {verified};
                if (TileXRCcuOverlayVerifiedEndpointRoutes(routes, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "overlay failed: " << report.message << "\n";
                    return 1;
                }

                const auto& route0 = snapshot.routes[0];
                if (!route0.endpointRouteVerified ||
                    route0.remoteEid[0] != 0xc0 ||
                    route0.tpn != verified.tpn ||
                    route0.doorbellVa != verified.doorbellVa ||
                    route0.doorbellTokenId != verified.doorbellTokenId ||
                    route0.doorbellTokenValue != verified.doorbellTokenValue ||
                    route0.sqDepth != verified.sqDepth) {
                    std::cerr << "verified endpoint fields were not overlaid\n";
                    return 2;
                }
                if (route0.peerRank != 3 ||
                    route0.remoteXnId != 0x1a0 ||
                    route0.remoteNotifyCke != 0x360 ||
                    route0.wqeBasicBlockStartId != 0x12 ||
                    route0.memoryTokenId != 0x2001 ||
                    route0.remoteCcuVa != 0x90000000ULL) {
                    std::cerr << "overlay changed non-endpoint resource fields\n";
                    return 3;
                }
                if (snapshot.routes[1].endpointRouteVerified ||
                    snapshot.routes[1].remoteEid[0] != 0 ||
                    snapshot.routes[1].tpn != 0 ||
                    snapshot.routes[1].doorbellVa != 0) {
                    std::cerr << "overlay changed an unmatched route\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_overlay_verified_endpoint_route_reuses_shared_jetty_for_multi_route_snapshot(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            uint16_t DecodeChannelStartJettyId(const TileXRCcuChannelCtxDataV1& ctx)
            {
                const uint16_t word18 = Read16(ctx.raw, 18);
                const uint16_t word20 = Read16(ctx.raw, 20);
                return static_cast<uint16_t>(((word18 >> 12U) & 0xfU) | ((word20 & 0xfffU) << 4U));
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 3};
                allocation.localXn = {1, 0x1a0, 3};
                allocation.remoteXn = {1, 0x2a0, 3};
                allocation.notifyCke = {1, 0x360, 3};
                allocation.localWaitCke = {1, 0x220, 3};
                allocation.remoteNotifyCke = {1, 0x360, 3};

                TileXRCcuRemoteCcuBufferInfo remote;
                remote.remoteCcuVa = 0x0000009234000000ULL;
                remote.memoryTokenId = 0x23456;
                remote.memoryTokenValue = 0x5678;
                remote.remoteXnId = 0x2a0;
                remote.remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    remote.remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                remote.tpn = 0x010203;
                remote.doorbellVa = 0x1111222233334444ULL;
                remote.doorbellTokenId = 0x12345;
                remote.doorbellTokenValue = 0;
                remote.sqDepth = 8;
                remote.startJettyId = 0x400;
                remote.endpointRouteVerified = true;

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers {remote, remote, remote};
                for (uint32_t i = 0; i < remoteCcuBuffers.size(); ++i) {
                    remoteCcuBuffers[i].remoteCcuVa += i * 0x1000ULL;
                    remoteCcuBuffers[i].memoryTokenValue += i;
                    remoteCcuBuffers[i].remoteXnId = static_cast<uint16_t>(0x2a0 + i);
                    remoteCcuBuffers[i].remoteNotifyCke = static_cast<uint16_t>(0x360 + i);
                }

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }

                TileXRCcuLowerLayerTransportRoute verified = snapshot.routes[0];
                std::vector<TileXRCcuLowerLayerTransportRoute> verifiedRoutes {verified};
                if (TileXRCcuOverlayVerifiedEndpointRoutes(verifiedRoutes, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "overlay failed: " << report.message << "\n";
                    return 2;
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install plan build failed: " << report.message
                              << " pfeJettyCount=" << snapshot.pfeJettyCount
                              << " routeCount=" << snapshot.routes.size() << "\n";
                    return 3;
                }
                if (plan.jettys.empty() || plan.jettys[0].ctxs.size() != 1 || plan.pfes.empty()) {
                    std::cerr << "install plan shape mismatch\n";
                    return 4;
                }
                if (plan.channels.size() != 3 ||
                    plan.remoteXnBindings.size() != 3 ||
                    snapshot.pfeJettyCount < snapshot.routes.size()) {
                    std::cerr << "multi-route lower-layer plan did not preserve all routes\n";
                    return 5;
                }
                if (DecodeChannelStartJettyId(plan.channels[0].ctx) != 0x400 ||
                    DecodeChannelStartJettyId(plan.channels[1].ctx) != 0x400 ||
                    DecodeChannelStartJettyId(plan.channels[2].ctx) != 0x400) {
                    std::cerr << "multi-route channels did not reuse the verified endpoint jetty\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_uses_peer_exchanged_remote_xn_ids_when_present(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 2};
                allocation.localXn = {1, 0x2a0, 2};
                allocation.remoteXn = {1, 0x2b0, 2};
                allocation.notifyCke = {1, 0x360, 2};
                allocation.localWaitCke = {1, 0x220, 2};
                allocation.remoteNotifyCke = {1, 0x360, 2};

                std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
                TileXRCcuRemoteCcuBufferInfo remote0;
                remote0.remoteCcuVa = 0x0000009234000000ULL;
                remote0.memoryTokenId = 0x23456;
                remote0.memoryTokenValue = 0x5678;
                remote0.remoteXnId = 0x1a0;
                remote0.remoteNotifyCke = 0x360;
                remoteCcuBuffers.push_back(remote0);
                TileXRCcuRemoteCcuBufferInfo remote1 = remote0;
                remote1.remoteCcuVa = 0x0000009334000000ULL;
                remote1.memoryTokenId = 0x23457;
                remote1.memoryTokenValue = 0x5679;
                remote1.remoteXnId = 0x1a1;
                remote1.remoteNotifyCke = 0x361;
                remoteCcuBuffers.push_back(remote1);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, remoteCcuBuffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template build failed: " << report.message << "\n";
                    return 1;
                }
                if (snapshot.routes.size() != 2 ||
                    snapshot.routes[0].remoteXnId != 0x1a0 ||
                    snapshot.routes[1].remoteXnId != 0x1a1) {
                    std::cerr << "peer exchanged remote XN IDs were not preserved\n";
                    return 2;
                }
                for (uint32_t i = 0; i < snapshot.routes.size(); ++i) {
                    for (uint32_t eidIndex = 0; eidIndex < TILEXR_CCU_EID_BYTES; ++eidIndex) {
                        snapshot.routes[i].remoteEid[eidIndex] =
                            static_cast<uint8_t>(0x40 + i * 0x10 + eidIndex);
                    }
                    snapshot.routes[i].tpn = 0x010200 + i;
                    snapshot.routes[i].doorbellVa = 0x1111222233334444ULL + i * 0x1000ULL;
                    snapshot.routes[i].doorbellTokenId = 0x12345;
                    snapshot.routes[i].sqDepth = 8;
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install plan build failed: " << report.message << "\n";
                    return 3;
                }
                if (plan.remoteXnBindings.size() != 2 ||
                    plan.remoteXnBindings[0].remoteXn != 0x1a0 ||
                    plan.remoteXnBindings[1].remoteXn != 0x1a1 ||
                    !plan.remoteXnBindings[0].peerExchangeObserved ||
                    !plan.remoteXnBindings[1].peerExchangeObserved ||
                    plan.remoteXnBindings[0].endpointRouteVerified ||
                    plan.remoteXnBindings[1].endpointRouteVerified) {
                    std::cerr << "peer exchanged remote XN proof mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transport_template_carries_explicit_channel_owner_exchange_proof(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {1, 2, 1};
                allocation.localXn = {1, 0x2a0, 1};
                allocation.remoteXn = {1, 0x2b0, 1};
                allocation.notifyCke = {1, 0x360, 1};
                allocation.localWaitCke = {1, 0x220, 1};
                allocation.remoteNotifyCke = {1, 0x360, 1};

                TileXRCcuRemoteCcuBufferInfo endpointOnly;
                endpointOnly.remoteCcuVa = 0x0000009234000000ULL;
                endpointOnly.memoryTokenId = 0x23456;
                endpointOnly.memoryTokenValue = 0x5678;
                endpointOnly.remoteXnId = 0x1a0;
                endpointOnly.remoteNotifyCke = 0x360;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    endpointOnly.remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                endpointOnly.tpn = 0x010200;
                endpointOnly.doorbellVa = 0x1111222233334444ULL;
                endpointOnly.doorbellTokenId = 0x12345;
                endpointOnly.sqDepth = 8;
                endpointOnly.endpointRouteVerified = true;

                TileXRCcuLowerLayerTransportSnapshot endpointOnlySnapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, {endpointOnly}, &endpointOnlySnapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "endpoint-only template build failed: " << report.message << "\n";
                    return 1;
                }
                TileXRCcuLowerLayerInstallPlan endpointOnlyPlan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(
                        endpointOnlySnapshot, &endpointOnlyPlan, &report) != TILEXR_SUCCESS) {
                    std::cerr << "endpoint-only plan build failed: " << report.message << "\n";
                    return 2;
                }
                if (!endpointOnlyPlan.remoteXnBindings[0].endpointRouteVerified ||
                    endpointOnlyPlan.remoteXnBindings[0].channelResourceOwnerVerified ||
                    endpointOnlyPlan.remoteXnBindings[0].transportResourceExchangeVerified) {
                    std::cerr << "endpoint route alone was promoted to owner/exchange proof\n";
                    return 3;
                }

                TileXRCcuRemoteCcuBufferInfo proven = endpointOnly;
                proven.channelResourceOwnerVerified = true;
                proven.transportResourceExchangeVerified = true;

                TileXRCcuLowerLayerTransportSnapshot provenSnapshot;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, {proven}, &provenSnapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "proven template build failed: " << report.message << "\n";
                    return 4;
                }
                if (!provenSnapshot.routes[0].channelResourceOwnerVerified ||
                    !provenSnapshot.routes[0].transportResourceExchangeVerified) {
                    std::cerr << "route did not preserve owner/exchange proof\n";
                    return 5;
                }

                TileXRCcuLowerLayerInstallPlan provenPlan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(
                        provenSnapshot, &provenPlan, &report) != TILEXR_SUCCESS) {
                    std::cerr << "proven plan build failed: " << report.message << "\n";
                    return 6;
                }
                const auto& proof = provenPlan.remoteXnBindings[0];
                if (!proof.endpointRouteVerified ||
                    !proof.channelResourceOwnerVerified ||
                    !proof.transportResourceExchangeVerified) {
                    std::cerr << "install proof did not preserve owner/exchange proof\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_shared_peer_jetty_routes_reuse_one_wqe_window_per_peer(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdlib>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            uint16_t DecodeWqeStart(const TileXRCcuLocalJettyCtxData& ctx)
            {
                const uint16_t word22 = static_cast<uint16_t>(ctx.raw[22]) |
                    static_cast<uint16_t>(ctx.raw[23] << 8U);
                const uint16_t word24 = static_cast<uint16_t>(ctx.raw[24]) |
                    static_cast<uint16_t>(ctx.raw[25] << 8U);
                return static_cast<uint16_t>(((word22 >> 12U) & 0xfU) | ((word24 & 0xffU) << 4U));
            }

            uint16_t DecodeChannelJetty(const TileXRCcuChannelCtxDataV1& ctx)
            {
                const uint16_t word18 = static_cast<uint16_t>(ctx.raw[18]) |
                    static_cast<uint16_t>(ctx.raw[19] << 8U);
                const uint16_t word20 = static_cast<uint16_t>(ctx.raw[20]) |
                    static_cast<uint16_t>(ctx.raw[21] << 8U);
                return static_cast<uint16_t>(((word18 >> 12U) & 0xfU) | ((word20 & 0xfffU) << 4U));
            }

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE", "hcomm_cap", 1);
                TileXRCcuBasicInfo basic;
                basic.dieId = 0;
                basic.msId = 0x45;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.valid = true;

                TileXRCcuResourceAllocation allocation;
                allocation.channels = {0, 2, 9};
                allocation.localXn = {0, 1961, 9};
                allocation.remoteXn = {0, 2361, 9};
                allocation.notifyCke = {0, 332, 9};
                allocation.localWaitCke = {0, 332, 9};
                allocation.remoteNotifyCke = {0, 364, 9};

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers(9);
                for (uint32_t route = 0; route < buffers.size(); ++route) {
                    const uint32_t peerOrdinal = route / 3U;
                    auto& buffer = buffers[route];
                    buffer.remoteCcuVa = 0x90000000ULL + route * 0x1000ULL;
                    buffer.memoryTokenId = 0x2000U + route;
                    buffer.memoryTokenValue = 0x3000U + route;
                    buffer.remoteXnId = static_cast<uint16_t>(2361U + route);
                    buffer.remoteNotifyCke = static_cast<uint16_t>(364U + route);
                    buffer.peerRank = peerOrdinal + 1U;
                    for (uint32_t byte = 0; byte < buffer.remoteEid.size(); ++byte) {
                        buffer.remoteEid[byte] = static_cast<uint8_t>(0x20U + peerOrdinal * 0x10U + byte);
                    }
                    buffer.tpn = 0x50U + peerOrdinal;
                    buffer.doorbellVa = 0x10000000ULL + peerOrdinal * 0x10000ULL;
                    buffer.doorbellTokenId = 0x4000U + peerOrdinal;
                    buffer.sqDepth = 8;
                    buffer.localDoorbellVa = 0x20000000ULL + peerOrdinal * 0x10000ULL;
                    buffer.localDoorbellTokenId = 0x5000U + peerOrdinal;
                    buffer.localSqDepth = 8;
                    buffer.startJettyId = static_cast<uint16_t>(1024U + peerOrdinal);
                    buffer.endpointRouteVerified = true;
                }

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                TileXRCcuLowerLayerPlanBuilderReport report;
                if (TileXRCcuBuildLowerLayerTransportTemplate(
                        basic, allocation, buffers, &snapshot, &report) != TILEXR_SUCCESS) {
                    std::cerr << "template failed: " << report.message << "\n";
                    return 1;
                }
                for (uint32_t route = 0; route < snapshot.routes.size(); ++route) {
                    const uint16_t expected = static_cast<uint16_t>((route / 3U) * 32U);
                    if (snapshot.routes[route].wqeBasicBlockStartId != expected) {
                        std::cerr << "route WQE mismatch route=" << route << " observed="
                                  << snapshot.routes[route].wqeBasicBlockStartId << "\n";
                        return 2;
                    }
                }

                TileXRCcuLowerLayerInstallPlan plan;
                if (TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "plan failed: " << report.message << "\n";
                    return 3;
                }
                if (plan.jettys.size() != 1U || plan.jettys[0].ctxs.size() != 3U ||
                    plan.channels.size() != 9U) {
                    std::cerr << "unexpected plan shape\n";
                    return 4;
                }
                for (uint32_t peer = 0; peer < 3U; ++peer) {
                    if (DecodeWqeStart(plan.jettys[0].ctxs[peer]) != peer * 32U) {
                        std::cerr << "jetty context WQE mismatch peer=" << peer << "\n";
                        return 5;
                    }
                    for (uint32_t route = 0; route < 3U; ++route) {
                        if (DecodeChannelJetty(plan.channels[peer * 3U + route].ctx) != 1024U + peer) {
                            std::cerr << "channel jetty mismatch peer=" << peer << " route=" << route << "\n";
                            return 6;
                        }
                    }
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_plan_builder_rejects_incomplete_lower_layer_inputs(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuLowerLayerInstallPlan plan;
                TileXRCcuLowerLayerPlanBuilderReport report;
                TileXRCcuLowerLayerPlanSpec spec;
                if (TileXRCcuBuildLowerLayerInstallPlan(spec, &plan, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty lower-layer spec accepted\n";
                    return 1;
                }
                if (report.message.find("missing lower-layer CCU MSID token") == std::string::npos) {
                    std::cerr << "weak empty-spec diagnostic: " << report.message << "\n";
                    return 2;
                }

                spec.msidToken.valid = true;
                spec.msidToken.tokenId = 0;
                spec.pfe.dieId = 0;
                spec.pfe.pfeOffset = 1;
                spec.pfe.startJettyId = 0x10;
                spec.pfe.startLocalJettyCtxId = 0x2;
                TileXRCcuLowerLayerJettySpec jetty;
                jetty.startJettyCtxId = 0x2;
                jetty.doorbellVa = 0x1000;
                jetty.doorbellTokenId = 9;
                jetty.sqDepth = 8;
                spec.jettys.push_back(jetty);
                TileXRCcuLowerLayerChannelSpec channel;
                channel.channelId = 1;
                channel.sourcePfeId = 1;
                channel.startJettyId = 0x10;
                channel.memoryTokenId = 7;
                channel.remoteCcuVa = 0x200000;
                spec.channels.push_back(channel);
                spec.xnClear.valid = true;
                spec.xnClear.count = 1;
                spec.ckeClear.valid = true;
                spec.ckeClear.count = 1;
                if (TileXRCcuBuildLowerLayerInstallPlan(spec, &plan, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "channel with empty remote EID accepted\n";
                    return 3;
                }
                if (report.message.find("invalid CCU channel context v1 spec") == std::string::npos) {
                    std::cerr << "weak channel diagnostic: " << report.message << "\n";
                    return 4;
                }

                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    spec.channels[0].remoteEid[i] = static_cast<uint8_t>(0x40 + i);
                }
                if (TileXRCcuBuildLowerLayerInstallPlan(spec, &plan, &report) != TILEXR_SUCCESS) {
                    std::cerr << "valid zero-token lower-layer spec rejected: " << report.message << "\n";
                    return 5;
                }
                if (plan.msidTokens.empty() || plan.msidTokens[0].tokenId != 0 ||
                    plan.channels.empty() || plan.channels[0].ctx.raw[0] != 0x40) {
                    std::cerr << "valid zero-token lower-layer plan mismatch\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_plan_builder_surface_is_wired_into_tilexr_comm_without_udma_boundary(self):
        header = BUILDER_HEADER.read_text(encoding="utf-8")
        source = BUILDER_SOURCE.read_text(encoding="utf-8")
        runtime_header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime_source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TileXRCcuLowerLayerTransportSnapshot", header)
        self.assertIn("TileXRCcuLowerLayerTransportRoute", header)
        self.assertIn("TileXRCcuRemoteCcuBufferInfo", header)
        self.assertIn("TileXRCcuBuildLowerLayerTransportTemplate", header)
        self.assertIn("TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot", header)
        self.assertIn("remoteXnBindings", header)
        self.assertIn("localWaitCke", source)
        self.assertIn("TileXRCcuBuildPfeCtx", source)
        self.assertIn("TileXRCcuBuildLocalJettyCtx", source)
        self.assertIn("TileXRCcuBuildChannelCtxV1", source)
        self.assertIn("allocation.channels.num < remoteCcuBuffers.size()", source)
        self.assertIn("channel allocation count does not match lower-layer route count", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_SYNC_RESOURCE_MAP", source)
        self.assertNotIn("UseHcommTraceSyncResourceMap", source)
        self.assertIn("SelectLowerLayerWqeBasicBlockStride", source)
        self.assertIn("TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE", source)
        self.assertIn("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE", source)
        self.assertIn("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION", source)

        self.assertIn("TileXRCcuDirectRuntime", runtime_header)
        self.assertIn("ExportLowerLayerTransportSnapshot", runtime_header)
        self.assertIn("RegisterCcuResourceRmaBuffer", runtime_header)
        self.assertIn("ExportRemoteCcuRmaBuffers", runtime_header)
        self.assertIn("TileXRCcuLocalResourceWindowInfo", runtime_header)
        self.assertIn("TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot", runtime_source)
        self.assertIn("TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE", runtime_source)
        self.assertIn("UseImportedPeerEndpointRoute", runtime_source)
        self.assertIn("useImportedPeerRoute &&", runtime_source)
        self.assertIn("usePeerExportedRoute", runtime_source)
        self.assertRegex(
            runtime_source,
            r"(?s)\} else \{\s*remote\.tpn = peerWindow\.tpn;.*?usePeerExportedRoute",
        )
        self.assertNotIn(
            "} else if (!useImportedPeerRoute && TraceEndpointRoute() && peerWindow.endpointRouteVerified)",
            runtime_source,
        )
        self.assertNotIn("udma/", runtime_header + "\n" + runtime_source)

        combined = header + "\n" + source + "\n" + runtime_header + "\n" + runtime_source
        for needle in [
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "Hccl",
            "CcuDevMgrImp",
            "CcuResRepository",
            "CcuResBatchAllocator",
            "RT_RES_TYPE_CCU_XN",
            "RT_RES_TYPE_CCU_CKE",
        ]:
            self.assertNotIn(needle, combined)

    def test_tilexr_comm_caches_direct_ccu_lower_layer_plan_from_ccu_runtime(self):
        comm_header = COMM_HEADER_FILE.read_text(encoding="utf-8")
        comm_source = COMM_SOURCE_FILE.read_text(encoding="utf-8")
        backend_source = CCU_BACKEND_SOURCE.read_text(encoding="utf-8")
        planner_header = CCU_PLANNER_HEADER.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")

        for leaked in [
            'ccu/tilexr_ccu_lower_layer_plan_builder.h',
            'ccu/tilexr_ccu_direct_runtime.h',
            "ConfigureDirectCcuLowerLayerTemplate",
            "ConfigureDirectCcuLowerLayerTemplateFromAllocation",
            "RefreshDirectCcuLowerLayerPlan",
            "PrepareDirectCcuLowerLayerTemplateFromAllocation",
            "HasDirectCcuLowerLayerPlan",
            "GetDirectCcuLowerLayerPlanStatus",
            "GetDirectCcuLowerLayerPlanReport",
            "ccuDirectRuntime_",
        ]:
            with self.subTest(leaked=leaked):
                self.assertNotIn(leaked, comm_header)

        self.assertIn('ccu/tilexr_ccu_direct_orchestrator.h', planner_header + "\n" + planner_source)
        self.assertIn('ccu/tilexr_ccu_direct_runtime.h', CCU_RUNTIME_SESSION_HEADER.read_text(encoding="utf-8"))
        self.assertIn('ccu/tilexr_ccu_repository.h', planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::ConfigureDirectCcuLowerLayerTemplate", planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::ConfigureDirectCcuLowerLayerTemplateFromAllocation", planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerTemplateFromAllocation", planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::RefreshDirectCcuLowerLayerPlan", planner_source)
        self.assertIn("TileXRCcuBuildLowerLayerTransportTemplate", planner_source)
        self.assertIn("const std::vector<TileXRCcuRemoteCcuBufferInfo> &remoteCcuBuffers", planner_source)
        self.assertIn("TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot", planner_source)
        self.assertIn("session.RegisterCcuResourceRmaBuffer(basicInfo->resourceAddr)", planner_source)
        self.assertIn("session.ExportRemoteCcuRmaBuffers", planner_source)
        self.assertIn("ccuDirectRuntime_->ExportLowerLayerTransportSnapshot", runtime_session_source)
        self.assertIn("RefreshDirectCcuLowerLayerPlan(session);", planner_source)
        self.assertIn("direct CCU lower-layer template is not configured", planner_source)
        self.assertIn("direct CCU lower-layer install plan cached", planner_source)

        register_body = comm_source[
            comm_source.index("int TileXRComm::RegisterUDMAMemory"):
            comm_source.index("int TileXRComm::UnregisterUDMAMemory")
        ]
        self.assertIn("return udmaContext_->RegisterMemory(localPtr, bytes, handle);", register_body)
        self.assertNotIn("RefreshDirectCcuLowerLayerPlan();", register_body)
        self.assertNotIn("ResetDirectCcuLowerLayerPlan();", register_body)

        apply_state_body = comm_source[
            comm_source.index("int TileXRComm::ApplyUDMACommArgsState("):
            comm_source.index("int TileXRComm::ApplyUDMACommArgsStateCallback")
        ]
        self.assertIn("const int ret = UpdateCommArgsDev();", apply_state_body)

        unregister_body = comm_source[
            comm_source.index("int TileXRComm::UnregisterUDMAMemory"):
            comm_source.index("GM_ADDR TileXRComm::GetUDMARegistryPtr")
        ]
        self.assertNotIn("ResetDirectCcuLowerLayerPlan();", unregister_body)

        init_udma_body = comm_source[
            comm_source.index("int TileXRComm::InitUDMA"):
            comm_source.index("int TileXRComm::InitCcuBackend")
        ]
        self.assertNotIn("RefreshDirectCcuBasicInfo", init_udma_body)
        self.assertNotIn("ResetDirectCcuBasicInfo", init_udma_body)

        forbidden_patterns = [
            ("udmaTransport_->", "RegisterCcuResourceRmaBuffer"),
            ("udmaTransport_->", "ExportLocalCcuRmaBuffer"),
            ("udmaTransport_->", "ExportRemoteCcuRmaBuffers"),
            ("udmaTransport_->", "ExportLowerLayerTransportSnapshot"),
        ]
        for prefix, suffix in forbidden_patterns:
            self.assertNotIn(prefix + suffix, comm_header + "\n" + comm_source)
        for forbidden in [
            "rtCCULaunch",
            "TileXRCcuSubmitTask",
            "HcclGetCcuTaskInfo",
            "HcomGetCcuTaskInfo",
            "libhcomm",
            "libhccl_v2",
        ]:
            self.assertNotIn(forbidden, comm_header + "\n" + comm_source + "\n" + backend_source)

    def test_direct_ccu_runtime_owns_resource_window_boundary(self):
        runtime_header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime_source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")
        specs_header = (REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.h").read_text(encoding="utf-8")
        comm_header = COMM_HEADER_FILE.read_text(encoding="utf-8")
        comm_source = COMM_SOURCE_FILE.read_text(encoding="utf-8")
        backend_header = CCU_BACKEND_HEADER.read_text(encoding="utf-8")
        backend_source = CCU_BACKEND_SOURCE.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        builder_header = BUILDER_HEADER.read_text(encoding="utf-8")

        self.assertIn("TileXRCcuLocalResourceWindowInfo", runtime_header)
        self.assertIn("rawTokenId", runtime_header)
        self.assertIn("rawMemoryTokenId", builder_header)
        self.assertIn("TileXRCcuDirectAllGatherFn", runtime_header)
        self.assertIn("localResourceWindow_", runtime_header)
        self.assertIn("resourceWindowRegistered_", runtime_header)
        self.assertIn("int RegisterCcuResourceRmaBuffer(", runtime_header)
        self.assertIn("ExportLocalCcuRmaBuffer", runtime_header)
        self.assertIn("int ExportRemoteCcuRmaBuffers(", runtime_header)
        self.assertIn("int TileXRCcuDirectRuntime::RegisterCcuResourceRmaBuffer", runtime_source)
        self.assertIn("int TileXRCcuDirectRuntime::ExportLocalCcuRmaBuffer", runtime_source)
        self.assertIn("int TileXRCcuDirectRuntime::ExportRemoteCcuRmaBuffers", runtime_source)
        self.assertIn("int TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot", runtime_source)
        self.assertIn("localResourceWindow_.addr = resourceAddr", runtime_source)
        self.assertIn("resourceWindowRegistered_ = true", runtime_source)
        self.assertIn("TILEXR_CCU_RESOURCE_WINDOW_BYTES", specs_header)
        self.assertIn("72ULL * 1024ULL * 1024ULL", specs_header)
        self.assertIn("return TILEXR_CCU_RESOURCE_WINDOW_BYTES", runtime_source)
        self.assertIn("options_.allGather", runtime_source)
        self.assertIn("route.remoteEid", runtime_source)
        self.assertIn("route.doorbellVa", runtime_source)
        self.assertIn("route.sqDepth", runtime_source)

        for leaked in [
            "ConfigureDirectCcuLowerLayerTemplateFromAllocation(",
            "PrepareDirectCcuLowerLayerTemplateFromAllocation",
            "ExchangeDirectCcuRemoteNotifyCke",
            "DirectCcuAllGatherCallback",
            "DirectCcuThreadAllGather",
            "directCcuVerifiedEndpointRoutes_",
            "directCcuLocalVerifiedEndpointRoute_",
        ]:
            with self.subTest(leaked=leaked):
                self.assertNotIn(leaked, comm_header)
                self.assertNotIn(leaked, backend_header)

        self.assertIn("const std::vector<TileXRCcuRemoteCcuBufferInfo> &remoteCcuBuffers", planner_source)
        self.assertIn("ccuDirectRuntime_->RegisterCcuResourceRmaBuffer", runtime_session_source)
        self.assertIn("ccuDirectRuntime_->ExportLocalCcuRmaBuffer", runtime_session_source)
        self.assertIn("ccuDirectRuntime_->ExportRemoteCcuRmaBuffers", runtime_session_source)
        self.assertIn("runtimeOptions.allGather = &TileXRCcuRuntimeSession::DirectCcuAllGatherCallback", runtime_session_source)
        self.assertIn("runtimeOptions.allGatherUserData = this", runtime_session_source)
        self.assertIn("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke", planner_source)
        self.assertIn("int TileXRCcuRuntimeSession::DirectCcuThreadAllGather", runtime_session_source)
        self.assertIn("session.AllGather(&local, sizeof(local), all.data())", planner_source)
        self.assertIn("session->DirectCcuThreadAllGather(sendBuf, sendBytes, recvBuf)", runtime_session_source)
        self.assertIn("TileXRComm::InitCcuBackend", comm_source)
        self.assertIn("return ccuBackend_->Init(options);", comm_source)
        exchange_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback")
        ]
        self.assertNotIn("SelectDirectCcuRemoteBindingOverride", exchange_body)
        self.assertIn("peerLocalWaitCkeOffset", exchange_body)
        self.assertIn("peerResources.localWaitCkeCount", exchange_body)
        self.assertIn("peerResources.remoteNotifyCkeCount", exchange_body)
        self.assertNotIn("allocation.remoteNotifyCke.startId,\n            routeIndex", exchange_body)
        self.assertIn("allocation.localXn.startId", planner_source)
        self.assertIn("remoteXnStartId", exchange_body)
        self.assertIn("remoteXnCount", exchange_body)
        self.assertNotIn("TILEXR_CCU_V1_XN_RESOURCE_OFFSET", exchange_body)
        self.assertNotIn("TILEXR_CCU_XN_SLOT_BYTES", exchange_body)
        self.assertIn("remoteXnId", planner_source)
        self.assertIn("remoteNotifyCke", planner_source)
        self.assertIn("templateSnapshot.msidToken.tokenId = localCcuResourceWindow.tokenId", planner_source)
        self.assertIn("templateSnapshot.msidToken.tokenValue = localCcuResourceWindow.tokenValue", planner_source)
        self.assertIn("templateSnapshot.msidToken.valid = true", planner_source)
        self.assertIn("directCcuVerifiedEndpointRoutes_", planner_source)
        self.assertIn("TileXRCcuCollectivePlanner::ConfigureDirectCcuVerifiedEndpointRoutes", planner_source)
        self.assertIn("directCcuLocalVerifiedEndpointRoute_", planner_source)
        self.assertIn("TileXRCcuCollectivePlanner::ConfigureDirectCcuLocalVerifiedEndpointRoute", planner_source)
        self.assertIn("ccuDirectRuntime_->ConfigureLocalVerifiedEndpointRoute", runtime_session_source)
        self.assertIn("ccuDirectRuntime_->RefreshLocalVerifiedEndpointRoute", runtime_session_source)
        self.assertIn("TileXRCcuLocalEndpointRouteCollectorFn", runtime_header)
        self.assertIn("localEndpointRouteCollector", runtime_header)
        self.assertIn("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID", runtime_source)
        self.assertIn("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA", runtime_source)
        self.assertIn("direct CCU local endpoint route collected", runtime_source)
        self.assertIn("TileXRCcuOverlayVerifiedEndpointRoutes", builder_header)
        self.assertGreaterEqual(planner_source.count("TileXRCcuOverlayVerifiedEndpointRoutes("), 3)
        init_runtime_body = runtime_session_source[
            runtime_session_source.index("int TileXRCcuRuntimeSession::Init("):
            runtime_session_source.index("void TileXRCcuRuntimeSession::ResetDirectCcuBasicInfo")
        ]
        self.assertIn("logicDevId", init_runtime_body)
        self.assertIn("devicePhyId", init_runtime_body)
        self.assertIn("hdcType", init_runtime_body)
        self.assertIn("raInitialized", init_runtime_body)
        self.assertNotIn("udma/", runtime_header + "\n" + runtime_source)

        register_body = runtime_source[
            runtime_source.index("int TileXRCcuDirectRuntime::RegisterCcuResourceRmaBuffer"):
            runtime_source.index("int TileXRCcuDirectRuntime::ExportLocalCcuRmaBuffer")
        ]
        export_local_body = runtime_source[
            runtime_source.index("int TileXRCcuDirectRuntime::ExportLocalCcuRmaBuffer"):
            runtime_source.index("int TileXRCcuDirectRuntime::ExportRemoteCcuRmaBuffers")
        ]
        export_remote_body = runtime_source[
            runtime_source.index("int TileXRCcuDirectRuntime::ExportRemoteCcuRmaBuffers"):
            runtime_source.index("int TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot")
        ]
        prepare_from_allocation_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerTemplateFromAllocation"):
            planner_source.index("int TileXRCcuCollectivePlanner::FillDirectCcuLowerLayerPlanFromAllocation")
        ]
        export_snapshot_body = runtime_source[
            runtime_source.index("int TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot"):
        ]
        self.assertNotIn("int TileXRCcuDirectRuntime::RegisterCcuResourceRmaBuffer(uint64_t)\n{\n    return TILEXR_ERROR_NOT_FOUND;\n}", runtime_source)
        self.assertNotIn("*info = TileXRCcuLocalResourceWindowInfo{};\n    return TILEXR_ERROR_NOT_FOUND;", export_local_body)
        self.assertNotIn("buffers->clear();\n    return TILEXR_ERROR_NOT_FOUND;", export_remote_body)
        self.assertNotIn("*snapshot = TileXRCcuLowerLayerTransportSnapshot{};\n    return TILEXR_ERROR_NOT_FOUND;", export_snapshot_body)
        self.assertLess(
            prepare_from_allocation_body.index("session.ConfigureLocalVerifiedEndpointRoute"),
            prepare_from_allocation_body.index("session.ExportRemoteCcuRmaBuffers"),
        )

        register_memory_body = comm_source[
            comm_source.index("int TileXRComm::RegisterUDMAMemory"):
            comm_source.index("int TileXRComm::UnregisterUDMAMemory")
        ]
        self.assertNotIn("RefreshDirectCcuLowerLayerPlan();", register_memory_body)
        self.assertNotIn("ResetDirectCcuLowerLayerPlan();", register_memory_body)

    def test_remote_xn_exchange_uses_peer_channel_bound_remote_xn_operand(self):
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        exchange_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback")
        ]
        compact_body = " ".join(exchange_body.split())

        self.assertIn(
            "peerLocalXnId = static_cast<uint16_t>(static_cast<uint32_t>(peerResources.localXnStartId) + peerLocalXnOffset)",
            compact_body)
        self.assertIn("selectedRemoteXnOffset >= peerResources.remoteXnCount", compact_body)
        self.assertIn("peerLocalIndex * routesPerPeer + routeWithinPeer", compact_body)
        self.assertIn("peerResources.remoteXnStartId", compact_body)
        self.assertNotIn("SelectDirectCcuRemoteBindingOverride", compact_body)
        self.assertNotIn("(*remoteCcuBuffers)[routeIndex].remoteCcuVa +=", compact_body)
        self.assertNotIn("static_cast<uint64_t>(peerLocalXnId) * TILEXR_CCU_XN_SLOT_BYTES", compact_body)
        self.assertNotIn("TILEXR_CCU_V1_XN_RESOURCE_OFFSET + static_cast<uint64_t>(peerLocalXnId)", compact_body)
        self.assertIn("(*remoteCcuBuffers)[routeIndex].remoteXnId = channelBoundRemoteXnId", compact_body)
        self.assertNotIn(
            "channelBoundRemoteXnId = static_cast<uint16_t>(allocation.remoteXn.startId + routeIndex)",
            compact_body)
        self.assertNotIn(
            "static_cast<uint64_t>((*remoteCcuBuffers)[routeIndex].remoteXnId) * TILEXR_CCU_XN_SLOT_BYTES",
            compact_body)

    def test_lower_layer_clears_the_complete_allocated_remote_xn_range(self):
        source = BUILDER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("result.remoteXnStartId = allocation.remoteXn.startId", source)
        self.assertIn("result.remoteXnCount = allocation.remoteXn.num", source)
        self.assertIn("snapshot.remoteXnStartId", source)
        self.assertIn("snapshot.remoteXnCount", source)

    def test_remote_notify_cke_targets_peer_local_wait_cke(self):
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        exchange_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback")
        ]
        compact_body = " ".join(exchange_body.split())

        self.assertIn("peerLocalWaitCkeOffset", exchange_body)
        self.assertIn("peerLocalWaitCkeOffset >= peerResources.localWaitCkeCount", compact_body)
        self.assertIn("peerLocalWaitCkeOffset >= peerResources.remoteNotifyCkeCount", compact_body)
        self.assertIn(
            "remoteNotifyCke = static_cast<uint16_t>(static_cast<uint32_t>(peerResources.localWaitCkeStartId) + peerLocalWaitCkeOffset)",
            compact_body)
        self.assertIn("remoteNotifyCke) >= peerResources.localWaitCkeStartId", compact_body)
        self.assertIn("peerResources.localWaitCkeStartId) + peerResources.localWaitCkeCount", compact_body)
        self.assertNotIn(
            "remoteNotifyCke = SelectDirectCcuRemoteNotifyCkeId( allocation.remoteNotifyCke.startId, routeIndex)",
            compact_body)
        self.assertIn("notifyCkeOwnerVerified", exchange_body)
        self.assertIn("notifyCkeOwnerVerified &&", compact_body)

    def test_peer_xn_exchange_expands_one_peer_window_to_multiple_sync_routes(self):
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        exchange_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback")
        ]
        compact_body = " ".join(exchange_body.split())

        self.assertIn("const size_t peerRouteCount = static_cast<size_t>(rankSize - 1)", compact_body)
        self.assertIn("const size_t syncRouteCount = allocation.remoteXn.num", compact_body)
        self.assertIn("allocation.remoteXn.num < routedPeerCount", compact_body)
        self.assertNotIn("allocation.remoteXn.num != static_cast<uint16_t>(rankSize - 1)", compact_body)
        self.assertIn("peerCcuBuffersByRank", compact_body)
        self.assertIn("peerCcuBuffer.peerRank", compact_body)
        self.assertIn("invalid direct CCU peer buffer rank mapping", compact_body)
        self.assertIn("incomplete direct CCU peer buffer rank mapping", compact_body)
        self.assertIn("remoteCcuBuffers->assign(syncRouteCount, TileXRCcuRemoteCcuBufferInfo{})", compact_body)
        self.assertIn("for (uint32_t syncIndex = 0; syncIndex < allocation.remoteXn.num; ++syncIndex)", compact_body)
        self.assertIn("const size_t peerBufferIndex = syncIndex / routesPerPeer", compact_body)
        self.assertIn(
            "(*remoteCcuBuffers)[routeIndex] = *peerCcuBuffersByRank[static_cast<size_t>(peer)]",
            compact_body)
        self.assertIn("peerLocalIndex * routesPerPeer + routeWithinPeer", compact_body)
        self.assertIn("peerResources.remoteXnStartId", compact_body)
        self.assertIn("DirectCcuRemoteXnProofSpan(allocation.remoteXn.num)", compact_body)

    def test_direct_ccu_runtime_imports_peer_endpoint_route_before_export(self):
        runtime_source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")
        export_body = runtime_source[
            runtime_source.index("int TileXRCcuDirectRuntime::ExportRemoteCcuRmaBuffers"):
            runtime_source.index("int TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot")
        ]
        compact_body = " ".join(export_body.split())

        self.assertIn("ImportPeerEndpointRoute(", runtime_source)
        self.assertIn(
            "if (importedPeerRoute) { remote.remoteEid = importedRoute.remoteEid; remote.tpn = importedRoute.tpn",
            compact_body)
        self.assertIn("} else { remote.tpn = peerWindow.tpn", compact_body)
        self.assertIn("TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET", runtime_source)
        self.assertIn("const uint64_t remoteCcuVaOffset = SelectRemoteCcuVaOffset()", compact_body)
        self.assertIn("remote.remoteCcuVa = peerWindow.addr + remoteCcuVaOffset", compact_body)
        self.assertNotIn("remote.remoteCcuVa = peerWindow.addr;", compact_body)
        self.assertIn("remote.localDoorbellVa = localVerifiedEndpointRoute_.doorbellVa", compact_body)
        self.assertIn("remote.localDoorbellTokenId = localVerifiedEndpointRoute_.doorbellTokenId", compact_body)
        self.assertIn("remote.localDoorbellTokenValue = localVerifiedEndpointRoute_.doorbellTokenValue", compact_body)

    def test_peer_endpoints_keep_per_peer_resource_and_jetty_tokens(self):
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("offer.resourceTokenId = state.resourceWindow.tokenId", source)
        self.assertIn("offer.resourceTokenValue = state.resourceWindow.tokenValue", source)
        self.assertIn("offer.jettyTokenValue = state.resourceWindow.tokenValue", source)
        self.assertIn("importInfo.in.ub.tokenValue = peerOffer.jettyTokenValue", source)
        self.assertIn("state.route.memoryTokenValue = peerOffer.resourceTokenValue", source)

    def test_direct_ccu_runtime_can_override_resource_window_token_from_rank_env(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int FakeRaCustomChannel(TileXRCcuRaInfo, TileXRCcuCustomChannelIn*, TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.loader_.RaCustomChannel = FakeRaCustomChannel;
                runtime.loader_.loaded_ = true;
                runtime.options_.rank = 1;
                runtime.cachedBasicInfoValid_ = true;
                runtime.cachedBasicInfo_.resourceAddr = 0x10000000ULL;
                runtime.cachedBasicInfo_.msidToken.tokenId = 0x1234;
                runtime.cachedBasicInfo_.msidToken.tokenValue = 0x5678;
                runtime.cachedBasicInfo_.msidToken.valid = true;
                runtime.cachedBasicInfo_.caps.cap1 = 7U << 16U;

                if (runtime.RegisterCcuResourceRmaBuffer(0x10000000ULL) != TILEXR_SUCCESS) {
                    std::cerr << "register failed\n";
                    return 1;
                }
                TileXRCcuLocalResourceWindowInfo local;
                if (runtime.ExportLocalCcuRmaBuffer(&local) != TILEXR_SUCCESS) {
                    std::cerr << "export failed\n";
                    return 2;
                }
                if (local.tokenId != 0x2222U ||
                    local.rawTokenId != 0x3333U ||
                    local.tokenValue != 0x4444U) {
                    std::cerr << "rank override was not applied tokenId=" << local.tokenId
                              << " rawTokenId=" << local.rawTokenId
                              << " tokenValue=" << local.tokenValue << "\n";
                    return 3;
                }

                TileXRCcuDirectRuntime defaultRuntime;
                defaultRuntime.initialized_ = true;
                defaultRuntime.loader_.RaCustomChannel = FakeRaCustomChannel;
                defaultRuntime.loader_.loaded_ = true;
                defaultRuntime.options_.rank = 0;
                defaultRuntime.cachedBasicInfoValid_ = true;
                defaultRuntime.cachedBasicInfo_ = runtime.cachedBasicInfo_;
                if (defaultRuntime.RegisterCcuResourceRmaBuffer(0x10000000ULL) != TILEXR_SUCCESS ||
                    defaultRuntime.ExportLocalCcuRmaBuffer(&local) != TILEXR_SUCCESS) {
                    std::cerr << "default register/export failed\n";
                    return 4;
                }
                if (local.tokenId != 0x1111U ||
                    local.rawTokenId != 0x1111U ||
                    local.tokenValue != 0x7777U) {
                    std::cerr << "common override fallback was not applied\n";
                    return 5;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID"] = "0x1111"
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE"] = "0x7777"
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_RANK1"] = "0x2222"
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID_RANK1"] = "0x3333"
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_RANK1"] = "0x4444"

        result = self.compile_and_run(
            code,
            env=env,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_can_register_resource_window_with_public_ra_ctx(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;
            constexpr uint64_t kResourceAddr = 0x10001234ULL;
            constexpr uint64_t kAlignedResourceAddr = 0x10001000ULL;
            constexpr uint64_t kAlignedResourceBytes =
                TILEXR_CCU_RESOURCE_WINDOW_BYTES + (kResourceAddr - kAlignedResourceAddr);

            int FakeRaCustomChannel(TileXRCcuRaInfo, TileXRCcuCustomChannelIn*, TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            int FakeRaGetDevEidInfoNum(TileXRCcuRaInfo, uint32_t* num)
            {
                *num = 1;
                return 0;
            }

            int FakeRaGetDevEidInfoList(TileXRCcuRaInfo, TileXRCcuHccpDevEidInfo* list, uint32_t* num)
            {
                if (list == nullptr || num == nullptr || *num != 1) {
                    return -1;
                }
                list[0].eidIndex = 3;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    list[0].eid.raw[i] = static_cast<uint8_t>(0xa0 + i);
                }
                return 0;
            }

            int FakeRaCtxInit(TileXRCcuHccpCtxInitCfg*, TileXRCcuHccpCtxInitAttr* attr, void** ctx)
            {
                if (attr == nullptr || attr->phyId != 0x55 || attr->ub.eidIndex != 3) {
                    return -1;
                }
                *ctx = reinterpret_cast<void*>(0x1000);
                return 0;
            }

            int FakeRaCtxDeinit(void*)
            {
                return 0;
            }

            int FakeRaCtxTokenIdAlloc(void* ctx, TileXRCcuHccpTokenId* token, void** tokenHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000)) {
                    return -1;
                }
                token->tokenId = 0x12345600U;
                *tokenHandle = reinterpret_cast<void*>(0x2000);
                return 0;
            }

            int FakeRaCtxTokenIdFree(void*, void*)
            {
                return 0;
            }

            int FakeRaGetSecRandom(TileXRCcuRaInfo* info, uint32_t* value)
            {
                if (info == nullptr || info->phyId != 0x55 || info->mode != TILEXR_CCU_NETWORK_OFFLINE) {
                    return -1;
                }
                *value = 0xabcdef01U;
                return 0;
            }

            int FakeRaCtxLmemRegister(void* ctx, TileXRCcuHccpMrRegInfo* mr, void** handle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || mr == nullptr ||
                    mr->in.mem.addr != kAlignedResourceAddr || mr->in.mem.size != kAlignedResourceBytes ||
                    mr->in.ub.tokenValue != 0xabcdef01U ||
                    mr->in.ub.tokenIdHandle != reinterpret_cast<void*>(0x2000) ||
                    mr->in.ub.flags.bs.tokenIdValid != 1 ||
                    mr->in.ub.flags.bs.nonPin != 1) {
                    return -1;
                }
                mr->out.ub.tokenId = 0x12345600U;
                mr->out.ub.targetSegHandle = 0x4455667788ULL;
                mr->out.key.size = 5;
                *handle = reinterpret_cast<void*>(0x3000);
                return 0;
            }

            int FakeRaCtxLmemUnregister(void*, void*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.loader_.RaCustomChannel = FakeRaCustomChannel;
                runtime.loader_.loaded_ = true;
                runtime.loader_.RaGetDevEidInfoNum = FakeRaGetDevEidInfoNum;
                runtime.loader_.RaGetDevEidInfoList = FakeRaGetDevEidInfoList;
                runtime.loader_.RaCtxInit = FakeRaCtxInit;
                runtime.loader_.RaCtxDeinit = FakeRaCtxDeinit;
                runtime.loader_.RaCtxTokenIdAlloc = FakeRaCtxTokenIdAlloc;
                runtime.loader_.RaCtxTokenIdFree = FakeRaCtxTokenIdFree;
                runtime.loader_.RaGetSecRandom = FakeRaGetSecRandom;
                runtime.loader_.RaCtxLmemRegister = FakeRaCtxLmemRegister;
                runtime.loader_.RaCtxLmemUnregister = FakeRaCtxLmemUnregister;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 0;
                runtime.cachedBasicInfoValid_ = true;
                runtime.cachedBasicInfo_.resourceAddr = kResourceAddr;
                runtime.cachedBasicInfo_.msidToken.tokenId = 0x1111;
                runtime.cachedBasicInfo_.msidToken.tokenValue = 0x2222;
                runtime.cachedBasicInfo_.msidToken.valid = true;
                runtime.cachedBasicInfo_.caps.cap1 = 7U << 16U;

                if (runtime.RegisterCcuResourceRmaBuffer(kResourceAddr) != TILEXR_SUCCESS) {
                    std::cerr << "ra ctx resource window register failed\n";
                    return 1;
                }

                TileXRCcuLocalResourceWindowInfo local;
                if (runtime.ExportLocalCcuRmaBuffer(&local) != TILEXR_SUCCESS) {
                    std::cerr << "export failed\n";
                    return 2;
                }
                if (local.tokenId != 0x123456U ||
                    local.rawTokenId != 0x12345600U ||
                    local.tokenValue != 0xabcdef01U ||
                    local.addr != kResourceAddr ||
                    local.bytes != TILEXR_CCU_RESOURCE_WINDOW_BYTES) {
                    std::cerr << "unexpected registered resource window tokenId=" << local.tokenId
                              << " rawTokenId=" << local.rawTokenId
                              << " tokenValue=" << local.tokenValue
                              << " bytes=" << local.bytes << "\n";
                    return 3;
                }

                runtime.Shutdown();
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE"] = "ra_ctx"

        result = self.compile_and_run(
            code,
            env=env,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_source_supports_selecting_ra_ctx_resource_window_eid(self):
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX", source)
        self.assertIn("BuildRaCtxResourceWindowEidCandidates", source)
        self.assertIn("SelectRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_ENV", source)
        self.assertIn("SelectDirectCcuCleanupDieId()", source)
        self.assertIn("it->dieId == dieId", source)
        self.assertIn("TILEXR_CCU_UBOE_DEV_FLAG_RIGHT_SHIFT = 19U", source)
        self.assertIn("QueryTpHandleForPeer(ctxHandle, candidateEid, candidateEid", source)
        self.assertIn("loopEidCandidate", source)
        self.assertIn("TraceRaCtxEidInfos", source)
        self.assertIn("ctxAttr.ub.eidIndex = candidate.eidIndex", source)
        self.assertEqual(2, source.count(
            "qpAttr.ub.errTimeout = TILEXR_CCU_DIRECT_ENDPOINT_ERR_TIMEOUT"))

    def test_direct_ccu_runtime_can_select_ra_ctx_resource_window_eid_by_env(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;
            constexpr uint64_t kResourceAddr = 0x10001234ULL;
            constexpr uint64_t kAlignedResourceAddr = 0x10001000ULL;
            constexpr uint64_t kAlignedResourceBytes =
                TILEXR_CCU_RESOURCE_WINDOW_BYTES + (kResourceAddr - kAlignedResourceAddr);

            int FakeRaCustomChannel(TileXRCcuRaInfo, TileXRCcuCustomChannelIn*, TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            int FakeRaGetDevEidInfoNum(TileXRCcuRaInfo, uint32_t* num)
            {
                *num = 2;
                return 0;
            }

            int FakeRaGetDevEidInfoList(TileXRCcuRaInfo, TileXRCcuHccpDevEidInfo* list, uint32_t* num)
            {
                if (list == nullptr || num == nullptr || *num != 2) {
                    return -1;
                }
                list[0].eidIndex = 3;
                list[1].eidIndex = 9;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    list[0].eid.raw[i] = static_cast<uint8_t>(0xa0 + i);
                    list[1].eid.raw[i] = static_cast<uint8_t>(0xc0 + i);
                }
                return 0;
            }

            int FakeRaCtxInit(TileXRCcuHccpCtxInitCfg*, TileXRCcuHccpCtxInitAttr* attr, void** ctx)
            {
                if (attr == nullptr || attr->ub.eidIndex != 9 || attr->ub.eid.raw[0] != 0xc0) {
                    return -1;
                }
                *ctx = reinterpret_cast<void*>(0x1000);
                return 0;
            }

            int FakeRaCtxDeinit(void*) { return 0; }
            int FakeRaCtxTokenIdAlloc(void*, TileXRCcuHccpTokenId* token, void** tokenHandle)
            {
                token->tokenId = 0x12345600U;
                *tokenHandle = reinterpret_cast<void*>(0x2000);
                return 0;
            }
            int FakeRaCtxTokenIdFree(void*, void*) { return 0; }
            int FakeRaGetSecRandom(TileXRCcuRaInfo*, uint32_t* value)
            {
                *value = 0xabcdef01U;
                return 0;
            }
            int FakeRaCtxLmemRegister(void*, TileXRCcuHccpMrRegInfo* mr, void** handle)
            {
                if (mr == nullptr || mr->in.mem.addr != kAlignedResourceAddr ||
                    mr->in.mem.size != kAlignedResourceBytes) {
                    return -1;
                }
                mr->out.ub.tokenId = 0x12345600U;
                mr->out.ub.targetSegHandle = 0x4455667788ULL;
                *handle = reinterpret_cast<void*>(0x3000);
                return 0;
            }
            int FakeRaCtxLmemUnregister(void*, void*) { return 0; }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.loader_.RaCustomChannel = FakeRaCustomChannel;
                runtime.loader_.loaded_ = true;
                runtime.loader_.RaGetDevEidInfoNum = FakeRaGetDevEidInfoNum;
                runtime.loader_.RaGetDevEidInfoList = FakeRaGetDevEidInfoList;
                runtime.loader_.RaCtxInit = FakeRaCtxInit;
                runtime.loader_.RaCtxDeinit = FakeRaCtxDeinit;
                runtime.loader_.RaCtxTokenIdAlloc = FakeRaCtxTokenIdAlloc;
                runtime.loader_.RaCtxTokenIdFree = FakeRaCtxTokenIdFree;
                runtime.loader_.RaGetSecRandom = FakeRaGetSecRandom;
                runtime.loader_.RaCtxLmemRegister = FakeRaCtxLmemRegister;
                runtime.loader_.RaCtxLmemUnregister = FakeRaCtxLmemUnregister;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 1;
                runtime.cachedBasicInfoValid_ = true;
                runtime.cachedBasicInfo_.resourceAddr = kResourceAddr;
                runtime.cachedBasicInfo_.caps.cap1 = 7U << 16U;

                if (runtime.RegisterCcuResourceRmaBuffer(kResourceAddr) != TILEXR_SUCCESS) {
                    std::cerr << "ra ctx resource window register failed\n";
                    return 1;
                }
                TileXRCcuLocalResourceWindowInfo local;
                if (runtime.ExportLocalCcuRmaBuffer(&local) != TILEXR_SUCCESS ||
                    local.eidIndex != 9 || local.eid[0] != 0xc0) {
                    std::cerr << "wrong eid selection index=" << local.eidIndex
                              << " first=" << static_cast<uint32_t>(local.eid[0]) << "\n";
                    return 2;
                }
                runtime.Shutdown();
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE"] = "ra_ctx"
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_RANK1"] = "9"

        result = self.compile_and_run(
            code,
            env=env,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_collects_ra_ctx_endpoint_route_when_resource_window_uses_ra_ctx(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstdlib>
            #include <cstring>
            #include <iostream>
            #include <vector>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;
            constexpr uint32_t kExpectedSqDepth = 8;

            int EchoLocalExchangeAsPeer(const void* sendBuf, size_t sendBytes, void* recvBuf, void*)
            {
                if (sendBuf == nullptr || recvBuf == nullptr ||
                    sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                if (!local->endpointRouteVerified ||
                    local->remoteEid[0] != 0xb0 ||
                    local->tpn != 0x010203 ||
                    local->doorbellVa != 0x1122334455667788ULL ||
                    local->doorbellTokenId != 0x3456 ||
                    local->sqDepth != kExpectedSqDepth) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[1] = *local;
                return TILEXR_SUCCESS;
            }

            int FakeRaCtxCqCreate(void* ctx, TileXRCcuHccpCqInfo*, void** cqHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000)) {
                    return -1;
                }
                *cqHandle = reinterpret_cast<void*>(0x2000);
                return 0;
            }

            int FakeRaCtxCqDestroy(void*, void*)
            {
                return 0;
            }

            int FakeRaCtxQpCreate(
                void* ctx,
                TileXRCcuHccpQpCreateAttr* attr,
                TileXRCcuHccpQpCreateInfo* info,
                void** qpHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) ||
                    attr == nullptr ||
                    attr->ub.tokenIdHandle != reinterpret_cast<void*>(0x1100) ||
                    attr->ub.tokenValue != 0xabcdef01U ||
                    info == nullptr) {
                    return -1;
                }
                info->key.size = 4;
                info->ub.dbAddr = 0x1122334455667788ULL;
                info->ub.dbTokenId = 0x345600U;
                *qpHandle = reinterpret_cast<void*>(0x3000);
                return 0;
            }

            int FakeRaCtxQpDestroy(void*)
            {
                return 0;
            }

            int FakeRaGetTpInfoListAsync(
                void* ctx,
                TileXRCcuHccpGetTpCfg*,
                TileXRCcuHccpTpInfo infoList[],
                uint32_t* num,
                void** reqHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) ||
                    infoList == nullptr ||
                    num == nullptr ||
                    *num == 0) {
                    return -1;
                }
                infoList[0].tpHandle = 0x99887766ULL;
                *num = 1;
                *reqHandle = reinterpret_cast<void*>(0x4000);
                return 0;
            }

            int FakeRaGetAsyncReqResult(void* reqHandle, int* reqResult)
            {
                if (reqHandle != reinterpret_cast<void*>(0x4000) || reqResult == nullptr) {
                    return -1;
                }
                *reqResult = 0;
                return 0;
            }

            int FakeRaCtxQpImport(void* ctx, TileXRCcuHccpQpImportInfo* info, void** remoteQpHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) ||
                    info == nullptr ||
                    info->in.ub.expImportCfg.tpHandle != 0x99887766ULL) {
                    return -1;
                }
                info->out.ub.tpn = 0x010203;
                *remoteQpHandle = reinterpret_cast<void*>(0x5000);
                return 0;
            }

            int FakeRaCtxQpUnimport(void*, void*)
            {
                return 0;
            }

            int main()
            {
                unsetenv("TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE");
                unsetenv("TILEXR_CCU_ENDPOINT_ROUTE_PROVIDER");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_ID");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_VALUE");
                unsetenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_SQ_DEPTH");

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = EchoLocalExchangeAsPeer;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = TILEXR_CCU_RESOURCE_WINDOW_BYTES;
                runtime.localResourceWindow_.tokenId = 0x123456U;
                runtime.localResourceWindow_.rawTokenId = 0x12345600U;
                runtime.localResourceWindow_.tokenValue = 0xabcdef01U;
                runtime.localResourceWindow_.raCtxHandle = reinterpret_cast<void*>(0x1000);
                runtime.localResourceWindow_.tokenIdHandle = reinterpret_cast<void*>(0x1100);
                runtime.localResourceWindow_.raCtxRegistered = true;
                for (uint32_t i = 0; i < runtime.localResourceWindow_.eid.size(); ++i) {
                    runtime.localResourceWindow_.eid[i] = static_cast<uint8_t>(0xb0 + i);
                }
                runtime.loader_.RaCtxCqCreate = FakeRaCtxCqCreate;
                runtime.loader_.RaCtxCqDestroy = FakeRaCtxCqDestroy;
                runtime.loader_.RaCtxQpCreate = FakeRaCtxQpCreate;
                runtime.loader_.RaCtxQpDestroy = FakeRaCtxQpDestroy;
                runtime.loader_.RaCtxQpImport = FakeRaCtxQpImport;
                runtime.loader_.RaCtxQpUnimport = FakeRaCtxQpUnimport;
                runtime.loader_.RaGetTpInfoListAsync = FakeRaGetTpInfoListAsync;
                runtime.loader_.RaGetAsyncReqResult = FakeRaGetAsyncReqResult;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_SUCCESS) {
                    std::cerr << "ra ctx endpoint route was not collected by default: "
                              << report.message << "\n";
                    return 1;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed after ra ctx route collection\n";
                    return 2;
                }
                if (buffers.size() != 1 ||
                    !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0xbf ||
                    buffers[0].remoteEid[15] != 0xb0 ||
                    buffers[0].tpn != 0x010203 ||
                    buffers[0].doorbellVa != 0x1122334455667788ULL ||
                    buffers[0].doorbellTokenId != 0x3456 ||
                    buffers[0].doorbellTokenValue != 0xabcdef01U ||
                    buffers[0].sqDepth != kExpectedSqDepth) {
                    std::cerr << "ra ctx collected endpoint route was not exported\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE"] = "ra_ctx"

        result = self.compile_and_run(
            code,
            env=env,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_rejects_zero_resource_window_token_override(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int FakeRaCustomChannel(TileXRCcuRaInfo, TileXRCcuCustomChannelIn*, TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.loader_.RaCustomChannel = FakeRaCustomChannel;
                runtime.loader_.loaded_ = true;
                runtime.options_.rank = 0;
                runtime.cachedBasicInfoValid_ = true;
                runtime.cachedBasicInfo_.resourceAddr = 0x10000000ULL;
                runtime.cachedBasicInfo_.msidToken.tokenId = 0x1234;
                runtime.cachedBasicInfo_.msidToken.valid = true;

                if (runtime.RegisterCcuResourceRmaBuffer(0x10000000ULL) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "zero token override was accepted\n";
                    return 1;
                }
                TileXRCcuLocalResourceWindowInfo local;
                if (runtime.ExportLocalCcuRmaBuffer(&local) != TILEXR_ERROR_NOT_INITIALIZED) {
                    std::cerr << "resource window remained registered after invalid override\n";
                    return 2;
                }
                return 0;
            }
            '''
        )
        env = os.environ.copy()
        env["TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID"] = "0"

        result = self.compile_and_run(
            code,
            env=env,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_preserves_verified_endpoint_routes_from_template(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x1234;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.localResourceWindow_.addr = 0x100000000ULL;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.tokenValue = 0x5678;

                TileXRCcuLowerLayerTransportSnapshot templ;
                templ.dieId = 1;
                templ.xnStartId = 1961;
                TileXRCcuLowerLayerTransportRoute route;
                route.peerRank = 1;
                route.channelId = 9;
                for (uint32_t i = 0; i < route.remoteEid.size(); ++i) {
                    route.remoteEid[i] = static_cast<uint8_t>(0xa0 + i);
                }
                route.tpn = 0;
                route.doorbellVa = 0x1122334455667788ULL;
                route.doorbellTokenId = 0x2345;
                route.doorbellTokenValue = 0;
                route.sqDepth = 64;
                route.endpointRouteVerified = true;
                templ.routes.push_back(route);

                TileXRCcuLowerLayerTransportRoute synthetic = route;
                synthetic.peerRank = 2;
                synthetic.channelId = 10;
                synthetic.remoteEid = {};
                synthetic.tpn = 0;
                synthetic.doorbellVa = 0;
                synthetic.doorbellTokenId = 0;
                synthetic.doorbellTokenValue = 0;
                synthetic.sqDepth = 0;
                synthetic.endpointRouteVerified = true;
                templ.routes.push_back(synthetic);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                if (runtime.ExportLowerLayerTransportSnapshot(templ, &snapshot) != TILEXR_SUCCESS) {
                    std::cerr << "export snapshot failed\n";
                    return 4;
                }
                if (snapshot.routes.size() != 2 || !snapshot.routes[0].endpointRouteVerified ||
                    snapshot.routes[0].remoteEid[0] != 0xa0 ||
                    snapshot.routes[0].tpn != route.tpn ||
                    snapshot.routes[0].doorbellVa != route.doorbellVa ||
                    snapshot.routes[0].doorbellTokenId != route.doorbellTokenId ||
                    snapshot.routes[0].doorbellTokenValue != route.doorbellTokenValue ||
                    snapshot.routes[0].sqDepth != route.sqDepth) {
                    std::cerr << "verified endpoint route was not preserved\n";
                    return 5;
                }
                if (snapshot.routes[1].endpointRouteVerified ||
                    snapshot.routes[1].remoteEid[0] == 0 ||
                    snapshot.routes[1].tpn == 0 ||
                    snapshot.routes[1].doorbellVa == 0 ||
                    snapshot.routes[1].doorbellTokenId != runtime.localResourceWindow_.tokenId ||
                    snapshot.routes[1].doorbellTokenValue != runtime.localResourceWindow_.tokenValue ||
                    snapshot.routes[1].sqDepth == 0) {
                    std::cerr << "synthetic endpoint route did not fail closed\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_can_trust_synthetic_endpoint_routes_only_with_diagnostic_env(self):
        code = textwrap.dedent(
            r'''
            #include <cstdlib>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE", "1", 1);

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x1234;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.localResourceWindow_.addr = 0x100000000ULL;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.tokenValue = 0x5678;

                TileXRCcuLowerLayerTransportSnapshot templ;
                templ.dieId = 1;
                templ.xnStartId = 1961;
                TileXRCcuLowerLayerTransportRoute route;
                route.peerRank = 1;
                route.channelId = 9;
                route.endpointRouteVerified = true;
                templ.routes.push_back(route);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                if (runtime.ExportLowerLayerTransportSnapshot(templ, &snapshot) != TILEXR_SUCCESS) {
                    std::cerr << "export snapshot failed\n";
                    return 1;
                }
                if (snapshot.routes.size() != 1 ||
                    !snapshot.routes[0].endpointRouteVerified ||
                    snapshot.routes[0].remoteEid[0] == 0 ||
                    snapshot.routes[0].tpn == 0 ||
                    snapshot.routes[0].doorbellVa == 0 ||
                    snapshot.routes[0].doorbellTokenId != runtime.localResourceWindow_.tokenId ||
                    snapshot.routes[0].doorbellTokenValue != runtime.localResourceWindow_.tokenValue ||
                    snapshot.routes[0].sqDepth == 0) {
                    std::cerr << "synthetic endpoint route was not trusted under diagnostic env\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_exchanges_verified_endpoint_route_with_resource_window(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int FakeAllGather(const void* sendBuf, size_t sendBytes, void* recvBuf, void* userData)
            {
                (void)sendBuf;
                if (sendBytes != sizeof(TileXRCcuResourceWindowExchange) || userData == nullptr) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                auto* peer = static_cast<TileXRCcuResourceWindowExchange*>(userData);
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[1] = *peer;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuResourceWindowExchange peer {};
                peer.addr = 0x0000009234000000ULL;
                peer.bytes = 0x2000;
                peer.tokenId = 0x23456;
                peer.rawTokenId = 0x33456;
                peer.tokenValue = 0x5678;
                for (uint32_t i = 0; i < peer.remoteEid.size(); ++i) {
                    peer.remoteEid[i] = static_cast<uint8_t>(0x90 + i);
                }
                peer.tpn = 0x010203;
                peer.doorbellVa = 0x1122334455667788ULL;
                peer.doorbellTokenId = 0x3456;
                peer.doorbellTokenValue = 0;
                peer.sqDepth = 64;
                peer.endpointRouteVerified = true;
                peer.channelResourceOwnerVerified = true;
                peer.transportResourceExchangeVerified = true;

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = FakeAllGather;
                runtime.options_.allGatherUserData = &peer;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed\n";
                    return 1;
                }
                if (buffers.size() != 1 ||
                    buffers[0].remoteCcuVa != peer.addr ||
                    buffers[0].memoryTokenId != peer.tokenId ||
                    buffers[0].rawMemoryTokenId != peer.rawTokenId ||
                    buffers[0].memoryTokenValue != peer.tokenValue ||
                    !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0x9f ||
                    buffers[0].remoteEid[15] != 0x90 ||
                    buffers[0].tpn != peer.tpn ||
                    buffers[0].doorbellVa != peer.doorbellVa ||
                    buffers[0].doorbellTokenId != peer.doorbellTokenId ||
                    buffers[0].doorbellTokenValue != peer.doorbellTokenValue ||
                    buffers[0].sqDepth != peer.sqDepth ||
                    !buffers[0].channelResourceOwnerVerified ||
                    !buffers[0].transportResourceExchangeVerified) {
                    std::cerr << "verified endpoint route was not exchanged with resource window\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_imported_peer_route_uses_hcomm_reverse_channel_eid(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>
            #include <vector>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            struct TpExchange {
                uint64_t tpHandles[128] = {};
                uint32_t psn = 0;
            };

            struct ExchangeState {
                TileXRCcuResourceWindowExchange peer;
            };

            int FakeAllGather(const void* sendBuf, size_t sendBytes, void* recvBuf, void* userData)
            {
                if (sendBuf == nullptr || recvBuf == nullptr || userData == nullptr) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                auto* state = static_cast<ExchangeState*>(userData);
                if (sendBytes == sizeof(TileXRCcuResourceWindowExchange)) {
                    const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                    if (!local->endpointRouteVerified || local->qpKey.size == 0) {
                        return TILEXR_ERROR_NOT_FOUND;
                    }
                    auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                    std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                    out[1] = state->peer;
                    return TILEXR_SUCCESS;
                }
                if (sendBytes == sizeof(TpExchange)) {
                    const auto* local = static_cast<const TpExchange*>(sendBuf);
                    if (local->tpHandles[1] != 0x1111222233334444ULL || local->psn != 5) {
                        return TILEXR_ERROR_NOT_FOUND;
                    }
                    auto* out = static_cast<TpExchange*>(recvBuf);
                    std::memset(out, 0, sizeof(TpExchange) * 2);
                    out[1].tpHandles[0] = 0x5555666677778888ULL;
                    out[1].psn = 9;
                    return TILEXR_SUCCESS;
                }
                return TILEXR_ERROR_PARA_CHECK_FAIL;
            }

            int FakeGetTpInfoListAsync(
                void* ctx,
                TileXRCcuHccpGetTpCfg* cfg,
                TileXRCcuHccpTpInfo* tpInfo,
                uint32_t* tpInfoNum,
                void** reqHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || cfg == nullptr ||
                    tpInfo == nullptr || tpInfoNum == nullptr || reqHandle == nullptr ||
                    cfg->peerEid.raw[0] != 0xa0 || cfg->peerEid.raw[15] != 0xaf) {
                    return -1;
                }
                tpInfo->tpHandle = 0x1111222233334444ULL;
                *tpInfoNum = 1;
                *reqHandle = reinterpret_cast<void*>(0x2000);
                return 0;
            }

            int FakeGetAsyncReqResult(void* reqHandle, int* reqResult)
            {
                if (reqHandle != reinterpret_cast<void*>(0x2000) || reqResult == nullptr) {
                    return -1;
                }
                *reqResult = 0;
                return 0;
            }

            int FakeQpImport(void* ctx, TileXRCcuHccpQpImportInfo* info, void** remoteQpHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || info == nullptr || remoteQpHandle == nullptr ||
                    info->in.ub.expImportCfg.tpHandle != 0x1111222233334444ULL ||
                    info->in.ub.expImportCfg.peerTpHandle != 0x5555666677778888ULL ||
                    info->in.ub.expImportCfg.txPsn != 5 ||
                    info->in.ub.expImportCfg.rxPsn != 9 ||
                    info->in.key.size == 0) {
                    return -1;
                }
                info->out.ub.tpn = 0x47;
                *remoteQpHandle = reinterpret_cast<void*>(0x3000);
                return 0;
            }

            int main()
            {
                ExchangeState state;
                state.peer.addr = 0x0000009234000000ULL;
                state.peer.bytes = 0x2000;
                state.peer.tokenId = 0x23456;
                state.peer.rawTokenId = 0x33456;
                state.peer.tokenValue = 0x5678;
                for (uint32_t i = 0; i < state.peer.remoteEid.size(); ++i) {
                    state.peer.remoteEid[i] = static_cast<uint8_t>(0xa0 + i);
                    state.peer.qpKey.value[i] = static_cast<uint8_t>(0x40 + i);
                }
                state.peer.qpKey.size = TILEXR_CCU_HCCP_QP_KEY_BYTES;
                state.peer.endpointRouteVerified = true;

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = FakeAllGather;
                runtime.options_.allGatherUserData = &state;
                runtime.localResourceWindow_.raCtxHandle = reinterpret_cast<void*>(0x1000);
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;
                runtime.localResourceWindow_.eid[0] = 0x10;
                runtime.localResourceWindow_.eid[15] = 0x1f;
                runtime.loader_.RaGetTpInfoListAsync = FakeGetTpInfoListAsync;
                runtime.loader_.RaGetAsyncReqResult = FakeGetAsyncReqResult;
                runtime.loader_.RaCtxQpImport = FakeQpImport;
                runtime.endpointPsn_ = 5;
                runtime.endpointQpKeyValid_ = true;
                runtime.endpointQpKey_.size = TILEXR_CCU_HCCP_QP_KEY_BYTES;
                runtime.localVerifiedEndpointRouteValid_ = true;
                runtime.localVerifiedEndpointRoute_.endpointRouteVerified = true;
                runtime.localVerifiedEndpointRoute_.remoteEid[0] = 0x10;
                runtime.localVerifiedEndpointRoute_.tpn = 0x22;
                runtime.localVerifiedEndpointRoute_.doorbellVa = 0x1122334455667788ULL;
                runtime.localVerifiedEndpointRoute_.doorbellTokenId = 0x3456;
                runtime.localVerifiedEndpointRoute_.doorbellTokenValue = 0x4567;
                runtime.localVerifiedEndpointRoute_.sqDepth = 8;

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed\n";
                    return 1;
                }
                if (buffers.size() != 1 || buffers[0].tpn != 0x47 ||
                    buffers[0].remoteEid[0] != 0xaf || buffers[0].remoteEid[15] != 0xa0 ||
                    buffers[0].doorbellVa != runtime.localVerifiedEndpointRoute_.doorbellVa ||
                    buffers[0].doorbellTokenId != runtime.localVerifiedEndpointRoute_.doorbellTokenId ||
                    !buffers[0].endpointRouteVerified) {
                    std::cerr << "imported peer route did not use hcomm-style reverse EID and imported TPN"
                              << " size=" << buffers.size();
                    if (!buffers.empty()) {
                        std::cerr << " tpn=0x" << std::hex << buffers[0].tpn
                                  << " eid0=0x" << static_cast<uint32_t>(buffers[0].remoteEid[0])
                                  << " eid15=0x" << static_cast<uint32_t>(buffers[0].remoteEid[15])
                                  << " doorbellVa=0x" << buffers[0].doorbellVa
                                  << " doorbellTokenId=0x" << buffers[0].doorbellTokenId
                                  << std::dec
                                  << " endpointRouteVerified=" << buffers[0].endpointRouteVerified;
                    }
                    std::cerr << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_exports_global_peer_rank_with_remote_buffer(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>
            #include <vector>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int FakeAllGather(const void* sendBuf, size_t sendBytes, void* recvBuf, void*)
            {
                if (sendBuf == nullptr || recvBuf == nullptr ||
                    sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[1] = *local;
                out[1].addr = 0x20000000ULL;
                out[1].bytes = 0x2000;
                out[1].tokenId = 0x2222;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = FakeAllGather;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed\n";
                    return 1;
                }
                if (buffers.size() != 1 || buffers[0].peerRank != 1) {
                    std::cerr << "global peer rank was not exported with remote buffer\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_exports_configured_local_verified_endpoint_route(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int EchoLocalExchangeAsPeer(const void* sendBuf, size_t sendBytes, void* recvBuf, void* userData)
            {
                (void)userData;
                if (sendBuf == nullptr || sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                if (!local->endpointRouteVerified ||
                    local->remoteEid[0] != 0x70 ||
                    local->tpn != 0x010203 ||
                    local->doorbellVa != 0x1122334455667788ULL ||
                    local->doorbellTokenId != 0x3456 ||
                    local->sqDepth != 64) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[1] = *local;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuLowerLayerTransportRoute localRoute;
                for (uint32_t i = 0; i < localRoute.remoteEid.size(); ++i) {
                    localRoute.remoteEid[i] = static_cast<uint8_t>(0x70 + i);
                }
                localRoute.tpn = 0x010203;
                localRoute.doorbellVa = 0x1122334455667788ULL;
                localRoute.doorbellTokenId = 0x3456;
                localRoute.doorbellTokenValue = 0;
                localRoute.sqDepth = 64;
                localRoute.endpointRouteVerified = true;

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = EchoLocalExchangeAsPeer;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;

                if (runtime.ConfigureLocalVerifiedEndpointRoute(localRoute) != TILEXR_SUCCESS) {
                    std::cerr << "failed to configure local verified endpoint route\n";
                    return 1;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed\n";
                    return 2;
                }
                if (buffers.size() != 1 ||
                    !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0x7f ||
                    buffers[0].remoteEid[15] != 0x70 ||
                    buffers[0].tpn != localRoute.tpn ||
                    buffers[0].doorbellVa != localRoute.doorbellVa ||
                    buffers[0].doorbellTokenId != localRoute.doorbellTokenId ||
                    buffers[0].doorbellTokenValue != localRoute.doorbellTokenValue ||
                    buffers[0].sqDepth != localRoute.sqDepth) {
                    std::cerr << "configured local verified endpoint route was not exported\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_collects_local_verified_endpoint_route_before_exchange(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            struct CollectorState {
                int calls = 0;
                uint32_t observedDevicePhyId = 0;
                uint64_t observedResourceWindow = 0;
            };

            int FakeCollector(
                uint32_t devicePhyId,
                const TileXRCcuLocalResourceWindowInfo& localResourceWindow,
                TileXRCcuLowerLayerTransportRoute* route,
                void* userData)
            {
                auto* state = static_cast<CollectorState*>(userData);
                state->calls++;
                state->observedDevicePhyId = devicePhyId;
                state->observedResourceWindow = localResourceWindow.addr;
                for (uint32_t i = 0; i < route->remoteEid.size(); ++i) {
                    route->remoteEid[i] = static_cast<uint8_t>(0x80 + i);
                }
                route->tpn = 0x010203;
                route->doorbellVa = 0x1122334455667788ULL;
                route->doorbellTokenId = 0x3456;
                route->doorbellTokenValue = 0;
                route->sqDepth = 64;
                route->endpointRouteVerified = true;
                return TILEXR_SUCCESS;
            }

            int EchoLocalExchangeAsPeer(const void* sendBuf, size_t sendBytes, void* recvBuf, void*)
            {
                if (sendBuf == nullptr || sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                if (!local->endpointRouteVerified || local->remoteEid[0] != 0x80 ||
                    local->tpn != 0x010203 || local->doorbellVa != 0x1122334455667788ULL ||
                    local->doorbellTokenId != 0x3456 || local->sqDepth != 64) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[1] = *local;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                CollectorState state;
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x1234;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = EchoLocalExchangeAsPeer;
                runtime.options_.localEndpointRouteCollector = FakeCollector;
                runtime.options_.localEndpointRouteCollectorUserData = &state;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_SUCCESS) {
                    std::cerr << "failed to refresh local endpoint route: " << report.message << "\n";
                    return 1;
                }
                if (state.calls != 1 || state.observedDevicePhyId != 0x1234 ||
                    state.observedResourceWindow != runtime.localResourceWindow_.addr) {
                    std::cerr << "collector did not receive runtime context\n";
                    return 2;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed after collector refresh\n";
                    return 3;
                }
                if (buffers.size() != 1 || !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0x8f ||
                    buffers[0].remoteEid[15] != 0x80 ||
                    buffers[0].tpn != 0x010203 ||
                    buffers[0].doorbellVa != 0x1122334455667788ULL ||
                    buffers[0].doorbellTokenId != 0x3456 ||
                    buffers[0].doorbellTokenValue != 0 ||
                    buffers[0].sqDepth != 64) {
                    std::cerr << "collected local endpoint route was not exported\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_endpoint_collector_failure_fails_closed(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int IncompleteCollector(
                uint32_t,
                const TileXRCcuLocalResourceWindowInfo&,
                TileXRCcuLowerLayerTransportRoute* route,
                void*)
            {
                route->remoteEid[0] = 0x90;
                route->tpn = 0;
                route->doorbellVa = 0x1122334455667788ULL;
                route->doorbellTokenId = 0;
                route->sqDepth = 64;
                route->endpointRouteVerified = true;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x1234;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 2;
                runtime.options_.localEndpointRouteCollector = IncompleteCollector;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "incomplete collected route was accepted\n";
                    return 1;
                }
                if (runtime.localVerifiedEndpointRouteValid_) {
                    std::cerr << "incomplete collected route remained verified\n";
                    return 2;
                }

                TileXRCcuLowerLayerTransportSnapshot templ;
                templ.dieId = 1;
                templ.xnStartId = 1961;
                TileXRCcuLowerLayerTransportRoute route;
                route.peerRank = 1;
                route.channelId = 9;
                templ.routes.push_back(route);

                TileXRCcuLowerLayerTransportSnapshot snapshot;
                if (runtime.ExportLowerLayerTransportSnapshot(templ, &snapshot) != TILEXR_SUCCESS) {
                    std::cerr << "export snapshot failed\n";
                    return 3;
                }
                if (snapshot.routes.size() != 1 || snapshot.routes[0].endpointRouteVerified ||
                    snapshot.routes[0].remoteEid[0] == 0 || snapshot.routes[0].tpn == 0 ||
                    snapshot.routes[0].doorbellVa == 0 || snapshot.routes[0].doorbellTokenId == 0 ||
                    snapshot.routes[0].sqDepth == 0) {
                    std::cerr << "collector failure did not fail closed to synthetic unverified route\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_collects_ranked_env_local_verified_endpoint_route(self):
        code = textwrap.dedent(
            r'''
            #include <cstdint>
            #include <cstdlib>
            #include <cstring>
            #include <iostream>

            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            using namespace TileXR;

            int EchoLocalExchangeAsPeer(const void* sendBuf, size_t sendBytes, void* recvBuf, void*)
            {
                if (sendBuf == nullptr || sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                if (!local->endpointRouteVerified || local->remoteEid[0] != 0xa0 ||
                    local->tpn != 0x010203 || local->doorbellVa != 0x1122334455667788ULL ||
                    local->doorbellTokenId != 0x3456 || local->doorbellTokenValue != 0 ||
                    local->sqDepth != 64) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[0] = *local;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID", "00112233445566778899aabbccddeeff", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN", "7", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA", "0x1111111111111111", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_ID", "0x1111", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_VALUE", "0x22", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_SQ_DEPTH", "8", 1);
                setenv(
                    "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID_RANK1",
                    "a0:a1:a2:a3:a4:a5:a6:a7:a8:a9:aa:ab:ac:ad:ae:af",
                    1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN_RANK1", "0x010203", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA_RANK1", "0x1122334455667788", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_ID_RANK1", "0x3456", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_VALUE_RANK1", "0", 1);
                setenv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_SQ_DEPTH_RANK1", "64", 1);

                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x1234;
                runtime.options_.rank = 1;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = EchoLocalExchangeAsPeer;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0x4567;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_SUCCESS) {
                    std::cerr << "ranked env route was not collected: " << report.message << "\n";
                    return 1;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed after env route collection\n";
                    return 2;
                }
                if (buffers.size() != 1 || !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0xaf ||
                    buffers[0].remoteEid[15] != 0xa0 ||
                    buffers[0].tpn != 0x010203 ||
                    buffers[0].doorbellVa != 0x1122334455667788ULL ||
                    buffers[0].doorbellTokenId != 0x3456 ||
                    buffers[0].doorbellTokenValue != 0 ||
                    buffers[0].sqDepth != 64) {
                    std::cerr << "ranked env route was not exported\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
            ],
            extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_tilexr_comm_caches_direct_ccu_basic_info_without_submit_readiness(self):
        comm_header = COMM_HEADER_FILE.read_text(encoding="utf-8")
        comm_source = COMM_SOURCE_FILE.read_text(encoding="utf-8")
        backend_header = CCU_BACKEND_HEADER.read_text(encoding="utf-8")
        backend_source = CCU_BACKEND_SOURCE.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")

        for leaked in [
            "RefreshDirectCcuBasicInfo",
            "HasDirectCcuBasicInfo",
            "GetDirectCcuBasicInfoStatus",
            "GetDirectCcuBasicInfo",
            "GetDirectCcuBasicInfoReport",
            "directCcuBasicInfo_",
            "directCcuBasicInfoReport_",
        ]:
            with self.subTest(leaked=leaked):
                self.assertNotIn(leaked, comm_header)
                self.assertNotIn(leaked, backend_header)

        self.assertIn("int TileXRCcuRuntimeSession::RefreshDirectCcuBasicInfo", runtime_session_source)
        self.assertIn("bool TileXRCcuRuntimeSession::HasDirectCcuBasicInfo", runtime_session_source)
        self.assertIn("ccuDirectRuntime_->QueryBasicInfo", runtime_session_source)
        self.assertIn("direct CCU basic info cached", runtime_session_source)
        self.assertIn("ResetDirectCcuBasicInfo", runtime_session_source)
        self.assertIn("ResetDirectCcuBasicInfo();", runtime_session_source)
        self.assertNotIn("udmaTransport_->" + "QueryCcuBasicInfo", comm_source + "\n" + backend_source + "\n" + runtime_session_source)

        for forbidden in [
            "TileXRCcuPrepareSubmitTasks",
            "TileXRCcuSubmitTask",
            "rtCCULaunch",
            "HcclGetCcuTaskInfo",
            "HcomGetCcuTaskInfo",
            "libhcomm",
            "libhccl_v2",
        ]:
            self.assertNotIn(forbidden, comm_header + "\n" + comm_source + "\n" + backend_source)

    def test_tilexr_comm_prepares_direct_ccu_install_attempt_without_submitting(self):
        comm_header = COMM_HEADER_FILE.read_text(encoding="utf-8")
        comm_source = COMM_SOURCE_FILE.read_text(encoding="utf-8")
        backend_header = CCU_BACKEND_HEADER.read_text(encoding="utf-8")
        backend_source = CCU_BACKEND_SOURCE.read_text(encoding="utf-8")
        planner_header = CCU_PLANNER_HEADER.read_text(encoding="utf-8")
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        runtime_header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime_source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn('ccu/tilexr_ccu_direct_orchestrator.h', planner_header + "\n" + planner_source)
        self.assertIn('ccu/tilexr_ccu_direct_runtime.h', CCU_RUNTIME_SESSION_HEADER.read_text(encoding="utf-8"))
        for leaked in [
            "PrepareDirectCcuInstallAttempt",
            "FillDirectCcuLowerLayerPlanFromAllocation",
            "PrepareDirectCcuLowerLayerPlanCallback",
        ]:
            with self.subTest(leaked=leaked):
                self.assertNotIn(leaked, comm_header)
                self.assertNotIn(leaked, backend_header)
        self.assertIn("int CreateDriverAdapter(", runtime_header)

        self.assertIn("int TileXRCcuCollectivePlanner::PrepareDirectCcuInstallAttempt", planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::FillDirectCcuLowerLayerPlanFromAllocation", planner_source)
        self.assertIn("int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback", planner_source)
        self.assertIn("ccuDirectRuntime_->CreateDriverAdapter", runtime_session_source)
        self.assertIn("TileXRCcuMakeRepositoryDeviceMemoryOps(next.repositoryMemoryAllocMode)", planner_source)
        self.assertIn("next.lowerLayerPlan = nullptr", planner_source)
        self.assertIn(
            "next.prepareLowerLayerPlan = &TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback",
            planner_source,
        )
        self.assertIn("next.lowerLayerPlanUserData = &callbackContext", planner_source)
        self.assertIn("TileXRCcuRunDirectInstallAttempt(next, attempt, report)", planner_source)
        self.assertRegex(
            planner_header,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*PrepareDirectCcuMemoryCopyInstallAttempt.*#endif",
        )
        self.assertRegex(
            planner_source,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*PrepareDirectCcuMemoryCopyInstallAttempt.*#endif",
        )
        self.assertIn("int TileXRCcuDirectRuntime::CreateDriverAdapter", runtime_source)
        self.assertNotIn("udmaTransport_->" + "CreateCcuDriverAdapter", comm_source + "\n" + backend_source + "\n" + runtime_session_source)

        combined = (
            comm_header + "\n" + comm_source + "\n" + backend_source + "\n" +
            planner_source + "\n" + runtime_session_source + "\n" + runtime_header + "\n" + runtime_source
        )
        for forbidden in [
            "TileXRCcuPrepareSubmitTasks",
            "TileXRCcuSubmitTask",
            "rtCCULaunch",
            "HcclGetCcuTaskInfo",
            "HcomGetCcuTaskInfo",
            "libhcomm",
            "libhccl_v2",
        ]:
            self.assertNotIn(forbidden, combined)

    def test_tilexr_comm_direct_ccu_prepare_fails_fast_after_device_init_failure(self):
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("g_ccuDirectRuntimeUnavailableByDevice", runtime_session_source)
        init_body = runtime_session_source[
            runtime_session_source.index("int TileXRCcuRuntimeSession::Init("):
            runtime_session_source.index("void TileXRCcuRuntimeSession::ResetDirectCcuBasicInfo")
        ]
        prepare_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuInstallAttempt"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuMemoryCopyInstallAttempt")
        ]

        self.assertIn("g_ccuDirectRuntimeUnavailableByDevice[devId_] = runtimeReport.message", init_body)
        self.assertIn("direct CCU runtime unavailable for device", runtime_session_source)
        self.assertIn("session.DirectCcuRuntimeUnavailableMessage()", prepare_body)
        self.assertLess(
            prepare_body.index("session.DirectCcuRuntimeUnavailableMessage()"),
            prepare_body.index("RefreshDirectCcuBasicInfo(installDieId)"),
        )

    def test_tilexr_comm_direct_ccu_runtime_init_serializes_ra_initialization(self):
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        init_body = runtime_session_source[
            runtime_session_source.index("int TileXRCcuRuntimeSession::Init("):
            runtime_session_source.index("void TileXRCcuRuntimeSession::ResetDirectCcuBasicInfo")
        ]

        lock_pos = init_body.index("lock_guard<mutex> lock(g_ccuDirectRuntimeMtx);")
        allocation_pos = init_body.index("ccuDirectRuntime_.reset(new (nothrow) TileXRCcuDirectRuntime())")
        runtime_init_pos = init_body.index("ccuDirectRuntime_->Init(runtimeOptions, &runtimeReport)")
        unavailable_set_pos = init_body.index("g_ccuDirectRuntimeUnavailableByDevice[devId_] = runtimeReport.message")

        self.assertLess(lock_pos, allocation_pos)
        self.assertLess(allocation_pos, runtime_init_pos)
        self.assertLess(runtime_init_pos, unavailable_set_pos)
        self.assertEqual(1, init_body.count("lock_guard<mutex> lock(g_ccuDirectRuntimeMtx);"))

    def test_tilexr_comm_direct_ccu_prepare_can_select_install_die_for_diagnostics(self):
        planner_source = CCU_PLANNER_SOURCE.read_text(encoding="utf-8")
        prepare_body = planner_source[
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuInstallAttempt"):
            planner_source.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuMemoryCopyInstallAttempt")
        ]

        self.assertIn("TILEXR_CCU_DIRECT_INSTALL_DIE_ID", planner_source)
        self.assertIn("SelectDirectCcuInstallDieId", planner_source)
        self.assertIn("RefreshDirectCcuBasicInfo(installDieId)", prepare_body)
        self.assertIn("basicInfo->dieId != installDieId", prepare_body)
        self.assertNotIn("RefreshDirectCcuBasicInfo(0)", prepare_body)

    def test_tilexr_comm_direct_ccu_thread_allgather_aborts_after_device_init_failure(self):
        runtime_session_source = CCU_RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        thread_allgather_body = runtime_session_source[
            runtime_session_source.index("int TileXRCcuRuntimeSession::DirectCcuThreadAllGather"):
            runtime_session_source.index("} // namespace TileXR")
        ]

        self.assertIn("DirectCcuRuntimeUnavailableMessage()", thread_allgather_body)
        self.assertIn("direct CCU thread allgather abort", thread_allgather_body)
        self.assertLess(
            thread_allgather_body.index("DirectCcuRuntimeUnavailableMessage()"),
            thread_allgather_body.index("TILEXR_INIT_TIMEOUT"),
        )

    def test_tilexr_comm_direct_ccu_lower_layer_plan_api_is_header_visible(self):
        comm_header = COMM_HEADER_FILE.read_text(encoding="utf-8")
        for leaked in [
            "ConfigureDirectCcuLowerLayerTemplate",
            "ConfigureDirectCcuLowerLayerTemplateFromAllocation",
            "PrepareDirectCcuLowerLayerTemplateFromAllocation",
            "RefreshDirectCcuLowerLayerPlan",
            "HasDirectCcuLowerLayerPlan",
            "GetDirectCcuLowerLayerPlanStatus",
            "GetDirectCcuLowerLayerPlanReport",
            "GetDirectCcuLowerLayerPlan",
            "RefreshDirectCcuBasicInfo",
            "HasDirectCcuBasicInfo",
            "GetDirectCcuBasicInfoStatus",
            "GetDirectCcuBasicInfo",
            "GetDirectCcuBasicInfoReport",
            "ConfigureDirectCcuVerifiedEndpointRoutes",
            "ConfigureDirectCcuLocalVerifiedEndpointRoute",
            "PrepareDirectCcuInstallAttempt",
        ]:
            with self.subTest(leaked=leaked):
                self.assertNotIn(leaked, comm_header)

        code = textwrap.dedent(
            r'''
            #include "tilexr_comm.h"

            #include <type_traits>

            using namespace TileXR;

            int main()
            {
                using InitFn = int (TileXRComm::*)();
                using GetterFn = TileXRCcuBackend* (TileXRComm::*)();
                using ConstGetterFn = const TileXRCcuBackend* (TileXRComm::*)() const;
                using EnableForTestFn = int (TileXRComm::*)();

                InitFn init = &TileXRComm::InitCcuBackend;
                GetterFn getter = &TileXRComm::GetCcuBackendForCollectives;
                ConstGetterFn constGetter = &TileXRComm::GetCcuBackendForCollectives;
                EnableForTestFn enableForTest = &TileXRComm::EnableCcuBackendForTest;
                (void)init;
                (void)getter;
                (void)constGetter;
                (void)enableForTest;
                return 0;
            }
            '''
        )

        result = self.compile_only(code)

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
