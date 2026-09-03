#include <cstdint>
#include <iostream>

#include "collective_launcher.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;

void CheckStatus(const char *label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckPointer(const char *label, const void *actual, const void *expected)
{
    if (actual != expected) {
        std::cerr << label << " was " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckMagic(const char *label, int64_t actual, int64_t expected)
{
    if (actual != expected) {
        std::cerr << label << " wrote magic " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckResetContext(const TileXRCollectives::Host::HostLaunchContext &context)
{
    CheckPointer("context.hostArgs", context.hostArgs, nullptr);
    CheckPointer("context.devArgs", context.devArgs, nullptr);
    CheckMagic("context.magic", context.magic, 0);
}

} // namespace

int main()
{
    TileXRCollectives::Host::HostLaunchContext context;

    CheckStatus("PrepareHostLaunchContext(nullptr, context)",
                TileXRCollectives::Host::PrepareHostLaunchContext(nullptr, context),
                TileXR::TILEXR_ERROR_INTERNAL);
    CheckResetContext(context);

    TileXRCommPtr comm = nullptr;
    CheckStatus("TileXRCommInit(0, 1, &comm)",
                TileXRCommInit(0, 1, &comm),
                TileXR::TILEXR_SUCCESS);
    if (comm == nullptr) {
        std::cerr << "TileXRCommInit returned success but left comm null" << std::endl;
        return 1;
    }

    context.hostArgs = reinterpret_cast<TileXR::CommArgs *>(0x1);
    context.devArgs = reinterpret_cast<GM_ADDR>(0x2);
    context.magic = 99;
    CheckStatus("PrepareHostLaunchContext(uninitialized comm, context)",
                TileXRCollectives::Host::PrepareHostLaunchContext(comm, context),
                TileXR::TILEXR_ERROR_NOT_INITIALIZED);
    CheckResetContext(context);

    int64_t magic = -1;
    CheckStatus("TileXRCommNextMagic(comm, &magic)",
                TileXRCommNextMagic(comm, &magic),
                TileXR::TILEXR_SUCCESS);
    CheckMagic("TileXRCommNextMagic(comm, &magic)", magic, 1);

    CheckStatus("TileXRCommDestroy(comm)",
                TileXRCommDestroy(comm),
                TileXR::TILEXR_SUCCESS);
    return g_failures == 0 ? 0 : 1;
}
