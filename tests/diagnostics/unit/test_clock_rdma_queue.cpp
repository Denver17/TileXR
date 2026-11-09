/*
 * 功能：模拟 NIC 消费 WQE 和生成 CQE，检查 clock queue 的发布顺序、source 有效期、完成计数及错误处理。
 * 调用关系：为 clock_rdma_queue.h 提供 Host IO model，并用 clock_rdma_profile.h 检查发送时间戳窗口。
 * 资源与生命周期：queue、payload 和远端内存均为测试容器，不分配真实 NPU/NIC 资源。
 * Purpose: Model NIC WQE/CQE processing to check publication order, source lifetime, completion counts and failures.
 * Calls: Supply Host IO to clock_rdma_queue.h and check publication timestamps with clock_rdma_profile.h.
 * Lifetime: Test containers hold queues, payloads and remote memory; no real NPU/NIC resources are allocated.
 */
#include "clock_rdma_queue.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Only the scalar timestamp observer is exercised here. Device publication and
// cycle-counter ordering still require the target CANN build and hardware.
static uint64_t modeledCycle = 0;
namespace AscendC {
static uint64_t GetSystemCycle() { return modeledCycle; }
}
#define __aicore__
#include "clock_rdma_profile.h"
#undef __aicore__

namespace Queue = TileXR::ClockRdmaQueue;
namespace Wire = TileXR::RdmaWire;

static void Check(bool ok, const char *expression, int line)
{
    if (!ok) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

static TileXRRDMADeviceView View(uint32_t mode, uint32_t sqDepth = 32, uint32_t cqDepth = 32)
{
    TileXRRDMADeviceView view {};
    view.version = TILEXR_RDMA_DEVICE_VERSION;
    view.structBytes = sizeof(view);
    view.localAddress = 0x10000000;
    view.remoteAddress = 0x20000000;
    view.localBytes = view.remoteBytes = 4096;
    view.localKey = 0x12345678;
    view.remoteKey = 0x34567890;
    auto &q = view.queue;
    q.sq = 0x30000000;
    q.cq = 0x40000000;
    q.sqDbr = 0x50000000;
    q.cqDbr = 0x50000040;
    q.doorbell = 0x60000000;
    q.state = 0x70000000;
    q.qpn = 0x12345;
    q.sqDbValue = Wire::MakeSqDbValue(q.qpn, 1, 5, 3);
    q.sqDepth = sqDepth;
    q.cqDepth = cqDepth;
    q.sqEntryBytes = q.cqEntryBytes = 64;
    q.dbMode = mode;
    q.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
    q.maxSendSge = 1;
    q.maxSendWr = 31;
    q.doorbellBytes = mode == TILEXR_RDMA_DB_HARD ? 2048 : 8;
    return view;
}

enum class Fault { None, Syndrome, ZeroSyndrome, Qpn, Sequence, Opcode, Receive, Merge, Invalid, Owner };

// The NIC consumes published WQE bytes, not the encoder's arguments. Source
// reads happen at CQ readiness, after Post has returned. No test-only encoder.
struct Nic {
    struct Request {
        uint64_t ordinal, source, destination;
        std::array<uint8_t, 64> bytes;
        uint32_t polls = 0, delay = 0;
        Fault fault = Fault::None;
        bool delivered = false;
    };

    explicit Nic(const TileXRRDMADeviceView &v) : view(v), sq(v.queue.sqDepth * 64),
        local(v.localBytes), remote(v.remoteBytes, 0xEE)
    {
        scratch.fill(0xCC);
    }

    void SetByte(uint32_t offset, uint8_t value) { scratch.at(offset) = value; }
    void SetWord(uint32_t offset, uint32_t value)
    {
        CHECK(offset % 4 == 0 && offset + 4 <= scratch.size());
        std::memcpy(scratch.data() + offset, &value, 4);
    }
    uint8_t GetByte(uint32_t offset) const { return scratch.at(offset); }
    TileXRRDMADeviceState LoadState() const { return state; }
    void StoreState(const TileXRRDMADeviceState &s) { state = s; events.push_back("state"); }

    static uint64_t Be(const uint8_t *p, uint32_t size)
    {
        uint64_t result = 0;
        for (uint32_t i = 0; i < size; ++i) { result = (result << 8) | p[i]; }
        return result;
    }
    void Le(uint32_t offset, uint32_t value, uint32_t size = 4)
    {
        for (uint32_t i = 0; i < size; ++i) { scratch.at(Wire::kCqeOffset + offset + i) = uint8_t(value >> (8 * i)); }
    }

    void Publish(uint64_t address, uint32_t offset, uint32_t bytes)
    {
        modeledCycle += 7;
        const auto &q = view.queue;
        CHECK(offset + bytes <= scratch.size());
        if (address >= q.sq && address < q.sq + sq.size()) {
            CHECK(bytes == 64 && address + bytes <= q.sq + sq.size());
            CHECK(state.producer > state.completed); // Reservation precedes ANY publication.
            CHECK(address == q.sq + (state.producer - (offset == 0 ? 1u : 0u)) * 64);
            std::memcpy(sq.data() + size_t(address - q.sq), scratch.data() + offset, bytes);
            events.push_back(offset == Wire::kStopOffset ? "stop" : "wqe");
        } else if (address == q.sqDbr) {
            CHECK(bytes == 4 && Be(scratch.data() + offset, 4) == state.producer);
            events.push_back("pi");
        } else {
            CHECK(address == q.cqDbr && bytes == 4);
            ci = Be(scratch.data() + offset, 4);
            events.push_back("ci");
            ++ciWrites;
        }
    }

    void Ring(uint64_t address, uint64_t value)
    {
        modeledCycle += 3;
        doorbellCycle = modeledCycle;
        const auto &q = view.queue;
        CHECK(events.size() >= 3);
        const size_t n = events.size();
        CHECK(events[n - 1] == (q.dbMode == TILEXR_RDMA_DB_SOFT ? "pi" : "wqe"));
        CHECK(events[n - (q.dbMode == TILEXR_RDMA_DB_SOFT ? 3 : 2)] == "stop");
        CHECK(address == q.doorbell + (q.dbMode == TILEXR_RDMA_DB_HARD ? (state.producer & 255) * 8 : 0));
        CHECK(value == (q.sqDbValue | (((state.producer >> 8) & 255) << 32)));
        const uint64_t sequence = state.producer - 1;
        const auto *wqe = sq.data() + sequence * 64;
        CHECK(wqe[0] == 0x40 && wqe[1] == 0xA4 && wqe[3] == 2);
        CHECK(Be(wqe + 16, 4) == 0x24000000); // Every write is signaled.
        CHECK(Be(wqe + 20, 4) == 64);
        CHECK(Be(wqe + 40, 4) == view.remoteKey);
        CHECK(Be(wqe + 56, 4) == 64);
        CHECK(Be(wqe + 60, 4) == (view.localKey | 0x80000000U));
        const uint64_t ordinal = state.cqConsumer + sequence - state.completed;
        CHECK(((Be(wqe + 2, 2) >> 12) & 3) == (ordinal & 3));
        CHECK(sq.at(size_t(state.producer * 64)) == 0xFF); // Stop visible before doorbell.
        Request request {};
        request.ordinal = ordinal;
        request.delay = readyDelay;
        CHECK(Be(wqe + 32, 8) >= view.remoteAddress);
        request.destination = Be(wqe + 32, 8) - view.remoteAddress;
        CHECK(request.destination + 64 <= remote.size());
        CHECK(Be(wqe + 48, 8) >= view.localAddress);
        request.source = Be(wqe + 48, 8) - view.localAddress;
        CHECK(request.source + 64 <= local.size());
        std::copy_n(local.begin() + request.source, 64, request.bytes.begin());
        requests.push_back(request);
        events.push_back("ring");
    }

    void ReadCqe(uint64_t address)
    {
        modeledCycle += pollCycles;
        CHECK(address >= view.queue.cq && (address - view.queue.cq) % 64 == 0);
        const uint64_t ordinal = (address - view.queue.cq) / 64;
        CHECK(ordinal < view.queue.cqDepth);
        auto it = std::find_if(requests.begin(), requests.end(), [ordinal](const Request &r) { return r.ordinal == ordinal; });
        CHECK(it != requests.end());
        auto &request = *it;
        for (uint32_t i = 0; i < 64; ++i) { scratch.at(Wire::kCqeOffset + i) = 0; }
        Le(4, 31U << 27); // INVALID CQE stays pending even when owner happens to match.
        ++reads;
        if (request.polls++ < request.delay) { return; }
        const Fault fault = request.fault;
        Le(0, (view.queue.qpn + (fault == Fault::Qpn ? 1 : 0)) | (fault == Fault::Owner ? 0x80000000U : 0));
        uint32_t opcode = 4;
        if (fault == Fault::Syndrome || fault == Fault::ZeroSyndrome) { opcode = 30; }
        if (fault == Fault::Opcode) { opcode = 8; }
        if (fault == Fault::Invalid) { opcode = 31; }
        Le(4, (opcode << 27) | (fault == Fault::Receive ? 0 : 1U << 26) |
            (fault == Fault::Merge ? 1U << 24 : 0) | 0xBEEF);
        Le(28, fault == Fault::Syndrome ? 0x13 : 0, 1);
        Le(30, uint32_t(ordinal + (fault == Fault::Sequence ? 1 : 0)), 2);
        if (fault == Fault::None && !request.delivered) {
            CHECK(std::equal(request.bytes.begin(), request.bytes.end(), local.begin() + request.source));
            std::copy_n(request.bytes.begin(), 64, remote.begin() + request.destination);
            request.delivered = true;
        }
    }

    void Stage(uint64_t offset, uint8_t value) { std::fill_n(local.begin() + offset, 64, value); }
    TileXRRDMADeviceView view;
    TileXRRDMADeviceState state {};
    std::array<uint8_t, TILEXR_RDMA_DEVICE_SCRATCH_BYTES> scratch;
    std::vector<uint8_t> sq, local, remote;
    std::vector<Request> requests;
    std::vector<std::string> events;
    uint64_t ci = 0;
    uint64_t pollCycles = 1000;
    uint64_t doorbellCycle = 0;
    uint32_t readyDelay = 0;
    uint32_t reads = 0, ciWrites = 0;
};

struct Observer {
    void Stage(Wire::BatchTraceStage stage) { stages.push_back(stage); }
    void WqeStart() { ++starts; }
    void WqeEncoded() { ++encoded; }
    void WqePublished() { ++published; }
    void PollStart() { ++polls; }
    void PollRead() { ++pollReads; }
    void PollDecoded(bool pending) { ++decoded; pendingReads += pending ? 1u : 0u; }
    std::vector<Wire::BatchTraceStage> stages;
    uint32_t starts = 0, encoded = 0, published = 0;
    uint32_t polls = 0, pollReads = 0, decoded = 0, pendingReads = 0;
};

static void TestPublicationTimestampExcludesCq(uint32_t mode)
{
    uint64_t expected = 0;
    for (bool deferred : {true, false}) {
        for (uint32_t delay : {0u, 2u}) {
            Nic nic(View(mode));
            Observer observer;
            TileXR::ClockRdmaPublicationTrace<Observer> trace(observer);
            modeledCycle = 1000;
            nic.pollCycles = delay == 0 ? 1 : 1000000;
            if (deferred) {
                CHECK(Queue::Post(nic.view, 0, 0, nic, trace) == TILEXR_RDMA_DEVICE_SUCCESS);
                nic.requests.back().delay = delay;
            } else {
                // Set readiness at ring time, before the synchronous path polls.
                nic.readyDelay = delay;
                CHECK(Wire::PutStridedBatch(nic.view, 0, 0, 64, 1, 0, 0, 1, 3, nic, trace) ==
                    TILEXR_RDMA_DEVICE_SUCCESS);
            }
            const uint64_t timestamp = trace.Midpoint();
            CHECK(timestamp > 1000 && timestamp <= nic.doorbellCycle);
            if (expected == 0) { expected = timestamp; }
            CHECK(timestamp == expected);
            CHECK(observer.starts == 1 && observer.encoded == 1 && observer.published == 1);
            CHECK(std::count(observer.stages.begin(), observer.stages.end(),
                Wire::BatchTraceStage::WqesPublished) == 1);
            CHECK(std::count(observer.stages.begin(), observer.stages.end(),
                Wire::BatchTraceStage::DoorbellIssued) == 1);
            if (deferred) {
                CHECK(nic.reads == 0 && observer.polls == 0);
                CHECK(Queue::Drain(nic.view, nic, 3) == TILEXR_RDMA_DEVICE_SUCCESS);
            } else {
                CHECK(observer.polls == delay + 1 && observer.pollReads == observer.polls &&
                    observer.decoded == observer.polls && observer.pendingReads == delay);
            }
            CHECK(nic.reads == delay + 1 && nic.state.producer == nic.state.completed);
            CHECK(trace.Midpoint() == timestamp && modeledCycle > timestamp);
        }
    }
}

static void Post(Nic &nic, uint64_t source, uint64_t destination)
{
    Wire::NoBatchTrace trace;
    CHECK(Queue::Post(nic.view, source, destination, nic, trace) == TILEXR_RDMA_DEVICE_SUCCESS);
}

static void TestFullDrainAndSourceSlots(uint32_t mode)
{
    Nic nic(View(mode, 16, 16));
    for (uint32_t i = 0; i < 15; ++i) {
        nic.Stage(i * 64, uint8_t(i + 1));
        Post(nic, i * 64, (14 - i) * 64);
        nic.requests.back().delay = i % 3;
        CHECK(nic.state.producer == i + 1 && nic.state.completed == 0 && nic.state.cqConsumer == 0);
    }
    CHECK(nic.reads == 0 && nic.ciWrites == 0);
    CHECK(std::all_of(nic.remote.begin(), nic.remote.end(), [](uint8_t b) { return b == 0xEE; }));
    CHECK(Queue::Drain(nic.view, nic, 3) == TILEXR_RDMA_DEVICE_SUCCESS);
    CHECK(nic.state.producer == 15 && nic.state.completed == 15 && nic.state.cqConsumer == 15);
    CHECK(nic.ci == 15 && nic.ciWrites == 1 && nic.reads == 30);
    for (const auto &request : nic.requests) {
        CHECK(request.delivered && request.polls == request.delay + 1);
        CHECK(std::equal(request.bytes.begin(), request.bytes.end(), nic.remote.begin() + request.destination));
    }
    const uint32_t reads = nic.reads;
    CHECK(Queue::Drain(nic.view, nic, 0) == TILEXR_RDMA_DEVICE_SUCCESS);
    CHECK(nic.reads == reads && nic.ciWrites == 1); // Empty drain never touches CQ/CI.
}

static void TestFreshOrdinalsAndReuse(uint32_t mode)
{
    Nic nic(View(mode));
    nic.state.producer = nic.state.completed = 5;
    nic.state.cqConsumer = 3; // SQ BB and CQ WR counters need not start equal.
    for (uint32_t round = 0; round < 3; ++round) {
        nic.Stage(0, uint8_t(round + 1));
        nic.Stage(64, uint8_t(round + 10));
        Post(nic, 0, 0);
        Post(nic, 64, 64);
        CHECK(nic.requests.back().ordinal == 4 + 2 * round);
        CHECK(Queue::Drain(nic.view, nic, 1) == TILEXR_RDMA_DEVICE_SUCCESS);
        CHECK(nic.state.producer == 7 + 2 * round && nic.state.completed == nic.state.producer);
        CHECK(nic.state.cqConsumer == 5 + 2 * round && nic.ci == nic.state.cqConsumer);
        CHECK(nic.remote[0] == round + 1 && nic.remote[64] == round + 10);
    }
}

static void TestFailuresRetainSources(uint32_t mode)
{
    for (Fault fault : {Fault::Syndrome, Fault::ZeroSyndrome, Fault::Qpn, Fault::Sequence,
                       Fault::Opcode, Fault::Receive, Fault::Merge, Fault::Invalid, Fault::Owner}) {
        Nic nic(View(mode));
        for (uint32_t i = 0; i < 3; ++i) { nic.Stage(i * 64, uint8_t(i)); Post(nic, i * 64, i * 64); }
        nic.requests[1].fault = fault;
        const uint32_t expected = fault == Fault::Syndrome || fault == Fault::ZeroSyndrome ? TILEXR_RDMA_DEVICE_CQE_ERROR :
            fault == Fault::Invalid || fault == Fault::Owner ? TILEXR_RDMA_DEVICE_TIMEOUT : TILEXR_RDMA_DEVICE_CQE_MISMATCH;
        CHECK(Queue::Drain(nic.view, nic, 3) == expected);
        CHECK(nic.state.producer == 3 && nic.state.completed == 1 && nic.state.cqConsumer == 1);
        CHECK(nic.state.error == expected && nic.state.syndrome == (fault == Fault::Syndrome ? 0x13U : 0U));
        CHECK(nic.ci == 1 && nic.ciWrites == 1 && !nic.requests[2].delivered);
        CHECK(nic.reads == (expected == TILEXR_RDMA_DEVICE_TIMEOUT ? 4U : 2U));
        const auto events = nic.events.size();
        const auto reads = nic.reads;
        Wire::NoBatchTrace trace;
        CHECK(Queue::Post(nic.view, 192, 192, nic, trace) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(Queue::Drain(nic.view, nic, 3) == TILEXR_RDMA_DEVICE_POISONED);
        CHECK(nic.events.size() == events && nic.reads == reads && nic.state.producer == 3);
    }
    Nic timeout(View(mode));
    Post(timeout, 0, 0);
    CHECK(Queue::Drain(timeout.view, timeout, 0) == TILEXR_RDMA_DEVICE_TIMEOUT);
    CHECK(timeout.reads == 0 && timeout.state.error == TILEXR_RDMA_DEVICE_TIMEOUT && timeout.state.producer == 1);
}

static void RejectedPost(Nic &nic, uint64_t source = 0, uint64_t destination = 0)
{
    const auto before = nic.state;
    const auto events = nic.events.size();
    Wire::NoBatchTrace trace;
    CHECK(Queue::Post(nic.view, source, destination, nic, trace) != TILEXR_RDMA_DEVICE_SUCCESS);
    CHECK(std::memcmp(&before, &nic.state, sizeof(before)) == 0 && nic.events.size() == events);
}

static void TestLimitsAndNoWrap(uint32_t mode)
{
    for (bool sqLimit : {true, false}) {
        Nic nic(View(mode, sqLimit ? 4 : 16, sqLimit ? 16 : 4));
        for (uint32_t i = 0; i < 3; ++i) { Post(nic, i * 64, i * 64); }
        RejectedPost(nic, 192, 192);
        CHECK(Queue::Drain(nic.view, nic, 1) == TILEXR_RDMA_DEVICE_SUCCESS);
        RejectedPost(nic, 192, 192); // Retiring work does not grant permission to wrap.
    }
    Nic wrLimit(View(mode));
    wrLimit.view.queue.maxSendWr = 2;
    Post(wrLimit, 0, 0); Post(wrLimit, 64, 64);
    RejectedPost(wrLimit, 128, 128);
    CHECK(Queue::Drain(wrLimit.view, wrLimit, 1) == TILEXR_RDMA_DEVICE_SUCCESS);
    Post(wrLimit, 128, 128);
    CHECK(Queue::Drain(wrLimit.view, wrLimit, 1) == TILEXR_RDMA_DEVICE_SUCCESS);
    for (uint32_t invalid = 0; invalid < 7; ++invalid) {
        Nic nic(View(mode));
        if (invalid == 0) { nic.state.reserved[0] = 1; }
        if (invalid == 1) { nic.state.completed = 1; }
        if (invalid == 2) { nic.state.producer = nic.view.queue.sqDepth; }
        if (invalid == 3) { nic.state.cqConsumer = nic.view.queue.cqDepth; }
        if (invalid == 4) { nic.view.version = 0; }
        if (invalid == 5) { nic.view.localBytes = 63; }
        if (invalid == 6) { nic.view.remoteBytes = 63; }
        RejectedPost(nic);
    }
    Nic offsets(View(mode));
    RejectedPost(offsets, UINT64_MAX, 0);
    RejectedPost(offsets, 0, UINT64_MAX);
    RejectedPost(offsets, offsets.view.localBytes - 63, 0);
    RejectedPost(offsets, 0, offsets.view.remoteBytes - 63);
    for (uint32_t invalid = 0; invalid < 5; ++invalid) {
        Nic nic(View(mode));
        if (invalid == 0) { nic.state.completed = 1; }
        if (invalid == 1) { nic.state.producer = nic.view.queue.sqDepth; }
        if (invalid == 2) { nic.state.cqConsumer = nic.view.queue.cqDepth; }
        if (invalid == 3) { nic.state.producer = 3; nic.state.cqConsumer = nic.view.queue.cqDepth - 2; }
        if (invalid == 4) { nic.state.reserved[0] = 2; }
        CHECK(Queue::Drain(nic.view, nic, 1) == TILEXR_RDMA_DEVICE_INVALID);
        CHECK(nic.events.empty() && nic.reads == 0);
    }
}

int main()
{
    for (uint32_t mode : {TILEXR_RDMA_DB_HARD, TILEXR_RDMA_DB_SOFT}) {
        TestFullDrainAndSourceSlots(mode);
        TestFreshOrdinalsAndReuse(mode);
        TestFailuresRetainSources(mode);
        TestLimitsAndNoWrap(mode);
        TestPublicationTimestampExcludesCq(mode);
    }
    std::puts("PASS clock RDMA queue: hard/soft publication, source lifetime, full CQ drain, freshness, errors, no-wrap limits and CQ-independent timestamps");
}
