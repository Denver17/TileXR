#include "tilexr_rdma_wire.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace Wire = TileXR::RdmaWire;

static void Check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

static TileXRRDMADeviceView View(uint32_t mode = TILEXR_RDMA_DB_HARD)
{
    TileXRRDMADeviceView v = {};
    v.version = TILEXR_RDMA_DEVICE_VERSION;
    v.structBytes = sizeof(v);
    v.localAddress = 0x8877665544332200ULL;
    v.localBytes = 0x100000U;
    v.remoteAddress = 0x1122334455667788ULL;
    v.remoteBytes = 0x200000U;
    v.localKey = 0x12345678U;
    v.remoteKey = 0xAABBCCDDU;
    v.queue.sq = 0x10000000U;
    v.queue.cq = 0x20000000U;
    v.queue.sqDbr = 0x30000000U;
    v.queue.cqDbr = 0x30000040U;
    v.queue.doorbell = 0x40000000U;
    v.queue.state = 0x50000000U;
    v.queue.qpn = 0x12345U;
    v.queue.cqn = 0x678U;
    v.queue.sqDepth = 4U;
    v.queue.cqDepth = 2U;
    v.queue.sqEntryBytes = 64U;
    v.queue.cqEntryBytes = 64U;
    v.queue.dbMode = mode;
    v.queue.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
    v.queue.maxSendSge = 16;
    v.queue.maxSendWr = 8191;
    v.queue.doorbellBytes = mode == TILEXR_RDMA_DB_HARD ? 2048U : 8U;
    v.queue.sqDbValue = Wire::MakeSqDbValue(v.queue.qpn, 1U, 5U, 3U);
    return v;
}

enum class Fault { None, Syndrome, ZeroSyndrome, Qpn, Opcode, Sequence, Receive, Merge, Invalid, Owner };

struct Nic {
    explicit Nic(const TileXRRDMADeviceQueue& queue) : q(queue), sq(queue.sqDepth * 64U), cq(queue.cqDepth * 64U)
    {
        for (uint32_t slot = 0; slot < q.cqDepth; ++slot) {
            cq[slot * 64U + 7U] = 0xF8U; // Initial CQ opcode=INVALID; never zero CQ memory.
        }
    }

    void SetByte(uint32_t offset, uint8_t value) { scratch.at(offset) = value; }
    uint8_t GetByte(uint32_t offset) const { return scratch.at(offset); }
    TileXRRDMADeviceState LoadState() const { return state; }
    void StoreState(const TileXRRDMADeviceState& value)
    {
        state = value;
        events.push_back("state");
    }

    void Publish(uint64_t address, uint32_t offset, uint32_t bytes)
    {
        CHECK(offset + bytes <= scratch.size());
        if (address >= q.sq && address < q.sq + sq.size()) {
            CHECK(bytes % 64U == 0U);
            CHECK(address + bytes <= q.sq + sq.size());
            if (offset == 0U) {
                const uint32_t blocks = (48U + uint32_t(scratch[3]) * 8U + 63U) / 64U;
                const size_t tail = size_t(address - q.sq) + 64U;
                if (blocks > 1U) {
                    CHECK(tail + (blocks - 1U) * 64U <= sq.size());
                    CHECK(std::memcmp(sq.data() + tail, scratch.data() + 64U, (blocks - 1U) * 64U) == 0);
                }
            }
            std::memcpy(sq.data() + size_t(address - q.sq), scratch.data() + offset, bytes);
            events.push_back(offset == Wire::kStopOffset ? "stop" : offset == 0U ? "wqe" : "tail");
        } else if (address == q.sqDbr) {
            CHECK(bytes == 4U);
            std::memcpy(pi.data(), scratch.data() + offset, bytes);
            events.push_back("pi");
        } else {
            CHECK(address == q.cqDbr && bytes == 4U);
            std::memcpy(ci.data(), scratch.data() + offset, bytes);
            events.push_back("ci");
        }
    }

    void Ring(uint64_t address, uint64_t value)
    {
        CHECK(state.producer > state.completed);
        batchFirst = state.completed;
        batchFirstCq = state.cqConsumer;
        batchCount = 0;
        const uint64_t next = state.producer;
        CHECK(next - batchFirst < q.sqDepth);
        if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
            CHECK(events.back() == "pi" && address == q.doorbell);
            CHECK(Be32(pi) == uint32_t(next));
        } else {
            CHECK(address == q.doorbell + (next % 256U) * 8U);
        }
        CHECK(value == (q.sqDbValue | (((next / 256U) % 256U) << 32)));
        for (uint64_t sequence = batchFirst; sequence < next;) {
            const size_t wqe = size_t(sequence % q.sqDepth) * 64U;
            CHECK(sq[wqe] == uint8_t(0x40U | ((sequence / q.sqDepth) % 2U) * 128U));
            CHECK(sq[wqe + 16U] == 0x24U);
            CHECK(((sq[wqe + 2U] >> 4) & 3U) == ((batchFirstCq + batchCount) % 4U));
            const uint32_t nSge = sq[wqe + 3U] / 2U;
            CHECK(nSge >= 1 && nSge <= q.maxSendSge && sq[wqe + 3U] == 2U * nSge);
            const uint32_t blocks = (48U + 16U * nSge + 63U) / 64U;
            CHECK(sequence + blocks <= next);
            const uint64_t remoteAddress = Be64(sq, wqe + 32U);
            uint32_t total = 0;
            for (uint32_t i = 0; i < nSge; ++i) {
                const size_t segment = wqe + 48U + i * 16U;
                const uint64_t localAddress = Be64(sq, segment);
                const uint32_t length = uint32_t(Be64(sq, segment + 8U) >> 32);
                const uint32_t key = uint32_t(Be64(sq, segment + 8U));
                CHECK((key & 0x7FFFFFFFU) == 0x12345678U);
                CHECK((key >> 31) == uint32_t(i + 1U == nSge));
                if (gatherData) {
                    CHECK(localAddress >= localBase && localAddress - localBase + length <= local.size());
                    CHECK(remoteAddress >= remoteBase && remoteAddress - remoteBase + total + length <= remote.size());
                    std::memcpy(remote.data() + size_t(remoteAddress - remoteBase) + total,
                        local.data() + size_t(localAddress - localBase), length);
                }
                total += length;
            }
            CHECK(total == uint32_t(Be64(sq, wqe + 16U)));
            postedLocal.push_back(Be64(sq, wqe + 48U));
            postedRemote.push_back(remoteAddress);
            postedSgeCounts.push_back(nSge);
            sequence += blocks;
            ++batchCount;
        }
        CHECK(batchCount <= q.maxSendWr && batchCount < q.cqDepth);
        if (batchCount > peakOutstanding) { peakOutstanding = batchCount; }
        const size_t stop = size_t(next % q.sqDepth) * 64U;
        CHECK(sq[stop] == (next / q.sqDepth % 2U == 0U ? 0xFFU : 0x7FU));
        events.push_back("ring");
        ++rings;
        readsThisRing = 0U;
        pending = true;
    }

    static uint64_t Be64(const std::vector<uint8_t>& b, size_t offset)
    {
        uint64_t value = 0;
        for (size_t i = 0; i < 8U; ++i) { value = (value << 8) | b.at(offset + i); }
        return value;
    }

    static uint32_t Be32(const std::array<uint8_t, 4>& b)
    {
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    }

    static void Le32(std::vector<uint8_t>& b, size_t offset, uint32_t value)
    {
        for (uint32_t i = 0; i < 4U; ++i) {
            b.at(offset + i) = uint8_t(value >> (i * 8U));
        }
    }

    void ReadCqe(uint64_t address)
    {
        CHECK(pending);
        CHECK(address >= q.cq && address < q.cq + cq.size() && (address - q.cq) % 64U == 0);
        const uint64_t delta = ((address - q.cq) / 64U + q.cqDepth - batchFirstCq % q.cqDepth) % q.cqDepth;
        CHECK(delta < batchCount);
        const uint64_t consumer = batchFirstCq + delta;
        const uint64_t sequence = batchFirstCq + delta;
        if (lastReadAddress != address) { readsThisRing = 0U; lastReadAddress = address; }
        const Fault activeFault = sequence >= faultAtSequence ? fault : Fault::None;
        ++reads;
        const size_t offset = size_t(address - q.cq);
        if (readsThisRing++ >= delay) {
            uint32_t owner = uint32_t((consumer / q.cqDepth) % 2U) << 31;
            if (activeFault == Fault::Owner) {
                owner ^= 0x80000000U;
            }
            Le32(cq, offset, owner | (activeFault == Fault::Qpn ? q.qpn + 1U : q.qpn));
            uint32_t opcode = 4U;
            if (activeFault == Fault::Syndrome || activeFault == Fault::ZeroSyndrome) {
                opcode = 30U;
            } else if (activeFault == Fault::Opcode) {
                opcode = 8U;
            } else if (activeFault == Fault::Invalid) {
                opcode = 31U;
            }
            // Deliberately unrelated low WQEBB index: SQ matching uses counter@30.
            uint32_t op = (opcode << 27) | (1U << 26) | 0xBEEFU;
            if (activeFault == Fault::Receive) {
                op &= ~(1U << 26);
            }
            if (activeFault == Fault::Merge) {
                op |= 1U << 24;
            }
            Le32(cq, offset + 4U, op);
            cq[offset + 28U] = activeFault == Fault::Syndrome ? 0x13U : 0U;
            const uint16_t counter = uint16_t(sequence + (activeFault == Fault::Sequence ? 1U : 0U));
            cq[offset + 30U] = uint8_t(counter);
            cq[offset + 31U] = uint8_t(counter >> 8);
        }
        std::memcpy(scratch.data() + Wire::kCqeOffset, cq.data() + offset, 64U);
        events.push_back("poll");
    }

    TileXRRDMADeviceQueue q;
    TileXRRDMADeviceState state = {};
    std::array<uint8_t, TILEXR_RDMA_DEVICE_SCRATCH_BYTES> scratch{};
    std::vector<uint8_t> sq;
    std::vector<uint8_t> cq;
    std::array<uint8_t, 4> pi{};
    std::array<uint8_t, 4> ci{};
    std::vector<std::string> events;
    Fault fault = Fault::None;
    uint64_t faultAtSequence = 0U;
    uint64_t batchFirst = 0U, batchFirstCq = 0U, batchCount = 0U, peakOutstanding = 0U;
    uint64_t lastReadAddress = UINT64_MAX;
    std::vector<uint64_t> postedLocal, postedRemote;
    std::vector<uint32_t> postedSgeCounts;
    bool gatherData = false;
    uint64_t localBase = 0, remoteBase = 0;
    std::vector<uint8_t> local, remote;
    uint32_t delay = 0U;
    uint32_t readsThisRing = 0U;
    uint32_t rings = 0U;
    uint32_t reads = 0U;
    bool pending = false;
};

static void TestGoldenEncoding()
{
    const auto view = View();
    Nic nic(view.queue);
    nic.scratch.fill(0xCCU); // Encoder must initialize every reserved byte.
    Wire::EncodeWrite(nic, view, 5U, 5U, 16U, 8U, 0x123U);
    const uint8_t golden[64] = {
        0xC0,0xA4,0x10,0x02,0x10,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x24,0x00,0x00,0x00,0x00,0x00,0x01,0x23, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x90, 0xAA,0xBB,0xCC,0xDD,0x12,0x34,0x56,0x00,
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x10, 0x00,0x00,0x01,0x23,0x92,0x34,0x56,0x78
    };
    CHECK(std::memcmp(nic.scratch.data(), golden, sizeof(golden)) == 0);
    CHECK(view.queue.sqDbValue == 0x00300000AB112345ULL);
    CHECK(Wire::SqDbValue(view.queue.sqDbValue, 256U) == 0x00300001AB112345ULL);
    CHECK(Wire::SqDbValue(view.queue.sqDbValue, 65536U) == view.queue.sqDbValue);
}

static void TestWrapAndSharedState(uint32_t mode)
{
    auto view = View(mode);
    Nic nic(view.queue);
    nic.delay = 1U;
    for (uint32_t i = 0; i < 20U; ++i) { // Multiple SQ phases and CQ depth wraps.
        CHECK(Wire::Put(view, 0U, 0U, 64U, 4U, nic) == TILEXR_RDMA_DEVICE_SUCCESS);
        CHECK(nic.state.producer == i + 1U && nic.state.completed == i + 1U);
        CHECK(nic.state.cqConsumer == i + 1U && Nic::Be32(nic.ci) == i + 1U);
    }
    auto secondRegion = view;
    secondRegion.localAddress += 4096U;
    secondRegion.remoteAddress += 8192U;
    CHECK(secondRegion.queue.state == view.queue.state);
    CHECK(Wire::Put(secondRegion, 8U, 16U, 32U, 4U, nic) == TILEXR_RDMA_DEVICE_SUCCESS);
    CHECK(nic.state.producer == 21U && nic.state.completed == 21U && nic.rings == 21U);
}

static void TestCounterRollover(uint32_t mode)
{
    const auto view = View(mode);
    Nic nic(view.queue);
    nic.state.producer = nic.state.completed = nic.state.cqConsumer = 65534U;
    for (uint32_t i = 0; i < 4U; ++i) {
        CHECK(Wire::Put(view, 0U, 0U, 64U, 2U, nic) == TILEXR_RDMA_DEVICE_SUCCESS);
    }
    CHECK(nic.state.producer == 65538U && nic.state.cqConsumer == 65538U);
    CHECK(Nic::Be32(nic.ci) == 65538U);
    nic.state.cqConsumer = 0xFFFFFEU;
    for (uint32_t i = 0; i < 3U; ++i) {
        CHECK(Wire::Put(view, 0U, 0U, 64U, 2U, nic) == TILEXR_RDMA_DEVICE_SUCCESS);
    }
    CHECK(nic.state.cqConsumer == 0x1000001U && Nic::Be32(nic.ci) == 1U);
}

static void TestCompletionFailures()
{
    const auto view = View();
    const Fault faults[] = {Fault::Syndrome, Fault::ZeroSyndrome, Fault::Qpn, Fault::Opcode,
        Fault::Sequence, Fault::Receive, Fault::Merge, Fault::Invalid, Fault::Owner};
    for (Fault fault : faults) {
        Nic nic(view.queue);
        nic.fault = fault;
        const uint32_t expected = (fault == Fault::Syndrome || fault == Fault::ZeroSyndrome) ?
            TILEXR_RDMA_DEVICE_CQE_ERROR : (fault == Fault::Invalid || fault == Fault::Owner) ?
            TILEXR_RDMA_DEVICE_TIMEOUT : TILEXR_RDMA_DEVICE_CQE_MISMATCH;
        CHECK(Wire::Put(view, 0U, 0U, 64U, 3U, nic) == expected);
        CHECK(nic.state.producer == 1U && nic.state.completed == 0U && nic.state.cqConsumer == 0U);
        CHECK(nic.state.error == expected);
        CHECK(nic.state.syndrome == (fault == Fault::Syndrome ? 0x13U : 0U));
        CHECK(nic.reads == (expected == TILEXR_RDMA_DEVICE_TIMEOUT ? 3U : 1U));
        CHECK(Nic::Be32(nic.ci) == 0U);
        CHECK(Wire::Put(view, 0U, 0U, 64U, 3U, nic) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(nic.rings == 1U); // A timeout never silently reuses the SQ/MR.
    }
    Nic busy(view.queue);
    busy.state.producer = 1U;
    CHECK(Wire::Put(view, 0U, 0U, 64U, 3U, busy) == TILEXR_RDMA_DEVICE_BUSY);
    CHECK(busy.events.empty());
}

static void Rejected(const TileXRRDMADeviceView& view, uint64_t localOffset = 0U,
    uint64_t remoteOffset = 0U, uint32_t bytes = 64U, uint32_t polls = 2U)
{
    Nic nic(View().queue);
    CHECK(Wire::Put(view, localOffset, remoteOffset, bytes, polls, nic) != TILEXR_RDMA_DEVICE_SUCCESS);
    CHECK(nic.events.empty() && nic.rings == 0U);
}

static void TestValidation()
{
    const auto valid = View();
    CHECK(Wire::ValidateView(valid) == TILEXR_RDMA_DEVICE_SUCCESS);
    auto v = valid; v.version = 0U; Rejected(v);
    v = valid; v.structBytes = 0U; Rejected(v);
    v = valid; v.queue.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V1; Rejected(v);
    v = valid; v.queue.dbMode = TILEXR_RDMA_DB_UNKNOWN; Rejected(v);
    v = valid; v.queue.sqDepth = 1U; Rejected(v);
    v = valid; v.queue.sqDepth = 3U; Rejected(v);
    v = valid; v.queue.cqDepth = 0U; Rejected(v);
    v = valid; v.queue.cqEntryBytes = 32U; Rejected(v);
    v = valid; v.queue.sqEntryBytes = 128U; Rejected(v);
    v = valid; v.queue.doorbellBytes = 2040U; Rejected(v);
    v = valid; v.queue.state += 8U; Rejected(v);
    v = valid; v.queue.cqDbr = 0U; Rejected(v);
    v = valid; v.queue.sqDbValue |= 1ULL << 32; Rejected(v);
    v = valid; v.queue.sqDbValue ^= 1U; Rejected(v);
    v = valid; v.queue.sqDbValue |= 1ULL << 60; Rejected(v);
    v = valid; v.queue.sq = UINT64_MAX - 63U; Rejected(v);
    v = valid; v.localAddress = UINT64_MAX - 64U; Rejected(v);
    v = View(TILEXR_RDMA_DB_SOFT); v.queue.sqDbr = 0U; Rejected(v);
    Rejected(valid, 0U, 0U, 0U);
    Rejected(valid, 0U, 0U, 64U, 0U);
    Rejected(valid, 0U, 0U, 0x80000000U);
    Rejected(valid, valid.localBytes - 63U);
    Rejected(valid, 0U, valid.remoteBytes - 63U);
    Rejected(valid, UINT64_MAX);
    Nic edge(valid.queue);
    CHECK(Wire::Put(valid, valid.localBytes - 64U, valid.remoteBytes - 64U, 64U, 2U, edge) == 0U);
    Nic exhausted(valid.queue);
    exhausted.state.producer = exhausted.state.completed = UINT64_MAX;
    CHECK(Wire::Put(valid, 0U, 0U, 64U, 2U, exhausted) == TILEXR_RDMA_DEVICE_INVALID);
    CHECK(exhausted.events.empty());
}

static void TestBatches(uint32_t mode)
{
    auto v = View(mode);
    v.queue.sqDepth = 256U;
    v.queue.cqDepth = 64U; // Deliberately tighter than SQ; exercise both rings.
    for (uint32_t count : {1U, 48U, 1000U}) {
        for (uint32_t requested : {1U, 16U, 48U, 128U}) {
            Nic nic(v.queue);
            nic.delay = 1;
            nic.state.producer = nic.state.completed = 65530U;
            nic.state.cqConsumer = 0xFFFFFDU;
            CHECK(Wire::PutStridedBatch(v, 16, 32, 64, count, 80, 128, requested, 4, nic) == 0);
            CHECK(nic.state.producer == 65530U + count && nic.state.completed == nic.state.producer);
            CHECK(nic.state.cqConsumer == 0xFFFFFDU + count && nic.state.error == 0);
            const uint32_t window = requested < 63U ? requested : 63U;
            CHECK(nic.rings == (count + window - 1) / window);
            CHECK(nic.peakOutstanding == (count < window ? count : window));
            CHECK(nic.postedLocal.size() == count && nic.postedRemote.size() == count);
            CHECK(Nic::Be32(nic.ci) == ((0xFFFFFDU + count) & 0xFFFFFFU));
            for (uint32_t i = 0; i < count; ++i) {
                CHECK(nic.postedLocal[i] == v.localAddress + 16 + uint64_t(i) * 80);
                CHECK(nic.postedRemote[i] == v.remoteAddress + 32 + uint64_t(i) * 128);
            }
            // A second region must continue shared QP counters, not reset them.
            auto other = v; other.localAddress += 4096;
            CHECK(Wire::PutStridedBatch(other, 0, 0, 64, 48, 0, 64, 48, 4, nic) == 0);
            CHECK(nic.state.completed == 65530U + count + 48);
            CHECK(nic.postedLocal.back() == other.localAddress);
        }
    }
}

static void TestNoWrapBatches(uint32_t mode)
{
    auto v = View(mode);
    v.queue.sqDepth = v.queue.cqDepth = 8192U;
    // Fresh QPs: each count fits one 4096-WR window without ring wrap.
    for (uint32_t count : {48U, 1000U, 4096U}) {
        Nic nic(v.queue);
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, count, 0, 64,
            TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT, 2, nic) == 0);
        CHECK(nic.rings == 1 && nic.peakOutstanding == count);
        CHECK(nic.state.producer == count && nic.state.completed == count && nic.state.cqConsumer == count);
        CHECK(nic.state.error == 0 && nic.postedRemote.size() == count);
        CHECK(Nic::Be32(nic.ci) == count);
    }
    // Seven 1000-WR samples plus one control WRITE each still fit an 8192-entry ring.
    Nic repeated(v.queue);
    for (uint32_t sample = 0; sample < 7U; ++sample) {
        CHECK(Wire::Put(v, 0, 0, 64, 2, repeated) == 0);
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 1000, 0, 64,
            TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT, 2, repeated) == 0);
        CHECK(repeated.state.producer == uint64_t(sample + 1U) * 1001U);
        CHECK(repeated.state.producer < v.queue.sqDepth && repeated.state.cqConsumer < v.queue.cqDepth);
    }
    CHECK(repeated.rings == 14U && repeated.state.completed == 7007U && repeated.state.error == 0U);
    // A provider capped at 4095 WRs can export a 4096-entry SQ. The default
    // upper limit must leave its stop slot unused, even when it is larger.
    v.queue.sqDepth = 4096U;
    for (uint32_t count : {48U, 1000U, 4095U}) {
        Nic nic(v.queue);
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, count, 0, 64,
            TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT, 2, nic) == 0);
        CHECK(nic.rings == 1U && nic.peakOutstanding == count);
        CHECK(nic.state.completed == count && nic.state.cqConsumer == count && nic.state.error == 0U);
        CHECK(nic.state.producer < v.queue.sqDepth);
    }
    // Two warmups and one measurement fit without resetting or wrapping queues.
    Nic shortRun(v.queue);
    for (uint32_t sample = 0; sample < 3U; ++sample) {
        CHECK(Wire::Put(v, 0, 0, 64, 2, shortRun) == 0);
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 1000, 0, 64,
            TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT, 2, shortRun) == 0);
    }
    CHECK(shortRun.state.producer == 3003U && shortRun.state.completed == 3003U);
    CHECK(shortRun.state.producer < v.queue.sqDepth && shortRun.state.cqConsumer == 3003U);
}

struct CountingTrace : Wire::NoBatchTrace {
    void Stage(Wire::BatchTraceStage stage) { stages.push_back(stage); }
    void WqeStart() { ++started; }
    void WqeEncoded() { ++encoded; }
    void WqePublished() { ++published; }
    void PollStart() { ++polls; }
    void PollRead() { ++snapshots; }
    void PollDecoded(bool pending) { ++decoded; pendingPolls += pending ? 1U : 0U; }
    std::vector<Wire::BatchTraceStage> stages;
    uint32_t started = 0, encoded = 0, published = 0, polls = 0, snapshots = 0, decoded = 0, pendingPolls = 0;
};

static void TestTracePreservesIO(uint32_t mode)
{
    auto v = View(mode);
    v.queue.sqDepth = v.queue.cqDepth = 8192U;
    for (uint32_t count : {48U, 1000U}) {
        for (Fault fault : {Fault::None, Fault::Syndrome, Fault::Invalid}) {
            Nic plain(v.queue), observed(v.queue);
            plain.delay = observed.delay = 1;
            plain.fault = observed.fault = fault;
            plain.faultAtSequence = observed.faultAtSequence = 3;
            CountingTrace trace;
            const uint32_t a = Wire::PutStridedBatch(v, 0, 0, 64, count, 0, 64, 4096, 3, plain);
            const uint32_t b = Wire::PutStridedBatch(v, 0, 0, 64, count, 0, 64, 4096, 3, observed, trace);
            CHECK(a == b && plain.events == observed.events);
            CHECK(plain.sq == observed.sq && plain.cq == observed.cq && plain.ci == observed.ci);
            CHECK(plain.pi == observed.pi && plain.postedRemote == observed.postedRemote);
            CHECK(std::memcmp(&plain.state, &observed.state, sizeof(plain.state)) == 0);
            CHECK(trace.started == count && trace.encoded == count && trace.published == count);
            CHECK(trace.polls == observed.reads && trace.snapshots == trace.polls && trace.decoded == trace.polls);
            CHECK(trace.pendingPolls == trace.polls - observed.state.completed - (fault == Fault::Syndrome ? 1U : 0U));
            const std::vector<Wire::BatchTraceStage> expected{
                Wire::BatchTraceStage::Validated, Wire::BatchTraceStage::Reserved,
                Wire::BatchTraceStage::StopPublished, Wire::BatchTraceStage::WqesPublished,
                Wire::BatchTraceStage::DoorbellIssued, Wire::BatchTraceStage::FirstCqe,
                Wire::BatchTraceStage::CqDrained, Wire::BatchTraceStage::Retired};
            CHECK(trace.stages == expected);
            CountingTrace rejected;
            CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 0, 0, 64, 4096, 3, observed, rejected) ==
                TILEXR_RDMA_DEVICE_INVALID);
            CHECK(rejected.stages.empty() && rejected.polls == 0 && rejected.started == 0);
        }
    }
}

static void TestBatchFailures()
{
    auto v = View(); v.queue.sqDepth = v.queue.cqDepth = 8;
    for (Fault fault : {Fault::Syndrome, Fault::Sequence, Fault::Invalid, Fault::Owner}) {
        Nic nic(v.queue); nic.fault = fault; nic.faultAtSequence = 3;
        const uint32_t expected = fault == Fault::Syndrome ? TILEXR_RDMA_DEVICE_CQE_ERROR :
            fault == Fault::Sequence ? TILEXR_RDMA_DEVICE_CQE_MISMATCH : TILEXR_RDMA_DEVICE_TIMEOUT;
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 48, 0, 64, 7, 3, nic) == expected);
        CHECK(nic.state.producer == 7 && nic.state.completed == 3 && nic.state.cqConsumer == 3);
        CHECK(nic.state.error == expected && Nic::Be32(nic.ci) == 3 && nic.rings == 1);
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 48, 0, 64, 7, 3, nic) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(nic.rings == 1);
    }
    for (uint32_t kind = 0; kind < 6; ++kind) {
        Nic nic(v.queue);
        uint64_t localStride = 0, remoteStride = 64;
        uint32_t count = 48, window = 7;
        if (kind == 0) { count = 0; }
        if (kind == 1) { window = 0; }
        if (kind == 2) { remoteStride = UINT64_MAX; }
        if (kind == 3) { localStride = UINT64_MAX; }
        if (kind == 4) { nic.state.producer = nic.state.completed = UINT64_MAX - 47; }
        if (kind == 5) { nic.state.cqConsumer = UINT64_MAX - 47; }
        CHECK(Wire::PutStridedBatch(v, 0, 0, 64, count, localStride, remoteStride, window, 3, nic) ==
            TILEXR_RDMA_DEVICE_INVALID);
        CHECK(nic.events.empty() && nic.rings == 0);
    }
    // Reject a bad final slot without sending the otherwise valid prefix.
    Nic shortSpan(v.queue);
    CHECK(Wire::PutStridedBatch(v, 0, v.remoteBytes - 128, 64, 3, 0, 64, 7, 3,
        shortSpan) == TILEXR_RDMA_DEVICE_INVALID);
    CHECK(shortSpan.events.empty());
    // Minimal rings force a one-WR window even with a large request.
    v.queue.sqDepth = v.queue.cqDepth = 2;
    Nic minimal(v.queue);
    CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 48, 0, 64, UINT32_MAX, 3, minimal) == 0);
    CHECK(minimal.rings == 48 && minimal.peakOutstanding == 1);
    // A timeout in a later window retains earlier completions and all posted WRs.
    v.queue.sqDepth = v.queue.cqDepth = 8;
    Nic later(v.queue); later.fault = Fault::Invalid; later.faultAtSequence = 9;
    CHECK(Wire::PutStridedBatch(v, 0, 0, 64, 48, 0, 64, 7, 3, later) == TILEXR_RDMA_DEVICE_TIMEOUT);
    CHECK(later.state.producer == 14 && later.state.completed == 9 && later.state.cqConsumer == 9);
    CHECK(later.rings == 2 && Nic::Be32(later.ci) == 9);
}


static void TestSgeList(uint32_t mode)
{
    auto v = View(mode);
    v.queue.sqDepth = v.queue.cqDepth = 256;
    for (uint32_t n = 1; n <= 16; ++n) {
        Nic nic(v.queue);
        nic.gatherData = true;
        nic.localBase = v.localAddress; nic.remoteBase = v.remoteAddress;
        nic.local.resize(32768); nic.remote.assign(32768, 0xCD);
        for (size_t i = 0; i < nic.local.size(); ++i) { nic.local[i] = uint8_t((i * 7 + i / 31) % 251); }
        TileXRRDMADeviceSge sg[16] = {};
        std::vector<uint8_t> expected = nic.remote;
        uint32_t total = 0;
        for (uint32_t i = 0; i < n; ++i) {
            sg[i] = TileXRRDMADeviceSge{uint64_t(n - i) * 1024U + 3U, i * 7U + 1U, 0};
            std::memcpy(expected.data() + 37 + total, nic.local.data() + sg[i].localOffset, sg[i].bytes);
            total += sg[i].bytes;
        }
        // Mix variable-BB and single-BB WRs on the SAME QP. WR ordinal must
        // continue independently of BB PI, including the later strided batch.
        CHECK(Wire::Put(v, 0, 32000, 8, 2, nic) == 0);
        std::memcpy(expected.data() + 32000, nic.local.data(), 8);
        CHECK(Wire::PutSgeList(v, sg, n, 37, 2, nic) == 0);
        CHECK(nic.state.producer == 1U + Wire::WqeBlocks(n) && nic.state.completed == nic.state.producer);
        CHECK(nic.state.cqConsumer == 2 && nic.rings == 2 && Nic::Be32(nic.ci) == 2);
        CHECK(nic.postedSgeCounts.back() == n && nic.remote == expected);
        CHECK(Wire::PutStridedBatch(v, 0, 32128, 8, 3, 8, 16, 3, 2, nic) == 0);
        for (uint32_t i = 0; i < 3; ++i) { std::memcpy(expected.data() + 32128 + 16 * i, nic.local.data() + 8 * i, 8); }
        CHECK(nic.remote == expected && nic.state.cqConsumer == 5);
        CHECK(nic.state.completed == 4U + Wire::WqeBlocks(n));
        CHECK(nic.state.error == 0 && nic.peakOutstanding == 3);
    }
    // Independent golden two-SGE descriptor, uneven lengths and different
    // BB/WR sequences; unused padding must be zero, only final key has bit31.
    Nic bytes(v.queue); bytes.scratch.fill(0xCC);
    const TileXRRDMADeviceSge pair[] = {{16, 7, 0}, {8192, 33, 0}};
    Wire::EncodeWriteSgeList(bytes, v, 9, 6, pair, 2, 8, 40);
    const uint8_t golden[128] = {
        0x40,0xA4,0x20,0x04,0x10,0,0,0, 0,0,0,0,0,0,0,0,
        0x24,0,0,0,0,0,0,0x28, 0,0,0,0,0,0,0,0,
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x90, 0xAA,0xBB,0xCC,0xDD,0x12,0x34,0x56,0,
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x10, 0,0,0,7,0x12,0x34,0x56,0x78,
        0x88,0x77,0x66,0x55,0x44,0x33,0x42,0, 0,0,0,33,0x92,0x34,0x56,0x78
    };
    CHECK(std::memcmp(bytes.scratch.data(), golden, 128) == 0);
}

static void TestSgeFailures()
{
    auto v = View(); v.queue.sqDepth = v.queue.cqDepth = 16;
    const TileXRRDMADeviceSge valid[] = {{16, 7, 0}, {8192, 33, 0}};
    for (uint32_t kind = 0; kind < 14; ++kind) {
        auto view = v;
        TileXRRDMADeviceSge sg[2] = {valid[0], valid[1]};
        uint32_t count = 2, polls = 2;
        uint64_t remote = 0;
        Nic nic(view.queue);
        if (kind == 0) { count = 0; }
        if (kind == 1) { count = 17; }
        if (kind == 2) { polls = 0; }
        if (kind == 3) { sg[1].bytes = 0; }
        if (kind == 4) { sg[1].localOffset = UINT64_MAX; }
        if (kind == 5) { sg[1].bytes = 0x80000000U; }
        if (kind == 6) { sg[1].reserved = 1; }
        if (kind == 7) { remote = view.remoteBytes - 39; }
        if (kind == 8) { nic.state.producer = nic.state.completed = 14; }
        if (kind == 9) { nic.state.producer = nic.state.completed = UINT64_MAX; }
        if (kind == 10) { nic.state.cqConsumer = UINT64_MAX; }
        if (kind == 11) { view.localBytes = 1ULL << 32; sg[0].bytes = sg[1].bytes = 0x40000000U; }
        if (kind == 12) { sg[1].localOffset = view.localBytes - sg[1].bytes + 1; }
        if (kind == 13) { remote = UINT64_MAX; }
        CHECK(Wire::PutSgeList(view, sg, count, remote, polls, nic) == TILEXR_RDMA_DEVICE_INVALID);
        CHECK(nic.events.empty());
    }
    Nic nullList(v.queue);
    CHECK(Wire::PutSgeList(v, nullptr, 2, 0, 2, nullList) == TILEXR_RDMA_DEVICE_INVALID);
    CHECK(nullList.events.empty());
    Nic limited(v.queue); v.queue.maxSendSge = 1;
    CHECK(Wire::PutSgeList(v, valid, 2, 0, 2, limited) == TILEXR_RDMA_DEVICE_UNSUPPORTED);
    CHECK(limited.events.empty());
    v.queue.maxSendSge = 16;
    Nic busy(v.queue); busy.state.producer = 1;
    CHECK(Wire::PutSgeList(v, valid, 2, 0, 2, busy) == TILEXR_RDMA_DEVICE_BUSY);
    CHECK(busy.events.empty());
    for (Fault fault : {Fault::Invalid, Fault::Syndrome, Fault::Sequence}) {
        Nic nic(v.queue); nic.fault = fault;
        const uint32_t expected = fault == Fault::Invalid ? TILEXR_RDMA_DEVICE_TIMEOUT :
            fault == Fault::Syndrome ? TILEXR_RDMA_DEVICE_CQE_ERROR : TILEXR_RDMA_DEVICE_CQE_MISMATCH;
        CHECK(Wire::PutSgeList(v, valid, 2, 0, 2, nic) == expected);
        CHECK(nic.state.producer == 2 && nic.state.completed == 0 && nic.state.cqConsumer == 0);
        CHECK(nic.state.error == expected && nic.rings == 1 && Nic::Be32(nic.ci) == 0);
        CHECK(Wire::PutSgeList(v, valid, 2, 0, 2, nic) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(Wire::Put(v, 0, 0, 8, 2, nic) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(nic.rings == 1);
    }
    // WR admission can be smaller than either exported ring.
    v.queue.maxSendWr = 2;
    Nic wrLimit(v.queue);
    CHECK(Wire::PutStridedBatch(v, 0, 0, 8, 5, 0, 8, 16, 2, wrLimit) == 0);
    CHECK(wrLimit.rings == 3 && wrLimit.peakOutstanding == 2);
}

int main(int argc, char** argv)
{
    static_assert(std::is_standard_layout<TileXRRDMADeviceView>::value, "view is POD layout");
    static_assert(std::is_trivial<TileXRRDMADeviceView>::value, "view is trivial");
    TestSgeList(TILEXR_RDMA_DB_SOFT);
    TestSgeList(TILEXR_RDMA_DB_HARD);
    TestSgeFailures();
    if (argc == 2 && std::strcmp(argv[1], "--no-wrap") == 0) {
        TestNoWrapBatches(TILEXR_RDMA_DB_SOFT);
        TestNoWrapBatches(TILEXR_RDMA_DB_HARD);
        TestTracePreservesIO(TILEXR_RDMA_DB_SOFT);
        TestTracePreservesIO(TILEXR_RDMA_DB_HARD);
        std::puts("RDMA wire: depth4096, 48/1000/4096-WR batches and seven 1000-WR samples without ring wrap passed");
        return 0;
    }
    CHECK(argc == 1);
    TestGoldenEncoding();
    TestValidation();
    TestCompletionFailures();
    TestBatchFailures();
    for (uint32_t mode : {TILEXR_RDMA_DB_SOFT, TILEXR_RDMA_DB_HARD}) {
        TestWrapAndSharedState(mode);
        TestCounterRollover(mode);
        TestBatches(mode);
        TestNoWrapBatches(mode);
        TestTracePreservesIO(mode);
    }
    std::puts("RDMA wire: golden WRITE, both DB modes, SQ/CQ wraps, 16/24-bit rollover, bounded errors, "
        "48/1000-WR batches, strided bounds, partial failures and windowed doorbells passed");
}
