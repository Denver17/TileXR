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
        source.write_text('''#include <initializer_list>
#include "ep_memory_server_dedup_combine_config.h"
using namespace TileXREp;
static_assert(kEpServerDedupCombineSendCoreNum + kEpServerDedupCombineRecvCoreNum == 64, "all cores");
static_assert(kEpServerDedupCombineRecvCoreNum >= 8, "eight notifier cores are Read cores");
static_assert(56 - kEpServerDedupCombineSendCoreNum == kEpServerDedupCombineRecvCoreNum - 8,
    "notifier LocalID");
static_assert(EpCombineM4InputLaneCount(10000, 7168, 1) > 0, "K1 budget");
static_assert(EpCombineM4InputLaneCount(10000, 7168, 6) > 0, "main shape input budget");
static_assert(EpCombineM4OutputLaneCount(10000, 7168, 6) > 0, "main shape output budget");
static_assert(EpCombineM4InputLaneCount(10000, 7168, 32) > 0, "K32 budget");
static_assert(EpCombineM4OutputLaneCount(10000, 7168, 32) > 0, "K32 output budget");
static_assert(EpCombineM4InputLaneCount(10001, 7168, 6) == 0, "too many rows");
static_assert(EpCombineM4InputLaneCount(10000, 0, 6) == 0, "zero width");
static_assert(EpCombineM4InputLaneCount(10000, 7168, 0) == 0, "zero topk");
static_assert(EpCombineM4InputLaneCount(10000, 7168, 33) == 0, "unsupported topk");
static_assert(EpCombineM4FixedUbBytes(10000, 7168, 32) +
    EpCombineM4InputLaneCount(10000, 7168, 32) * EpCombineAlign(7168 * 2, 32) +
    EpCombineM4OutputLaneCount(10000, 7168, 32) * ((7168 * 2 + 479) / 480 * 512) <=
    kEpServerDedupCombineM4UbBytes, "simultaneous resources fit the arena");
static_assert(EpCombineM5InputLaneCount(96, 7168, 1) > 0, "M5 K1");
static_assert(EpCombineM5InputLaneCount(96, 7168, 6) > 0, "M5 default");
static_assert(EpCombineM5OutputLaneCount(96, 7168, 6) > 0, "M5 output");
static_assert(EpCombineM5InputLaneCount(96, 7168, 32) > 0, "M5 K32");
static_assert(EpCombineM5OutputLaneCount(96, 7168, 32) > 0, "M5 K32 output");
static_assert(EpCombineM5TokenCount(0) == 0, "empty M5 partition budget");
static_assert(EpCombineM5TokenCount(1) == 1, "M5 underfilled Send group");
static_assert(EpCombineM5TokenCount(3 * kEpServerDedupCombineSendCoreNum) == 3,
    "M5 partitions by Send cores");
static_assert(EpCombineM5TokenCount(3 * kEpServerDedupCombineSendCoreNum + 1) == 4,
    "M5 reserves the largest uneven Send segment");
static_assert(EpCombineM5InputLaneCount(UINT32_MAX, 7168, 32) == 0, "BS metadata does not wrap");
static_assert(EpCombineM5InputLaneCount(96, 0, 1) == 0, "M5 zero width");
static_assert(EpCombineM5InputLaneCount(96, 7168, 33) == 0, "M5 bad K");
int main() {
    for (uint32_t tokens : {0U, 1U, 576U, 10000U}) {
        for (uint32_t h : {1U, 17U, 240U, 241U, 7168U, 10000U, 16000U}) {
            for (uint32_t k : {1U, 6U, 32U}) {
                const uint32_t reads = EpCombineM4InputLaneCount(tokens, h, k);
                const uint32_t sends = EpCombineM4OutputLaneCount(tokens, h, k);
                if (reads > 3 || sends > 8 || (reads == 0) != (sends == 0)) return 2;
                if (!reads) continue;
                const uint32_t packet = (h * 2U + 479U) / 480U * 512U;
                const uint32_t used = EpCombineM4FixedUbBytes(tokens, h, k) +
                    reads * EpCombineAlign(h * 2U, 32U) + sends * packet;
                if (used > kEpServerDedupCombineM4UbBytes) return 3;
                if (sends < 8 && used + packet <= kEpServerDedupCombineM4UbBytes) return 4;
            }
        }
    }
    if (EpCombineM4InputLaneCount(576, 7168, 6) != 3 ||
        EpCombineM4OutputLaneCount(576, 7168, 6) != 6) return 5;
    return 0;
}
''')
        for send in (16, 24, 32, 40, 48, 56, 0, 8, 20, 57, 64):
            binary = Path(directory) / ('config-' + str(send))
            result = subprocess.run([
                args.compiler, "-std=c++14", "-I", args.include, "-o", str(binary),
                "-DTILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=" + str(send), str(source),
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
            valid = send in (16, 24, 32, 40, 48, 56)
            if (result.returncode == 0) != valid:
                raise RuntimeError("unexpected compile result for Send=%d:\n%s" % (send, result.stdout))
            if valid:
                subprocess.run([str(binary)], check=True, timeout=30)
    print("EP_SERVER_DEDUP_COMBINE_CONFIG_PASS")


if __name__ == "__main__":
    main()
