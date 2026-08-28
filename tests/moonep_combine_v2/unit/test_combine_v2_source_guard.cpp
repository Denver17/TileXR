#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef TILEXR_SOURCE_ROOT
#error TILEXR_SOURCE_ROOT must be defined
#endif

namespace {

std::string Read(const std::string &path)
{
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool Require(const std::string &text, const std::string &needle,
    const char *message)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool Reject(const std::string &text, const std::string &needle,
    const char *message)
{
    if (text.find(needle) != std::string::npos) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool RequireBefore(const std::string &text, const std::string &first,
    const std::string &second, const char *message)
{
    const std::size_t firstPos = text.find(first);
    const std::size_t secondPos = text.find(second);
    if (firstPos == std::string::npos || secondPos == std::string::npos ||
        firstPos >= secondPos) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::size_t Count(const std::string &text, const std::string &needle)
{
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string Section(const std::string &text, const std::string &begin,
    const std::string &end)
{
    const std::size_t beginPos = text.find(begin);
    if (beginPos == std::string::npos) {
        return std::string();
    }
    const std::size_t endPos = text.find(end, beginPos + begin.size());
    return text.substr(beginPos, endPos == std::string::npos ?
        std::string::npos : endPos - beginPos);
}

} // namespace

int main()
{
    const std::string root = TILEXR_SOURCE_ROOT;
    const std::string rootCmake = Read(root + "/CMakeLists.txt");
    const std::string moonEpCmake = Read(root + "/src/moonep/CMakeLists.txt");
    const std::string v1Cmake = Read(root + "/src/moonep/combine/CMakeLists.txt");
    const std::string v2Cmake = Read(root +
        "/src/moonep/combine_v2/CMakeLists.txt");
    const std::string kernelEntry = Read(root +
        "/src/moonep/combine_v2/kernels/"
        "tilexr_moonep_combine_v2_kernel.cpp");
    const std::string legacyKernel = Read(root +
        "/src/moonep/combine_v2/kernels/"
        "tilexr_moonep_combine_v2_kernel.h");
    const std::string host = Read(root +
        "/src/moonep/combine_v2/host/combine_v2_host.cpp");
    const std::string hostHeader = Read(root +
        "/src/moonep/combine_v2/host/combine_v2_host.h");
    const std::string launch = Read(root +
        "/src/moonep/combine_v2/host/combine_v2_launch.cpp");
    const std::string publicHeader = Read(root +
        "/src/include/tilexr_moonep_combine_v2.h");
    const std::string registration = Read(root +
        "/src/moonep/common/moonep_kernel_registration.h");
    const std::string schedule = Read(root +
        "/src/moonep/common/moonep_combine_schedule.h");
    const std::string profile = Read(root +
        "/src/moonep/combine_v2/common/combine_v2_profile.h");

    const std::string legacyProcess = Section(legacyKernel,
        "__aicore__ inline void MoonEpCombineV2::Process()",
        "} // namespace");
    const std::string legacySubmitPair = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::SubmitPair(",
        "__aicore__ inline bool MoonEpCombineV2::SendRemoteStep(");
    const std::string legacySubmitFullmesh = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::SubmitFullmeshBatch(",
        "__aicore__ inline bool MoonEpCombineV2::SendFullmeshStep(");
    const std::string legacyFullmeshBuilder = Section(legacyKernel,
        "inline void MoonEpCombineV2BuildFullmeshPayloadWqesVf(",
        "#endif");
    const std::string legacyPublishCredit = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::PublishNextCredit(",
        "__aicore__ inline bool MoonEpCombineV2::BuildPayloadWqes(");
    const std::string legacyWaitCredit = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::WaitStepCredit(",
        "__aicore__ inline bool MoonEpCombineV2::PublishNextCredit(");
    const std::string legacyWeightSend = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::SendWeightMemoryStep(",
        "__aicore__ inline bool MoonEpCombineV2::WaitInboundWeightDone(");
    const std::string legacyInboundDone = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::WaitInboundDone(",
        "__aicore__ inline bool MoonEpCombineV2::BeginCollectiveStage(");
    const std::string legacyWeightOutput = Section(legacyKernel,
        "__aicore__ inline bool MoonEpCombineV2::CopyReceivedWeights()",
        "__aicore__ inline bool MoonEpCombineV2::WaitInboundDone(");
    const std::string activeProtocolSource = legacyKernel + schedule;

    bool ok = true;
    ok &= Require(rootCmake, "add_subdirectory(tests/moonep_combine_v2)",
        "Combine V2 tests are not wired at the root");
    ok &= Require(moonEpCmake, "add_subdirectory(combine_v2)",
        "Combine V2 source directory is not configured");
    ok &= Reject(v1Cmake, "combine_v2",
        "Combine V1 build definition contains V2 changes");
    ok &= Require(v2Cmake, "add_library(tilexr-moonep-combine-v2 SHARED",
        "Combine V2 standalone library is missing");
    ok &= Require(v2Cmake, "tilexr_moonep_combine_v2_kernel.h\"",
        "Combine V2 build does not depend on the legacy kernel header");
    ok &= Reject(v2Cmake, "tilexr_moonep_combine_v2_group_kernel.h",
        "Combine V2 build still depends on the retired Group kernel header");
    ok &= Require(v2Cmake, "-DCATLASS_ARCH=3510",
        "Combine V2 CATLASS target is missing");
    ok &= Reject(v2Cmake, "/devlib",
        "Combine V2 links the toolkit stub library directory");

    ok &= Require(kernelEntry,
        "#include \"tilexr_moonep_combine_v2_kernel.h\"",
        "Combine V2 entry does not include the legacy implementation");
    ok &= Require(kernelEntry, "MoonEpCombineV2 op;",
        "Combine V2 entry does not use the unified implementation");
    ok &= Reject(kernelEntry, "MoonEpCombineV2Group",
        "Combine V2 entry still references the retired Group implementation");
    ok &= Reject(kernelEntry, "<<<",
        "Combine V2 entry contains Host launch syntax");

    ok &= Require(legacyKernel, "class MoonEpCombineV2",
        "Legacy Combine V2 implementation is missing");
    ok &= Require(legacyFullmeshBuilder, "sqe->flag = 0U;",
        "Legacy fullmesh builder retains an ordered-completion flag from a reused WQE");
    ok &= Require(legacyKernel,
        "__aicore__ inline void MoonEpCombineV2::PublishControlToken(",
        "Legacy Combine V2 must publish UDMA control sources through MTE3");
    ok &= Require(legacySubmitPair, "PublishControlToken(doneSource,",
        "Legacy pair completion token bypasses the MTE3 publication helper");
    ok &= Reject(legacySubmitPair, "\n            *doneSource =",
        "Legacy pair completion token uses a scalar GM store");
    ok &= Require(legacySubmitFullmesh, "PublishControlToken(doneSource,",
        "Legacy fullmesh completion token bypasses the MTE3 publication helper");
    ok &= Reject(legacySubmitFullmesh, "\n        *doneSource =",
        "Legacy fullmesh completion token uses a scalar GM store");
    ok &= Require(schedule, "MoonEpCombineV2GroupSendDstRank(",
        "Group send mapping helper is missing");
    ok &= Require(schedule, "MoonEpCombineV2GroupRecvSrcRank(",
        "Group receive mapping helper is missing");
    ok &= Require(schedule, "struct alignas(64) MoonEpCombineV2CreditSignal",
        "Combine V2 Credit signal is missing");
    ok &= Require(schedule, "kMoonEpCombineV2CreditBaseBytes",
        "Combine V2 Credit IPC base is missing");
    ok &= Require(schedule, "MoonEpCombineV2CreditReceiveOffset(",
        "Combine V2 Credit IPC offset helper is missing");
    ok &= Require(schedule, "MoonEpCombineV2ReceiveCore(",
        "Combine V2 Credit receiver-core inverse is missing");
    ok &= Require(schedule, "struct MoonEpCombineV2StepRoute",
        "Combine V2 step route contract is missing");
    ok &= Require(schedule, "MoonEpCombineV2MapSendDst(",
        "Combine V2 pluggable send mapping is missing");
    ok &= Require(schedule, "MoonEpCombineV2MapRecvSrc(",
        "Combine V2 pluggable receive mapping is missing");
    ok &= Require(schedule, "MoonEpCombineV2FindRecvCore(",
        "Combine V2 receive-core inverse is missing");
    ok &= Require(schedule, "MoonEpCombineV2FindSendCore(",
        "Combine V2 send-core inverse is missing");
    ok &= Require(schedule, "MoonEpCombineV2FindSendSuccessor(",
        "Combine V2 send-successor inverse is missing");
    ok &= Require(schedule, "MoonEpCombineV2BuildStepRoute(",
        "Combine V2 step route builder is missing");
    ok &= Require(schedule, "MOONEP_COMBINE_V2_GROUP = 4U",
        "Combine V2 Group schedule mode is missing");
    ok &= Require(schedule, "MoonEpCombineV2ScheduleModeForRankSize(",
        "Combine V2 rank-size schedule selection is missing");
    ok &= Require(legacyKernel,
        "scheduleMode_ = TileXRMoonEp::MoonEpCombineV2ScheduleModeForRankSize(",
        "Unified kernel does not select Group only by rank size");
    ok &= Require(legacyProcess, "scheduleMode_, rank_, core_, step, rankSize_",
        "Unified kernel does not build routes with the selected mode");
    ok &= Reject(legacyKernel, "kCombineV2ScheduleMode",
        "Unified kernel retains a fixed schedule mode");

    ok &= Require(legacyPublishCredit, "args_->creditMems[targetRank]",
        "Legacy Credit publish does not use the target Credit IPC mapping");
    ok &= Require(legacyPublishCredit,
        "DataCopy(remoteCreditGlobal, creditLocal",
        "Legacy Credit publish is not an MTE3 GM write");
    ok &= Require(legacyPublishCredit, "SyncFunc<HardEvent::S_MTE3>();",
        "Legacy Credit publish lacks scalar-to-MTE3 ordering");
    ok &= Require(legacyPublishCredit, "SyncFunc<HardEvent::MTE3_S>();",
        "Legacy Credit publish does not wait for MTE3 completion");
    ok &= Require(legacyWaitCredit, "args_->creditMems[rank_]",
        "Legacy Credit wait does not use the local Credit IPC mapping");
    ok &= Require(legacyWaitCredit, "DataCopy(creditLocal, creditGlobal",
        "Legacy Credit wait is not an MTE2 GM read");
    ok &= Require(legacyWaitCredit, "SyncFunc<HardEvent::S_MTE2>();",
        "Legacy Credit wait lacks scalar-to-MTE2 ordering");
    ok &= Require(legacyWaitCredit, "SyncFunc<HardEvent::MTE2_S>();",
        "Legacy Credit wait does not wait for MTE2 completion");
    ok &= Require(legacyWaitCredit, "MOONEP_COMBINE_V2_CREDIT_TIMEOUT",
        "Legacy Credit wait is not bounded");

    const std::string creditSections =
        legacyPublishCredit + legacyWaitCredit;
    const char *forbiddenCreditTransport[] = {
        "AppendControlWqe(",
        "SubmitClosControl(",
        "CopyIssueToSq(",
        "PollCqOnce(",
        "WaitLaneCq(",
        "st_dev(",
        "dbAddr",
        "cqTarget",
        "completionCount"
    };
    for (const char *symbol : forbiddenCreditTransport) {
        if (!Reject(creditSections, symbol,
                "Credit path retained a WQE, doorbell, or CQ dependency")) {
            std::cerr << "forbidden Credit transport symbol: " << symbol
                      << '\n';
            ok = false;
        }
    }

    const char *retiredProtocol[] = {
        "kEnableFullSync",
        "MoonEpCombineV2FullSyncBuildContext",
        "MoonEpCombineV2BuildFullSyncWqesVf",
        "RunGlobalBarrier(",
        "WaitStepGrant(",
        "PublishLocalGrant(",
        "SubmitSelfGrant(",
        "ServerGrant",
        "RunServerGrantAdmission(",
        "MoonEpCombineV2PhaseBarrierAfterRound(",
        "fullSyncReceiveBase_",
        "fullSyncSourceBase_",
        "fullSyncReceiveOffset_",
        "fullSyncStartCycles_"
    };
    for (const char *symbol : retiredProtocol) {
        if (!Reject(activeProtocolSource, symbol,
                "Combine V2 retained a retired protocol symbol")) {
            std::cerr << "retired symbol: " << symbol << '\n';
            ok = false;
        }
    }
    ok &= Require(legacyProcess, "MoonEpCombineV2BuildStepRoute(",
        "Legacy process does not build one step route");
    ok &= Count(legacyProcess, "MoonEpCombineV2BuildStepRoute(") == 1U;
    if (Count(legacyProcess, "MoonEpCombineV2BuildStepRoute(") != 1U) {
        std::cerr << "Legacy process does not build exactly one route per step\n";
    }
    ok &= Require(legacyProcess, "const uint32_t peer = route.sendDstRank;",
        "Legacy process does not send to the route destination");
    ok &= RequireBefore(legacyProcess, "WaitStepCredit(step, route)",
        "const uint32_t peer = route.sendDstRank;",
        "Legacy process sends before pre-step Credit");
    ok &= RequireBefore(legacyProcess,
        "const uint32_t peer = route.sendDstRank;", "WaitStepCqs(step)",
        "Legacy process waits for CQ before entering the send block");
    ok &= RequireBefore(legacyProcess, "WaitStepCqs(step)",
        "WaitInboundDone(step, route.recvSrcRank)",
        "Legacy process waits for inbound Done before outbound CQ");
    ok &= RequireBefore(legacyProcess,
        "WaitInboundDone(step, route.recvSrcRank)",
        "PublishNextCredit(step, route)",
        "Legacy process publishes Credit before inbound Done");
    ok &= Reject(legacyWaitCredit, "MoonEpCombineV2Peer(",
        "Legacy Credit wait recalculates the send mapping");
    ok &= Reject(legacyWaitCredit, "MoonEpCombineV2ReceiveCore(",
        "Legacy Credit wait recalculates the receiver core");
    ok &= Require(legacyWaitCredit, "route.creditWaitSrcRank",
        "Legacy Credit wait does not use the step route rank");
    ok &= Require(legacyWaitCredit, "route.creditWaitSrcCore",
        "Legacy Credit wait does not use the step route core");
    ok &= Reject(legacyPublishCredit, "MoonEpCombineV2ReceiveSource(",
        "Legacy Credit publish recalculates the next sender rank");
    ok &= Reject(legacyPublishCredit, "MoonEpCombineV2TransferCore(",
        "Legacy Credit publish recalculates the next sender core");
    ok &= Require(legacyPublishCredit, "route.creditPublishDstRank",
        "Legacy Credit publish does not use the step route rank");
    ok &= Require(legacyPublishCredit, "route.creditPublishDstCore",
        "Legacy Credit publish does not use the step route core");
    ok &= Reject(legacyInboundDone, "MoonEpCombineV2ReceiveSource(",
        "Legacy inbound Done wait recalculates its source");
    ok &= Reject(legacyProcess, "MoonEpCombineV2Successor(",
        "Legacy process bypasses the route for the profile successor");
    ok &= Require(legacyProcess, "route.sendSuccessorRank",
        "Legacy process does not use the route profile successor");
    ok &= RequireBefore(legacyProcess, "SendWeightMemoryStep(peer, step)",
        "WaitStepCqs(step)",
        "Legacy process does not send weight before CQ waiting");
    ok &= Require(legacyInboundDone,
        "return WaitInboundWeightDone(step, source);",
        "Legacy inbound completion does not include weight Done");
    ok &= Require(legacyKernel, "selfRelayPingBuf_",
        "Legacy kernel does not expose a fixed Self relay ping buffer");
    ok &= Require(legacyKernel, "selfRelayPongBuf_",
        "Legacy kernel does not expose a fixed Self relay pong buffer");
    ok &= Reject(legacyKernel, "selfCopyQueue_",
        "Legacy kernel still models Self relay ownership as a queue");
    ok &= Require(legacyWeightSend, "SelectPeerDestinations(",
        "Legacy weight send does not gather dense destinations");
    ok &= Require(legacyWeightSend, "GatherMask(packedWeights, rawWeights",
        "Legacy weight send does not gather FP32 weights");
    ok &= Require(legacyWeightSend,
        "AllocEventID<HardEvent::MTE3_S>()",
        "Legacy weight send does not allocate independent stage events");
    ok &= Require(legacyWeightSend,
        "ReleaseEventID<HardEvent::MTE3_S>(stage0Event)",
        "Legacy weight send leaks its stage events");
    ok &= Require(legacyWeightSend,
        "Simt::VF_CALL<MoonEpCombineV2BuildWeightRecordsVf>",
        "Legacy weight send does not batch-build records with SIMT");
    ok &= Require(legacyWeightSend, "SyncFunc<HardEvent::V_MTE3>()",
        "Legacy record builder does not establish a V-to-MTE3 dependency");
    ok &= Reject(legacyWeightSend, "MoonEpCombineV2WeightBuildContext",
        "Legacy record builder still consumes shared UB context asynchronously");
    ok &= Require(profile, "kMoonEpCombineV2ProfileVersion = 7U",
        "Combine V2 profile version does not cover batched weight metrics");
    ok &= Require(profile,
        "MOONEP_COMBINE_V2_METRIC_WEIGHT_DESTINATION_GATHER",
        "Combine V2 profile dropped weight destination timing");
    ok &= Require(profile,
        "MOONEP_COMBINE_V2_WEIGHT_COUNTER_MTE3_COMMANDS",
        "Combine V2 profile dropped per-step weight command counts");
    ok &= Require(legacyWeightSend, "AddWeightStepCounter(step",
        "Legacy weight send does not publish per-step counters");
    ok &= Reject(legacyWeightSend, "PipeBarrier<PIPE_ALL>()",
        "Legacy weight send serializes the alternate MTE3 stage");
    ok &= RequireBefore(legacyWeightSend,
        "DataCopyPad(destination,", "DataCopyPad(done, doneScratch",
        "Legacy weight Done can be published before weight records");
    ok &= Reject(legacyWeightSend, "SelectPeerIndices(peer, chunkElements)",
        "Legacy weight send still gathers source indices");
    ok &= Reject(legacyWeightSend, "words.SetValue(",
        "Legacy weight send still constructs each record with Scalar stores");
    ok &= Count(legacyWeightSend,
        "SyncFunc<HardEvent::MTE3_S>()") == 1U;
    if (Count(legacyWeightSend, "SyncFunc<HardEvent::MTE3_S>()") != 1U) {
        std::cerr << "Legacy weight send hard-waits MTE3 outside final Done\n";
    }
    ok &= Require(legacyWeightOutput, "0x11111111U",
        "Legacy weight output does not gather record values");
    ok &= Require(legacyWeightOutput, "0x44444444U",
        "Legacy weight output does not gather magic low words");
    ok &= Require(legacyWeightOutput, "0x88888888U",
        "Legacy weight output does not gather magic high words");
    ok &= Require(legacyWeightOutput, "magic_ >> 32U",
        "Legacy weight output does not validate full magic");
    ok &= Require(legacyWeightOutput, "Select(filteredValues, validMask",
        "Legacy stale weight output is not zeroed in bulk");
    ok &= Reject(legacyWeightOutput, ".GetValue(",
        "Legacy weight output still reads records element by element");
    ok &= Reject(legacyWeightOutput, ".SetValue(",
        "Legacy weight output still writes records element by element");
    if (Count(legacyKernel, "weightRecordEpochStride_ +") < 2U ||
        Count(legacyKernel, "weightDoneEpochStride_ +") < 2U) {
        std::cerr << "Legacy weight record/Done accesses are not epoch isolated\n";
        ok = false;
    }
    ok &= Reject(legacyWeightSend, "st_dev(",
        "Legacy weight Memory path uses a device doorbell");

    ok &= Require(host, "UDMA_SHARED_QP",
        "Combine V2 Host does not validate shared-QP capability");
    ok &= Require(host, "UDMA_FULLMESH",
        "Combine V2 Host does not validate Fullmesh capability");
    ok &= Require(launch, "LaunchRegisteredMoonEpKernel",
        "Combine V2 does not use registered Runtime launch");
    ok &= Require(launch,
        "static_assert(sizeof(CombineV2KernelArgs) == 29U * sizeof(uint64_t)",
        "Combine V2 launch ABI no longer has 29 64-bit slots");
    ok &= Count(launch, "        0U,") >= 3U;
    if (Count(launch, "        0U,") < 3U) {
        std::cerr << "Combine V2 launch does not zero all retired ABI slots\n";
    }
    ok &= Require(launch, "context.layout.collectiveStatusOffset",
        "Combine V2 launch dropped the card-local collective status offset");
    ok &= Require(host, "commArgs.creditMems[peer] == nullptr",
        "Combine V2 Host does not fail fast on a missing Credit mapping");
    ok &= Reject(hostHeader, "bool fullSync",
        "Combine V2 launch context exposes a retired full-sync switch");
    ok &= Require(registration, "kCombineV2KernelSignature",
        "Combine V2 stable registration signature is missing");
    ok &= Require(publicHeader, "TileXRMoonEpCombineGetWorkspaceSizeV2",
        "Combine V2 workspace API is missing");
    ok &= Require(publicHeader, "TileXRMoonEpCombineV2",
        "Combine V2 launch API is missing");
    ok &= Require(publicHeader, "TileXRMoonEpCombineStageV2Fused",
        "Combine V2 fused Stage API is missing");

    return ok ? 0 : 1;
}
