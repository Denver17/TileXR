#include "../../tools/rdma_compare/multiqp_summary.h"
#include <cassert>
#include <cmath>
#include <initializer_list>

int main()
{
    BenchResult r[16]{};
    for (auto& value : r) { value.magic = 0x5458524D41544348ULL; }
    MultiQpSummary summary;
    r[0].start = 100; r[0].end = 300;
    r[1].start = 150; r[1].end = 250;
    assert(SummarizeQps(r,2,512,10,summary));
    assert(summary.start == 100 && summary.end == 300 && summary.duration == 200);
    assert(summary.latestStart == 150 && summary.earliestEnd == 250);
    assert(summary.payloadBytes == 10240 && std::abs(summary.gbps - 409.6) < 1e-9);
    // Sequential intervals must include the gap; summing QP bandwidths is wrong.
    r[1].start = 500; r[1].end = 700;
    assert(SummarizeQps(r,2,512,10,summary));
    assert(summary.duration == 600 && summary.latestStart > summary.earliestEnd);
    assert(SummarizeQps(r,1,512,10,summary) && summary.payloadBytes == 5120);
    assert(!SummarizeQps(nullptr,1,512,10,summary));
    assert(!SummarizeQps(r,0,512,10,summary));
    assert(!SummarizeQps(r,17,512,10,summary));
    assert(!SummarizeQps(r,2,0,10,summary));
    assert(!SummarizeQps(r,2,512,0,summary));
    for (uint32_t q = 0; q < 16; ++q) { r[q].start = 100 + q; r[q].end = 300 + q; }
    for (uint32_t qps : {1U, 2U, 4U, 8U, 16U}) {
        for (uint32_t n : {1U, 2U, 4U, 8U, 16U}) {
            for (uint32_t bytes = 512; bytes <= 1048576; bytes *= 2) {
                assert(SummarizeQps(r,qps,bytes,512,summary,n));
                assert(summary.payloadBytes == uint64_t(qps) * 512 * bytes * n);
                assert(summary.duration == 200 + qps - 1);
                assert(std::abs(summary.gbps - double(summary.payloadBytes)*8/summary.duration) < 1e-9);
            }
        }
    }
    assert(SummarizeQps(r,16,1048576,512,summary,8));
    assert(summary.payloadBytes == (uint64_t(64) << 30));
    for (uint32_t n : {0U, 3U, 17U}) { assert(!SummarizeQps(r,2,512,512,summary,n)); }
    assert(!SummarizeQps(r,1,UINT32_MAX,UINT32_MAX,summary,2)); // SGE multiplication overflow
    assert(!SummarizeQps(r,2,UINT32_MAX,UINT32_MAX,summary)); // QP multiplication overflow
    r[1].start = 500; r[1].end = 700;
    r[1].status = 1; assert(!SummarizeQps(r,2,512,10,summary));
    r[1].status = 0; r[1].error = 1; assert(!SummarizeQps(r,2,512,10,summary));
    r[1].error = 0; r[1].end = r[1].start; assert(!SummarizeQps(r,2,512,10,summary));
    r[1].end = 700; r[1].magic = 0; assert(!SummarizeQps(r,2,512,10,summary));
}
