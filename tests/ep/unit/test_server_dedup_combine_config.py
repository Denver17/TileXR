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
#include <algorithm>
#include "ep_memory_server_dedup_combine_config.h"
using namespace TileXREp;
static_assert(kEpServerDedupCombineSendCoreNum + kEpServerDedupCombineRecvCoreNum == 64, "all cores");
static_assert(kEpServerDedupCombineRecvCoreNum >= 8, "eight notifier cores are Read cores");
static_assert(56 - kEpServerDedupCombineSendCoreNum == kEpServerDedupCombineRecvCoreNum - 8,
    "notifier LocalID");
static_assert(EpCombineM45Layout(10000, 96, 7168, 1).inputLanes > 0, "K1");
static_assert(EpCombineM45Layout(10000, 96, 7168, 6).outputLanes > 0, "K6");
static_assert(EpCombineM45Layout(10000, 96, 7168, 32).finalOutputLanes > 0, "K32");
static_assert(EpCombineM45Layout(10001, 96, 7168, 6).inputLanes == 0, "row limit");
static_assert(EpCombineM45Layout(10000, UINT32_MAX, 7168, 32).inputLanes == 0, "BS overflow");
static_assert(EpCombineM45Layout(10000, 0, 7168, 6).inputLanes == 0, "zero BS");
static_assert(EpCombineM45Layout(10000, 96, 0, 6).inputLanes == 0, "zero H");
static_assert(EpCombineM45Layout(10000, 96, UINT32_MAX, 6).inputLanes == 0, "H overflow");
static_assert(EpCombineM45Layout(10000, 96, 7168, 0).inputLanes == 0, "zero K");
static_assert(EpCombineM45Layout(10000, 96, 7168, 33).inputLanes == 0, "K limit");
static_assert(EpCombineM5TokenCount(3 * kEpServerDedupCombineRecvCoreNum + 1) == 4,
    "largest uneven Recv segment");
int main() {
    for (uint32_t tokens : {0U, 1U, 576U, 10000U}) {
        // The M3 prescan reuses an entire aligned row array, not a count-sized
        // allocation. Check tiny/uneven/max per-core Table2 triples and indices.
        const uint32_t count = (tokens + kEpServerDedupCombineSendCoreNum - 1) /
            kEpServerDedupCombineSendCoreNum;
        if (count * 12U > EpCombineAlign(tokens == 0 ? 4 : tokens * 4, 256)) return 1;
        for (uint32_t bs : {1U, 96U, 512U, 16384U})
        for (uint32_t h : {1U, 17U, 240U, 241U, 7168U, 10000U, 16000U}) {
            for (uint32_t k : {1U, 6U, 32U}) {
                const auto p = EpCombineM45Layout(tokens, bs, h, k);
                if (p.inputLanes > 3 || p.outputLanes > 8 || p.finalOutputLanes > 2) return 2;
                if (!p.inputLanes) { if (p.outputLanes) return 3; continue; }
                if (p.totalBytes > kEpServerDedupCombineRecvUbBytes) return 4;
                if (p.outputLanes < 8 && p.totalBytes + p.outputBytes <= kEpServerDedupCombineRecvUbBytes) return 5;
                if (p.phaseOffset != 3 * p.computeBytes || p.inputOffset < p.phaseOffset + p.phaseBytes ||
                    p.outputOffset < p.inputOffset + p.inputLanes * p.inputBytes ||
                    p.totalBytes != kEpServerDedupCombineResidentUbBytes + p.outputOffset + p.outputLanes * p.outputBytes) return 6;
                if (p.inputBytes < h * 2 || p.outputBytes < p.inputBytes || p.inputOffset % 32 || p.outputOffset % 32) return 7;
                const uint32_t m4 = EpCombineM4TaskBytes(tokens) + 384 + 32*k*8 + p.inputBytes;
                const uint64_t n = EpCombineM5TokenCount(bs);
                const uint64_t m5 = 2*((n*4+31)/32*32) + (n*k*4+31)/32*32 +
                    EpCombineM5FlagBytes(h,k) + EpCombineM5WorkBytes(h,k) + 32;
                if (p.phaseBytes < m4 || p.phaseBytes < m5 || p.finalInputOffset != p.phaseOffset + m5) return 8;
                for (uint32_t tasks = 0; tasks < 3 * p.outputLanes + 2; ++tasks) {
                    const auto f = EpCombineM5Layout(p, tasks);
                    if (!f.inputLanes || f.inputLanes > 7 || f.inputLanes < p.inputLanes) return 11;
                    uint32_t kept = 0, boundaries[2] = {};
                    for (uint32_t o = 0; o < p.finalOutputLanes; ++o) {
                        if (f.outputSlots[o] >= p.outputLanes || (kept >> f.outputSlots[o] & 1)) return 12;
                        kept |= 1U << f.outputSlots[o];
                        boundaries[o] = p.outputOffset + f.outputSlots[o] * p.outputBytes;
                    }
                    for (uint32_t recent = 0; recent < std::min(tasks, p.finalOutputLanes); ++recent)
                        if (!(kept & (1U << ((tasks - 1 - recent) % p.outputLanes)))) return 13;
                    const uint32_t limit = kEpServerDedupCombineRecvUbBytes - kEpServerDedupCombineResidentUbBytes;
                    for (uint32_t i = 0; i < f.inputLanes; ++i) {
                        const uint32_t begin = f.inputOffsets[i], end = begin + p.inputBytes;
                        if (begin < p.finalInputOffset || end > limit || begin % 32 ||
                            (i && begin < f.inputOffsets[i-1] + p.inputBytes)) return 14;
                        uint32_t overlap = 0;
                        for (uint32_t old = 0; old < p.outputLanes; ++old) {
                            const uint32_t ob = p.outputOffset + old * p.outputBytes;
                            if (begin < ob + p.outputBytes && end > ob) overlap |= 1U << old;
                        }
                        if ((overlap & kept) || overlap != f.inputRetireMasks[i]) return 15;
                    }
                    // Independent capacity oracle: sum complete buffers in free
                    // intervals bounded by the two retained physical outputs.
                    std::sort(boundaries, boundaries + p.finalOutputLanes);
                    uint32_t available = 0, start = p.finalInputOffset;
                    for (uint32_t o = 0; o < p.finalOutputLanes; ++o) {
                        available += (boundaries[o] - start) / p.inputBytes;
                        start = boundaries[o] + p.outputBytes;
                    }
                    available += (limit - start) / p.inputBytes;
                    if (f.inputLanes != std::min(available, 7U)) return 16;
                }
            }
        }
    }
    const auto main = EpCombineM45Layout(576, 96, 7168, 6);
    if (main.inputLanes != 3 || main.outputLanes != 6 || main.finalOutputLanes != 2) return 9;
    if (kEpServerDedupCombineSendCoreNum == 32 && main.totalBytes != 245280) return 10;
    for (uint32_t tasks = 0; tasks < 20; ++tasks)
        if (EpCombineM5Layout(main, tasks).inputLanes != 7) return 17;
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
