#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

std::string RepoPath(const std::string& path)
{
#ifdef TILEXR_SOURCE_ROOT
    return std::string(TILEXR_SOURCE_ROOT) + "/" + path;
#else
    return path;
#endif
}

std::string ReadFile(const std::string& path)
{
    std::ifstream input(RepoPath(path).c_str());
    if (!input.is_open()) {
        std::cerr << "failed to open " << RepoPath(path) << std::endl;
        ++g_failures;
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void CheckContains(const std::string& path, const std::string& text, const std::string& needle)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << "expected text not found in " << path << ": " << needle << std::endl;
        ++g_failures;
    }
}

void CheckNotContains(const std::string& path, const std::string& text, const std::string& needle)
{
    const auto pos = text.find(needle);
    if (pos != std::string::npos) {
        std::cerr << "unexpected text in " << path << ": " << needle
                  << " at byte " << pos << std::endl;
        ++g_failures;
    }
}

void CheckNoNeedles(const std::string& path, const std::vector<std::string>& needles)
{
    const auto text = ReadFile(path);
    for (const auto& needle : needles) {
        CheckNotContains(path, text, needle);
    }
}

size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void TestTileXRCommUsesUDMAContextBoundary()
{
    const std::string headerPath = "src/comm/tilexr_comm.h";
    const auto header = ReadFile(headerPath);

    CheckContains(headerPath, header, "class TileXRUDMAContext;");
    CheckContains(headerPath, header, "std::unique_ptr<TileXRUDMAContext> udmaContext_");

    const std::vector<std::string> forbiddenHeaderText = {
        "class TileXRUDMATransport;",
        "GM_ADDR udmaInfoDev_",
        "GM_ADDR udmaRegistryDev_",
        "GM_ADDR udmaRegisteredPtr_",
        "TileXRUDMARegistry udmaRegistry_",
        "std::unique_ptr<TileXRUDMATransport>",
    };
    for (const auto& needle : forbiddenHeaderText) {
        CheckNotContains(headerPath, header, needle);
    }
}

void TestUDMATransportStaysBehindContext()
{
    CheckNoNeedles("src/comm/tilexr_comm.cpp", {
        "#include \"udma/tilexr_udma_transport.h\"",
        "new (nothrow) TileXRUDMATransport",
        "udmaTransport_->",
        "udmaTransport_.",
    });

    CheckContains("src/comm/udma/tilexr_udma_context.h",
                  ReadFile("src/comm/udma/tilexr_udma_context.h"),
                  "class TileXRUDMAContext");
    CheckContains("src/comm/udma/tilexr_udma_context.cpp",
                  ReadFile("src/comm/udma/tilexr_udma_context.cpp"),
                  "#include \"udma/tilexr_udma_transport.h\"");
}

void TestUDMAContextShutdownIsLocalOnly()
{
    const std::string path = "src/comm/udma/tilexr_udma_context.cpp";
    const auto text = ReadFile(path);
    const auto shutdownPos = text.find("void TileXRUDMAContext::Shutdown()");
    const auto registerPos = text.find("int TileXRUDMAContext::RegisterMemory", shutdownPos);
    if (shutdownPos == std::string::npos || registerPos == std::string::npos) {
        std::cerr << "failed to locate TileXRUDMAContext::Shutdown body" << std::endl;
        ++g_failures;
        return;
    }

    const auto shutdownBody = text.substr(shutdownPos, registerPos - shutdownPos);
    CheckNotContains(path, shutdownBody, "UnregisterMemory(");
    CheckContains(path, shutdownBody, "transport_->Shutdown();");
}

void TestUDMAReviewFeedbackGuards()
{
    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    CheckContains(contextPath, context,
                  "TILEXR_LOG(ERROR) << \"TileXRUDMARegister called while UDMA is unavailable\"");
    CheckContains(contextPath, context,
                  "TILEXR_LOG(ERROR) << \"TileXRUDMARegister is not supported in InitThread mode\"");
    CheckContains(contextPath, context, "transport_->PrepareMemory(localPtr, bytes)");
    CheckContains(contextPath, context, "transport_->CommitPreparedMemory()");
    CheckContains(contextPath, context, "EnterCleanupPending(");
    CheckContains(contextPath, context,
                  "TileXRUDMARegister requires pending UDMA memory cleanup first");
    CheckContains(contextPath, context,
                  "TILEXR_LOG(ERROR) << \"TileXRUDMAUnregister failed to clear comm args: \"");
    CheckContains(contextPath, context,
                  "TILEXR_LOG(ERROR) << \"TileXR UDMA memory unregistration failed: \"");
    CheckContains(contextPath, context,
                  "TileXR UDMA failed to hide registry in cleanup-pending state:");
    CheckContains(contextPath, context, "TILEXR_LOG(ERROR) << \"Free UDMA registry failed: \"");

    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const auto comm = ReadFile(commPath);
    CheckContains(commPath, comm, "TILEXR_LOG(ERROR) << \"TileXR UDMA update comm args failed: \"");
    CheckContains(commPath, comm,
                  "TILEXR_LOG(ERROR) << \"ApplyUDMACommArgsStateCallback missing user data\"");
    CheckContains(commPath, comm,
                  "TILEXR_LOG(ERROR) << \"TileXRUDMARegister called while UDMA is unavailable\"");
    CheckContains(commPath, comm, "int TileXRComm::GetUDMAQpCount(uint32_t *qpCount) const");

    const std::string apiPath = "src/include/tilexr_api.h";
    const auto api = ReadFile(apiPath);
    CheckContains(apiPath, api, "int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount);");

    const std::string wrapPath = "src/comm/comm_wrap.cpp";
    const auto wrap = ReadFile(wrapPath);
    CheckContains(wrapPath, wrap, "int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount)");
    CheckContains(wrapPath, wrap, "*qpCount = 0;");
}

void TestUDMAMemoryCleanupIsRetryable()
{
    const std::string headerPath = "src/comm/udma/tilexr_udma_transport.h";
    const auto header = ReadFile(headerPath);
    CheckContains(headerPath, header, "int CleanupLocalRegistrations(");
    CheckContains(headerPath, header, "int CleanupRetiredMemory();");
    CheckContains(headerPath, header, "int CleanupAllMemory();");
    CheckContains(headerPath, header, "struct RegistrationState;");

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    CheckContains(transportPath, transport,
                  "const int localRet = CleanupLocalRegistrations(region.localRegistrations);");
    CheckContains(transportPath, transport, "const int cleanupRet = CleanupAllMemory();");
    CheckContains(transportPath, transport, "GM_ADDR TileXRUDMATransport::GetRegisteredMemoryPtr() const");
    CheckContains(transportPath, transport, "RaCtxRmemUnimport failed for region");
    CheckContains(transportPath, transport, "RaCtxLmemUnregister failed for eid");
    CheckContains(transportPath, transport,
                  "sq[index].localTokenId = registrationIt->second.tokenId;");
    CheckContains(transportPath, transport, "it = byEid.erase(it);");
    CheckContains(transportPath, transport, "it = handles.erase(it);");

    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    CheckContains(contextPath, context, "lifecycle_ = Lifecycle::CleanupPending;");
    CheckContains(contextPath, context,
                  "TileXR UDMA failed to hide registry in cleanup-pending state:");
}

void TestUDMARegistrationIsTransactional()
{
    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    const auto registerPos = context.find("int TileXRUDMAContext::RegisterMemory");
    const auto unregisterPos = context.find("int TileXRUDMAContext::UnregisterMemory", registerPos);
    if (registerPos == std::string::npos || unregisterPos == std::string::npos) {
        std::cerr << "failed to locate UDMA registration transaction" << std::endl;
        ++g_failures;
        return;
    }
    const auto body = context.substr(registerPos, unregisterPos - registerPos);
    const auto preparePos = body.find("transport_->PrepareMemory(localPtr, bytes)");
    const auto candidateInfoPos = body.find("transport_->GetPreparedUDMAInfoDev()");
    const auto publishPos = body.find("ApplyCommArgsState(nextState)");
    const auto commitPos = body.find("transport_->CommitPreparedMemory()");
    if (preparePos == std::string::npos || candidateInfoPos == std::string::npos ||
        publishPos == std::string::npos || commitPos == std::string::npos ||
        !(preparePos < candidateInfoPos && candidateInfoPos < publishPos && publishPos < commitPos)) {
        std::cerr << "UDMA registration must prepare candidate image, publish it, then commit" << std::endl;
        ++g_failures;
    }
    CheckContains(contextPath, body, "const TileXRUDMACommArgsState previousState = GetCommArgsState();");
    CheckContains(contextPath, body, "ApplyCommArgsState(previousState)");
    CheckContains(contextPath, body, "AgreeStatus(localRestoreStatus)");

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    CheckContains(transportPath, transport,
                  "reinterpret_cast<void**>(&registration.infoDev), registration.infoSize");
    CheckContains(transportPath, transport, "udmaInfoDev_ = activeRegistration_->infoDev;");
}

void TestUDMAUnregisterIsLocalAfterPublication()
{
    const std::string path = "src/comm/udma/tilexr_udma_context.cpp";
    const auto text = ReadFile(path);
    const auto unregisterPos = text.find("int TileXRUDMAContext::UnregisterMemory");
    const auto profileRegisterPos = text.find("int TileXRUDMAContext::RegisterProfile", unregisterPos);
    if (unregisterPos == std::string::npos || profileRegisterPos == std::string::npos) {
        std::cerr << "failed to locate UDMA unregister body" << std::endl;
        ++g_failures;
        return;
    }
    const auto body = text.substr(unregisterPos, profileRegisterPos - unregisterPos);
    CheckContains(path, body, "transport_->GetBaseUDMAInfoDev()");
    CheckContains(path, body, "transport_->CleanupAllMemory()");
    CheckNotContains(path, body, "AllGather(");
    CheckNotContains(path, body, "AgreeStatus(");
    CheckNotContains(path, body, "RefreshUDMAInfo(");
}

void TestUDMARegistrationGenerationSurvivesUnregister()
{
    const std::string path = "src/comm/udma/tilexr_udma_context.cpp";
    const auto text = ReadFile(path);
    const auto unregisterPos = text.find(
        "int TileXRUDMAContext::UnregisterMemory");
    const auto profileRegisterPos = text.find(
        "int TileXRUDMAContext::RegisterProfile", unregisterPos);
    const auto shutdownPos = text.find("void TileXRUDMAContext::Shutdown()");
    const auto registerPos = text.find(
        "int TileXRUDMAContext::RegisterMemory", shutdownPos);
    if (unregisterPos == std::string::npos ||
        profileRegisterPos == std::string::npos ||
        shutdownPos == std::string::npos || registerPos == std::string::npos) {
        std::cerr << "failed to locate UDMA generation lifecycle" << std::endl;
        ++g_failures;
        return;
    }
    const auto unregisterBody = text.substr(
        unregisterPos, profileRegisterPos - unregisterPos);
    CheckNotContains(path, unregisterBody, "registrationGeneration_ = 0U;");
    const auto shutdownBody = text.substr(
        shutdownPos, registerPos - shutdownPos);
    CheckContains(path, shutdownBody, "registrationGeneration_ = 0U;");
}

void TestUDMAFailedDeviceFreeRetainsPointer()
{
    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    const auto freePos = transport.find("int TileXRUDMATransport::FreeDeviceInfo");
    const auto cleanupPos = transport.find("int TileXRUDMATransport::CleanupRegistration", freePos);
    if (freePos == std::string::npos || cleanupPos == std::string::npos) {
        std::cerr << "failed to locate UDMA device-info free helper" << std::endl;
        ++g_failures;
    } else {
        const auto body = transport.substr(freePos, cleanupPos - freePos);
        const auto failurePos = body.find("if (ret != ACL_SUCCESS)");
        const auto clearPos = body.find("infoDev = nullptr;");
        if (failurePos == std::string::npos || clearPos == std::string::npos || failurePos > clearPos) {
            std::cerr << "UDMA info pointer must only clear after successful aclrtFree" << std::endl;
            ++g_failures;
        }
    }

    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    const auto registryFreePos = context.find("int TileXRUDMAContext::FreeDeviceRegistry");
    const auto retiredPos = context.find("int TileXRUDMAContext::CleanupRetiredRegistries", registryFreePos);
    if (registryFreePos == std::string::npos || retiredPos == std::string::npos) {
        std::cerr << "failed to locate UDMA registry free helper" << std::endl;
        ++g_failures;
    } else {
        const auto body = context.substr(registryFreePos, retiredPos - registryFreePos);
        const auto failurePos = body.find("if (ret != ACL_SUCCESS)");
        const auto clearPos = body.find("registryDev = nullptr;");
        if (failurePos == std::string::npos || clearPos == std::string::npos || failurePos > clearPos) {
            std::cerr << "UDMA registry pointer must only clear after successful aclrtFree" << std::endl;
            ++g_failures;
        }
    }
}

void TestUDMAMultiQpHostTransportContract()
{
    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    CheckContains(contextPath, context, "LoadUDMAQpConfigFromEnv(config, &parseError)");
    CheckContains(contextPath, context, "if (options_.sharedQpDomain)");
    CheckContains(contextPath, context, "config = BuildUDMASharedQpConfig();");
    CheckContains(contextPath, context,
                  "BuildUDMAQpConfigWireDescriptor(config, parseStatus)");
    CheckContains(contextPath, context,
                  "options_.exchange->AllGather(&localDescriptor, 1, allDescriptors.data())");
    CheckContains(contextPath, context, "UDMAQpConfigWireDescriptorsEqual(");
    CheckContains(contextPath, context,
                  "nextState.sharedQp = transport_->UsesSharedQps();");
    CheckContains(contextPath, context,
                  "hiddenState.sharedQp = transport_->UsesSharedQps();");
    CheckContains(contextPath, context, "const int transportAllocationStatus = AgreeStatus(");
    CheckContains(contextPath, context,
                  "TileXRUDMATransport allocation failed on at least one rank");
    CheckContains(contextPath, context,
                  "std::array<int32_t, TILEXR_MAX_RANK_SIZE> allStatus");
    CheckNotContains(contextPath, context, "g_udmaUnavailable");

    const std::string headerPath = "src/comm/udma/tilexr_udma_transport.h";
    const auto header = ReadFile(headerPath);
    CheckContains(headerPath, header, "struct PerPeerQpState;");
    CheckContains(headerPath, header, "struct SharedQpState;");
    CheckContains(headerPath, header, "std::vector<std::unique_ptr<PerPeerQpState>> peerQpStates_");
    CheckContains(headerPath, header, "std::vector<std::unique_ptr<SharedQpState>> sharedQpStates_");
    CheckContains(headerPath, header, "std::vector<uint32_t> localRouteByPeerQp_");
    CheckContains(headerPath, header, "std::vector<uint32_t> remoteRouteByPeerQp_");

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    CheckNotContains(transportPath, transport, "std::regex");
    CheckNotContains(transportPath, transport, "#include <regex>");
    CheckContains(transportPath, transport, "LoadUDMARootInfo(rootInfo, &error)");
    CheckContains(transportPath, transport, "ResolveUDMAPortCountEid(");
    CheckContains(transportPath, transport, "ResolveUDMATopologyEid(");
    CheckContains(transportPath, transport,
                  "peerQpStates_[index].reset(new (std::nothrow) PerPeerQpState())");
    CheckContains(transportPath, transport,
                  "sharedQpStates_[qpIdx].reset(new (std::nothrow) SharedQpState())");
    CheckContains(transportPath, transport, "int TileXRUDMATransport::ImportSharedQueues()");
    CheckContains(transportPath, transport,
                  "state.remoteQpHandles.assign(options_.rankSize, nullptr)");
    CheckContains(transportPath, transport,
                  "if ((!sharedQp_ && sameNode) ||");
    CheckContains(transportPath, transport,
                  "ValidateUDMASharedQpConfig(");
    CheckContains(transportPath, transport,
                  "if (remoteClean && state.qpHandle != nullptr)");
    CheckContains(transportPath, transport,
                  "const bool hardwareClean = remoteClean && state.qpHandle == nullptr");
    CheckContains(transportPath, transport, "region.remoteMemHandles.count(importKey) == 0");
    CheckContains(transportPath, transport,
                  "BuildUDMAInfoImage(reinterpret_cast<uintptr_t>(registration.infoDev), qpCount_");
    CheckContains(transportPath, transport, "return IsAvailable() ? qpCount_ : 0U;");
    CheckContains(transportPath, transport, "ret = AgreeInitStatus(localStatus);");
    CheckContains(transportPath, transport, "ret = AgreeRaOwnership();");
    CheckContains(transportPath, transport, "int TileXRUDMATransport::AgreeRaOwnership() const");
    CheckContains(transportPath, transport,
                  "options_.exchange->AllGather(&local, 1, allAttached.data())");
    CheckContains(transportPath, transport, "TILEXR_HCCP_RA_ALREADY_INITIALIZED = 328002");
    CheckContains(transportPath, transport,
                  "std::getenv(\"TILEXR_UDMA_ATTACH_EXISTING_RA\")");
    CheckContains(transportPath, transport,
                  "ret == TILEXR_HCCP_RA_ALREADY_INITIALIZED && AttachExistingRaEnabled()");
    CheckContains(transportPath, transport,
                  "TileXR UDMA attaching to existing RA initialization");
    CheckContains(transportPath, transport, "if (raInitialized_)");
    CheckContains(transportPath, transport, "int TileXRUDMATransport::AgreeEidCount() const");
    CheckContains(transportPath, transport,
                  "std::array<uint32_t, TILEXR_MAX_RANK_SIZE> allEidCounts");
    CheckContains(transportPath, transport,
                  "options_.exchange->AllGather(&eidCount_, 1, allEidCounts.data())");
    CheckNotContains(transportPath, transport, "MoonEP");
    CheckNotContains(transportPath, transport, "moonep");

    const std::string configPath = "src/comm/udma/tilexr_udma_config.h";
    const auto config = ReadFile(configPath);
    CheckContains(configPath, config, "TILEXR_UDMA_MAX_QP_COUNT = 48");
    CheckContains(configPath, config, "BuildUDMASharedQpConfig");
    CheckNotContains(configPath, config, "TILEXR_UDMA_SHARED_QP_ENV");

    const std::string commArgsPath = "src/include/comm_args.h";
    const auto commArgs = ReadFile(commArgsPath);
    CheckContains(commArgsPath, commArgs, "UDMA_SHARED_QP");

    const std::string typesPath = "src/include/tilexr_udma_types.h";
    const auto types = ReadFile(typesPath);
    CheckContains(typesPath, types, "uint32_t localTokenId;");

    const std::string devicePath = "src/include/tilexr_udma.h";
    const auto device = ReadFile(devicePath);
    CheckContains(devicePath, device,
                  "UDMAFillSgeCtx(sgeCtx, messageLen, localAddr, qpCtxEntry->localTokenId)");
    CheckContains(devicePath, device,
                  "__attribute__((always_inline)) inline __aicore__ void UDMAFillSqeCtx");
    CheckContains(devicePath, device,
                  "__attribute__((always_inline)) inline __aicore__ uint32_t UDMAPostSend");
}

void TestUDMAFullmeshDomainContract()
{
    const std::string fullmeshPath = "src/include/tilexr_udma_fullmesh.h";
    const auto fullmesh = ReadFile(fullmeshPath);
    CheckContains(fullmeshPath, fullmesh,
                  "TILEXR_UDMA_FULLMESH_SLOT_COUNT = 8U");
    CheckContains(fullmeshPath, fullmesh,
                  "return activeMask & ~(1U << localRank);");
    CheckContains(fullmeshPath, fullmesh,
                  "view.registrationGeneration == registrationGeneration");

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    CheckContains(transportPath, transport,
                  "fullmeshQpStates_.resize(options_.rankSize);");
    CheckContains(transportPath, transport,
                  "peer == options_.rank ||");
    CheckContains(transportPath, transport,
                  "state.qpIdx = static_cast<uint32_t>(");
    CheckContains(transportPath, transport,
                  "peer % options_.localRankSize);");
    CheckContains(transportPath, transport,
                  "const uint32_t remoteSlot = static_cast<uint32_t>(");
    CheckContains(transportPath, transport,
                  "TILEXR_UDMA_FULLMESH_SLOT_COUNT + remoteSlot");
    CheckContains(transportPath, transport,
                  "FullmeshEntryIndex(peer, slot)");
    CheckContains(transportPath, transport,
                  "fullmeshLocalRegistrations");
    CheckContains(transportPath, transport,
                  "fullmeshRemoteMemHandles");
    CheckContains(transportPath, transport,
                  "CleanupFullmeshRegistration(*registration)");
    CheckContains(transportPath, transport,
                  "mem(entryCount)");
    CheckContains(transportPath, transport,
                  "PrepareRegistration(desc, preparedProfile_, false)");
    CheckContains(transportPath, transport,
                  "return IsAvailable() ? qpCount_ : 0U;");
    CheckContains(transportPath, transport,
                  "matchingEdges != 1U || !directEdge");
    CheckContains(transportPath, transport,
                  "portCounts->second.at(localEid) != 1U");
    CheckContains(transportPath, transport,
                  "!usedLocalEids.emplace(localEid, true).second");
    CheckContains(transportPath, transport,
                  "previousContexts = ctxHandleByEid_");
    CheckContains(transportPath, transport,
                  "previousTokens = tokenHandleByEid_");
    CheckContains(transportPath, transport, "RollbackAddedContexts(");
    CheckContains(transportPath, transport,
                  "previousContexts, previousTokens)");
    CheckContains(transportPath, transport,
                  "previousTokens.count(eidIndex) != 0U");
    CheckContains(transportPath, transport,
                  "previousContexts.count(eidIndex) != 0U");

    const auto registerPos = transport.find(
        "int TileXRUDMATransport::RegisterMemoryOnContexts");
    const auto importPos = transport.find(
        "int TileXRUDMATransport::ExchangeAndImportMemory", registerPos);
    if (registerPos == std::string::npos || importPos == std::string::npos) {
        std::cerr << "failed to locate UDMA registration EID selection" << std::endl;
        ++g_failures;
    } else {
        const auto body = transport.substr(registerPos, importPos - registerPos);
        CheckContains(transportPath, body,
                      "if (fullmeshDomain)");
        CheckContains(transportPath, body, "fullmeshLocalRouteByPeer_");
        CheckContains(transportPath, body,
                      "region.fullmeshLocalRegistrations");
        CheckNotContains(transportPath, body,
                         "for (const auto& ctxEntry : ctxHandleByEid_)");
    }

    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    const auto contextRegisterPos = context.find(
        "int TileXRUDMAContext::RegisterMemory");
    const auto contextUnregisterPos = context.find(
        "int TileXRUDMAContext::UnregisterMemory", contextRegisterPos);
    if (contextRegisterPos == std::string::npos ||
        contextUnregisterPos == std::string::npos) {
        std::cerr << "failed to locate Fullmesh registration transaction" << std::endl;
        ++g_failures;
    } else {
        const auto body = context.substr(contextRegisterPos,
            contextUnregisterPos - contextRegisterPos);
        const auto generationPos = body.find("candidateGeneration");
        const auto fullmeshViewPos = body.find("nextFullmeshViewDev");
        const auto publishPos = body.find("ApplyCommArgsState(nextState)");
        const auto commitPos = body.find("transport_->CommitPreparedMemory()");
        if (generationPos == std::string::npos ||
            fullmeshViewPos == std::string::npos ||
            publishPos == std::string::npos || commitPos == std::string::npos ||
            !(generationPos < fullmeshViewPos && fullmeshViewPos < publishPos &&
                publishPos < commitPos)) {
            std::cerr << "CLOS registry and Fullmesh view are not one publication transaction"
                      << std::endl;
            ++g_failures;
        }
        CheckContains(contextPath, body,
                      "nextState.registrationGeneration = candidateGeneration;");
        CheckContains(contextPath, body,
                      "nextState.fullmeshViewDev = publishFullmesh ?");
    }

    const auto profilePos = transport.find(
        "int TileXRUDMATransport::PrepareProfile");
    const auto profileInfoPos = transport.find(
        "GM_ADDR TileXRUDMATransport::GetPreparedProfileInfoDev", profilePos);
    if (profilePos == std::string::npos || profileInfoPos == std::string::npos) {
        std::cerr << "failed to locate profile registration isolation" << std::endl;
        ++g_failures;
    } else {
        const auto body = transport.substr(profilePos, profileInfoPos - profilePos);
        CheckContains(transportPath, body,
                      "PrepareRegistration(desc, preparedProfile_, false)");
    }

    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const auto comm = ReadFile(commPath);
    CheckContains(commPath, comm, "ExtraFlag::UDMA_FULLMESH");
    CheckContains(commPath, comm, "commArgs_.udmaRegistrationGeneration");

    const std::string cmakePath = "src/comm/CMakeLists.txt";
    CheckContains(cmakePath, ReadFile(cmakePath),
                  "tilexr_udma_fullmesh.h");
}

void TestUDMADevicePostingContract()
{
    const std::string devicePath = "src/include/tilexr_udma.h";
    const auto device = ReadFile(devicePath);
    CheckContains(devicePath, device, "__ubuf__ UDMASqeCtx* sqeCtx");
    CheckContains(devicePath, device, "__ubuf__ UDMANotifyCtx* notifyCtx");
    CheckContains(devicePath, device, "__ubuf__ UDMASgeCtx* sgeCtx");
    CheckContains(devicePath, device,
                  "AscendC::DataCopyPad(sqGlobal, wqeScratch[scratchOffset], copyParams);");
    CheckContains(devicePath, device, "UDMASyncEvent<AscendC::HardEvent::S_MTE3>();");
    CheckContains(devicePath, device, "UDMASyncEvent<AscendC::HardEvent::MTE3_S>();");
    CheckNotContains(devicePath, device, "scratchAddr != 0U");
    CheckNotContains(devicePath, device, "UDMAFillWrappedNotifyData");
    CheckNotContains(devicePath, device, "UDMAGetSqLogicalAddr");
    CheckNotContains(devicePath, device, "UDMACleanWrappedWqe");

    const auto postPos = device.find("inline __aicore__ uint32_t UDMAPostSend(");
    const auto writePos = device.find("inline __aicore__ uint32_t UDMAWrite(", postPos);
    if (postPos == std::string::npos || writePos == std::string::npos) {
        std::cerr << "failed to locate UDMA post-send body" << std::endl;
        ++g_failures;
    } else {
        const auto body = device.substr(postPos, writePos - postPos);
        const auto publishPos = body.find("UDMAPublishWqe(");
        const auto headPos = body.find("qpCtxEntry->headAddr", publishPos);
        const auto countPos = body.find("qpCtxEntry->wqeCntAddr", headPos);
        const auto doorbellPos = body.find("UDMARingDoorbell(curHead, qpCtxEntry)", countPos);
        if (publishPos == std::string::npos || headPos == std::string::npos ||
            countPos == std::string::npos || doorbellPos == std::string::npos ||
            !(publishPos < headPos && headPos < countPos && countPos < doorbellPos)) {
            std::cerr << "UDMA post order must be MTE3 publish, head, count, then doorbell"
                      << std::endl;
            ++g_failures;
        }
    }

    const auto doorbellFunction = device.find("inline void UDMARingDoorbell(");
    const auto nextFunction = device.find("inline __aicore__ void UDMAMte3CopyToSq(", doorbellFunction);
    if (doorbellFunction == std::string::npos || nextFunction == std::string::npos) {
        std::cerr << "failed to locate UDMA doorbell body" << std::endl;
        ++g_failures;
    } else {
        const auto body = device.substr(doorbellFunction, nextFunction - doorbellFunction);
        CheckContains(devicePath, body,
                      "st_dev(curHead, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->dbAddr), 0);");
    }

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    const std::string inOrderQp = "qpAttr.ub.jfsFlag.value = 2;";
    if (CountOccurrences(transport, inOrderQp) != 4U) {
        std::cerr << "all UDMA QP creation paths must disable out-of-order completion"
                  << std::endl;
        ++g_failures;
    }
}

void TestPublicHeadersDoNotExposeUDMAContext()
{
    const std::vector<std::string> publicHeaders = {
        "src/include/tilexr_api.h",
        "src/include/tilexr_udma.h",
        "src/include/tilexr_udma_reg.h",
        "src/include/tilexr_udma_types.h",
        "src/include/comm_args.h",
    };
    for (const auto& path : publicHeaders) {
        const auto text = ReadFile(path);
        CheckNotContains(path, text, "tilexr_udma_context.h");
        CheckNotContains(path, text, "TileXRUDMAContext");
    }
}

void TestCommSourcesDoNotUseShmem()
{
    const std::vector<std::string> paths = {
        "src/comm/CMakeLists.txt",
        "src/comm/tilexr_comm.cpp",
        "src/comm/comm_wrap.cpp",
        "src/comm/tilexr_comm.h",
        "src/comm/udma/tilexr_udma_context.cpp",
        "src/comm/udma/tilexr_udma_context.h",
        "src/comm/udma/tilexr_udma_transport.cpp",
        "src/comm/udma/tilexr_udma_transport.h",
    };
    const std::vector<std::string> forbidden = {
        "shmem",
        "shmem.h",
        "libshmem",
        "aclshmem",
        "ACLSHMEM",
    };
    for (const auto& path : paths) {
        CheckNoNeedles(path, forbidden);
    }
}

void TestUDMALocalRegistrationFlags()
{
    const std::string path = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto text = ReadFile(path);
    const auto registerFunction = text.find("int TileXRUDMATransport::RegisterMemoryOnContexts");
    const auto nextFunction = text.find("int TileXRUDMATransport::ExchangeAndImportMemory", registerFunction);
    if (registerFunction == std::string::npos || nextFunction == std::string::npos) {
        std::cerr << "failed to locate UDMA local registration body" << std::endl;
        ++g_failures;
        return;
    }

    const auto body = text.substr(registerFunction, nextFunction - registerFunction);
    const std::vector<std::string> requiredFlags = {
        "mrInfo.in.ub.flags.bs.cacheable = 0;",
        "mrInfo.in.ub.flags.bs.access = MEM_SEG_ACCESS_DEFAULT;",
        "mrInfo.in.ub.flags.bs.nonPin = options_.nonPinRegistration ? 1 : 0;",
        "mrInfo.in.ub.flags.bs.userIova = 0;",
        "mrInfo.in.ub.flags.bs.tokenIdValid = 1;",
        "mrInfo.in.ub.flags.bs.tokenPolicy = MEM_SEG_TOKEN_PLAIN_TEXT;",
    };
    const auto registerCall = body.find("loader_.RaCtxLmemRegister(");
    for (const auto& flag : requiredFlags) {
        CheckContains(path, body, flag);
        const auto flagPosition = body.find(flag);
        if (flagPosition != std::string::npos &&
            (registerCall == std::string::npos || flagPosition > registerCall)) {
            std::cerr << "UDMA registration flag must precede RaCtxLmemRegister: " << flag << std::endl;
            ++g_failures;
        }
    }

    size_t callCount = 0;
    size_t callPosition = 0;
    const std::string callNeedle = "loader_.RaCtxLmemRegister(";
    while ((callPosition = body.find(callNeedle, callPosition)) != std::string::npos) {
        ++callCount;
        callPosition += callNeedle.size();
    }
    if (callCount != 1) {
        std::cerr << "expected one UDMA local registration call, got " << callCount << std::endl;
        ++g_failures;
    }

    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const auto comm = ReadFile(commPath);
    CheckContains(commPath, comm, "physicalInfo_.chipName == ChipName::CHIP_950");
    CheckContains(commPath, comm, "physicalInfo_.chipName == ChipName::CHIP_950PR");
}

void TestUDMAPersistentProfilesStayIndependent()
{
    const std::string apiPath = "src/include/tilexr_api.h";
    const auto api = ReadFile(apiPath);
    CheckContains(apiPath, api, "typedef uint32_t TileXRUDMAProfileHandle;");
    CheckContains(apiPath, api, "int TileXRUDMAProfileRegister(");
    CheckContains(apiPath, api, "int TileXRUDMAProfileUnregister(");
    CheckContains(apiPath, api, "int TileXRUDMAProfileQuery(");

    const std::string registryPath = "src/include/tilexr_udma_reg.h";
    const auto registry = ReadFile(registryPath);
    CheckContains(registryPath, registry, "TILEXR_UDMA_PROFILE_MAX_REGIONS = 8U");
    CheckContains(registryPath, registry, "TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS = 48U");
    CheckContains(registryPath, registry, "struct TileXRUDMAProfileQpBinding");
    CheckContains(registryPath, registry, "struct TileXRUDMAProfileView");
    CheckContains(registryPath, registry, "registrationBase");
    CheckContains(registryPath, registry, "registrationBytes");
    CheckContains(registryPath, registry,
                  "TileXRUDMAProfileQpBinding qpBindings[TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS]");

    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto context = ReadFile(contextPath);
    const auto registerPos = context.find("int TileXRUDMAContext::RegisterProfile");
    const auto unregisterPos = context.find("int TileXRUDMAContext::UnregisterProfile", registerPos);
    if (registerPos == std::string::npos || unregisterPos == std::string::npos) {
        std::cerr << "failed to locate persistent profile registration body" << std::endl;
        ++g_failures;
    } else {
        const auto body = context.substr(registerPos, unregisterPos - registerPos);
        CheckContains(contextPath, body, "transport_->PrepareProfile(desc)");
        CheckContains(contextPath, body,
                      "candidate->registry.regions[index].registrationBase = nullptr;");
        CheckContains(contextPath, body, "transport_->CommitPreparedProfile(candidateHandle)");
        CheckContains(contextPath, body, "profiles_.emplace(candidateHandle, std::move(candidate))");
        CheckNotContains(contextPath, body, "ApplyCommArgsState(");
        CheckNotContains(contextPath, body, "udmaInfoDev_ =");
    }

    const std::string transportPath = "src/comm/udma/tilexr_udma_transport.cpp";
    const auto transport = ReadFile(transportPath);
    CheckContains(transportPath, transport,
                  "registration.qpBindings[qpIdx].localRegion");
    CheckContains(transportPath, transport,
                  "registration.qpBindings[qpIdx].remoteRegion");
    CheckContains(transportPath, transport,
                  "UDMAProfileRegistrationBase(desc.regions[region])");
    CheckContains(transportPath, transport,
                  "profiles_.emplace(handle, std::move(preparedProfile_))");
    CheckContains(transportPath, transport, "registration.cleanupPending = true;");
    CheckContains(transportPath, transport,
                  "if (ret == TILEXR_SUCCESS && it->second->regions.empty()");

    const std::string devicePath = "src/include/tilexr_udma.h";
    const auto device = ReadFile(devicePath);
    CheckContains(devicePath, device, "UDMAProfileRegisteredRangeValid(");
    CheckContains(devicePath, device, "UDMAProfileGetNbiOnQpDeferred(");
    CheckContains(devicePath, device, "UDMAProfilePutNbiOnQpDeferred(");
    CheckContains(devicePath, device,
                  "UDMAPostSend<UDMAOpcode::READ, false>(udmaInfo, wqeScratch");
    CheckContains(devicePath, device, "UDMAProfileCompletionFrontier(");
    CheckContains(devicePath, device, "UDMAProfileFlushQpDoorbell(");
    CheckContains(devicePath, device, "UDMAProfileQuietStatusOnQpUntil(");
    CheckContains(devicePath, device, "UDMAPollCQUntilSqTail(");
}

} // namespace

int main()
{
    TestTileXRCommUsesUDMAContextBoundary();
    TestUDMATransportStaysBehindContext();
    TestUDMAContextShutdownIsLocalOnly();
    TestUDMAReviewFeedbackGuards();
    TestPublicHeadersDoNotExposeUDMAContext();
    TestCommSourcesDoNotUseShmem();
    TestUDMALocalRegistrationFlags();
    TestUDMAPersistentProfilesStayIndependent();
    TestUDMAMemoryCleanupIsRetryable();
    TestUDMARegistrationIsTransactional();
    TestUDMAUnregisterIsLocalAfterPublication();
    TestUDMARegistrationGenerationSurvivesUnregister();
    TestUDMAFailedDeviceFreeRetainsPointer();
    TestUDMAMultiQpHostTransportContract();
    TestUDMAFullmeshDomainContract();
    TestUDMADevicePostingContract();
    if (g_failures != 0) {
        std::cerr << g_failures << " UDMA source guard checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR UDMA source guard checks passed" << std::endl;
    return 0;
}
