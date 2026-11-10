/*
 * 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "acl/acl_rt.h"
#include "combine_v2_host.h"
#include "combine_v2_launch.h"
#include "tilexr_types.h"
#include "tilexr_udma_reg.h"

namespace {

int failures = 0;
int hostReturn = TileXR::TILEXR_SUCCESS;
int registryReturn = TileXR::TILEXR_SUCCESS;
int qpReturn = TileXR::TILEXR_SUCCESS;
int fullmeshReturn = TileXR::TILEXR_SUCCESS;
int devReturn = TileXR::TILEXR_SUCCESS;
int magicReturn = TileXR::TILEXR_SUCCESS;
int launchReturn = TILEXR_MOONEP_SUCCESS;
int aclDeviceReturn = ACL_SUCCESS;
int aclInfoReturn = ACL_SUCCESS;
uint32_t qpCount = TileXRMoonEp::kMoonEpCombineV2QpCount;
int64_t vectorCoreCount = TileXRMoonEp::kMoonEpCombineV2CoreCount;
int64_t nextMagic = 17;
uint32_t magicCallCount = 0U;
TileXR::CommArgs commArgs {};
TileXR::CommArgs weightCommArgs {};
TileXR::TileXRUDMARegistry registry {};
TileXR::TileXRUDMAFullmeshHostView fullmeshView {};
GM_ADDR devArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x9000});
GM_ADDR weightDevArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0xA000});
TileXRMoonEp::CombineV2Params launchedParams {};
TileXRMoonEp::CombineV2LaunchContext launchedContext {};
bool launchedReduceHidden[2] = {};
size_t launchCount = 0;

struct MemcpyRecord {
    void *dst = nullptr;
    size_t dstMax = 0;
    const void *src = nullptr;
    size_t count = 0;
};

MemcpyRecord memcpyRecords[4] = {};
size_t memcpyCount = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void CheckStatus(int actual, int expected, const char *message)
{
    if (actual != expected) {
        std::cerr << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

TileXRMoonEp::CombineV2Params ValidParams()
{
    TileXRMoonEp::CombineV2Params params {};
    params.registeredWorkspace = reinterpret_cast<void *>(uintptr_t {0x100000000ULL});
    params.dstLocal = reinterpret_cast<const int32_t *>(uintptr_t {0x200000000ULL});
    params.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x3000});
    params.bs = TileXRMoonEp::kMoonEpCombineV2SmallBs;
    params.h = TileXRMoonEp::kMoonEpCombineV2TargetH;
    params.topK = TileXRMoonEp::kMoonEpCombineV2TargetTopK;
    params.nvS = TileXRMoonEp::kMoonEpCombineV2SmallSlots;
    params.aivCoreNum = TileXRMoonEp::kMoonEpCombineV2CoreCount;
    static uint64_t activeOutputOffset = 0;
    params.activeOutputOffset = &activeOutputOffset;
    params.dtype = TILEXR_MOONEP_DTYPE_BFLOAT16;
    params.reduceHidden = true;
    params.stream = reinterpret_cast<aclrtStream>(uintptr_t {0x4000});
    return params;
}

bool RankSizeSupported(int rankSize)
{
    return (rankSize >= 2 && rankSize <= 8) || rankSize == 16 ||
        rankSize == 32 || rankSize == 64 || rankSize == 128;
}

void ConfigureRankSize(int rankSize, int rank = 0)
{
    commArgs.rank = rank;
    commArgs.rankSize = rankSize;
    commArgs.localRankSize = rankSize <= 8 ? rankSize : 8;
    commArgs.localRank = rank % commArgs.localRankSize;
    weightCommArgs.rank = commArgs.rank;
    weightCommArgs.rankSize = commArgs.rankSize;
    weightCommArgs.localRankSize = commArgs.localRankSize;
    weightCommArgs.localRank = commArgs.localRank;
    fullmeshView = TileXR::TileXRUDMAFullmeshHostView {};
    fullmeshView.slotCount = TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    fullmeshView.connectedCount =
        static_cast<uint32_t>(commArgs.localRankSize - 1);
    fullmeshView.localRank = static_cast<uint32_t>(commArgs.localRank);
    fullmeshView.validPeerMask = TileXR::UDMAFullmeshExpectedPeerMask(
        fullmeshView.localRank,
        static_cast<uint32_t>(commArgs.localRankSize));
    fullmeshView.registrationReady = 1U;
    fullmeshView.registrationGeneration =
        commArgs.udmaRegistrationGeneration;
    fullmeshView.infoDev = reinterpret_cast<GM_ADDR>(
        uintptr_t {0x510000000ULL});
    fullmeshView.viewDev = commArgs.udmaFullmeshPtr;

    registry = TileXR::TileXRUDMARegistry {};
    registry.rankSize = rankSize;
    registry.regionCount = 1;
    const TileXRMoonEp::CombineV2Params params = ValidParams();
    for (int peer = 0; peer < rankSize; ++peer) {
        commArgs.creditMems[peer] = reinterpret_cast<GM_ADDR>(
            uintptr_t {0x700000000ULL} +
            static_cast<uintptr_t>(peer) * TileXR::CREDIT_IPC_BYTES);
        registry.regions[peer].base =
            static_cast<GM_ADDR>(params.registeredWorkspace);
        registry.regions[peer].bytes = 4194304U;
    }
}

void Reset()
{
    hostReturn = registryReturn = qpReturn = fullmeshReturn =
        devReturn = magicReturn =
        TileXR::TILEXR_SUCCESS;
    launchReturn = TILEXR_MOONEP_SUCCESS;
    aclDeviceReturn = aclInfoReturn = ACL_SUCCESS;
    qpCount = 48U;
    vectorCoreCount = TileXRMoonEp::kMoonEpCombineV2CoreCount;
    nextMagic = 17;
    magicCallCount = 0U;
    devArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x9000});
    weightDevArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0xA000});
    launchedParams = TileXRMoonEp::CombineV2Params {};
    launchedContext = TileXRMoonEp::CombineV2LaunchContext {};
    launchCount = 0;
    for (size_t index = 0; index < 2U; ++index) {
        launchedReduceHidden[index] = false;
    }
    memcpyCount = 0;
    for (MemcpyRecord &record : memcpyRecords) {
        record = MemcpyRecord {};
    }

    commArgs = TileXR::CommArgs {};
    weightCommArgs = TileXR::CommArgs {};
    commArgs.extraFlag = TileXR::ExtraFlag::UDMA |
        TileXR::ExtraFlag::UDMA_SHARED_QP |
        TileXR::ExtraFlag::UDMA_FULLMESH;
    commArgs.udmaInfoPtr = reinterpret_cast<GM_ADDR>(uintptr_t {0x500000000ULL});
    commArgs.udmaRegistryPtr = reinterpret_cast<GM_ADDR>(uintptr_t {0x600000000ULL});
    commArgs.udmaFullmeshPtr = reinterpret_cast<GM_ADDR>(
        uintptr_t {0x610000000ULL});
    commArgs.udmaRegistrationGeneration = 9U;
    commArgs.commDomain = 0;
    ConfigureRankSize(TileXRMoonEp::kMoonEpCombineV2RankCount);
    weightCommArgs.rank = commArgs.rank;
    weightCommArgs.rankSize = commArgs.rankSize;
    weightCommArgs.localRank = commArgs.localRank;
    weightCommArgs.localRankSize = commArgs.localRankSize;
    weightCommArgs.extraFlag = TileXR::ExtraFlag::MEMORY_ONLY;
    weightCommArgs.commDomain = 1;
    weightCommArgs.peerMemBytes = 12U * 1024U * 1024U;
    for (int peer = 0; peer < weightCommArgs.rankSize; ++peer) {
        weightCommArgs.peerMems[peer] = reinterpret_cast<GM_ADDR>(
            uintptr_t {0x800000000ULL} +
            static_cast<uintptr_t>(peer) * weightCommArgs.peerMemBytes);
    }
}

void TestValidLaunch()
{
    Reset();
    const TileXRMoonEp::CombineV2Params params = ValidParams();
    uint64_t workspaceBytes = 0;
    uint64_t epoch0Offset = 0;
    uint64_t epoch1Offset = 0;
    CheckStatus(TileXRMoonEpCombineGetWorkspaceSizeV2(
        params.bs, params.h, params.topK, params.nvS, params.dtype,
        &workspaceBytes, &epoch0Offset, &epoch1Offset),
        TILEXR_MOONEP_SUCCESS, "public workspace query");
    Check(workspaceBytes == 4194304U && epoch0Offset < epoch1Offset,
        "public workspace layout mismatch");
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_SUCCESS, "valid V2 launch");
    Check(launchedParams.registeredWorkspace == params.registeredWorkspace &&
        launchedParams.dstLocal == params.dstLocal &&
        launchedParams.aivCoreNum == params.aivCoreNum,
        "launch parameters mismatch");
    Check(launchedContext.hostArgs == &commArgs &&
        launchedContext.devArgs == devArgs && launchedContext.magic == nextMagic,
        "launch context mismatch");
    Check(*params.activeOutputOffset == launchedContext.layout.scratchOffset[1],
        "active output epoch mismatch");
}

void TestValidation()
{
    TileXRMoonEp::CombineV2Params params = ValidParams();

    Reset();
    commArgs.creditMems[commArgs.rankSize - 1] = nullptr;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "missing Combine Credit IPC mapping accepted");
    Check(magicCallCount == 0U,
        "missing Combine Credit IPC mapping consumed a magic");

    Reset();
    commArgs.extraFlag &= ~TileXR::ExtraFlag::UDMA_SHARED_QP;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED, "shared QP capability required");

    Reset();
    qpCount = 8;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED, "at least 32 QPs required");

    Reset();
    commArgs.extraFlag &= ~TileXR::ExtraFlag::UDMA_FULLMESH;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh capability required");
    Check(magicCallCount == 0U,
        "missing Fullmesh consumed a magic");

    Reset();
    commArgs.udmaFullmeshPtr = nullptr;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh device view required");
    Check(magicCallCount == 0U,
        "null Fullmesh view consumed a magic");

    Reset();
    fullmeshReturn = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh query failure must reject launch");

    Reset();
    fullmeshView.version = TileXR::TILEXR_UDMA_FULLMESH_VERSION + 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh version mismatch must reject launch");

    Reset();
    fullmeshView.connectedCount -= 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "incomplete Fullmesh connection count must reject launch");

    Reset();
    fullmeshView.validPeerMask ^= 1U << 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "incomplete Fullmesh peer mask must reject launch");

    Reset();
    fullmeshView.registrationReady = 0U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "unregistered Fullmesh generation must reject launch");

    Reset();
    fullmeshView.registrationGeneration += 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh generation mismatch must reject launch");

    Reset();
    fullmeshView.viewDev = reinterpret_cast<GM_ADDR>(
        uintptr_t {0x620000000ULL});
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "Fullmesh publication pointer mismatch must reject launch");

    Reset();
    ConfigureRankSize(12);
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "supported rank class required");

    Reset();
    registry.regions[17].bytes = 4096;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "workspace capacity required");

    Reset();
    vectorCoreCount = TileXRMoonEp::kMoonEpCombineV2CoreCount - 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED, "16 vector cores required");

    Reset();
    nextMagic = 0;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INTERNAL, "positive magic required");

    Reset();
    params.bs = 16;
    params.nvS = 255;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT, "unsupported shape rejected");
}

void TestRankAndCoreGeneralization()
{
    for (int rankSize = 1; rankSize <= 128; ++rankSize) {
        Reset();
        ConfigureRankSize(rankSize);
        const int expected = RankSizeSupported(rankSize) ?
            TILEXR_MOONEP_SUCCESS : TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(ValidParams()),
            expected, "rank-size support predicate");
    }

    Reset();
    ConfigureRankSize(8);
    commArgs.localRankSize = 4;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(ValidParams()),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "small rank class requires one local domain");

    Reset();
    ConfigureRankSize(16);
    commArgs.localRankSize = 16;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(ValidParams()),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "ring rank class requires eight local ranks");

    Reset();
    ConfigureRankSize(16, 9);
    commArgs.localRank = 0;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(ValidParams()),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "local rank must match global rank mapping");

    Reset();
    TileXRMoonEp::CombineV2Params params = ValidParams();
    params.aivCoreNum = TileXRMoonEp::kMoonEpCombineV2CoreCount - 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "fifteen requested AIVs must be rejected");

    Reset();
    params = ValidParams();
    params.aivCoreNum = TileXRMoonEp::kMoonEpCombineV2CoreCount + 1U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "seventeen requested AIVs must be rejected");
}

void TestWeightMemoryValidation()
{
    TileXRMoonEp::CombineV2Params params = ValidParams();
    params.weightMemoryComm = reinterpret_cast<TileXRCommPtr>(
        uintptr_t {0x3500});
    params.routeWeightsNvs = reinterpret_cast<const float *>(
        uintptr_t {0x720000000ULL});
    params.routeWeightsSk = reinterpret_cast<float *>(
        uintptr_t {0x730000000ULL});

    Reset();
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_SUCCESS, "valid weight Memory communicator");

    Reset();
    params.weightMemoryComm = params.comm;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "hidden communicator accepted as weight Memory communicator");

    Reset();
    params.weightMemoryComm = reinterpret_cast<TileXRCommPtr>(
        uintptr_t {0x3500});
    weightCommArgs.rank = 1;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "mismatched weight Memory rank accepted");

    Reset();
    weightCommArgs.localRankSize -= 1;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "mismatched weight Memory local rank size accepted");

    Reset();
    weightCommArgs.commDomain = commArgs.commDomain;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "shared weight Memory domain accepted");

    Reset();
    weightCommArgs.extraFlag = 0U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "communicator without Memory-only capability accepted");

    Reset();
    weightCommArgs.extraFlag |= TileXR::ExtraFlag::UDMA;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "weight Memory communicator with UDMA enabled accepted");

    Reset();
    weightCommArgs.peerMems[weightCommArgs.rankSize - 1] = nullptr;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED,
        "incomplete weight Memory mapping accepted");

    Reset();
    weightCommArgs.peerMemBytes = TileXR::IPC_DATA_OFFSET + 64U;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "undersized weight Memory window accepted");

    Reset();
    weightDevArgs = nullptr;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INTERNAL,
        "missing weight Memory device arguments accepted");

    Reset();
    params.routeWeightsSk = nullptr;
    CheckStatus(TileXRMoonEp::TileXRMoonEpRunCombineV2(params),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT,
        "one-sided route weight pointers accepted");
}

} // namespace

extern "C" int TileXRGetCommArgsHost(TileXRCommPtr comm,
    TileXR::CommArgs *&args)
{
    args = hostReturn == TileXR::TILEXR_SUCCESS ?
        (comm == reinterpret_cast<TileXRCommPtr>(uintptr_t {0x3500}) ?
            &weightCommArgs : &commArgs) : nullptr;
    return hostReturn;
}

extern "C" int TileXRGetUDMARegistryHost(
    TileXRCommPtr, const TileXR::TileXRUDMARegistry **result)
{
    if (result != nullptr) {
        *result = registryReturn == TileXR::TILEXR_SUCCESS ? &registry : nullptr;
    }
    return registryReturn;
}

extern "C" int TileXRUDMAGetQpCount(TileXRCommPtr, uint32_t *result)
{
    if (result != nullptr) {
        *result = qpReturn == TileXR::TILEXR_SUCCESS ? qpCount : 0;
    }
    return qpReturn;
}

extern "C" int TileXRUDMAFullmeshQuery(TileXRCommPtr,
    TileXR::TileXRUDMAFullmeshHostView *result)
{
    if (result != nullptr) {
        *result = fullmeshReturn == TileXR::TILEXR_SUCCESS ?
            fullmeshView : TileXR::TileXRUDMAFullmeshHostView {};
    }
    return fullmeshReturn;
}

extern "C" int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &args)
{
    args = devReturn == TileXR::TILEXR_SUCCESS ?
        (comm == reinterpret_cast<TileXRCommPtr>(uintptr_t {0x3500}) ?
            weightDevArgs : devArgs) : nullptr;
    return devReturn;
}

extern "C" int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    ++magicCallCount;
    if (magicReturn == TileXR::TILEXR_SUCCESS && magic != nullptr) {
        *magic = nextMagic;
    }
    return magicReturn;
}

extern "C" aclError aclrtGetDevice(int32_t *deviceId)
{
    if (aclDeviceReturn == ACL_SUCCESS && deviceId != nullptr) {
        *deviceId = 0;
    }
    return aclDeviceReturn;
}

extern "C" aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr, int64_t *value)
{
    if (aclInfoReturn == ACL_SUCCESS && value != nullptr) {
        *value = vectorCoreCount;
    }
    return aclInfoReturn;
}

void TestStageUsesDedicatedHiddenOutput()
{
    Reset();
    const TileXRMoonEp::CombineV2Params params = ValidParams();
    TileXRMoonEp::CombineV2Layout hiddenLayout {};
    TileXRMoonEp::CombineV2Layout weightLayout {};
    CheckStatus(TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        params.bs, params.h, params.topK, params.nvS, params.dtype,
        &hiddenLayout), TILEXR_MOONEP_SUCCESS, "stage hidden layout");
    CheckStatus(TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        params.bs, 1, params.topK, params.nvS,
        TILEXR_MOONEP_DTYPE_FLOAT32, &weightLayout),
        TILEXR_MOONEP_SUCCESS, "stage weight layout");

    const void *hiddenNvsh = reinterpret_cast<const void *>(
        uintptr_t {0x700000000ULL});
    void *hiddenSh = reinterpret_cast<void *>(uintptr_t {0x710000000ULL});
    const float *weightsNvs = reinterpret_cast<const float *>(
        uintptr_t {0x720000000ULL});
    float *weightsSk = reinterpret_cast<float *>(uintptr_t {0x730000000ULL});
    CheckStatus(TileXRMoonEpCombineStageV2(params.registeredWorkspace,
        hiddenLayout.totalBytes, params.dstLocal, params.comm, params.bs,
        params.h, params.topK, params.nvS, params.aivCoreNum, hiddenNvsh,
        hiddenSh, weightsNvs, weightsSk, params.dtype, params.stream),
        TILEXR_MOONEP_SUCCESS, "valid V2 stage");

    const uint8_t *workspace = static_cast<const uint8_t *>(
        params.registeredWorkspace);
    Check(memcpyCount == 4U, "V2 stage memcpy count mismatch");
    Check(memcpyRecords[1].dst == hiddenSh &&
        memcpyRecords[1].src == workspace + hiddenLayout.outputOffset &&
        memcpyRecords[1].count == hiddenLayout.outputBytes,
        "hidden output copy does not use the dedicated workspace region");
    Check(hiddenLayout.outputOffset >= weightLayout.totalBytes,
        "hidden output can be overwritten by the weight workspace");
    Check(launchCount == 2U && launchedReduceHidden[0] &&
        !launchedReduceHidden[1],
        "V2 stage launch modes mismatch");
}

void TestFusedStageUsesOneLaunch()
{
    Reset();
    ConfigureRankSize(8);
    const TileXRMoonEp::CombineV2Params params = ValidParams();
    TileXRMoonEp::CombineV2Layout hiddenLayout {};
    CheckStatus(TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        params.bs, params.h, params.topK, params.nvS, params.dtype,
        &hiddenLayout), TILEXR_MOONEP_SUCCESS, "fused stage hidden layout");
    const void *hiddenNvsh = reinterpret_cast<const void *>(
        uintptr_t {0x700000000ULL});
    void *hiddenSh = reinterpret_cast<void *>(uintptr_t {0x710000000ULL});
    const float *weightsNvs = reinterpret_cast<const float *>(
        uintptr_t {0x720000000ULL});
    float *weightsSk = reinterpret_cast<float *>(uintptr_t {0x730000000ULL});
    TileXRCommPtr weightComm = reinterpret_cast<TileXRCommPtr>(
        uintptr_t {0x3500});
    CheckStatus(TileXRMoonEpCombineStageV2Fused(
        params.registeredWorkspace, hiddenLayout.totalBytes,
        params.dstLocal, params.comm, weightComm, params.bs, params.h,
        params.topK, params.nvS, params.aivCoreNum, hiddenNvsh, hiddenSh,
        weightsNvs, weightsSk, params.dtype, params.stream),
        TILEXR_MOONEP_SUCCESS, "valid fused V2 stage");
    Check(memcpyCount == 2U, "fused V2 stage memcpy count mismatch");
    Check(launchCount == 1U && launchedReduceHidden[0],
        "fused V2 stage must use one hidden reduction launch");
    Check(launchedParams.weightMemoryComm == weightComm &&
        launchedParams.routeWeightsNvs == weightsNvs &&
        launchedParams.routeWeightsSk == weightsSk,
        "fused V2 stage dropped weight parameters");
    Check(launchedContext.weightMemoryHostArgs == &weightCommArgs &&
        launchedContext.weightMemoryDevArgs == weightDevArgs &&
        launchedContext.weightOutputElements ==
            static_cast<uint64_t>(params.bs * params.topK),
        "fused V2 stage weight launch context mismatch");
}

void TestFusedStageFallsBackAcrossNodes()
{
    Reset();
    ConfigureRankSize(16);
    const TileXRMoonEp::CombineV2Params params = ValidParams();
    TileXRMoonEp::CombineV2Layout hiddenLayout {};
    CheckStatus(TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        params.bs, params.h, params.topK, params.nvS, params.dtype,
        &hiddenLayout), TILEXR_MOONEP_SUCCESS,
        "multi-node fallback hidden layout");
    const void *hiddenNvsh = reinterpret_cast<const void *>(
        uintptr_t {0x700000000ULL});
    void *hiddenSh = reinterpret_cast<void *>(uintptr_t {0x710000000ULL});
    const float *weightsNvs = reinterpret_cast<const float *>(
        uintptr_t {0x720000000ULL});
    float *weightsSk = reinterpret_cast<float *>(uintptr_t {0x730000000ULL});

    CheckStatus(TileXRMoonEpCombineStageV2Fused(
        params.registeredWorkspace, hiddenLayout.totalBytes,
        params.dstLocal, params.comm, nullptr, params.bs, params.h,
        params.topK, params.nvS, params.aivCoreNum, hiddenNvsh, hiddenSh,
        weightsNvs, weightsSk, params.dtype, params.stream),
        TILEXR_MOONEP_SUCCESS, "multi-node fused API fallback");
    Check(memcpyCount == 4U,
        "multi-node fused API must use the two-pass copy sequence");
    Check(launchCount == 2U && launchedReduceHidden[0] &&
        !launchedReduceHidden[1],
        "multi-node fused API must use hidden and weight launches");
    Check(launchedParams.weightMemoryComm == nullptr &&
        launchedParams.routeWeightsNvs == nullptr &&
        launchedParams.routeWeightsSk == nullptr,
        "multi-node fallback leaked fused weight arguments");
}

extern "C" aclError aclrtMemcpyAsync(
    void *dst, size_t dstMax, const void *src, size_t count,
    aclrtMemcpyKind, aclrtStream)
{
    if (memcpyCount < 4U) {
        memcpyRecords[memcpyCount] = MemcpyRecord {dst, dstMax, src, count};
    }
    ++memcpyCount;
    return ACL_SUCCESS;
}

namespace TileXRMoonEp {
int TileXRMoonEpLaunchCombineV2Kernel(
    const CombineV2Params &params,
    const CombineV2LaunchContext &context)
{
    launchedParams = params;
    launchedContext = context;
    if (launchCount < 2U) {
        launchedReduceHidden[launchCount] = params.reduceHidden;
    }
    ++launchCount;
    if (launchReturn == TILEXR_MOONEP_SUCCESS) {
        *params.activeOutputOffset = context.layout.scratchOffset[
            MoonEpCombineV2Epoch(static_cast<uint64_t>(context.magic))];
    }
    return launchReturn;
}
} // namespace TileXRMoonEp

int main()
{
    TestValidLaunch();
    TestValidation();
    TestStageUsesDedicatedHiddenOutput();
    TestFusedStageUsesOneLaunch();
    TestFusedStageFallsBackAcrossNodes();
    TestWeightMemoryValidation();
    TestRankAndCoreGeneralization();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
