"""Compile production Dispatch with distinct GM pointer types; no device execution."""
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ServerDedupAddressSpaceTests(unittest.TestCase):
    def test_dispatch_preserves_gm_types_with_and_without_profiling(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("host clang++ required for GM address-space syntax checking")
        includes = ("tests/ep/unit/server_dedup_count_fake", "src/include", "src/ep/common", "src/ep/kernels")
        for profiling in (0, 1):
            with self.subTest(profiling=profiling):
                command = [compiler, "-std=c++17", "-fsyntax-only", "-DTILEXR_TEST_GM_ADDRESS_SPACE=1",
                           f"-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING={profiling}"]
                command.extend("-I" + str(ROOT / path) for path in includes)
                command.append(str(ROOT / "src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp"))
                # Keep checking the staged receiver even while the current
                # source-send checkpoint intentionally does not execute it.
                command.extend(["-x", "c++", "-"])
                receiver = '#include "tilexr_ep_server_dedup_recv.h"\n'
                receiver += "template class TileXREp::ServerDedupReceiver<half, false, false>;\n"
                receiver += "template class TileXREp::ServerDedupReceiver<half, true, false>;\n"
                receiver += "template class TileXREp::ServerDedupReceiver<half, true, true>;\n"
                result = subprocess.run(command, input=receiver, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
