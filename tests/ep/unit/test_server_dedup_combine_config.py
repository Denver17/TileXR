#!/usr/bin/env python3
"""Compile the shared Host/device core contract with supported and invalid splits."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--include", default=str(Path(__file__).resolve().parents[3] / "src/ep/common"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tilexr-combine-config-") as directory:
        source = Path(directory) / "config.cpp"
        source.write_text('''#include "ep_memory_server_dedup_combine_config.h"
using namespace TileXREp;
static_assert(kEpServerDedupCombineSendCoreNum + kEpServerDedupCombineRecvCoreNum == 64, "all cores");
static_assert(kEpServerDedupCombineRecvCoreNum >= 8, "eight notifier cores are Read cores");
static_assert(56 - kEpServerDedupCombineSendCoreNum == kEpServerDedupCombineRecvCoreNum - 8,
    "notifier LocalID");
int main() { return 0; }
''')
        for send in (16, 24, 32, 40, 48, 56, 0, 8, 20, 57, 64):
            result = subprocess.run([
                args.compiler, "-std=c++14", "-fsyntax-only", "-I", args.include,
                "-DTILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=" + str(send), str(source),
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
            valid = send in (16, 24, 32, 40, 48, 56)
            if (result.returncode == 0) != valid:
                raise RuntimeError("unexpected compile result for Send=%d:\n%s" % (send, result.stdout))
    print("EP_SERVER_DEDUP_COMBINE_CONFIG_PASS")


if __name__ == "__main__":
    main()
