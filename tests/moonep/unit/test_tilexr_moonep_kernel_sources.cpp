#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#ifdef TILEXR_SOURCE_ROOT
const char *kSourceRoot = TILEXR_SOURCE_ROOT;
#else
const char *kSourceRoot = ".";
#endif

std::string ReadFile(const std::string &relativePath)
{
    std::ifstream stream((std::string(kSourceRoot) + "/" + relativePath).c_str());
    if (!stream.is_open()) {
        std::cerr << "missing file: " << relativePath << '\n';
        ++failures;
        return std::string();
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void Contains(const char *label, const std::string &contents, const char *needle)
{
    if (contents.find(needle) == std::string::npos) {
        std::cerr << label << " missing: " << needle << '\n';
        ++failures;
    }
}

void Excludes(const char *label, const std::string &contents, const char *needle)
{
    if (contents.find(needle) != std::string::npos) {
        std::cerr << label << " contains forbidden text: " << needle << '\n';
        ++failures;
    }
}

void CountEquals(const char *label, const std::string &contents,
    const char *needle, size_t expected)
{
    size_t count = 0U;
    for (size_t position = contents.find(needle); position != std::string::npos;
        position = contents.find(needle, position + 1U)) {
        ++count;
    }
    if (count != expected) {
        std::cerr << label << " expected " << expected << " occurrences of "
                  << needle << ", got " << count << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    const std::string plannerHost =
        ReadFile("src/moonep/planner/host/planner_host.cpp");
    const std::string plannerKernel =
        ReadFile("src/moonep/planner/kernels/tilexr_moonep_planner_kernel.cpp");
    const std::string dispatchCommon =
        ReadFile("src/moonep/dispatch/common/dispatch_common.h");
    const std::string dispatchHost =
        ReadFile("src/moonep/dispatch/host/dispatch_host.cpp");
    const std::string dispatchLaunch =
        ReadFile("src/moonep/dispatch/host/dispatch_launch.cpp");
    const std::string dispatchKernel =
        ReadFile("src/moonep/dispatch/kernels/tilexr_moonep_dispatch_kernel.cpp");
    const std::string dispatchUrmaLaunch =
        ReadFile("src/moonep/dispatch/urma/host/dispatch_launch.cpp");
    const std::string dispatchUrmaKernel = ReadFile(
        "src/moonep/dispatch/urma/kernels/tilexr_moonep_dispatch_kernel.cpp");
    const std::string dispatchCmake = ReadFile("src/moonep/dispatch/CMakeLists.txt");
    const std::string combineCommon =
        ReadFile("src/moonep/combine/common/combine_common.h");
    const std::string combineHost =
        ReadFile("src/moonep/combine/host/combine_host.cpp");
    const std::string combineLaunch =
        ReadFile("src/moonep/combine/host/combine_launch.cpp");
    const std::string combineKernel =
        ReadFile("src/moonep/combine/kernels/tilexr_moonep_combine_kernel.cpp");
    const std::string combineCmake = ReadFile("src/moonep/combine/CMakeLists.txt");
    const std::string prefetchLaunch =
        ReadFile("src/moonep/prefetch_weight/host/prefetch_weight_launch.cpp");
    const std::string prefetchHost =
        ReadFile("src/moonep/prefetch_weight/host/prefetch_weight_host.cpp");
    const std::string prefetchKernel = ReadFile(
        "src/moonep/prefetch_weight/kernels/tilexr_moonep_prefetch_weight_kernel.cpp");
    const std::string prefetchCmake = ReadFile("src/moonep/prefetch_weight/CMakeLists.txt");
    const std::string reduceLaunch =
        ReadFile("src/moonep/reduce_grad/host/reduce_grad_launch.cpp");
    const std::string reduceHost =
        ReadFile("src/moonep/reduce_grad/host/reduce_grad_host.cpp");
    const std::string reduceKernel = ReadFile(
        "src/moonep/reduce_grad/kernels/tilexr_moonep_reduce_grad_kernel.cpp");
    const std::string reduceCmake = ReadFile("src/moonep/reduce_grad/CMakeLists.txt");
    const std::string dataAsFlag = ReadFile("src/include/tilexr_data_as_flag.h");
    const std::string udma = ReadFile("src/include/tilexr_udma.h");
    const std::string registration =
        ReadFile("src/moonep/common/moonep_kernel_registration.h");
    const std::string registrationImpl =
        ReadFile("src/moonep/common/moonep_kernel_registration.cpp");
    const std::string stageHost =
        ReadFile("src/moonep/common/moonep_stage_host.h");
    const std::string kernelLaunch =
        ReadFile("src/moonep/common/moonep_kernel_launch.h");
    const std::string kernelBuild = ReadFile("src/moonep/cmake/MoonEpKernel.cmake");

    Excludes("planner host", plannerHost,
        "commArgs->localRankSize != commArgs->rankSize");
    Excludes("planner host", plannerHost,
        "commArgs.localRankSize != commArgs.rankSize");
    Contains("planner kernel", plannerKernel, "TileXR::IPC_DATA_OFFSET");
    Contains("planner Host registered mailbox", plannerHost,
        "RegisteredMailboxReady");
    Contains("planner kernel registered mailbox", plannerKernel,
        "TileXR::UDMAPutNbiOnQp");
    Contains("planner kernel registered mailbox", plannerKernel,
        "TileXR::UDMAQuietStatusOnQp");
    Contains("dispatch common", dispatchCommon, "moonep_peer_window.h");
    Excludes("common stage Host", stageHost,
        "context->hostArgs->localRankSize != context->hostArgs->rankSize");
    Contains("registration implementation", registrationImpl, "rtDevBinaryRegister");
    Contains("registration implementation", registrationImpl, "rtFunctionRegister");
    Contains("common kernel launch", kernelLaunch, "rtKernelLaunchWithFlagV2");
    Contains("common kernel launch", kernelLaunch, "LaunchRegisteredMoonEpKernel");
    Contains("dispatch launch", dispatchLaunch, "#include \"moonep_kernel_launch.h\"");
    Contains("dispatch launch", dispatchLaunch, "LaunchRegisteredMoonEpKernel(");
    Excludes("dispatch launch", dispatchLaunch, "rtKernelLaunchWithFlagV2");
    Contains("dispatch launch", dispatchLaunch, "kDispatchKernelSignature");
    Excludes("dispatch launch", dispatchLaunch, "launch_tilexr_moonep_dispatch_kernel");
    Contains("dispatch kernel", dispatchKernel,
        "extern \"C\" __global__ __aicore__ void tilexr_moonep_dispatch_kernel");
    Excludes("dispatch kernel", dispatchKernel, "launch_tilexr_moonep_dispatch_kernel");
    Excludes("dispatch kernel", dispatchKernel, "<<<");
    Contains("kernel build", kernelBuild, "--cce-aicore-only");
    Contains("dispatch cmake", dispatchCmake, "MoonEpKernel.cmake");
    Excludes("dispatch cmake", dispatchCmake, "libtilexr_moonep_dispatch_kernel.so");
    Contains("dispatch kernel", dispatchKernel, "TileXR::CommArgs");
    Contains("dispatch kernel", dispatchKernel, "TileXR::IPC_DATA_OFFSET");
    Contains("dispatch kernel", dispatchKernel, "SyncCollectives");
    Contains("dispatch kernel", dispatchKernel, "ClearLocalWindow");
    Contains("dispatch kernel", dispatchKernel, "CopyBytesGmToGm");
    Contains("dispatch kernel", dispatchKernel, "hiddenChunkStride");
    Contains("dispatch kernel", dispatchKernel, "routeWeightsSk");
    Contains("dispatch kernel", dispatchKernel, "zeroFillRanges");
    Contains("dispatch kernel", dispatchKernel, "BuildDuplicatePlan");
    Contains("dispatch kernel", dispatchKernel, "prefix != duplicateCount");
    Contains("dispatch kernel", dispatchKernel, "writeIndex >= duplicateCount");
    Contains("dispatch kernel", dispatchKernel, "ExpandDuplicateRows");
    Contains("dispatch kernel", dispatchKernel, "dedupParentsOffset");
    Contains("dispatch kernel", dispatchKernel, "ChunkStep");
    Contains("dispatch kernel", dispatchKernel, "DataCacheCleanAndInvalid");
    Contains("dispatch kernel", dispatchKernel, "static_cast<int64_t>(encoded)");
    Contains("dispatch kernel", dispatchKernel, "-encoded64 - 1");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchWindowClearedStep");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchDataReadyStep");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchWindowDrainedStep");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchFailedStep");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchStatusInvalidRoute");
    Contains("dispatch kernel", dispatchKernel, "kMoonEpDispatchStatusTimeoutBase");
    Contains("dispatch kernel", dispatchKernel, "AscendC::PipeBarrier<PIPE_ALL>()");
    Excludes("dispatch kernel", dispatchKernel, "WaitRankInnerFlag");
    Excludes("dispatch kernel", dispatchKernel, "aclrtSynchronizeStream");
    Excludes("dispatch kernel", Lower(dispatchKernel), "udma");
    CountEquals("URMA dispatch launcher", dispatchUrmaLaunch,
        "TileXRCommNextMagic(", 1U);
    CountEquals("URMA dispatch launcher", dispatchUrmaLaunch,
        "rtKernelLaunchWithFlagV2(", 1U);
    Contains("URMA Dispatch WQE", dispatchUrmaKernel,
        "__ubuf__ TileXR::UDMASqeCtx *sqe");
    Contains("URMA Dispatch WQE", dispatchUrmaKernel,
        "__ubuf__ TileXR::UDMASgeCtx *sge");
    Contains("URMA Dispatch SQ publish", dispatchUrmaKernel,
        "AscendC::DataCopyPad(wqeGlobal, issueLocal, copyParams)");
    Contains("URMA Dispatch SQ publish", dispatchUrmaKernel,
        "SyncFunc<AscendC::HardEvent::MTE3_S>()");
    Contains("URMA Dispatch doorbell", dispatchUrmaKernel,
        "state.qpCtxEntry->dbAddr), 0)");
    Contains("URMA Dispatch completion accounting", dispatchUrmaKernel,
        "batchEndCompletionCount = state.completionCount + 1U");
    Contains("URMA Dispatch completion accounting", dispatchUrmaKernel,
        "state.completionCount += 1U");
    Contains("URMA Dispatch SQ drain frontier", dispatchUrmaKernel,
        "state.finalHead = state.head");
    Contains("URMA Dispatch SQ drain frontier", dispatchUrmaKernel,
        "DispatchDrainSqToExpected(state, state.finalHead");
    Contains("URMA Dispatch grouped QP isolation", dispatchUrmaKernel,
        "constexpr bool kDispatchGroupedAllowFullmesh = false;");
    Contains("URMA Dispatch grouped QP isolation", dispatchUrmaKernel,
        "hasWeight, kDispatchGroupedAllowFullmesh, initContext");
    Excludes("URMA Dispatch completion accounting", dispatchUrmaKernel,
        "state.wqeCount += batchCount");
    Excludes("URMA Dispatch SQ drain frontier", dispatchUrmaKernel,
        "DispatchDrainSqToExpected(state, state.finalWqeCount");
    Contains("URMA Dispatch fused payload", dispatchUrmaKernel,
        "DispatchDataTaskIsWeight(");
    Contains("URMA Dispatch fused payload", dispatchUrmaKernel,
        "DispatchPayloadWqesPerRoute(hasWeight)");
    Contains("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "const bool payloadReady = upstreamStatus ==");
    Contains("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "bool completionSubmitted = optimizedSignalSubmitted");
    Contains("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "WaitDispatchIncomingPeer(receiveFlags");
    Contains("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "PublishDispatchPeerCredit(");
    Contains("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "DispatchCompletionLaneCount(");
    Excludes("URMA Dispatch grouped convergence", dispatchUrmaKernel,
        "peerValue == rank || upstreamStatus !=");
    Excludes("URMA Dispatch WQE", dispatchUrmaKernel, "WRITE_WITH_NOTIFY");
    Excludes("URMA Dispatch WQE", dispatchUrmaKernel, "sgeNum = 2U");
    Excludes("URMA Dispatch WQE", dispatchUrmaKernel,
        "__gm__ TileXR::UDMASqeCtx *");
    Contains("combine common", combineCommon, "moonep_peer_window.h");
    Contains("combine launch", combineLaunch, "#include \"moonep_kernel_launch.h\"");
    Contains("combine launch", combineLaunch, "LaunchRegisteredMoonEpKernel(");
    Excludes("combine launch", combineLaunch, "rtKernelLaunchWithFlagV2");
    Contains("combine launch", combineLaunch, "kCombineKernelSignature");
    Excludes("combine launch", combineLaunch, "launch_tilexr_moonep_combine_kernel");
    Contains("combine kernel", combineKernel,
        "extern \"C\" __global__ __aicore__ void tilexr_moonep_combine_kernel");
    Excludes("combine kernel", combineKernel, "launch_tilexr_moonep_combine_kernel");
    Excludes("combine kernel", combineKernel, "<<<");
    Contains("combine cmake", combineCmake, "MoonEpKernel.cmake");
    Excludes("combine cmake", combineCmake, "libtilexr_moonep_combine_kernel.so");
    Contains("combine kernel", combineKernel, "TileXR::CommArgs");
    Contains("combine kernel", combineKernel, "TileXR::IPC_DATA_OFFSET");
    Contains("combine kernel", combineKernel, "SyncCollectives");
    Contains("combine kernel", combineKernel,
        "sync_.Init(rank_, rankSize_, shareAddrs_, syncBuf_)");
    Contains("combine kernel", combineKernel, "dstLocal");
    Contains("combine kernel", combineKernel, "MoonEpCombineV2Peer");
    Contains("combine kernel", combineKernel, "PushPeerRows");
    Contains("combine kernel", combineKernel, "CopyBytesGmToGm");
    Contains("combine kernel", combineKernel, "hiddenChunkStride");
    Contains("combine kernel", combineKernel, "routeWeightsNvs");
    Contains("combine kernel", combineKernel, "PreReduceDuplicates");
    Contains("combine kernel", combineKernel, "BuildDuplicateMask");
    Contains("combine kernel", combineKernel, "DataCacheCleanAndInvalid");
    Contains("combine kernel", combineKernel, "DecodeReverseRoute");
    Contains("combine kernel", combineKernel, "ReduceHiddenChunk");
    Contains("combine kernel", combineKernel, "AscendC::LocalTensor<bfloat16_t>");
    Contains("combine kernel", combineKernel, "AscendC::LocalTensor<float>");
    Contains("combine kernel", combineKernel, "AscendC::RoundMode::CAST_NONE");
    Contains("combine kernel", combineKernel, "AscendC::Add");
    Contains("combine kernel", combineKernel, "AscendC::RoundMode::CAST_RINT");
    Contains("combine kernel", combineKernel, "CopyReceivedWeights");
    Excludes("combine kernel", combineKernel, "IsPublishOnly");
    Excludes("combine kernel", combineKernel, "IsConsumeOnly");
    Excludes("combine kernel", combineKernel, "RunPublishOnly");
    Excludes("combine kernel", combineKernel, "RunConsumeOnly");
    Excludes("combine kernel", Lower(combineKernel), "udma");
    Contains("combine kernel", combineKernel, "kMoonEpCombineDataReadyStep");
    Contains("combine kernel", combineKernel, "kChunkDrainedStepBase");
    Contains("combine kernel", combineKernel, "kMoonEpCombineFailedStep");
    Contains("combine kernel", combineKernel, "kMoonEpCombineStatusInvalidRoute");
    Contains("combine kernel", combineKernel, "kMoonEpCombineStatusTimeoutBase");
    Contains("combine kernel", combineKernel, "AscendC::PipeBarrier<PIPE_ALL>()");
    Excludes("combine kernel", combineKernel, "WaitRankInnerFlag");
    Excludes("combine kernel", combineKernel, "aclrtSynchronizeStream");
    Contains("prefetch launch", prefetchLaunch, "#include \"moonep_kernel_launch.h\"");
    Contains("prefetch launch", prefetchLaunch, "LaunchRegisteredMoonEpKernel(");
    Excludes("prefetch launch", prefetchLaunch, "rtKernelLaunchWithFlagV2");
    Contains("prefetch launch", prefetchLaunch, "kPrefetchWeightKernelSignature");
    Contains("prefetch launch", prefetchLaunch,
        "context.layout.qpNum, context.layout.physicalQpMap");
    Contains("prefetch launch", prefetchLaunch,
        "18U * sizeof(uint64_t)");
    Contains("prefetch kernel", prefetchKernel,
        "extern \"C\" __global__ __aicore__ void tilexr_moonep_prefetch_weight_kernel");
    Contains("prefetch kernel", prefetchKernel, "expertsToCopy");
    Contains("prefetch kernel", prefetchKernel, "expertsPerRank_ + slot");
    Contains("prefetch kernel", prefetchKernel,
        "rank_) * prefetchSlots_");
    Contains("prefetch kernel", prefetchKernel, "slot < prefetchSlots_");
    Contains("prefetch kernel", prefetchKernel, "localExpert");
    Excludes("prefetch kernel", prefetchKernel, "e_ + slot");
    Contains("prefetch kernel", prefetchKernel, "UDMAGetNbiOnQp");
    Contains("prefetch kernel", prefetchKernel, "PollSqUntil");
    Contains("prefetch kernel", prefetchKernel, "UDMAPollCQUpdateInfo");
    Contains("prefetch kernel", prefetchKernel, "UDMAGetSCQCtx");
    Contains("prefetch kernel", prefetchKernel, "cqCtxEntry->tailAddr");
    Contains("prefetch kernel", prefetchKernel, "physicalQp_");
    Contains("prefetch kernel", prefetchKernel,
        "owner, physicalQp_, destination");
    Contains("prefetch kernel", prefetchKernel,
        "peer, physicalQp_, sqTargets[queue]");
    Contains("prefetch kernel", prefetchKernel, "completionQueueIds");
    Contains("prefetch kernel", prefetchKernel,
        "++sqTargets[completionQueue]");
    Contains("prefetch kernel", prefetchKernel, "DataCacheCleanAndInvalid");
    Excludes("prefetch kernel", prefetchKernel, "<<<");
    Excludes("prefetch launch", prefetchLaunch, "launch_tilexr_moonep_prefetch_weight_kernel");
    Contains("prefetch cmake", prefetchCmake, "MoonEpKernel.cmake");
    Excludes("prefetch cmake", prefetchCmake, "libtilexr_moonep_prefetch_weight_kernel.so");
    Contains("prefetch kernel", Lower(prefetchKernel), "udma");
    Contains("reduce launch", reduceLaunch, "rtDevBinaryRegister");
    Contains("reduce launch", reduceLaunch, "rtFunctionRegister");
    Contains("reduce launch", reduceLaunch, "rtKernelLaunchWithFlagV2");
    Contains("reduce launch", reduceLaunch, "EnsureReduceGradKernelRegistered");
    Excludes("reduce launch", reduceLaunch, "LaunchMoonEpKernel(");
    Contains("reduce kernel", reduceKernel,
        "extern \"C\" __global__ __aicore__ void tilexr_moonep_reduce_grad_kernel");
    Contains("reduce kernel", reduceKernel, "RunLaneLeader");
    Contains("reduce kernel", reduceKernel, "RunLaneHelper");
    Contains("reduce kernel", reduceKernel, "IssueWave");
    Contains("reduce kernel", reduceKernel,
        "__attribute__((always_inline)) inline __aicore__ bool IssueWave");
    Contains("reduce kernel", reduceKernel,
        "__attribute__((always_inline)) inline __aicore__ bool CompleteWave");
    Contains("reduce kernel", reduceKernel,
        "__attribute__((always_inline)) inline __aicore__ void PublishItem");
    Contains("reduce kernel", reduceKernel,
        "__attribute__((always_inline)) inline __aicore__ void LoadItem");
    Excludes("reduce kernel", reduceKernel, "const ReduceGradBankItem &item");
    Excludes("reduce kernel", reduceKernel, "ReduceGradBankItem item");
    Contains("reduce kernel", reduceKernel,
        "entry / static_cast<uint64_t>(prefetchSlots_)");
    Excludes("reduce kernel", reduceKernel, "prefetchSlots_ > expertsPerRank_");
    Contains("reduce kernel", reduceKernel, "CollectWaveContributors");
    Excludes("reduce kernel", reduceKernel, "reduceBuffers_");
    Contains("reduce kernel", reduceKernel, "AscendC::Add");
    Contains("reduce kernel", reduceKernel, "accumPongBuf_");
    Contains("reduce kernel", reduceKernel, "HardEvent::MTE3_MTE2");
    Contains("reduce kernel", reduceKernel, "UDMAProfileGetNbiOnQpDeferred");
    Contains("reduce kernel", reduceKernel, "UDMAProfileFlushQpDoorbell");
    Contains("reduce kernel", reduceKernel, "UDMAProfileQuietStatusOnQpUntil");
    Contains("reduce kernel", reduceKernel, "AscendC::AtomicAdd");
    Contains("reduce kernel", reduceKernel, "bankIssued[kReduceGradBankCount]");
    Contains("reduce kernel", reduceKernel, "bankPublished[kReduceGradBankCount]");
    Contains("reduce kernel", reduceKernel, "nextReadySequence");
    Contains("reduce kernel", reduceKernel, "sequence >= kReduceGradBankCount");
    Contains("reduce kernel", reduceKernel, "ProjectionForLane");
    Contains("reduce kernel", reduceKernel,
        "workOrdinal % projectionQpCounts_[projection]");
    Excludes("reduce kernel", reduceKernel, "StageWave");
    Excludes("reduce kernel", reduceKernel, "PublishLaneReadDone");
    Excludes("reduce kernel", reduceKernel, "PublishLaneWorkDone");
    Excludes("reduce kernel", reduceKernel, "ReleaseSources");
    Contains("reduce kernel", reduceKernel, "AscendC::SyncAll<true>()");
    Contains("reduce kernel", reduceKernel, "SyncCollectives");
    Contains("reduce kernel", reduceKernel, "kReduceGradBarrierStep");
    Contains("reduce kernel", reduceKernel, "kReduceGradBarrierFailureStep");
    Contains("reduce kernel", reduceKernel, "CollectiveConfigurationValid");
    Contains("reduce kernel", reduceKernel,
        "const bool configured = ConfigurationValid()");
    Contains("reduce kernel", reduceKernel, "if (!configured)");
    Contains("reduce kernel", reduceKernel, "sync.GetInnerFlag(peer, 0)");
    Contains("reduce kernel", reduceKernel, "if (DeviceStatus() == 0U)");
    Contains("reduce kernel", reduceKernel, "ClearLocalSources");
    Contains("reduce kernel clear", reduceKernel,
        "tile += static_cast<uint64_t>(blockCount_)");
    Excludes("reduce kernel clear", reduceKernel,
        "rowOrdinal % blockCount_ != blockIdx_");
    Excludes("reduce kernel", reduceKernel, "RunSender");
    Excludes("reduce kernel", reduceKernel, "RunReceiver");
    Excludes("reduce kernel", reduceKernel, "DataAsFlag");
    Excludes("reduce kernel", reduceKernel, "UDMAPut");
    Excludes("reduce kernel", reduceKernel, "Acknowledgement");
    Excludes("reduce kernel", reduceKernel, "peerRecord");
    Excludes("reduce kernel", reduceKernel,
        "tilexr_moonep_reduce_grad_status_kernel");
    Excludes("reduce kernel", reduceKernel, "kReduceGradDeviceStatusSuccess");
    Contains("reduce Host", reduceHost, "TileXRMoonEpReduceGradV2");
    Excludes("reduce Host", reduceHost,
        "plan->r > kReduceGradMaxAivBlockCount");
    Excludes("reduce Host", reduceHost,
        "commArgs.rankSize <= kReduceGradMaxAivBlockCount");
    Contains("DataAsFlag helper", dataAsFlag, "DataAsFlagCheckBatchCleared");
    Contains("UDMA helper", udma, "UDMAPutRegisteredSignalNbiOnQp");
    Contains("UDMA helper", udma, "UDMAWriteNotify");
    Contains("UDMA doorbell", udma, "st_dev");
    Excludes("reduce kernel", reduceKernel, "<<<");
    Excludes("reduce launch", reduceLaunch, "launch_tilexr_moonep_reduce_grad_kernel");
    Contains("reduce cmake", reduceCmake, "MoonEpKernel.cmake");
    Contains("reduce cmake", reduceCmake, "${ARCH}-linux/asc/include");
    Excludes("reduce cmake", reduceCmake, "libtilexr_moonep_reduce_grad_kernel.so");
    Contains("registration", registration, "kPlannerKernelSignature");
    Contains("registration", registration, "kDispatchKernelSignature");
    Contains("registration", registration, "kCombineKernelSignature");
    Contains("registration", registration, "kPrefetchWeightKernelSignature");
    Contains("registration", registration, "kReduceGradKernelSignature");
    Excludes("registration", registration, "kReduceGradStatusKernelSignature");

    const std::string ipcStages = Lower(dispatchCommon + dispatchHost +
        dispatchLaunch + dispatchKernel + combineCommon + combineHost +
        stageHost + kernelLaunch);
    Excludes("IPC-only MoonEP stages", ipcStages, "udma");
    const std::string active = Lower(plannerHost + plannerKernel + ipcStages +
        prefetchHost + prefetchLaunch + prefetchKernel + reduceHost +
        reduceLaunch + reduceKernel);
    Excludes("active MoonEP sources", active, "3rdparty/moonep");
    Excludes("active MoonEP sources", active, "reference/");
    Excludes("active MoonEP sources", active, "src/ep");
    Excludes("active MoonEP sources", active, "hccl");
    Excludes("active MoonEP sources", active, "shmem");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
