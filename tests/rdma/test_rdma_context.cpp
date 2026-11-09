/*
 * 功能：验证 WRITE context 的 view 缓存、UB state/pending、Restore/Save、背压和延迟 Wait，并检查可选 profiling 记录。
 * 调用关系：直接调用 tilexr_rdma_write.h；write_stubs 替代 Ascend C I/O，fixture 提供队列和 CQE。
 * 资源与生命周期：fixture 持有 UB 和模拟 queue buffer；本测试不执行 NIC 传输或验证设备时序。
 *
 * Purpose: Test WRITE context view caching, UB state/pending, Restore/Save, backpressure, deferred Wait and optional profiling records.
 * Calls: Exercises tilexr_rdma_write.h with write_stubs Ascend C substitutes and fixture-provided queues/CQEs.
 * Lifetime: Fixtures own UB and simulated queue buffers; no NIC transfer or device-ordering validation occurs.
 */
#include "kernel_operator.h"
#include "tilexr_rdma_write.h"
#if TILEXR_RDMA_PROFILE_ACTIVE
#include "tilexr/profiling/file.h"
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace W = TileXR::RdmaWire;
template <typename T> static uint64_t Address(T* p) { return reinterpret_cast<uint64_t>(p); }
struct Fixture {
    alignas(64) std::array<uint8_t, 256U * 64U> sq{}, cq{};
    alignas(64) TileXRRDMADeviceState state{};
    alignas(64) std::array<uint64_t, 128> pending{};
    alignas(64) std::array<uint64_t, 256> doorbell{};
    uint32_t sqDb = 0, cqDb = 0;
    TileXRRDMADeviceView view{};
    alignas(32) std::array<uint8_t, TileXR::RDMAWriteWorkScratchBytes(16)> work{};
    alignas(32) std::array<uint8_t, TileXR::RDMAWriteContextBytes(128)> ub{};
    Fixture() {
        view.version = TILEXR_RDMA_DEVICE_VERSION; view.structBytes = sizeof(view);
        view.localAddress = 0x100000000ULL; view.remoteAddress = 0x200000000ULL;
        view.localBytes = view.remoteBytes = 4096; view.localKey = 0x12345678; view.remoteKey = 0x87654321;
        auto& q = view.queue;
        q.sq = Address(sq.data()); q.cq = Address(cq.data()); q.state = Address(&state);
        q.pendingBatches = Address(pending.data()); q.pendingCapacity = 128;
        q.sqDbr = Address(&sqDb); q.cqDbr = Address(&cqDb); q.doorbell = Address(doorbell.data());
        q.doorbellBytes = sizeof(doorbell); q.qpn = 7; q.cqn = 107;
        q.sqDepth = q.cqDepth = 256; q.sqEntryBytes = q.cqEntryBytes = 64;
        q.maxSendWr = 128; q.maxSendSge = 16; q.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
        q.dbMode = TILEXR_RDMA_DB_HARD; q.sqDbValue = W::MakeSqDbValue(7, 1, 5, 0);
        auto& log = RdmaWriteTest::ActivityLog(); log = {};
        log.state = q.state; log.pending = q.pendingBatches; log.pendingBytes = sizeof(pending);
        log.view = Address(&view); log.viewBytes = sizeof(view);
    }
    AscendC::LocalTensor<uint8_t> Storage() { return {ub.data(), uint32_t(ub.size())}; }
    AscendC::LocalTensor<uint8_t> Scratch() { return {work.data(), uint32_t(work.size())}; }
    void Cqe(uint32_t slot, uint32_t lastWr) {
        cq[slot * 64] = 7; cq[slot * 64 + 7] = 0x24;
        cq[slot * 64 + 30] = uint8_t(lastWr); cq[slot * 64 + 31] = uint8_t(lastWr >> 8);
    }
};
static void RetainedStateAndRestore()
{
    Fixture f;
    TileXR::RDMAWriteContext first(f.Storage());
    CHECK(first.Restore(&f.view) == 0);
    const auto initial = RdmaWriteTest::ActivityLog();
    TileXR::RDMAWriteOptions o; o.count = 7;
    f.Cqe(0, 6); f.Cqe(1, 7);
    CHECK(TileXR::RDMAWrite(&f.view, 0, 0, 8, o, f.Scratch(), first) == 0);
    CHECK(first.State().producer == 7 && f.state.producer == 0 && f.pending[0] == 0);
    CHECK(RdmaWriteTest::ActivityLog().metadataReads == initial.metadataReads &&
        RdmaWriteTest::ActivityLog().metadataWrites == initial.metadataWrites);
    CHECK(first.Save() == 0 && f.state.producer == 7 && W::PendingBatchCount(f.state) == 1);
    auto other = f.view; other.localAddress += 0x10000; other.localKey += 1;
    alignas(32) std::array<uint8_t, TileXR::RDMAWriteContextBytes(128)> nextStorage{};
    TileXR::RDMAWriteContext second({nextStorage.data(), uint32_t(nextStorage.size())});
    CHECK(second.Restore(&other) == 0);
    const auto restored = RdmaWriteTest::ActivityLog();
    o.count = 1;
    TileXRRDMADeviceSge sge{16, 8, 0};
    CHECK(TileXR::RDMAWriteSgeMultiQp(&other, 1, 0, &sge, 1, 32, o, f.Scratch(), second) == 0);
    CHECK(TileXR::RDMAWait(&other, f.Scratch(), second) == 0);
    CHECK(second.State().producer == 8 && second.State().completed == 8 && f.state.completed == 0);
    CHECK(RdmaWriteTest::ActivityLog().metadataReads == restored.metadataReads &&
        RdmaWriteTest::ActivityLog().metadataWrites == restored.metadataWrites);
    CHECK(second.Save() == 0 && f.state.completed == 8 && f.state.cqConsumer == 2);
}
static void CapacityAndErrors()
{
    Fixture f; TileXR::RDMAWriteContext context(f.Storage()); CHECK(context.Restore(&f.view) == 0);
    f.Cqe(0, 63); f.Cqe(1, 127); f.Cqe(2, 128);
    TileXR::RDMAWriteOptions o; o.count = 129;
    CHECK(TileXR::RDMAWriteMultiQp(&f.view, 1, 0, 0, 0, 8, o, f.Scratch(), context) == 0);
    CHECK(context.State().completed == 64 && context.State().cqConsumer == 1);
    CHECK(TileXR::RDMAWait(&f.view, f.Scratch(), context) == 0);
    CHECK(context.State().completed == 129 && context.State().cqConsumer == 3);
    CHECK(f.state.producer == 0 && RdmaWriteTest::ActivityLog().metadataWrites == 0);
    o.count = 256;
    CHECK(TileXR::RDMAWrite(&f.view, 0, 0, 8, o, f.Scratch(), context) == TILEXR_RDMA_DEVICE_INVALID);
    Fixture bad; TileXR::RDMAWriteContext poisoned(bad.Storage()); CHECK(poisoned.Restore(&bad.view) == 0);
    o.count = 1;
    CHECK(TileXR::RDMAWrite(&bad.view, 0, 0, 8, o, bad.Scratch(), poisoned) == 0);
    bad.cq[7] = 0xF8;
    CHECK(TileXR::RDMAWait(&bad.view, bad.Scratch(), poisoned, 2) == TILEXR_RDMA_DEVICE_TIMEOUT);
    CHECK(poisoned.State().error == TILEXR_RDMA_DEVICE_TIMEOUT && bad.state.error == 0);
    CHECK(poisoned.Save() == 0 && bad.state.error == TILEXR_RDMA_DEVICE_TIMEOUT);
    CHECK(TileXR::RDMAWrite(&bad.view, 0, 0, 8, o, bad.Scratch(), poisoned) == TILEXR_RDMA_DEVICE_POISONED);
}
static void SingleWqeCallsAllEntries()
{
    for (uint32_t api = 0; api < 4; ++api) {
        Fixture f;
        TileXR::RDMAWriteContext context(f.Storage());
        CHECK(context.Restore(&f.view) == 0);
        TileXR::RDMAWriteOptions options; options.count = options.postBatch = 1;
        options.maxPolls = 2;
        const bool multipleSge = api == 1 || api == 3;
        for (uint32_t pass = 0; pass < 2; ++pass) {
            const auto initial = RdmaWriteTest::ActivityLog();
            const uint32_t calls = pass == 0 ? 32U : 48U;
            const uint32_t preceding = pass == 0 ? 0U : 32U;
            for (uint32_t i = 0; i < calls; ++i) {
                const uint64_t offset = uint64_t(i) * 16;
                TileXRRDMADeviceSge sges[2]{{offset, 8, 0}, {offset + 8, 8, 0}};
                uint32_t status = 0;
                switch (api) {
                    case 0: status = TileXR::RDMAWrite(&f.view, offset, offset, 8, options, f.Scratch(), context); break;
                    case 1: status = TileXR::RDMAWriteSge(&f.view, sges, 2, offset, options, f.Scratch(), context); break;
                    case 2: status = TileXR::RDMAWriteMultiQp(&f.view, 1, 0, offset, offset, 8, options, f.Scratch(), context); break;
                    case 3: status = TileXR::RDMAWriteSgeMultiQp(&f.view, 1, 0, sges, 2, offset, options, f.Scratch(), context); break;
                }
                CHECK(status == 0);
            }
            const auto posted = RdmaWriteTest::ActivityLog();
            CHECK(initial.viewInvalidations != 0 && posted.viewInvalidations == initial.viewInvalidations);
            CHECK(posted.rings - initial.rings == calls && posted.reads == initial.reads);
            CHECK(posted.metadataReads == initial.metadataReads && posted.metadataWrites == initial.metadataWrites);
            CHECK(context.State().producer == (preceding + calls) * W::WqeBlocks(multipleSge ? 2U : 1U));
            CHECK(context.State().completed == preceding * W::WqeBlocks(multipleSge ? 2U : 1U) &&
                context.State().cqConsumer == preceding && W::PendingBatchCount(context.State()) == calls);
            // Supply CQEs only after posting: neither warmup nor measurement may poll per call.
            for (uint32_t i = preceding; i < preceding + calls; ++i) { f.Cqe(i, i); }
            CHECK(TileXR::RDMAWait(&f.view, f.Scratch(), context, 2) == 0);
            CHECK(context.State().completed == context.State().producer && context.State().cqConsumer == preceding + calls);
            CHECK(RdmaWriteTest::ActivityLog().viewInvalidations == initial.viewInvalidations);
        }
        CHECK(context.Save() == 0 && f.state.completed == context.State().producer);
    }
}
static void CachedViewBinding()
{
    Fixture f;
    TileXR::RDMAWriteContext context(f.Storage()); CHECK(context.Restore(&f.view) == 0);
    auto other = f.view; other.localKey += 1;
#if TILEXR_RDMA_DEBUG
    TileXR::RDMAWriteOptions o;
    CHECK(TileXR::RDMAWrite(&other, 0, 0, 8, o, f.Scratch(), context) == TILEXR_RDMA_DEVICE_INVALID);
    CHECK(context.State().producer == 0);
#endif
    CHECK(context.Save() == 0 && context.Restore(&other) == 0);
    CHECK(context.View().localKey == other.localKey && context.Matches(&other));
    CHECK(!context.Matches(&f.view));
}
static void WrappedPendingAndParameterPolicy()
{
    Fixture f; f.view.queue.pendingCapacity = f.view.queue.maxSendWr = 3;
    for (uint32_t i = 0; i < 5; ++i) { f.Cqe(i, i); }
    TileXR::RDMAWriteContext first(f.Storage()); CHECK(first.Restore(&f.view) == 0);
    TileXR::RDMAWriteOptions o; o.count = 5; o.postBatch = 1;
    CHECK(TileXR::RDMAWrite(&f.view, 0, 0, 8, o, f.Scratch(), first) == 0);
    CHECK(first.State().cqConsumer == 2 && W::PendingBatchCount(first.State()) == 3);
    CHECK(first.Save() == 0);
    TileXR::RDMAWriteContext next(f.Storage()); CHECK(next.Restore(&f.view) == 0);
    CHECK(TileXR::RDMAWait(&f.view, f.Scratch(), next) == 0);
    CHECK(next.State().completed == 5 && next.State().cqConsumer == 5);
    Fixture policy; policy.view.localBytes = 1;
    TileXR::RDMAWriteContext context(policy.Storage()); CHECK(context.Restore(&policy.view) == 0);
    o = {};
    // CPU only: encode, but never execute, a caller-invalid range to prove the
    // Debug/Release validation switch. No NIC ever reads this fake address.
    const auto result = TileXR::RDMAWrite(&policy.view, 0, 0, 8, o, policy.Scratch(), context);
#if TILEXR_RDMA_DEBUG
    CHECK(result == TILEXR_RDMA_DEVICE_INVALID && context.State().producer == 0);
#else
    CHECK(result == 0 && context.State().producer == 1);
#endif
}
#if TILEXR_RDMA_PROFILE_ACTIVE
static void ProfileAllEntries(const char* exportPath)
{
    Fixture f; TileXR::RDMAWriteContext context(f.Storage());
    alignas(64) std::array<uint64_t, 256> timestamps{};
    alignas(64) std::array<uint32_t, 256> events{};
    alignas(64) std::array<uint32_t, 16> counts{}, dropped{}, states{};
    TileXR::Profiling::DeviceView descriptor{TileXR::Profiling::DEVICE_VIEW_ABI_VERSION, sizeof(TileXR::Profiling::DeviceView), 1, 1,
        256, 256, 16, 0, Address(timestamps.data()), Address(events.data()), Address(counts.data()),
        Address(dropped.data()), Address(states.data()), 0, 0};
    TileXR::Profiling::DirectWriter writer;
    writer.Initialize(reinterpret_cast<GM_ADDR>(&descriptor), 0);
    context.Trace().Attach(writer); context.Trace().Mark(TileXR::RDMAWriteEvent::TotalBegin);
    CHECK(context.Restore(&f.view) == 0);
    for (uint32_t i = 0; i < 4; ++i) { f.Cqe(i, i); }
    TileXR::RDMAWriteOptions o; TileXRRDMADeviceSge sges[2]{{0, 8, 0}, {8, 8, 0}};
    CHECK(TileXR::RDMAWrite(&f.view, 0, 0, 8, o, f.Scratch(), context) == 0);
    CHECK(TileXR::RDMAWriteSge(&f.view, sges, 2, 0, o, f.Scratch(), context) == 0);
    CHECK(TileXR::RDMAWriteMultiQp(&f.view, 1, 0, 0, 0, 8, o, f.Scratch(), context) == 0);
    CHECK(TileXR::RDMAWriteSgeMultiQp(&f.view, 1, 0, sges, 2, 0, o, f.Scratch(), context) == 0);
    CHECK(TileXR::RDMAWait(&f.view, f.Scratch(), context) == 0 && context.Save() == 0);
    context.Trace().Mark(TileXR::RDMAWriteEvent::TotalEnd);
    CHECK(events[0] == uint32_t(TileXR::RDMAWriteEvent::TotalBegin) && counts[0] == 0 &&
        states[0] == uint32_t(TileXR::Profiling::Completion::WRITING));
    writer.Publish();
    CHECK(states[0] == uint32_t(TileXR::Profiling::Completion::COMMITTED) && counts[0] > 40 && dropped[0] == 0);
    CHECK(events[counts[0] - 1U] == uint32_t(TileXR::RDMAWriteEvent::TotalEnd));
    for (auto event : {TileXR::RDMAWriteEvent::WriteBegin, TileXR::RDMAWriteEvent::SgeBegin,
        TileXR::RDMAWriteEvent::MultiBegin, TileXR::RDMAWriteEvent::SgeMultiBegin,
        TileXR::RDMAWriteEvent::ValidateBegin, TileXR::RDMAWriteEvent::EncodeBegin, TileXR::RDMAWriteEvent::WaitBegin}) {
        bool found = false;
        for (uint32_t i = 0; i < counts[0]; ++i) { found = found || events[i] == uint32_t(event); }
        CHECK(found);
    }
    if (exportPath != nullptr) {
        TileXR::Profiling::CaptureConfig config;
        config.logicalStreamCount = 1; config.recordsPerStream = 256; config.captureId[0] = 1;
        TileXR::Profiling::CaptureSnapshot snapshot;
        CHECK(TileXR::Profiling::BuildSnapshot(config,
            {counts.begin(), counts.end()}, {dropped.begin(), dropped.end()}, {states.begin(), states.end()},
            {timestamps.begin(), timestamps.end()}, {events.begin(), events.end()}, &snapshot));
        CHECK(TileXR::Profiling::ExportSnapshot(snapshot, exportPath));
    }
}
#endif
int main(int argc, char** argv)
{
    RetainedStateAndRestore(); CapacityAndErrors(); WrappedPendingAndParameterPolicy(); SingleWqeCallsAllEntries(); CachedViewBinding();
#if TILEXR_RDMA_PROFILE_ACTIVE
    ProfileAllEntries(argc > 1 ? argv[1] : nullptr);
#else
    (void)argc; (void)argv;
#endif
    std::puts("rdma_context: PASS (UB metadata, shared MR, save/restore, unchanged batches, capacity and timeout)");
}
