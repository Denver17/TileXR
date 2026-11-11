"""校验 Memory Dispatch 路由基准、当前 CommArgs 序列化和仿真结果验收边界。"""

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
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            template = root / "template"
            for name in ("UBusSim_config", "v310_config"):
                (template / name).mkdir(parents=True)
            for name in ("test_UBusSim.py", "output_generator.py"):
                (template / name).write_text("# fixture\n")
            (template / "v310_config/workload_config.yml").write_text(
                yaml.safe_dump({"workload_config": {"chip0": {"ub_base_addr": 0x80000}}}))
            layout = root / "layout.json"
            layout.write_text(json.dumps(self.layout))
            kernel = root / "kernel.o"
            kernel.write_bytes(b"object fixture")
            output = root / "case"
            args = types.SimpleNamespace(layout=layout, kernel=kernel, template=template,
                                         output=output, ranks=2, cores=4, bs=2, h=256,
                                         top_k=2, experts=4, token_nums_type=0, magic=1)
            with contextlib.redirect_stdout(io.StringIO()):
                case.generate(args)
            scenario = case.preflight(output)
            workload = yaml.safe_load((output / "workload.yml").read_text())["workload_config"]
            self.assertEqual(workload["chip1"]["bin_param"]["addr"], case.GM_SIZE + 0x40000000)
            self.assertEqual(list(workload["chip0"]["kernel_param"]), list(case.ABI))
            (output / "bin").mkdir()
            for rank in range(2):
                for name in case.OUTPUT_TYPES:
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
            original = output / "input_bin/rank0/commArgs.bin"
            original.write_bytes(b"stale input")
            with self.assertRaisesRegex(ValueError, "inputs or deployed kernel changed"):
                case.preflight(output)


if __name__ == "__main__":
    unittest.main()
