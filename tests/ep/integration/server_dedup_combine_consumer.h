// Shared Combine tensor fixtures and invariant checks.
// These expert values exercise routing and copies; they do not model expert computation.
void InitializeCombineExpert(const Config &c, void *data, uint64_t bytes)
{
    std::vector<uint16_t> chunk(512U * 1024U);
    for (uint64_t offset = 0; offset < bytes;) {
        const uint64_t count = std::min<uint64_t>(bytes - offset, chunk.size() * sizeof(uint16_t));
        for (uint64_t i = 0; i < count / 2U; ++i) {
            const uint64_t element = offset / 2U + i;
            chunk[i] = CombineM4Reference::ExpertBits(c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16,
                c.rank, element / c.h, element % c.h, c.operatorMode == "both");
        }
        Check(aclrtMemcpy(static_cast<uint8_t *>(data) + offset, bytes - offset,
            chunk.data(), count, ACL_MEMCPY_HOST_TO_DEVICE), "initialize combine expertOut");
        offset += count;
    }
}

void InitializeStandaloneCombine(const Config &c,
    const TileXREp::EpMemoryServerDedupCombineLayout &layout, const std::vector<void *> &buffers)
{
    using namespace ServerDedupReference;
    auto shape = SourceSendShape(c);
    shape.quantized = false;
    const auto rows = DispatchRows(shape, c.rank);
    Require(rows.size() <= layout.capacityRows, "standalone Combine row capacity");
    std::vector<int32_t> prefix(c.experts, 0);
    std::vector<uint32_t> types(layout.capacityRows, 0);
    std::vector<uint32_t> assist(layout.assistInfoBytes / 4U, 0);
    std::vector<uint32_t> relay(layout.relayReadIndexBytes / 4U, 0);
    std::vector<uint32_t> mask(c.bs, 0);
    auto weights = Weights(shape, c.rank);
    for (uint32_t token = 0; token < uint32_t(c.bs); ++token)
        for (uint32_t k = 0; k < uint32_t(c.topK); ++k)
            weights[token * c.topK + k] = CombineM4Reference::Weight(c.rank, token, k);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        ++prefix[(row.expert % (c.experts / c.rankSize)) * c.rankSize + row.source];
        types[i] = TokenType(row);
        const auto address = DestinationIndex(shape, row);
        std::copy(address.begin(), address.end(), assist.begin() + i * 3U);
        if (row.FirstHop()) {
            auto words = RelayReadIndex(row);
            for (size_t member = 0; member < row.packet.members.size(); ++member)
                words[member * 2U + 1U] = FloatBits(CombineM4Reference::Weight(
                    row.source, row.token, row.packet.members[member].k));
            std::copy(words.begin(), words.end(), relay.begin() + i * c.topK * 2U);
        }
    }
    for (size_t i = 1; i < prefix.size(); ++i) prefix[i] += prefix[i - 1];

    const auto packets = SourcePackets(Ids(shape, c.rank), weights, c.rank, c.rankSize, c.experts, c.topK);
    for (const auto &packet : packets) {
        const auto &first = packet.members.front();
        mask[packet.token] |= uint32_t(1) << first.k;
        weights[packet.token * c.topK + first.k] = packet.members.size() == 1U ? first.weight : 1.0f;
    }
    const auto copy = [&](size_t index, const void *source, uint64_t bytes) {
        if (bytes != 0U) Check(aclrtMemcpy(buffers[index], bytes, source, bytes, ACL_MEMCPY_HOST_TO_DEVICE),
            "initialize standalone Combine tables");
    };
    copy(4, prefix.data(), layout.sendCountsBytes);
    copy(6, assist.data(), layout.assistInfoBytes);
    if (TileXREp::EpServerDedupEnabled(c.bs)) {
        copy(5, types.data(), layout.tokenTypeBytes);
        copy(7, relay.data(), layout.relayReadIndexBytes);
        copy(8, mask.data(), layout.sourceMaskBytes);
    }
    copy(10, weights.data(), layout.expertScalesBytes);
}

struct CombineTensorSnapshot {
    const char *name;
    const void *device;
    std::vector<uint8_t> bytes;
};

std::vector<CombineTensorSnapshot> CaptureCombineTensors(const Config &c,
    const TileXREp::EpMemoryServerDedupCombineLayout &layout, const std::vector<void *> &buffers,
    bool dispatchBatch = false)
{
    const bool aliasDispatch = c.operatorMode == "both" && c.quantMode == 0;
    const size_t indices[] = {aliasDispatch ? 2U : 12U, 6U, 4U, 10U, 5U, 7U, 8U, 13U};
    const uint64_t sizes[] = {layout.expertOutBytes, layout.assistInfoBytes, layout.sendCountsBytes,
        layout.expertScalesBytes, layout.tokenTypeBytes, layout.relayReadIndexBytes,
        layout.sourceMaskBytes, layout.yOutBytes};
    const char *names[] = {"expertOut", "assistInfo", "sendCounts", "expertScales", "tokenType",
        "relayReadIndex", "sourceMask", "yOut"};
    std::vector<CombineTensorSnapshot> snapshots;
    for (size_t i = 0; i < 8U; ++i) {
        if (sizes[i] == 0U || indices[i] == 13U) continue;
        if (!TileXREp::EpServerDedupEnabled(c.bs) &&
            (indices[i] == 5U || indices[i] == 7U || indices[i] == 8U)) continue;
        // A continuous Dispatch batch legitimately rewrites its output tensors.
        // Independent expertOut is invariant; yOut is validated numerically.
        if (dispatchBatch && indices[i] != 12U && indices[i] != 13U) continue;
        snapshots.push_back({names[i], buffers[indices[i]], std::vector<uint8_t>(sizes[i])});
        auto &snapshot = snapshots.back();
        Check(aclrtMemcpy(snapshot.bytes.data(), sizes[i], snapshot.device, sizes[i],
            ACL_MEMCPY_DEVICE_TO_HOST), "snapshot Combine tensor");
    }
    return snapshots;
}

void ValidateCombineTensors(const std::vector<CombineTensorSnapshot> &snapshots)
{
    std::vector<uint8_t> chunk(1024U * 1024U);
    for (const auto &snapshot : snapshots) {
        for (size_t offset = 0; offset < snapshot.bytes.size();) {
            const size_t count = std::min(chunk.size(), snapshot.bytes.size() - offset);
            Check(aclrtMemcpy(chunk.data(), chunk.size(), static_cast<const uint8_t *>(snapshot.device) + offset,
                count, ACL_MEMCPY_DEVICE_TO_HOST), "read Combine tensor");
            ValidateOutput(std::memcmp(chunk.data(), snapshot.bytes.data() + offset, count) == 0,
                std::string("Combine modified invariant tensor ") + snapshot.name + " at chunk=" + std::to_string(offset));
            offset += count;
        }
    }
}

// Caller-derived sequence: each successful public operator call consumes one
// magic on a fresh communicator. This is not device-side magic telemetry.
struct OperatorProgress {
    uint64_t submitted = 0, completed = 0, warmupSubmitted = 0, warmupCompleted = 0;
    int64_t firstMagic = 0, lastMagic = 0;
    uint32_t windowMask = 0;

    void Submit(bool warming, int64_t magic)
    {
        if (warming) ++warmupSubmitted;
        else ++submitted;
        if (firstMagic == 0) firstMagic = magic;
        lastMagic = magic;
        windowMask |= uint32_t(1) << (magic % 2);
    }
    void Complete()
    {
        completed = submitted;
        warmupCompleted = warmupSubmitted;
    }
    std::string Details(const char *name, uint64_t peerBytes) const
    {
        const std::string prefix = std::string(",\"") + name;
        return prefix + "_submitted\":" + std::to_string(submitted) +
            prefix + "_completed\":" + std::to_string(completed) +
            prefix + "_warmup_submitted\":" + std::to_string(warmupSubmitted) +
            prefix + "_warmup_completed\":" + std::to_string(warmupCompleted) +
            prefix + "_first_magic\":" + std::to_string(firstMagic) +
            prefix + "_last_magic\":" + std::to_string(lastMagic) +
            prefix + "_window_mask\":" + std::to_string(windowMask) +
            prefix + "_window_offset\":" + std::to_string(lastMagic == 0 ? 0U :
                TileXREp::TileXREpServerDedupWindowOffset(lastMagic, peerBytes));
    }
};
