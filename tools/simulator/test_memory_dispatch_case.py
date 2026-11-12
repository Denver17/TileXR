"""校验 Dispatch 输入布局、MXFP8 独立 golden 和 sparse GM 结果验收边界。

Host 编译器在临时目录构建布局探针和已有 C++ golden 的适配器；测试不启动 SST。
"""

import contextlib
import io
import json
import pathlib
import struct
import subprocess
import tempfile
import types
import unittest

import yaml

import memory_dispatch_case as case


class MemoryDispatchCaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.temporary.name)
        repo = pathlib.Path(__file__).resolve().parents[2]
        probe = cls.root / "layout"
        subprocess.run([
            "c++", "-std=c++14", "-I" + str(repo / "src/include"),
            "-I" + str(repo / "src/ep/common"),
            str(repo / "tools/simulator/memory_dispatch_layout.cpp"), "-o", str(probe),
        ], check=True)
        cls.layout = json.loads(subprocess.check_output([str(probe)]))
        golden_source = cls.root / "golden.cpp"
        golden_source.write_text('''// 将标准输入的 FP32 行交给已有 MXFP8 golden，输出元素和 scale 字节。
#include "tests/ep/demo/mxfp8_golden.h"
#include <iostream>
#include <iterator>
#include <cstring>
int main(int argc, char **argv) {
    const std::size_t h = std::stoul(argv[1]);
    std::vector<char> bytes((std::istreambuf_iterator<char>(std::cin)), {});
    if (bytes.size() % (h * sizeof(float))) return 1;
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    auto q = TileXREpDemo::QuantizeMxfp8(values, values.size() / h, h,
        TileXREpDemo::Mxfp8Format::E4M3);
    std::cout.write(reinterpret_cast<const char *>(q.elements.data()), q.elements.size());
    std::cout.write(reinterpret_cast<const char *>(q.scales.data()), q.scales.size());
}
''')
        cls.golden = cls.root / "golden"
        subprocess.run(["c++", "-std=c++14", "-O2", "-I" + str(repo),
                        str(golden_source), "-o", str(cls.golden)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_reference_route_and_serialized_peer_addresses(self):
        # 2 ranks, 2 tokens, topK=2: each expert receives one token from each rank.
        rows, expected = case.oracle(2, 2, 256, 2, 4, 0)
        self.assertEqual(struct.unpack("<4i", expected[0]["sendCountsOut"]), (1, 2, 3, 4))
        self.assertEqual(struct.unpack("<2q", expected[0]["expertTokenNumsOut"]), (2, 4))
        self.assertEqual(struct.unpack("<12i", expected[0]["assistInfoForCombineOut"]),
                         (0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1))
        self.assertEqual(expected[0]["expandXOut"], rows[0][0] + rows[1][0] + rows[0][0] + rows[1][0])
        data = case.comm_args(1, 2, self.layout)
        self.assertEqual(len(data), self.layout["comm_args_bytes"])
        self.assertEqual(struct.unpack_from("<4i", data), (1, 1, 2, 2))
        self.assertEqual(struct.unpack_from("<2Q", data, self.layout["peer_mems"]),
                         (case.PEER_BASE, case.GM_SIZE + case.PEER_BASE))
        self.assertEqual(struct.unpack_from("<Q", data, self.layout["peer_mem_bytes"])[0],
                         self.layout["peer_bytes"])

    def test_generation_provenance_and_numerical_failure_detection(self):
        for output_format in ("fp16", "mxfp8-e4m3"):
            with self.subTest(output_format=output_format):
                self.check_generation(output_format)

    def test_platform_profile_provenance_and_input_isolation(self):
        self.check_generation("fp16", with_profile=True)

    def test_platform_profile_rejects_stale_or_invalid_targets_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "v310_config/model.yml"
            config.parent.mkdir()
            config.write_text("vector:\n  frequency: 1.65GHz\n  verbose: 0\n")
            original = config.read_bytes()
            profile = root / "profile.json"
            change = {"file": "v310_config/model.yml", "path": ["vector", "frequency"],
                      "expected": "1.65GHz", "value": "0.825GHz"}
            valid = dict(change, path=["vector", "verbose"], expected=0, value=1)
            for invalid in (dict(change, expected="2GHz"),
                            dict(change, path=["typo", "frequency"]),
                            dict(change, file="v310_config/../model.yml"),
                            dict(change, file="v310_config/workload_config.yml"),
                            dict(change, value=0.825)):
                profile.write_text(json.dumps({"name": "invalid", "changes": [valid, invalid]}))
                with self.assertRaises(ValueError):
                    case.apply_platform_profile(root, profile)
                self.assertEqual(config.read_bytes(), original)
                self.assertFalse((root / "platform_profile.json").exists())
            profile.write_text(json.dumps({"name": "duplicate", "changes": [change, change]}))
            with self.assertRaisesRegex(ValueError, "target twice"):
                case.apply_platform_profile(root, profile)
            self.assertEqual(config.read_bytes(), original)

    def test_mxfp8_matches_existing_cpp_golden(self):
        # 覆盖全部有限 FP16 位型；另测不同 block 最大值、RNE 中点和尾块 scale。
        finite_bits = [bits for bits in range(65536) if bits & 0x7c00 != 0x7c00]
        full = struct.pack("<" + "H" * len(finite_bits), *finite_bits)
        rows = [full[i:i + 512] for i in range(0, len(full), 512)]
        for value in (0.0, -0.0, 2 ** -24, 0.125, 1.0625, 1.1875, 31.0, 65504.0):
            rows.append(struct.pack("<256e", *([value] * 256)))
        self.check_golden(rows, 256)
        for h in (1, 31, 32, 33, 64, 3584):
            self.check_golden([case.input_row(1, 3, h), case.input_row(0, 7, h)], h)
        with self.assertRaisesRegex(ValueError, "finite FP16"):
            case.quantize_mxfp8_e4m3(struct.pack("<e", float("inf")))

    def check_golden(self, rows, h):
        values = struct.unpack("<" + "e" * (len(rows) * h), b"".join(rows))
        expected = subprocess.check_output([str(self.golden), str(h)],
                                          input=struct.pack("<" + "f" * len(values), *values))
        actual = [case.quantize_mxfp8_e4m3(row) for row in rows]
        self.assertEqual(b"".join(q[0] for q in actual) + b"".join(q[1] for q in actual), expected)

    def check_generation(self, output_format, with_profile=False):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            template = root / "template"
            for name in ("UBusSim_config", "v310_config"):
                (template / name).mkdir(parents=True)
            for name in ("test_UBusSim.py", "output_generator.py"):
                (template / name).write_text("# fixture\n")
            (template / "v310_config/workload_config.yml").write_text(
                yaml.safe_dump({"workload_config": {"chip0": {"ub_base_addr": 0x80000}}}))
            shared_params = {"enable_dump": 0, "enable_dump_detail": 1, "frequency": "1.65GHz"}
            (template / "v310_config/davidV100_simulator_config.yml").write_text(
                yaml.safe_dump({"architecture_config": {"chip_num": 1, "enable_dump": 0,
                                                       "freq_aicore": "1.65GHz"},
                                "simulator_config": {"vector": {"params": shared_params},
                                                     "su": {"params": shared_params}}}))
            layout = root / "layout.json"
            layout.write_text(json.dumps(self.layout))
            kernel = root / "kernel.o"
            kernel.write_bytes(b"object fixture")
            output = root / "case"
            args = types.SimpleNamespace(layout=layout, kernel=kernel, template=template,
                                         output=output, ranks=2, cores=4, bs=96, h=256,
                                         top_k=2, experts=4, token_nums_type=0, magic=1,
                                         output_format=output_format, instruction_dump=True)
            if with_profile:
                args.platform_profile = root / "platform-profile.json"
                args.platform_profile.write_text(json.dumps({"name": "vector-clock-half", "changes": [{
                    "file": "v310_config/davidV100_simulator_config.yml",
                    "path": ["simulator_config", "vector", "params", "frequency"],
                    "expected": "1.65GHz", "value": "0.825GHz"}]}))
            with contextlib.redirect_stdout(io.StringIO()):
                case.generate(args)
            scenario = case.preflight(output)
            config = case.read_yaml(output / "v310_config/davidV100_simulator_config.yml")
            self.assertEqual(config["architecture_config"]["chip_num"], 2)
            vector = config["simulator_config"]["vector"]["params"]
            self.assertEqual(vector, {"enable_dump": 1, "enable_dump_detail": 0,
                                      "frequency": "0.825GHz" if with_profile else "1.65GHz"})
            self.assertEqual(config["architecture_config"]["freq_aicore"], "1.65GHz")
            self.assertEqual(config["simulator_config"]["su"]["params"]["frequency"], "1.65GHz")
            if with_profile:
                self.assertEqual(scenario["format_version"], 4)
                evidence = json.loads((output / "platform_profile.json").read_text())
                self.assertEqual(evidence["source_sha256"], case.sha256(args.platform_profile))
                self.assertEqual(evidence["configs"]["v310_config/davidV100_simulator_config.yml"]
                                 ["after_sha256"], case.sha256(output / "v310_config/davidV100_simulator_config.yml"))
            self.assertEqual(scenario["semantic_output_sizes"]["0"]["assistInfoForCombineOut"], 2304)
            self.assertEqual(scenario["output_sizes"]["0"]["assistInfoForCombineOut"], 2560)
            workload = yaml.safe_load((output / "workload.yml").read_text())["workload_config"]
            params = workload["chip0"]["kernel_param"]
            quantized = output_format != "fp16"
            self.assertEqual(params["quantMode"]["value"], 4 if quantized else 0)
            self.assertEqual(params["expandXOutDtype"]["value"], 15 if quantized else 3)
            self.assertEqual(bool(params["dynamicScalesOut"]["value"]), quantized)
            self.assertEqual(scenario["token_payload_bytes"], args.h * (1 if quantized else 2))
            self.assertEqual(workload["chip1"]["bin_param"]["addr"], case.GM_SIZE + 0x40000000)
            self.assertEqual(list(workload["chip0"]["kernel_param"]), list(case.ABI))
            (output / "bin").mkdir()
            for rank in range(2):
                for name in case.output_types(scenario):
                    data = (output / "expected" / ("rank%d" % rank) / (name + ".bin")).read_bytes()
                    size = scenario["output_sizes"][str(rank)][name]
                    data = data.ljust(size, b"\xa5")
                    chip = workload["chip%d" % rank]
                    address = chip["output_param"][name]["addr"] - chip["sys_va_base_addr"]
                    for die in (1, 2):
                        sparse = bytearray()
                        for offset in range(0, size, 512):
                            length = min(512, size - offset)
                            block = data[offset:offset + length] if offset // 512 % 2 == die - 1 else b"\xa5" * length
                            sparse.extend(struct.pack("<Q", address + offset) + block.ljust(512, b"\0"))
                        (output / "bin" / ("david_%d_die%d_%s.bin" % (rank, die, name))).write_bytes(sparse)
            with contextlib.redirect_stdout(io.StringIO()):
                case.extract(types.SimpleNamespace(case=output))
            log = root / "sim.log"
            log.write_text("Simulation is complete, simulated time: 1.25 us\n")
            validation = types.SimpleNamespace(case=output, log=log, exit_code=0)
            with contextlib.redirect_stdout(io.StringIO()):
                case.validate(validation)
            if quantized:
                scales = output / "bin/david_1_dynamicScalesOut.bin"
                original_scales = scales.read_bytes()
                scales.write_bytes(b"\0" + original_scales[1:])
                with self.assertRaisesRegex(ValueError, "field=dynamicScalesOut"):
                    case.validate(validation)
                scales.write_bytes(original_scales)
            payload = output / "bin/david_1_expandXOut.bin"
            data = bytearray(payload.read_bytes())
            data[0] ^= 1
            payload.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Output mismatch"):
                case.validate(validation)
            validation.exit_code = 124
            with self.assertRaisesRegex(ValueError, "did not exit successfully"):
                case.validate(validation)
            conflicting = output / "bin/david_1_die2_expandXOut.bin"
            address = workload["chip1"]["output_param"]["expandXOut"]["addr"] - case.GM_SIZE
            with conflicting.open("ab") as stream:
                stream.write(struct.pack("<Q", address) + b"b" * 512)
            with self.assertRaisesRegex(ValueError, "Conflicting die writes"):
                case.extract(types.SimpleNamespace(case=output))
            if with_profile:
                config_path = output / "v310_config/davidV100_simulator_config.yml"
                saved_config = config_path.read_bytes()
                saved_manifest = (output / "manifest.json").read_bytes()
                config["simulator_config"]["vector"]["params"]["frequency"] = "2GHz"
                config_path.write_text(case.encode_yaml(config))
                manifest = json.loads(saved_manifest)
                manifest["v310_config/davidV100_simulator_config.yml"] = case.sha256(config_path)
                case.write_json(output / "manifest.json", manifest)
                with self.assertRaisesRegex(ValueError, "config differs from applied profile"):
                    case.preflight(output)
                config_path.write_bytes(saved_config)
                (output / "manifest.json").write_bytes(saved_manifest)
            original = output / "input_bin/rank0/commArgs.bin"
            original.write_bytes(b"stale input")
            with self.assertRaisesRegex(ValueError, "inputs or deployed kernel changed"):
                case.preflight(output)


if __name__ == "__main__":
    unittest.main()
