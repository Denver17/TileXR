#ifndef TILEXR_EP_SERVER_DEDUP_TEST_CHECKS_H
#define TILEXR_EP_SERVER_DEDUP_TEST_CHECKS_H

#include <iostream>
#include <cstdint>

namespace ServerDedupTest {

// Deliberately not 8 GiB: real kernel substitutes must consume CommArgs capacity.
constexpr uint64_t kPeerBytes = 6ULL * 1024U * 1024U * 1024U;

inline int &Failures()
{
    static int failures = 0;
    return failures;
}

inline void Check(bool condition, const char *label)
{
    if (!condition) {
        std::cerr << "FAIL: " << label << std::endl;
        ++Failures();
    }
}

inline int Finish(const char *name)
{
    if (Failures() != 0) {
        return 1;
    }
    std::cout << "EP_SERVER_DEDUP_ABI_HOST_PASS test=" << name << std::endl;
    return 0;
}

} // namespace ServerDedupTest

#endif
