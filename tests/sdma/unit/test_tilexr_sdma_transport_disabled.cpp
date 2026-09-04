#include <cstdlib>
#include <iostream>
#include <string>

#include "sdma/tilexr_sdma_a5_backend.h"
#include "sdma/tilexr_sdma_transport.h"
#include "tilexr_sdma_types.h"
#include "tilexr_types.h"

extern "C" const char* aclrtGetSocName(void)
{
    return "Ascend910B";
}

namespace TileXR {

struct TileXRA5SDMABackend::Impl {};

TileXRA5SDMABackend::TileXRA5SDMABackend() = default;
TileXRA5SDMABackend::~TileXRA5SDMABackend() = default;

bool TileXRA5SDMABackend::Init(int32_t)
{
    return false;
}

bool TileXRA5SDMABackend::Shutdown()
{
    return true;
}

GM_ADDR TileXRA5SDMABackend::GetWorkspaceDev() const
{
    return nullptr;
}

} // namespace TileXR

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK_TRUE failed at line " << __LINE__ << ": " #expr << std::endl; \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(lhs, rhs) \
    do { \
        auto lhsValue = (lhs); \
        auto rhsValue = (rhs); \
        if (lhsValue != rhsValue) { \
            std::cerr << "CHECK_EQ failed at line " << __LINE__ << ": " #lhs " != " #rhs \
                      << " (" << static_cast<int>(lhsValue) << " vs " << static_cast<int>(rhsValue) << ")" \
                      << std::endl; \
            ++g_failures; \
        } \
    } while (0)

class EnvGuard {
public:
    EnvGuard()
    {
        const char* value = std::getenv("TILEXR_ENABLE_SDMA");
        if (value != nullptr) {
            hadValue_ = true;
            value_ = value;
        }
    }

    ~EnvGuard()
    {
        if (hadValue_) {
            setenv("TILEXR_ENABLE_SDMA", value_.c_str(), 1);
        } else {
            unsetenv("TILEXR_ENABLE_SDMA");
        }
    }

    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    bool hadValue_ = false;
    std::string value_;
};

void TestEnvDisabledSkipsInitialization()
{
    EnvGuard env;
    unsetenv("TILEXR_ENABLE_SDMA");
    TileXR::TileXRSDMATransport transport;
    TileXR::TileXRSDMATransportOptions options {};
    options.devId = 0;
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::DISABLED_BY_ENV);
}

void TestEnvZeroSkipsInitialization()
{
    EnvGuard env;
    setenv("TILEXR_ENABLE_SDMA", "0", 1);
    TileXR::TileXRSDMATransport transport;
    TileXR::TileXRSDMATransportOptions options {};
    options.devId = 0;
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::DISABLED_BY_ENV);
}

void TestEnvOneReportsPTOUnavailable()
{
    EnvGuard env;
    setenv("TILEXR_ENABLE_SDMA", "1", 1);
    TileXR::TileXRSDMATransport transport;
    TileXR::TileXRSDMATransportOptions options {};
    options.devId = 0;
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::PTO_UNAVAILABLE);
}

void TestSameInstanceTransitionsResetState()
{
    EnvGuard env;
    TileXR::TileXRSDMATransport transport;
    TileXR::TileXRSDMATransportOptions options {};
    options.devId = 0;

    unsetenv("TILEXR_ENABLE_SDMA");
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::DISABLED_BY_ENV);

    setenv("TILEXR_ENABLE_SDMA", "1", 1);
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::PTO_UNAVAILABLE);

    transport.Shutdown();
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::PTO_UNAVAILABLE);

    unsetenv("TILEXR_ENABLE_SDMA");
    CHECK_EQ(transport.Init(options), TileXR::TILEXR_SUCCESS);
    CHECK_TRUE(!transport.IsAvailable());
    CHECK_TRUE(transport.GetWorkspaceDev() == nullptr);
    CHECK_EQ(transport.GetLastStatus(), TileXR::SDMAInitStatus::DISABLED_BY_ENV);
}

} // namespace

int main()
{
    TestEnvDisabledSkipsInitialization();
    TestEnvZeroSkipsInitialization();
    TestEnvOneReportsPTOUnavailable();
    TestSameInstanceTransitionsResetState();
    if (g_failures != 0) {
        std::cerr << g_failures << " SDMA transport disabled checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR SDMA transport disabled checks passed" << std::endl;
    return 0;
}
