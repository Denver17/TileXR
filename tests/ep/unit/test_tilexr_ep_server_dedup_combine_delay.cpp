#include "combine_m12_test_fixture.h"
extern "C" void tilexr_ep_combine_memory_server_dedup_kernel(
    GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, int64_t);
int main()
{
    using namespace CombineM12Test;
    using ServerDedupTest::Check;
    try {
        Fixture f(0U, 17U);
        for (uint64_t start : {uint64_t(100U), uint64_t(INT64_MAX) - 10000U, UINT64_MAX - 10000U}) {
            ClockSamples() = {start, start, start + 49999U, start + 50000U}; ClockIndex() = 0U;
            f.Hooks(0U, 0U);
            tilexr_ep_combine_memory_server_dedup_kernel(reinterpret_cast<GM_ADDR>(&f.args),
                reinterpret_cast<GM_ADDR>(f.expert.data()), reinterpret_cast<GM_ADDR>(f.table.data()),
                reinterpret_cast<GM_ADDR>(f.counts.data()), f.untouched.data(), reinterpret_cast<GM_ADDR>(f.types.data()),
                f.untouched.data(), f.untouched.data(), f.untouched.data(), reinterpret_cast<GM_ADDR>(&f.tiling), 0);
            Check(ClockIndex() == 0U, "P2.2 performs no artificial clock wait");
            Check(f.observedPrepare == 8U && AscendC::PendingMte3().empty(), "M1 publication completed before return");
            f.ResetHooks();
        }
        ClockSamples().clear();
        const auto before = Clock();
        tilexr_ep_combine_memory_server_dedup_kernel(nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, 0);
        Check(Clock() == before, "null tiling returns before work");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS test=no-delay\n";
    return 0;
}
