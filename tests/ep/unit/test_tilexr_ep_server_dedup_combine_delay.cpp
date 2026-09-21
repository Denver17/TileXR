#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "kernel_operator.h"
#include "ep_memory_server_dedup_combine_tiling.h"
#include "server_dedup_test_checks.h"

extern "C" void tilexr_ep_combine_memory_server_dedup_kernel(
    GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR,
    GM_ADDR, GM_ADDR, int64_t);

namespace {
std::vector<uint64_t> timestamps;
size_t nextTimestamp = 0;

void Invoke(GM_ADDR tensor, GM_ADDR tiling)
{
    tilexr_ep_combine_memory_server_dedup_kernel(
        tensor, tensor, tensor, tensor, tensor, tensor, tensor, tensor, tensor, tiling, 2);
}

void CheckDelay(uint64_t start)
{
    // Repeating the first value also checks that a stationary tick does not
    // end the loop. Requiring every sample rejects both early and late exits.
    timestamps = {start, start, start + 49999U, start + 50000U};
    nextTimestamp = 0;
    TileXREp::EpServerDedupCombineTilingData tiling{2, 16, 32, 3, 0, 0, 0, {0, 0}};
    const auto originalTiling = tiling;
    std::array<uint8_t, 128> tensor;
    tensor.fill(0xa5);
    const auto originalTensor = tensor;
    Invoke(tensor.data(), reinterpret_cast<GM_ADDR>(&tiling));
    ServerDedupTest::Check(nextTimestamp == timestamps.size(), "wait reaches exactly 50,000 elapsed ticks");
    ServerDedupTest::Check(tensor == originalTensor, "empty Combine leaves all tensors and communication data unchanged");
    ServerDedupTest::Check(std::memcmp(&tiling, &originalTiling, sizeof(tiling)) == 0,
        "empty Combine leaves tiling unchanged");
}
} // namespace

namespace AscendC {
int64_t GetSystemCycle()
{
    if (nextTimestamp == timestamps.size()) {
        throw std::runtime_error("empty Combine requested a timestamp after its deadline");
    }
    // Preserve the signed API's bit pattern across INT64_MAX and UINT64_MAX.
    const uint64_t bits = timestamps[nextTimestamp++];
    int64_t value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace AscendC

int main()
{
    try {
        CheckDelay(100U);
        CheckDelay(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 10000U);
        CheckDelay(std::numeric_limits<uint64_t>::max() - 10000U);
        // No business or communication pointer is needed by the empty entry.
        TileXREp::EpServerDedupCombineTilingData tiling{};
        timestamps = {0, 50000U};
        nextTimestamp = 0;
        Invoke(nullptr, reinterpret_cast<GM_ADDR>(&tiling));
        ServerDedupTest::Check(nextTimestamp == 2U, "null business pointers are never dereferenced");
        timestamps.clear();
        nextTimestamp = 0;
        Invoke(nullptr, nullptr);
        ServerDedupTest::Check(nextTimestamp == 0U, "null tiling keeps the guarded no-op path");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (ServerDedupTest::Failures() != 0) { return 1; }
    std::cout << "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS test=delay\n";
    return 0;
}
