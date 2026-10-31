#include <cstdint>
#include <iostream>
#include <limits>

#include "collective_kernel.h"
#include "collective_utils.h"

namespace {

int g_failures = 0;

void CheckBool(const char *label, bool actual, bool expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckInt64(const char *label, int64_t actual, int64_t expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckUint32(const char *label, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

TileXR::CommArgs Args(int rankSize, uint32_t extraFlag)
{
    TileXR::CommArgs args {};
    args.rankSize = rankSize;
    args.extraFlag = extraFlag;
    return args;
}

void TestDataTypeSupport()
{
    using TileXRCollectives::Host::IsSupportedDataType;

    CheckBool("INT8 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_INT8), true);
    CheckBool("INT16 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_INT16), true);
    CheckBool("INT32 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_INT32), true);
    CheckBool("INT64 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_INT64), true);
    CheckBool("FP16 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_FP16), true);
    CheckBool("FP32 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_FP32), true);
    CheckBool("BFP16 supported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_BFP16), true);

    CheckBool("UINT8 unsupported", IsSupportedDataType(TileXR::TILEXR_DATA_TYPE_UINT8), false);
    CheckBool("unknown datatype unsupported",
              IsSupportedDataType(static_cast<TileXR::TileXRDataType>(999)), false);
}

void TestReduceOpSupport()
{
    using TileXRCollectives::Host::IsSupportedReductionDataType;
    using TileXRCollectives::Host::IsSupportedReduceOp;

    CheckBool("INT8 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_INT8), true);
    CheckBool("INT16 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_INT16), true);
    CheckBool("INT32 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_INT32), true);
    CheckBool("FP16 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_FP16), true);
    CheckBool("FP32 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_FP32), true);
    CheckBool("BFP16 reduction datatype supported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_BFP16), true);
    CheckBool("UINT8 reduction datatype unsupported", IsSupportedReductionDataType(TileXR::TILEXR_DATA_TYPE_UINT8), false);

    CheckBool("SUM reduce op supported", IsSupportedReduceOp(TileXR::TILEXR_REDUCE_SUM), true);
    CheckBool("MAX reduce op unsupported", IsSupportedReduceOp(TileXR::TILEXR_REDUCE_MAX), false);
    CheckBool("MIN reduce op unsupported", IsSupportedReduceOp(TileXR::TILEXR_REDUCE_MIN), false);
    CheckBool("PROD reduce op unsupported", IsSupportedReduceOp(TileXR::TILEXR_REDUCE_PROD), false);
    CheckBool("reserved reduce op unsupported", IsSupportedReduceOp(TileXR::TILEXR_REDUCE_RESERVED), false);
    CheckBool("unknown reduce op unsupported",
              IsSupportedReduceOp(static_cast<TileXR::TileXRReduceOp>(999)), false);
}

void TestCountToBytes()
{
    using TileXRCollectives::Host::CountToBytes;

    CheckInt64("INT8 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_INT8), 7);
    CheckInt64("INT16 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_INT16), 14);
    CheckInt64("FP16 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_FP16), 14);
    CheckInt64("BFP16 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_BFP16), 14);
    CheckInt64("INT32 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_INT32), 28);
    CheckInt64("FP32 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_FP32), 28);
    CheckInt64("INT64 byte count", CountToBytes(7, TileXR::TILEXR_DATA_TYPE_INT64), 56);
    CheckInt64("negative count rejected",
               CountToBytes(-1, TileXR::TILEXR_DATA_TYPE_INT32),
               TileXR::TILEXR_INVALID_VALUE);
    CheckInt64("unsupported datatype rejected",
               CountToBytes(7, TileXR::TILEXR_DATA_TYPE_UINT8),
               TileXR::TILEXR_INVALID_VALUE);
    CheckInt64("overflow rejected",
               CountToBytes(std::numeric_limits<int64_t>::max() / 2 + 1, TileXR::TILEXR_DATA_TYPE_INT16),
               TileXR::TILEXR_INVALID_VALUE);
}

void TestAllGatherBlockNum()
{
    using TileXRCollectives::Host::GetAllGatherBlockNum;

    CheckUint32("rank2 allgather uses two blocks per rank",
                GetAllGatherBlockNum(Args(2, 0), 1024),
                4);
    CheckUint32("small rank4 allgather uses one block per rank",
                GetAllGatherBlockNum(Args(4, 0), 1024),
                4);
    CheckUint32("large rank4 allgather uses two blocks per rank",
                GetAllGatherBlockNum(Args(4, 0), 2 * 1024 * 1024),
                8);
    CheckUint32("pcie allgather uses two blocks per rank",
                GetAllGatherBlockNum(Args(4, TileXR::ExtraFlag::TOPO_PCIE), 1024),
                8);
    CheckUint32("910B2C 16-rank allgather uses AX block count",
                GetAllGatherBlockNum(Args(16, TileXR::ExtraFlag::TOPO_910B2C), 1024),
                10);
    CheckUint32("910_93 large allgather uses HDB ring block count",
                GetAllGatherBlockNum(Args(8, TileXR::ExtraFlag::TOPO_910_93), 33LL * 1024 * 1024),
                32);
}

void TestAllToAllBlockNum()
{
    using TileXRCollectives::Host::GetAllToAllBlockNum;

    CheckUint32("default alltoall unsupported because CCE path is 910_93-only",
                GetAllToAllBlockNum(Args(4, 0), 1024),
                0);
    CheckUint32("910_93 small alltoall uses two blocks per rank",
                GetAllToAllBlockNum(Args(8, TileXR::ExtraFlag::TOPO_910_93), 1024),
                16);
    CheckUint32("910_93 aligned large alltoall uses two-step block count",
                GetAllToAllBlockNum(Args(8, TileXR::ExtraFlag::TOPO_910_93), 2LL * 1024 * 1024),
                32);
    CheckUint32("910_93 large rank alltoall caps at two-step block count",
                GetAllToAllBlockNum(Args(32, TileXR::ExtraFlag::TOPO_910_93), 1024),
                32);
}

void TestAllReduceBlockNum()
{
    using TileXRCollectives::Host::GetAllReduceBlockNum;

    CheckUint32("rank2 small allreduce uses two blocks per rank",
                GetAllReduceBlockNum(Args(2, 0), 1024),
                4);
    CheckUint32("rank4 small allreduce uses one block per rank",
                GetAllReduceBlockNum(Args(4, 0), 1024),
                4);
    CheckUint32("rank4 large allreduce uses two blocks per rank",
                GetAllReduceBlockNum(Args(4, 0), 2 * 1024 * 1024),
                8);
    CheckUint32("pcie rank4 small allreduce uses generic one block per rank",
                GetAllReduceBlockNum(Args(4, TileXR::ExtraFlag::TOPO_PCIE), 1024),
                4);
    CheckUint32("pcie rank4 large allreduce uses generic two blocks per rank",
                GetAllReduceBlockNum(Args(4, TileXR::ExtraFlag::TOPO_PCIE), 2 * 1024 * 1024),
                8);
    CheckUint32("910B2C rank16 small allreduce follows lcal split",
                GetAllReduceBlockNum(Args(16, TileXR::ExtraFlag::TOPO_910B2C), 1024),
                16);
    CheckUint32("910B2C rank16 large allreduce follows lcal split",
                GetAllReduceBlockNum(Args(16, TileXR::ExtraFlag::TOPO_910B2C), 2 * 1024 * 1024),
                26);
    CheckUint32("910_93 rank8 large allreduce uses double ring",
                GetAllReduceBlockNum(Args(8, TileXR::ExtraFlag::TOPO_910_93), 33LL * 1024 * 1024),
                34);
}

void TestReduceScatterBlockNum()
{
    using TileXRCollectives::Host::GetReduceScatterBlockNum;

    CheckUint32("rank2 small reducescatter uses two blocks per rank",
                GetReduceScatterBlockNum(Args(2, 0), 1024),
                4);
    CheckUint32("rank4 small reducescatter uses one block per rank",
                GetReduceScatterBlockNum(Args(4, 0), 1024),
                4);
    CheckUint32("rank4 large reducescatter uses two blocks per rank",
                GetReduceScatterBlockNum(Args(4, 0), 2 * 1024 * 1024),
                8);
    CheckUint32("pcie reducescatter uses two blocks per rank",
                GetReduceScatterBlockNum(Args(4, TileXR::ExtraFlag::TOPO_PCIE), 1024),
                8);
    CheckUint32("910_93 rank4 double ring reducescatter",
                GetReduceScatterBlockNum(Args(4, TileXR::ExtraFlag::TOPO_910_93), 512 * 1024),
                36);
    CheckUint32("910_93 rank16 large reducescatter uses four-step block count",
                GetReduceScatterBlockNum(Args(16, TileXR::ExtraFlag::TOPO_910_93), 2LL * 1024 * 1024),
                34);
    CheckUint32("910_93 rank4 huge reducescatter does not overflow double ring predicate",
                GetReduceScatterBlockNum(Args(4, TileXR::ExtraFlag::TOPO_910_93),
                                         std::numeric_limits<int64_t>::max()),
                8);
}

void TestBroadcastBlockNum()
{
    using TileXRCollectives::Host::GetBroadcastBlockNum;

    CheckUint32("rank1 broadcast has one block",
                GetBroadcastBlockNum(Args(1, 0), 1024),
                1);
    CheckUint32("rank8 broadcast uses rankSize blocks",
                GetBroadcastBlockNum(Args(8, 0), 1024),
                8);
    CheckUint32("rank9 broadcast unsupported",
                GetBroadcastBlockNum(Args(9, 0), 1024),
                0);
}

void TestKernelShimRejectsInvalidParametersBeforeMagic()
{
    TileXRCommPtr comm = nullptr;
    CheckInt64("TileXRCommInit for invalid kernel launch",
               TileXRCommInit(0, 1, &comm),
               TileXR::TILEXR_SUCCESS);
    if (comm == nullptr) {
        std::cerr << "TileXRCommInit returned success but left comm null" << std::endl;
        ++g_failures;
        return;
    }

    TileXR::CommArgs args {};
    args.rankSize = 2;
    TileXRCollectives::Host::HostLaunchContext context;
    context.hostArgs = &args;
    context.devArgs = reinterpret_cast<GM_ADDR>(0x1000);
    context.magic = 0;
    uint8_t sendStorage[16] = {};
    uint8_t recvStorage[16] = {};

    CheckInt64("LaunchCollectiveKernel rejects invalid blockDim",
               TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::ALL_GATHER, context,
                   sendStorage, recvStorage, 4, TileXR::TILEXR_DATA_TYPE_INT32, 0, nullptr),
               TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt64("LaunchCollectiveKernel preserved context magic after validation failure",
               context.magic,
               0);

    int64_t magic = -1;
    CheckInt64("TileXRCommNextMagic after invalid kernel launch",
               TileXRCommNextMagic(comm, &magic),
               TileXR::TILEXR_SUCCESS);
    CheckInt64("first magic after invalid kernel launch", magic, 1);

    CheckInt64("TileXRCommDestroy after invalid kernel launch",
               TileXRCommDestroy(comm),
               TileXR::TILEXR_SUCCESS);
}

void TestA5SimtLocalMemorySize()
{
    using TileXRCollectives::Host::ComputeA5SimtLocalMemorySize;

    uint32_t localMemorySize = 0;
    CheckBool("A5 SIMT memory accepts 192 KiB total UB",
              ComputeA5SimtLocalMemorySize(192LL * 1024, &localMemorySize),
              true);
    CheckUint32("A5 SIMT memory reserves 64 KiB DCache",
                localMemorySize,
                128U * 1024U);
    CheckBool("A5 SIMT memory rejects insufficient dynamic UB",
              ComputeA5SimtLocalMemorySize(128LL * 1024, &localMemorySize),
              false);
    CheckBool("A5 SIMT memory rejects null output",
              ComputeA5SimtLocalMemorySize(192LL * 1024, nullptr),
              false);
    CheckBool("A5 SIMT memory rejects uint32 overflow",
              ComputeA5SimtLocalMemorySize(
                  static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) +
                      64LL * 1024 + 1,
                  &localMemorySize),
              false);
}

} // namespace

int main()
{
    TestDataTypeSupport();
    TestReduceOpSupport();
    TestCountToBytes();
    TestAllGatherBlockNum();
    TestAllToAllBlockNum();
    TestAllReduceBlockNum();
    TestReduceScatterBlockNum();
    TestBroadcastBlockNum();
    TestKernelShimRejectsInvalidParametersBeforeMagic();
    TestA5SimtLocalMemorySize();
    return g_failures == 0 ? 0 : 1;
}
