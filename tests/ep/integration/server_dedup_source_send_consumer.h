// Included inside the integration consumer's namespace. No production debug ABI.
ServerDedupReference::Shape SourceSendShape(const Config &c)
{
    return {uint32_t(c.rankSize), uint32_t(c.experts), uint32_t(c.bs), uint32_t(c.h), uint32_t(c.topK),
        c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16, c.quantMode == 4, c.fp8Format == "e4m3"};
}

void SaveSourceSendInputs(const Config &c, const std::vector<uint16_t> &x,
    const std::vector<int32_t> &ids, const std::vector<float> &weights)
{
    const char *directory = std::getenv("TILEXR_VALIDATION_INPUT_DIR");
    if (directory == nullptr) return; // Direct executable callers may opt out of retaining inputs.
    const std::string stem = std::string(directory) + "/rank-" + std::to_string(c.rank);
    const auto write = [&](const char *suffix, const void *data, size_t bytes) {
        std::ofstream out(stem + suffix, std::ios::binary);
        out.write(static_cast<const char *>(data), bytes);
        Require(bool(out), "cannot retain source-send inputs: " + stem + suffix);
    };
    write("-x.bin", x.data(), x.size() * sizeof(uint16_t));
    write("-ids.bin", ids.data(), ids.size() * sizeof(int32_t));
    write("-weights.bin", weights.data(), weights.size() * sizeof(float));
}

void AppendDispatchDetail(std::ostringstream &out, const Config &c,
    const TileXREp::EpServerDedupDetailRecord &detail, uint32_t core, uint64_t generation,
    const uint64_t *time, const TileXREp::EpServerDedupDataLayout &layout,
    const std::vector<ServerDedupReference::DispatchRow> &rows)
{
    using namespace TileXREp;
    const auto require = [&](bool valid, const std::string &field) {
        Require(valid, "invalid dispatch detail core=" + std::to_string(core) + " field=" + field);
    };
    require(detail.marker == kEpServerDedupProfileMarker && detail.version == kEpServerDedupProfileVersion &&
        detail.recordBytes == sizeof(detail) && detail.rank == uint32_t(c.rank) && detail.core == core &&
        detail.generation == generation && detail.reserved == 0U, "identity");
    const auto *p = detail.points;
    const auto *n = detail.counters;
    for (uint32_t i = 14; i < 16; ++i) require(p[i] == 0U, "reserved point");
    for (uint32_t i = 10; i < 16; ++i) require(n[i] == 0U, "reserved counter");
    const bool receiver = core >= kEpServerDedupSendCoreNum;
    uint64_t expectedRows = 0U, firstHops = 0U, relays = 0U;
    if (receiver) {
        const uint64_t index = core - kEpServerDedupSendCoreNum;
        const uint64_t begin = rows.size() * index / kEpServerDedupRecvCoreNum;
        const uint64_t end = rows.size() * (index + 1U) / kEpServerDedupRecvCoreNum;
        expectedRows = end - begin;
        for (uint64_t row = begin; row < end; ++row) if (rows[row].FirstHop()) {
            ++firstHops;
            relays += rows[row].packet.members.size() - 1U;
        }
        require(p[0] >= time[6] && p[7] <= time[7], "receive parent");
        for (uint32_t i = 1; i < 8; ++i) require(p[i] >= p[i - 1U], "receive order");
    } else {
        for (uint32_t i = 0; i < 8; ++i) require(p[i] == 0U, "sender receive point");
    }
    require(n[0] == expectedRows && n[4] == firstHops && n[5] == relays, "receive counts");
    if (expectedRows != 0U) {
        require(n[6] >= 1U && n[6] <= 8U && n[7] == layout.blockCount, "receive capacity");
        require(n[1] >= (expectedRows + 15U) / 16U && n[2] >= expectedRows && n[2] >= n[1] &&
            n[2] / 16U + uint64_t(n[2] % 16U != 0U) <= n[1] && n[3] < n[1], "probe counts");
    } else {
        for (uint32_t i = 0; i < 8; ++i) require(n[i] == 0U, "empty receive counter");
        if (receiver) for (uint32_t i = 4; i < 8; ++i) require(p[i] == p[3], "empty receive point");
    }
    require(p[8] >= time[7] && p[13] <= time[8], "finish parent");
    for (uint32_t i = 9; i < 14; ++i) require(p[i] >= p[i - 1U], "finish order");
    const bool coordinator = core == kEpServerDedupCoreNum - 1U;
    if (coordinator) require(n[8] >= uint64_t(c.rankSize) && n[9] < uint64_t(c.rankSize), "round counters");
    else {
        require(n[8] == 0U && n[9] == 0U, "noncoordinator counter");
        for (uint32_t i = 11; i < 14; ++i) require(p[i] == p[10], "noncoordinator point");
    }
    const char *pointNames[] = {"recv_begin", "prefix_end", "tasks_end", "prepare_end", "loop_end", "drain_end",
        "clear_issue_end", "clear_end", "finish_begin", "local_end", "sync_end", "counter_end", "publish_end", "wait_end"};
    const char *metricNames[] = {"probe", "acquire_load", "metadata", "atomic", "output_issue", "relay_issue", "round_peer_wait"};
    const char *counterNames[] = {"rows", "probe_batches", "probe_slots", "empty_probe_batches", "first_hop_rows",
        "relay_packets", "lanes", "blocks", "round_polls", "round_slowest_peer"};
    const uint64_t expectedCounts[] = {n[1], expectedRows, expectedRows, firstHops, expectedRows, firstHops,
        coordinator ? uint64_t(c.rankSize) : 0U};
    out << ",\"detail\":{\"points\":{";
    for (uint32_t i = 0; i < 14; ++i) {
        if (i != 0U) out << ',';
        out << '\"' << pointNames[i] << "\":" << p[i];
    }
    out << "},\"metrics\":{";
    for (uint32_t i = 0; i < 7; ++i) {
        const auto &m = detail.metrics[i];
        require(m.count == expectedCounts[i], std::string(metricNames[i]) + " count");
        if (m.count == 0U) require(m.totalTicks == 0U && m.maxBegin == 0U && m.maxEnd == 0U,
            std::string(metricNames[i]) + " unused");
        else {
            const uint64_t begin = i == 6U ? p[12] : p[3];
            const uint64_t end = i == 6U ? p[13] : p[4];
            require(m.maxBegin >= begin && m.maxEnd >= m.maxBegin && m.maxEnd <= end &&
                m.totalTicks <= end - begin && m.maxEnd - m.maxBegin <= m.totalTicks &&
                m.totalTicks / m.count + uint64_t(m.totalTicks % m.count != 0U) <= m.maxEnd - m.maxBegin,
                std::string(metricNames[i]) + " interval");
        }
        if (i != 0U) out << ',';
        out << '\"' << metricNames[i] << "\":{\"total_ticks\":" << m.totalTicks << ",\"max_begin\":" << m.maxBegin
            << ",\"max_end\":" << m.maxEnd << ",\"count\":" << m.count << '}';
    }
    require(detail.metrics[3].totalTicks <= detail.metrics[2].totalTicks, "atomic subset");
    out << "},\"counters\":{";
    for (uint32_t i = 0; i < 10; ++i) {
        if (i != 0U) out << ',';
        out << '\"' << counterNames[i] << "\":" << n[i];
    }
    out << "}}";
}

std::string ReadDispatchProfile(const Config &c, const TileXR::CommArgs &args)
{
    using namespace TileXREp;
    std::array<EpServerDedupProfileRecord, kEpServerDedupCoreNum * kEpServerDedupProfileSegments> records {};
    Check(aclrtMemcpy(records.data(), sizeof(records), args.peerMems[c.rank], sizeof(records), ACL_MEMCPY_DEVICE_TO_HOST),
        "read dispatch breakdown");
    const auto detailLayout = TileXREpServerDedupSendProfileLayout(args.peerMemBytes, uint32_t(c.bs), uint32_t(c.topK));
    const auto dataLayout = TileXREpServerDedupDataLayout(c.bs, c.h, c.topK, c.experts, c.quantMode == 4);
    const uint64_t recvOffset = TileXREpServerDedupDetailOffset(detailLayout.offset);
    Require(recvOffset >= dataLayout.requiredBytes, "dispatch profile overlaps data or communicator flags");
    std::array<EpServerDedupDetailRecord, kEpServerDedupCoreNum> recvDetail {};
    Check(aclrtMemcpy(recvDetail.data(), sizeof(recvDetail), args.peerMems[c.rank] + recvOffset,
        sizeof(recvDetail), ACL_MEMCPY_DEVICE_TO_HOST), "read receive and finish breakdown");
    std::vector<uint8_t> detail(detailLayout.bytes);
    Check(aclrtMemcpy(detail.data(), detail.size(), args.peerMems[c.rank] + detailLayout.offset,
        detail.size(), ACL_MEMCPY_DEVICE_TO_HOST), "read send issue breakdown");
    const uint64_t generation = uint64_t(c.warmup) + c.loops;
    const auto outputRows = ServerDedupReference::DispatchRows(SourceSendShape(c), uint32_t(c.rank));
    const char *points[] = {"start", "init_end", "send_cnt_end", "send_data_end", "recv_cnt_end", "cumsum_end", "prefix_end", "recv_data_end", "end"};
    std::ostringstream out;
    out << ",\"schema_version\":" << kEpServerDedupProfileVersion << ",\"kernel_scope\":\"dispatch\",\"generation\":" << generation
        << ",\"block_dim\":" << kEpServerDedupCoreNum << ",\"records\":[";
    for (uint32_t core = 0; core < kEpServerDedupCoreNum; ++core) {
        uint64_t time[kEpServerDedupProfilePoints] {};
        for (uint32_t segment = 0; segment < kEpServerDedupProfileSegments; ++segment) {
            const auto &r = records[core * kEpServerDedupProfileSegments + segment];
            Require(r.marker == kEpServerDedupProfileMarker && r.version == kEpServerDedupProfileVersion &&
                r.recordBytes == sizeof(r) && r.rank == uint32_t(c.rank) && r.core == core &&
                r.blockDim == kEpServerDedupCoreNum && r.segment == segment && r.generation == generation,
                "invalid dispatch breakdown core=" + std::to_string(core) + " segment=" + std::to_string(segment));
            std::copy(r.time, r.time + 4, time + segment * 4U);
        }
        Require(time[0] > 0 && time[8] > time[0], "invalid dispatch breakdown duration");
        for (uint32_t i = 9; i < kEpServerDedupProfilePoints; ++i)
            Require(time[i] == time[8], "invalid reserved breakdown timestamp");
        if (core != 0) out << ',';
        out << "{\"core\":" << core;
        for (uint32_t i = 0; i < 9; ++i) {
            Require(i == 0 || time[i] >= time[i - 1], "unordered dispatch breakdown timestamps");
            out << ",\"" << points[i] << "\":" << time[i];
        }
        AppendDispatchDetail(out, c, recvDetail[core], core, generation, time, dataLayout, outputRows);
        if (core < kEpServerDedupSendCoreNum) {
            EpServerDedupSendProfileHeader header {};
            const auto *base = detail.data() + core * detailLayout.coreStride;
            std::memcpy(&header, base, sizeof(header));
            const uint32_t count = c.bs / kEpServerDedupSendCoreNum + (core < c.bs % kEpServerDedupSendCoreNum);
            const uint32_t start = core * (c.bs / kEpServerDedupSendCoreNum) +
                std::min(core, uint32_t(c.bs % kEpServerDedupSendCoreNum));
            Require(header.marker == kEpServerDedupProfileMarker && header.version == kEpServerDedupProfileVersion &&
                header.recordBytes == sizeof(EpServerDedupSendProfilePacket) && header.rank == uint32_t(c.rank) &&
                header.core == core && header.generation == generation && header.packetCount <= detailLayout.capacity &&
                header.packetCount >= count && header.packetCount <= count * c.topK && header.firstHopCount <= header.packetCount,
                "invalid send issue header core=" + std::to_string(core));
            out << ",\"first_hop_count\":" << header.firstHopCount << ",\"send_packets\":[";
            uint64_t previous = time[2];
            std::set<uint64_t> routes;
            for (uint32_t packet = 0; packet < header.packetCount; ++packet) {
                EpServerDedupSendProfilePacket p {};
                std::memcpy(&p, base + sizeof(header) + packet * sizeof(p), sizeof(p));
                Require(p.issue >= previous && p.issue <= time[3] && p.token >= start && p.token < start + count &&
                    p.k < uint32_t(c.topK) && routes.insert(uint64_t(p.token) * c.topK + p.k).second,
                    "invalid send issue packet core=" + std::to_string(core) + " packet=" + std::to_string(packet));
                if (packet != 0U) out << ',';
                out << "{\"issue\":" << p.issue << ",\"token\":" << p.token << ",\"k\":" << p.k << '}';
                previous = p.issue;
            }
            out << ']';
        }
        out << '}';
    }
    return out.str() + ']';
}
