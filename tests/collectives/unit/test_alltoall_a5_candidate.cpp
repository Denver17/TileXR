#include <cstdint>
#include <initializer_list>
#include <iostream>

#include "collective_launcher.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK_TRUE failed at line " << __LINE__ \
                      << ": " #expr << std::endl; \
            ++g_failures; \
        } \
    } while (0)

TileXRCollectives::Host::HostLaunchContext MakeContext(int rankSize)
{
    static TileXR::CommArgs args;
    args = TileXR::CommArgs {};
    args.rank = 0;
    args.rankSize = rankSize;
    args.localRankSize = 8;
    args.extraFlag = TileXR::ExtraFlag::TOPO_910A5 |
        TileXR::ExtraFlag::UDMA | TileXR::ExtraFlag::UDMA_SHARED_QP |
        TileXR::ExtraFlag::SDMA;
    args.udmaInfoPtr = reinterpret_cast<GM_ADDR>(0x1000);
    args.sdmaWorkspacePtr = reinterpret_cast<GM_ADDR>(0x2000);
    args.creditMems[0] = reinterpret_cast<GM_ADDR>(0x3000);
    TileXRCollectives::Host::HostLaunchContext context;
    context.hostArgs = &args;
    context.devArgs = reinterpret_cast<GM_ADDR>(0x4000);
    return context;
}

} // namespace

int main()
{
    for (int rankSize = 8; rankSize <= 128; rankSize += 8) {
        auto context = MakeContext(rankSize);
        CHECK_TRUE(TileXRCollectives::Host::IsA5AllToAllCandidate(
            context, TileXR::TILEXR_DATA_TYPE_INT32));
    }

    auto context = MakeContext(16);
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_FP32));
    context = MakeContext(4);
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_INT32));
    context = MakeContext(63);
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_INT32));
    context = MakeContext(16);
    context.hostArgs->localRankSize = 4;
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_INT32));
    context = MakeContext(16);
    context.hostArgs->extraFlag &= ~TileXR::ExtraFlag::SDMA;
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_INT32));
    context = MakeContext(16);
    context.hostArgs->creditMems[0] = nullptr;
    CHECK_TRUE(!TileXRCollectives::Host::IsA5AllToAllCandidate(
        context, TileXR::TILEXR_DATA_TYPE_INT32));

    return g_failures == 0 ? 0 : 1;
}
