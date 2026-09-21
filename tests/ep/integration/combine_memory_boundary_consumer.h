// Included by the shared consumer; this probe has its own fresh communicator.
// It never submits Dispatch or Combine and never writes a gap/runtime region.
int RunCombineMemoryBoundary(const Config &c)
{
    using namespace TileXRCombineMemoryBoundaryTest;
    bool initialized = false, deviceSet = false, safeCleanup = true, success = false;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    void *result = nullptr;
    uint32_t submitted = 0, completed = 0;
    std::string stage = "acl-init";
    ConfigureProfiling(c);
    Barrier barrier(c, Clock::now() + std::chrono::seconds(90));
    try {
        Check(aclInit(nullptr), "aclInit"); initialized = true;
        stage = "device";
        Check(aclrtSetDevice(c.device), "aclrtSetDevice"); deviceSet = true;
        const char *soc = aclrtGetSocName();
        Require(soc && (std::strncmp(soc, "Ascend950", 9) == 0 ||
            std::strncmp(soc, "Ascend910_9", 11) == 0 || std::strncmp(soc, "A5", 2) == 0), "A5 required");
        Check(aclrtCreateStream(&stream), "aclrtCreateStream");
        stage = "8g-communicator";
        Check(TileXRCommInitRankMemoryDomainWithSize(0, kServerDedupDataMiB, c.rankSize, c.rank, &comm),
            "TileXRCommInitRankMemoryDomainWithSize");
        safeCleanup = false;
        TileXR::CommArgs *host = nullptr;
        GM_ADDR device = nullptr;
        Check(TileXRGetCommArgsHost(comm, host), "TileXRGetCommArgsHost");
        Check(TileXRGetCommArgsDev(comm, device), "TileXRGetCommArgsDev");
        Require(host && device, "missing Host/device CommArgs");
        ValidateComm(c, *host);
        TileXR::CommArgs deviceArgs {};
        Check(aclrtMemcpy(&deviceArgs, sizeof(deviceArgs), device, sizeof(deviceArgs), ACL_MEMCPY_DEVICE_TO_HOST),
            "read device CommArgs");
        ValidateComm(c, deviceArgs);
        Require(host->peerMemBytes == kPeerBytes && deviceArgs.peerMemBytes == kPeerBytes,
            "Combine boundary probe requires exactly 8 GiB");
        for (int rank = 0; rank < c.rankSize; ++rank)
            Require(host->peerMems[rank] == deviceArgs.peerMems[rank], "Host/device peer mapping differs");
        const auto checkWords = [&](const uint32_t *words, uint32_t pattern, const char *name) {
            for (uint32_t word = 0; word < kProbeBytes / sizeof(uint32_t); ++word)
                Require(words[word] == pattern, std::string(name) + " word=" + std::to_string(word) +
                    " expected=" + std::to_string(pattern) + " actual=" + std::to_string(words[word]));
        };
        const auto read = [&](uint64_t offset, uint32_t pattern, const char *name) {
            Require(LegalRange(offset, kProbeBytes), "unsafe Host probe read");
            std::array<uint32_t, kProbeBytes / sizeof(uint32_t)> words {};
            words.fill(0x55555555U);
            Check(aclrtMemcpy(words.data(), kProbeBytes, host->peerMems[c.rank] + offset,
                kProbeBytes, ACL_MEMCPY_DEVICE_TO_HOST), name);
            checkWords(words.data(), pattern, name);
        };
        const auto guards = [&](uint32_t window, uint32_t index) {
            const uint64_t offset = ProbeOffset(window, index);
            read(offset - kProbeBytes, 0U, "guard before");
            if (LegalRange(offset + kProbeBytes, kProbeBytes)) read(offset + kProbeBytes, 0U, "guard after");
        };
        stage = "chain-barrier";
        barrier.Run(0);
        Marker(c, "EP_SERVER_DEDUP_CHAIN_PASS", ",\"peer_count\":" + std::to_string(c.rankSize) +
            ",\"peer_mem_bytes\":" + std::to_string(kPeerBytes));
        stage = "boundary-initial-zero";
        // Check every location before any write, using the fresh allocation's zero oracle.
        for (uint32_t window = 0; window < kWindowCount; ++window)
            for (uint32_t index = 0; index < kProbesPerWindow; ++index) {
                read(ProbeOffset(window, index), 0U, "initial point");
                guards(window, index);
            }
        Check(aclrtMalloc(&result, kResultBytes, ACL_MEM_MALLOC_HUGE_FIRST), "allocate probe readback");
        for (uint32_t window = 0; window < kWindowCount; ++window) {
            for (uint32_t index = 0; index < kProbesPerWindow; ++index) {
                const std::string point = " window=" + std::to_string(window) + " index=" + std::to_string(index);
                stage = "boundary-launch" + point;
                Check(aclrtMemset(result, kResultBytes, 0x55, kResultBytes), "initialize probe readback sentinel");
                Check(Launch(device, result, window, index, stream), "Combine boundary kernel");
                ++submitted;
                stage = "boundary-sync" + point;
                Check(aclrtSynchronizeStream(stream), "boundary aclrtSynchronizeStream");
                ++completed;
                stage = "boundary-verify" + point;
                std::array<uint32_t, kResultBytes / sizeof(uint32_t)> words {};
                Check(aclrtMemcpy(words.data(), kResultBytes, result, kResultBytes, ACL_MEMCPY_DEVICE_TO_HOST),
                    "read device boundary roundtrip");
                checkWords(words.data(), 0U, "device initial read");
                checkWords(words.data() + kProbeBytes / sizeof(uint32_t), ProbePattern(window, index), "device readback");
                read(ProbeOffset(window, index), ProbePattern(window, index), "Host point readback");
                guards(window, index);
            }
        }
        stage = "boundary-final-reread";
        // Later writes must not alias or corrupt an earlier window or point.
        for (uint32_t window = 0; window < kWindowCount; ++window)
            for (uint32_t index = 0; index < kProbesPerWindow; ++index) {
                read(ProbeOffset(window, index), ProbePattern(window, index), "final point readback");
                guards(window, index);
            }
        stage = "completion-barrier";
        barrier.Run(1);
        safeCleanup = true;
        success = true;
    } catch (const std::exception &error) {
        std::cerr << "EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=" << stage
            << " probe_submitted=" << submitted << " probe_completed=" << completed << ' ' << error.what() << std::endl;
    }
    if (!safeCleanup) {
        std::cerr << "rank=" << c.rank << " cleanup=deferred-to-process-exit: completion unconfirmed" << std::endl;
        return 1;
    }
    const auto cleanup = [&](int code, const char *name) {
        if (code != 0) {
            success = false;
            std::cerr << "EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=cleanup " << name << " ret=" << code << std::endl;
        }
    };
    if (result) cleanup(aclrtFree(result), "aclrtFree");
    if (comm) cleanup(TileXRCommDestroy(comm), "TileXRCommDestroy");
    if (stream) cleanup(aclrtDestroyStream(stream), "aclrtDestroyStream");
    if (deviceSet) cleanup(aclrtResetDevice(c.device), "aclrtResetDevice");
    if (initialized) cleanup(aclFinalize(), "aclFinalize");
    if (success) {
        std::ostringstream details;
        details << ",\"peer_mem_bytes\":" << kPeerBytes << ",\"buffer_stride\":" << kWindowBytes
            << ",\"runtime_reserved_bytes\":" << kRuntimeReservedBytes << ",\"legal_window_bytes\":" << kLegalWindowBytes
            << ",\"probe_bytes\":" << kProbeBytes << ",\"probe_submitted\":" << submitted
            << ",\"probe_completed\":" << completed << ",\"submitted\":" << submitted << ",\"completed\":" << completed
            << ",\"warmup_submitted\":0,\"warmup_completed\":0,\"dispatch_submitted\":0,\"dispatch_completed\":0"
            << ",\"combine_submitted\":0,\"combine_completed\":0,\"cleanup\":\"complete\""
            << ",\"kernel_scope\":\"memory-boundary\",\"native_status\":\"boundary-probe\",\"points\":[";
        for (uint32_t window = 0; window < kWindowCount; ++window)
            for (uint32_t index = 0; index < kProbesPerWindow; ++index) {
                if (window || index) details << ',';
                details << "{\"window\":" << window << ",\"index\":" << index << ",\"offset\":" << ProbeOffset(window, index)
                    << ",\"pattern\":" << ProbePattern(window, index)
                    << ",\"initial_zero\":true,\"device_initial_zero\":true,\"device_readback\":true,\"host_readback\":true"
                    << ",\"guard_before_zero\":true,\"guard_after_checked\":" << (index != 3U ? "true" : "false")
                    << ",\"guard_after_zero\":" << (index != 3U ? "true" : "null") << ",\"final_readback\":true}";
            }
        details << ']';
        Marker(c, "EP_SERVER_DEDUP_COMBINE_BOUNDARY_PASS", details.str());
    }
    return success ? 0 : 1;
}
