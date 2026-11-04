#include "kernel_operator.h" // The one CPU fixture; the production adapter is compiled below.
#include "tilexr_rdma_write.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace W = TileXR::RdmaWire;
using TileXR::RDMAWriteOptions;
using Scratch = AscendC::LocalTensor<uint8_t>;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

// Host/kernel ABI examples remain the existing V2 PODs. The options are a
// source-level convenience and introduce no fields into those exported views.
static_assert(sizeof(TileXRRDMADeviceView) == 152U, "V2 view ABI");
static_assert(offsetof(TileXRRDMADeviceView, queue) == 48U, "V2 queue offset");
static_assert(sizeof(TileXRRDMADeviceQueue) == 104U, "queue ABI");
static_assert(sizeof(TileXRRDMADeviceSge) == 16U, "SGE ABI");
static_assert(std::is_standard_layout<RDMAWriteOptions>::value, "value options");
static_assert(std::is_trivially_copyable<RDMAWriteOptions>::value, "copyable options");
static_assert(TileXR::RDMAWriteScratchBytes() == W::kPipelineScratchBytes, "default scratch");
static_assert(TileXR::RDMAWriteScratchBytes(16) == W::kPipelineSgeScratchBytes, "SGE scratch");

using SingleSignature = uint32_t (*)(const TileXRRDMADeviceView*, uint64_t, uint64_t, uint32_t,
    const RDMAWriteOptions&, const Scratch&);
using SgeSignature = uint32_t (*)(const TileXRRDMADeviceView*, const TileXRRDMADeviceSge*, uint32_t,
    uint64_t, const RDMAWriteOptions&, const Scratch&);
using MultiSignature = uint32_t (*)(const TileXRRDMADeviceView*, uint32_t, uint32_t,
    uint64_t, uint64_t, uint32_t, const RDMAWriteOptions&, const Scratch&);
using SgeMultiSignature = uint32_t (*)(const TileXRRDMADeviceView*, uint32_t, uint32_t,
    const TileXRRDMADeviceSge*, uint32_t, uint64_t, const RDMAWriteOptions&, const Scratch&);
static_assert(std::is_same<decltype(&TileXR::RDMAWrite), SingleSignature>::value, "WRITE signature");
static_assert(std::is_same<decltype(&TileXR::RDMAWriteSge), SgeSignature>::value, "SGE signature");
static_assert(std::is_same<decltype(&TileXR::RDMAWriteMultiQp), MultiSignature>::value, "multi signature");
static_assert(std::is_same<decltype(&TileXR::RDMAWriteSgeMultiQp), SgeMultiSignature>::value, "SGE multi signature");

namespace {
template <typename T> uint64_t Address(T* value) { return reinterpret_cast<uint64_t>(value); }

struct Qp {
    static constexpr uint32_t depth = 256U;
    alignas(64) std::array<uint8_t, depth * 64U> sq;
    alignas(64) std::array<uint8_t, depth * 64U> cq{};
    alignas(64) TileXRRDMADeviceState state{};
    alignas(8) std::array<uint64_t, 256> doorbell{};
    uint32_t sqDbr = 0U, cqDbr = 0U;
    TileXRRDMADeviceView view{};

    explicit Qp(uint32_t qpn = 7U)
    {
        sq.fill(0xCC);
        view.version = TILEXR_RDMA_DEVICE_VERSION;
        view.structBytes = sizeof(view);
        view.localAddress = 0x100000000ULL;
        view.remoteAddress = 0x200000000ULL;
        view.localBytes = view.remoteBytes = 4096U;
        view.localKey = 0x12345678U; view.remoteKey = 0x87654321U;
        auto& q = view.queue;
        q.sq = Address(sq.data()); q.cq = Address(cq.data());
        q.state = Address(&state); q.doorbell = Address(doorbell.data());
        q.sqDbr = Address(&sqDbr); q.cqDbr = Address(&cqDbr);
        q.doorbellBytes = sizeof(doorbell);
        q.qpn = qpn; q.cqn = qpn + 100U;
        q.sqDepth = q.cqDepth = depth; q.sqEntryBytes = q.cqEntryBytes = 64U;
        q.maxSendSge = TILEXR_RDMA_DEVICE_MAX_SGE; q.maxSendWr = 128U;
        q.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
        q.dbMode = TILEXR_RDMA_DB_HARD;
        q.sqDbValue = W::MakeSqDbValue(qpn, 1, 5, 0);
    }

    // Supply CQ snapshots only; no second WQE encoder or NIC implementation.
    void Completions(uint32_t count, uint32_t batch)
    {
        uint64_t wr = state.reserved[0] == 0U ? state.cqConsumer : state.reserved[1];
        uint32_t slot = static_cast<uint32_t>(state.cqConsumer);
        for (uint32_t sent = 0U; sent < count; ) {
            const uint32_t n = count - sent < batch ? count - sent : batch;
            CHECK(n != 0U && slot < depth);
            const uint32_t base = slot++ * 64U;
            for (uint32_t i = 0; i < 4U; ++i) {
                cq.at(base + i) = uint8_t(view.queue.qpn >> (8U * i));
            }
            cq.at(base + 7U) = 0x24U; // LE WRITE CQE, send completion, owner=0.
            const uint64_t last = wr + n - 1U;
            cq.at(base + 30U) = uint8_t(last);
            cq.at(base + 31U) = uint8_t(last >> 8);
            wr += n; sent += n;
        }
    }

    uint64_t Be(uint32_t offset, uint32_t bytes) const
    {
        uint64_t value = 0U;
        for (uint32_t i = 0; i < bytes; ++i) { value = (value << 8) | sq.at(offset + i); }
        return value;
    }
};

struct Ub {
    alignas(32) std::array<uint8_t, W::kPipelineSgeScratchBytes + 32U> bytes;
    Ub() { bytes.fill(0xA5); }
    Scratch Tensor(uint32_t numSge = 1U) { return Scratch(bytes.data(), TileXR::RDMAWriteScratchBytes(numSge)); }
    void CheckGuard(uint32_t numSge) const {
        for (uint32_t i = TileXR::RDMAWriteScratchBytes(numSge); i < bytes.size(); ++i) {
            CHECK(bytes[i] == 0xA5);
        }
    }
};

void NoIO(uint32_t status, uint32_t expected = TILEXR_RDMA_DEVICE_INVALID)
{
    CHECK(status == expected);
    const auto& log = RdmaWriteTest::ActivityLog();
    CHECK(log.publications == 0U && log.rings == 0U && log.reads == 0U);
}

void DefaultsAndScratch()
{
    constexpr RDMAWriteOptions o{};
    static_assert(o.count == 1U && o.localStride == 0U && o.remoteStride == 0U, "default spans");
    static_assert(o.postBatch == 64U && o.maxInFlight == 128U && o.cyclicSlots == 0U &&
        o.maxPolls == 1000000U, "default pipeline");
    CHECK(TileXR::RDMAWriteScratchBytes(0) == 0U);
    CHECK(TileXR::RDMAWriteScratchBytes(17) == 0U);
    CHECK(TileXR::RDMAWriteScratchBytes(UINT32_MAX) == 0U);
    for (uint32_t n = 1; n <= 16; ++n) {
        CHECK(TileXR::RDMAWriteScratchBytes(n) == (n == 1U ? W::kPipelineScratchBytes : W::kPipelineSgeScratchBytes));
        CHECK(TileXR::RDMAWriteScratchBytes(n) % TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT == 0U);
    }
}

void SingleAndOptions()
{
    for (uint32_t batch : {1U, 7U, 64U}) {
        for (uint32_t count : {1U, 64U, 65U, 128U, 129U}) {
            Qp q; Ub ub; RDMAWriteOptions o;
            o.count = count; o.postBatch = batch;
            o.localStride = 17U; o.remoteStride = 23U;
            q.Completions(count, batch);
            RdmaWriteTest::ActivityLog() = {};
            CHECK(TileXR::RDMAWrite(&q.view, 3, 5, 8, o, ub.Tensor()) == 0U);
            CHECK(q.state.producer == count && q.state.completed == count);
            CHECK(q.state.cqConsumer == (count + batch - 1U) / batch);
            CHECK(RdmaWriteTest::ActivityLog().rings == q.state.cqConsumer);
            for (uint32_t i = 0; i < count; ++i) {
                CHECK(q.Be(i * 64U + 48U, 8) == q.view.localAddress + 3U + i * 17U);
                CHECK(q.Be(i * 64U + 32U, 8) == q.view.remoteAddress + 5U + i * 23U);
                CHECK(q.Be(i * 64U + 56U, 4) == 8U);
            }
            ub.CheckGuard(1);
        }
    }
    for (uint32_t kind = 0; kind < 6; ++kind) {
        Qp q; Ub ub; RDMAWriteOptions o;
        o.count = 129U;
        if (kind == 0U) { o.count = 0U; }
        if (kind == 1U) { o.postBatch = 0U; }
        if (kind == 2U) { o.postBatch = 65U; }
        if (kind == 3U) { o.maxInFlight = 0U; }
        if (kind == 4U) { o.maxInFlight = 127U; }
        if (kind == 5U) { o.maxPolls = 0U; }
        RdmaWriteTest::ActivityLog() = {};
        NoIO(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, ub.Tensor()));
        CHECK(q.state.producer == 0U && q.sq[0] == 0xCC);
    }
    Qp q; Ub ub; RDMAWriteOptions o;
    o.count = 1; o.maxInFlight = 1; // A full two-batch window is unnecessary for one WR.
    q.Completions(1, 64);
    CHECK(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, ub.Tensor()) == 0U);
}

void GatherAndCyclic()
{
    for (uint32_t n = 1; n <= 16; ++n) {
        Qp q; Ub ub; RDMAWriteOptions o;
        std::array<TileXRRDMADeviceSge, 16> sges{};
        uint32_t total = 0U;
        for (uint32_t i = 0; i < n; ++i) {
            sges[i] = {uint64_t(16U - i) * 64U, i + 1U, 0U};
            total += i + 1U;
        }
        o.count = 7; o.postBatch = 3; o.cyclicSlots = 3;
        o.localStride = 128; o.remoteStride = 256;
        q.Completions(o.count, o.postBatch);
        CHECK(TileXR::RDMAWriteSge(&q.view, sges.data(), n, 9, o, ub.Tensor(n)) == 0U);
        const uint32_t blocks = (48U + n * 16U + 63U) / 64U;
        CHECK(q.state.producer == o.count * blocks && q.state.completed == q.state.producer);
        for (uint32_t wr = 0; wr < o.count; ++wr) {
            const uint32_t base = wr * blocks * 64U;
            const uint32_t slot = (wr + 1U) % 3U;
            CHECK(q.Be(base + 32U, 8) == q.view.remoteAddress + 9U + slot * 256U);
            CHECK(q.Be(base + 20U, 4) == total);
            for (uint32_t i = 0; i < n; ++i) {
                CHECK(q.Be(base + 48U + i * 16U, 8) ==
                    q.view.localAddress + sges[i].localOffset + slot * 128U);
            }
        }
        // The next call continues the QP's WR ordinal for cyclic addressing.
        q.Completions(1, o.postBatch); o.count = 1;
        CHECK(TileXR::RDMAWriteSge(&q.view, sges.data(), n, 9, o, ub.Tensor(n)) == 0U);
        CHECK(q.Be(7U * blocks * 64U + 32U, 8) == q.view.remoteAddress + 9U + 2U * 256U);
        ub.CheckGuard(n);
    }
    Qp q; Ub ub; RDMAWriteOptions o;
    o.count = 3; // Default zero strides reuse exactly the same source/destination.
    q.view.queue.dbMode = TILEXR_RDMA_DB_SOFT;
    q.Completions(3, 64);
    CHECK(TileXR::RDMAWrite(&q.view, 3, 5, 8, o, ub.Tensor()) == 0U);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(q.Be(i * 64U + 48U, 8) == q.view.localAddress + 3U);
        CHECK(q.Be(i * 64U + 32U, 8) == q.view.remoteAddress + 5U);
    }
    CHECK(q.sqDbr == W::BigEndianWord(3U) && q.cqDbr == W::BigEndianWord(1U));
}

void BoundsAndSgeValidation()
{
    for (uint32_t n : {1U, 2U, 16U}) {
        for (uint32_t kind = 0; kind < 17; ++kind) {
            Qp q; Ub ub; RDMAWriteOptions o;
            std::array<TileXRRDMADeviceSge, 16> sges{};
            for (auto& sge : sges) { sge = {0U, 8U, 0U}; }
            uint64_t remote = 0U;
            if (kind == 0U) { sges[n - 1].reserved = 1U; }
            if (kind == 1U) { sges[n - 1].bytes = 0U; }
            if (kind == 2U) { sges[n - 1].bytes = 0x80000000U; }
            if (kind == 3U) { sges[n - 1].localOffset = UINT64_MAX; }
            if (kind == 4U) { sges[n - 1].localOffset = q.view.localBytes - 7U; }
            if (kind == 5U) { remote = UINT64_MAX; }
            if (kind == 6U) { remote = q.view.remoteBytes - n * 8U + 1U; }
            if (kind == 7U) { o.count = 2; o.localStride = UINT64_MAX; }
            if (kind == 8U) { o.count = 2; o.remoteStride = UINT64_MAX; }
            if (kind == 9U) { o.cyclicSlots = UINT32_MAX; o.localStride = UINT64_MAX; }
            if (kind == 10U) { o.count = 128; q.view.queue.maxSendWr = 127; }
            if (kind == 11U) { q.state.producer = q.state.completed = Qp::depth - 1U; }
            if (kind == 12U) { q.state.cqConsumer = Qp::depth - 1U; }
            if (kind == 13U) { o.count = UINT32_MAX; }
            if (kind == 14U) { o.cyclicSlots = UINT32_MAX; o.remoteStride = UINT64_MAX; }
            if (kind == 15U) { o.count = 129; o.postBatch = UINT32_MAX; }
            if (kind == 16U) { o.maxPolls = 0; }
            RdmaWriteTest::ActivityLog() = {};
            NoIO(TileXR::RDMAWriteSge(&q.view, sges.data(), n, remote, o, ub.Tensor(n)));
        }
    }
    Qp q; Ub ub; RDMAWriteOptions o; TileXRRDMADeviceSge sge{4088U, 8U, 0U};
    RdmaWriteTest::ActivityLog() = {};
    NoIO(TileXR::RDMAWriteSge(&q.view, nullptr, 1, 0, o, ub.Tensor()));
    NoIO(TileXR::RDMAWriteSge(&q.view, &sge, 0, 0, o, ub.Tensor()));
    NoIO(TileXR::RDMAWriteSge(&q.view, &sge, 17, 0, o, ub.Tensor()));
    NoIO(TileXR::RDMAWriteSge(&q.view, &sge, UINT32_MAX, 0, o, ub.Tensor()));
    NoIO(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, Scratch(ub.bytes.data(), TileXR::RDMAWriteScratchBytes() - 1U)));
    NoIO(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, Scratch(ub.bytes.data() + 1, TileXR::RDMAWriteScratchBytes())));
    TileXRRDMADeviceSge pair[2] = {{0, 8, 0}, {8, 8, 0}};
    NoIO(TileXR::RDMAWriteSge(&q.view, pair, 2, 0, o, ub.Tensor()));
    q.Completions(1, 64);
    CHECK(TileXR::RDMAWriteSge(&q.view, &sge, 1, 4088, o, ub.Tensor()) == 0U); // Exact MR boundary.

    Qp large;
    large.view.localBytes = large.view.remoteBytes = 1ULL << 32;
    TileXRRDMADeviceSge overflow[2] = {{0, 0x40000000U, 0}, {0, 0x40000000U, 0}};
    RdmaWriteTest::ActivityLog() = {};
    NoIO(TileXR::RDMAWriteSge(&large.view, overflow, 2, 0, o, ub.Tensor(2))); // Gather sum reaches 2^31.
}

void MultiQp()
{
    Ub ub; RDMAWriteOptions o; TileXRRDMADeviceSge sge{17, 8, 0};
    std::array<Qp, TILEXR_RDMA_MAX_QPS> queues;
    std::array<TileXRRDMADeviceView, TILEXR_RDMA_MAX_QPS> views;
    for (uint32_t i = 0; i < queues.size(); ++i) {
        queues[i].view.queue.qpn = i + 1U;
        queues[i].view.queue.sqDbValue = W::MakeSqDbValue(i + 1U, 1, 5, 0);
        views[i] = queues[i].view;
        queues[i].Completions(1, 64);
    }
    for (uint32_t kind = 0; kind < 7; ++kind) {
        const TileXRRDMADeviceView* address = views.data();
        uint32_t count = TILEXR_RDMA_MAX_QPS, index = 0U;
        if (kind == 0U) { address = nullptr; }
        if (kind == 1U) { address = reinterpret_cast<const TileXRRDMADeviceView*>(Address(views.data()) + 1U); }
        if (kind == 2U) { count = 0U; }
        if (kind == 3U) { count = TILEXR_RDMA_MAX_QPS + 1U; }
        if (kind == 4U) { index = count; }
        if (kind == 5U) { count = UINT32_MAX; index = UINT32_MAX; }
        // The selected view alone fits, but the declared full array overflows.
        if (kind == 6U) { address = reinterpret_cast<const TileXRRDMADeviceView*>(UINT64_MAX - 159U); }
        RdmaWriteTest::ActivityLog() = {};
        NoIO(TileXR::RDMAWriteMultiQp(address, count, index, 0, 0, 8, o, ub.Tensor()));
        NoIO(TileXR::RDMAWriteSgeMultiQp(address, count, index, &sge, 1, 0, o, ub.Tensor()));
    }
    // A call must touch only its explicit QP, regardless of its position.
    for (uint32_t selected : {15U, 0U, 7U}) {
        CHECK(TileXR::RDMAWriteMultiQp(views.data(), 16, selected, 3, 5, 8, o, ub.Tensor()) == 0U);
        CHECK(queues[selected].state.completed == 1U);
    }
    for (uint32_t i = 0; i < queues.size(); ++i) {
        CHECK(queues[i].state.producer == ((i == 15U || i == 0U || i == 7U) ? 1U : 0U));
    }
    CHECK(TileXR::RDMAWriteSgeMultiQp(views.data(), 16, 14, &sge, 1, 9, o, ub.Tensor()) == 0U);
    CHECK(queues[14].Be(48, 8) == queues[14].view.localAddress + 17U);
    TileXRRDMADeviceSge pair[2] = {{33, 8, 0}, {17, 16, 0}};
    CHECK(TileXR::RDMAWriteSgeMultiQp(views.data(), 16, 13, pair, 2, 9, o, ub.Tensor(2)) == 0U);
    CHECK(queues[13].state.completed == 2U && queues[13].Be(20, 4) == 24U);
    Qp single; single.Completions(1, 64);
    CHECK(TileXR::RDMAWriteMultiQp(&single.view, 1, 0, 0, 0, 8, o, ub.Tensor()) == 0U);
}

void StatusPropagation()
{
    for (uint32_t kind = 0; kind < 5; ++kind) {
        Qp q; Ub ub; RDMAWriteOptions o;
        if (kind == 0U) { q.view.version = 1U; }
        if (kind == 1U) { q.state.error = TILEXR_RDMA_DEVICE_TIMEOUT; }
        if (kind == 2U) { q.state.producer = 1U; }
        if (kind == 3U) { q.state.reserved[0] = 123U; }
        if (kind == 4U) { q.view.queue.maxSendSge = 1U; }
        const uint32_t expected = kind == 1U ? TILEXR_RDMA_DEVICE_POISONED :
            kind == 2U ? TILEXR_RDMA_DEVICE_BUSY : TILEXR_RDMA_DEVICE_UNSUPPORTED;
        RdmaWriteTest::ActivityLog() = {};
        TileXRRDMADeviceSge pair[2] = {{0, 8, 0}, {8, 8, 0}};
        NoIO(TileXR::RDMAWriteSge(&q.view, pair, 2, 0, o, ub.Tensor(2)), expected);
    }
    Qp q; Ub ub; RDMAWriteOptions o;
    o.maxPolls = 3;
    q.cq[7] = 0xF8U; // Pending opcode 31: exhaust exactly the configured poll bound.
    RdmaWriteTest::ActivityLog() = {};
    CHECK(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, ub.Tensor()) == TILEXR_RDMA_DEVICE_TIMEOUT);
    CHECK(RdmaWriteTest::ActivityLog().reads == 3U);
    CHECK(q.state.error == TILEXR_RDMA_DEVICE_TIMEOUT && q.state.producer == 1U && q.state.completed == 0U);
    RdmaWriteTest::ActivityLog() = {};
    NoIO(TileXR::RDMAWrite(&q.view, 0, 0, 8, o, ub.Tensor()), TILEXR_RDMA_DEVICE_POISONED);
}
} // namespace

int main()
{
    DefaultsAndScratch();
    SingleAndOptions();
    GatherAndCyclic();
    BoundsAndSgeValidation();
    MultiQp();
    StatusPropagation();
    std::puts("rdma_write_options: PASS (CPU adapter, options, scratch, SGE, QP routing, bounds, ABI, errors)");
}
