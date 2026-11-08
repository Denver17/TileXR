/*
 * 功能：验证 WRITE pipeline 的编码、发布顺序、容量背压、跨调用 pending、显式 Wait 和错误状态。
 * 调用关系：直接运行 tilexr_rdma_pipeline.h；CPU Model 保存 SQ/state/pending，并按提交的 WQE 生成 CQ 快照。
 * 边界：Model 检查调用与数据内容，不模拟 MTE、cache 或真实 NIC 的执行时序。
 *
 * Purpose: Test WRITE pipeline encoding, publication order, capacity backpressure, cross-call pending state, explicit Wait and errors.
 * Calls: Runs tilexr_rdma_pipeline.h with a CPU Model that holds SQ/state/pending and derives CQ snapshots from submitted WQEs.
 * Scope: The model checks calls and contents, not MTE, cache or real NIC execution timing.
 */
#include "tilexr_rdma_pipeline.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace W = TileXR::RdmaWire;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

static TileXRRDMADeviceView View(uint32_t mode)
{
    TileXRRDMADeviceView v{};
    v.version = TILEXR_RDMA_DEVICE_VERSION; v.structBytes = sizeof(v);
    v.localAddress = 0x100000000ULL; v.remoteAddress = 0x200000000ULL;
    v.localBytes = v.remoteBytes = 1ULL << 31;
    v.localKey = 0x12345678; v.remoteKey = 0x87654321;
    auto& q = v.queue;
    q.sq = 0x100000; q.cq = 0x200000; q.sqDbr = 0x300000; q.cqDbr = 0x300040;
    q.doorbell = 0x400000; q.state = 0x500000; q.qpn = 7;
    q.sqDepth = q.cqDepth = 32768; q.sqEntryBytes = q.cqEntryBytes = 64;
    q.maxSendSge = 1; q.maxSendWr = 4096; q.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
    q.dbMode = mode; q.doorbellBytes = 2048;
    q.pendingBatches = 0x600000;
    q.pendingCapacity = W::PipelineRecordCapacity(q.sqDepth, q.cqDepth, q.maxSendWr);
    q.sqDbValue = W::MakeSqDbValue(q.qpn, 1, 5, 0);
    return v;
}

struct Model {
    TileXRRDMADeviceView v;
    TileXRRDMADeviceState state{};
    std::array<uint8_t, W::kPipelineSgeScratchBytes> scratch{};
    std::vector<uint8_t> sq;
    std::vector<uint64_t> completions, locals, remotes;
    std::vector<uint64_t> pending;
    std::vector<std::vector<uint64_t>> segmentLocals;
    uint64_t nextSq = 0, nextWr = 0, peak = 0;
    uint32_t publications = 0, rings = 0, reads = 0, signalCount = 0;
    uint32_t failCq = UINT32_MAX;
    bool timeout = false, mismatch = false;
    explicit Model(const TileXRRDMADeviceView& view) : v(view), sq(view.queue.sqDepth * 64U, 0xCC),
        pending(view.queue.pendingCapacity) {}
    void SetByte(uint32_t off, uint8_t value) { scratch.at(off) = value; }
    uint8_t GetByte(uint32_t off) const { return scratch.at(off); }
    void SetWord(uint32_t off, uint32_t value) {
        CHECK(off % 4U == 0U);
        for (uint32_t i = 0; i < 4; ++i) { SetByte(off + i, uint8_t(value >> (i * 8U))); }
    }
    TileXRRDMADeviceState LoadState() const { return state; }
    void StoreState(const TileXRRDMADeviceState& s) { state = s; }
    uint64_t LoadPending(uint64_t address) const { return pending.at((address - v.queue.pendingBatches) / 8U); }
    void StorePending(uint64_t address, uint64_t value) { pending.at((address - v.queue.pendingBatches) / 8U) = value; }
    void Publish(uint64_t address, uint32_t off, uint32_t bytes) {
        CHECK(off + bytes <= scratch.size());
        if (address >= v.queue.sq && address < v.queue.sq + sq.size()) {
            CHECK(address + bytes <= v.queue.sq + sq.size());
            if (off == W::kPipelineWqeOffset && state.producer-nextSq > 1) {
                CHECK(std::memcmp(sq.data()+address-v.queue.sq+64,
                    scratch.data()+off+64, (state.producer-nextSq-1)*64) == 0);
            }
            std::memcpy(sq.data() + address - v.queue.sq, scratch.data() + off, bytes);
            if (off == W::kPipelineWqeOffset) { ++publications; CHECK(bytes <= 4096); }
        } else {
            CHECK((address == v.queue.cqDbr || address == v.queue.sqDbr) && bytes == 4);
        }
    }
    uint64_t Be(uint64_t off, uint32_t bytes) const {
        uint64_t x = 0;
        for (uint32_t i = 0; i < bytes; ++i) { x = (x << 8) | sq.at(off + i); }
        return x;
    }
    void Ring(uint64_t address, uint64_t value) {
        CHECK(address == W::SqDbAddress(v.queue, state.producer));
        CHECK(value == W::SqDbValue(v.queue.sqDbValue, state.producer));
        CHECK(sq.at(state.producer * 64U) == 0xFF);
        while (nextSq < state.producer) {
            const uint64_t off = nextSq * 64U;
            CHECK(sq.at(off) == 0x40);
            CHECK(sq.at(off + 1) == ((Be(off + 16, 4) & (1U << 29)) ? 0xA4U : 0x24U));
            CHECK(((Be(off + 2, 2) >> 12) & 3) == (nextWr & 3));
            const uint32_t numSge = uint32_t(Be(off + 2, 2) & 0xFFFU) / 2U;
            CHECK(numSge >= 1 && numSge <= v.queue.maxSendSge);
            const uint32_t blocks = W::WqeBlocks(numSge);
            uint64_t total = 0;
            std::vector<uint64_t> addresses;
            for (uint32_t i = 0; i < numSge; ++i) {
                const uint64_t segment = off + 48U + 16U * i;
                total += Be(segment + 8U, 4);
                addresses.push_back(Be(segment, 8));
                CHECK(Be(segment + 12U, 4) == ((v.localKey & 0x7FFFFFFFU) |
                    (i + 1U == numSge ? 0x80000000U : 0U)));
            }
            CHECK(Be(off + 20, 4) == total);
            locals.push_back(Be(off + 48, 8)); remotes.push_back(Be(off + 32, 8));
            segmentLocals.push_back(addresses);
            CHECK(Be(off + 40, 4) == v.remoteKey);
            for (uint32_t pad = 48U + 16U * numSge; pad < blocks * 64U; ++pad) {
                CHECK(sq.at(off + pad) == 0);
            }
            if (Be(off + 16, 4) & (1U << 29)) { completions.push_back(nextWr); ++signalCount; }
            nextSq += blocks; ++nextWr;
        }
        const uint64_t completedWr = state.cqConsumer ? completions.at(state.cqConsumer - 1U) + 1U : 0U;
        const uint64_t outstandingWr = nextWr - completedWr;
        peak = peak > outstandingWr ? peak : outstandingWr;
        CHECK(outstandingWr <= v.queue.maxSendWr);
        CHECK(state.producer - state.completed < v.queue.sqDepth);
        ++rings;
    }
    void ReadCqe(uint64_t address) {
        CHECK(address == v.queue.cq + state.cqConsumer * 64U);
        const uint32_t base = W::kCqeOffset;
        for (uint32_t i = 0; i < 64; ++i) { SetByte(base + i, 0); }
        const bool error = state.cqConsumer == failCq;
        SetWord(base, v.queue.qpn);
        SetWord(base + 4, (timeout ? 31U : error ? 30U : 4U) << 27 | (1U << 26));
        if (error) { SetByte(base + 28, 0x13); }
        CHECK(state.cqConsumer < completions.size());
        const uint64_t wr = completions.at(state.cqConsumer) + uint32_t(mismatch);
        SetByte(base + 30, uint8_t(wr)); SetByte(base + 31, uint8_t(wr >> 8));
        ++reads;
    }
};

static void AdaptiveWindow()
{
    auto v = View(TILEXR_RDMA_DB_HARD);
    Model m(v); W::PipelineOptions options;
    options.postBatch = 16; options.maxInFlight = 128;
    CHECK(W::PutPipeline(v, 0, 0, 8, 128, 0, 0, options, 3, m) == 0);
    CHECK(m.reads == 0 && m.peak == 128 && m.state.completed == 0);
    CHECK(W::WaitPipeline(v, 3, m) == 0 && m.state.completed == 128);

    Model automatic(v); W::PipelineOptions defaults;
    CHECK(W::PutPipeline(v, 0, 0, 8, 1000, 0, 0, defaults, 3, automatic) == 0);
    CHECK(automatic.reads == 0 && automatic.peak == 1000);
    CHECK(W::WaitPipeline(v, 3, automatic) == 0 && automatic.state.completed == 1000);

    for (uint32_t depth : {1024U, 2048U, 32768U}) {
        for (uint32_t maxWr : {127U, 4095U, 65535U}) {
            for (uint32_t sge : {1U, 2U, 4U, 8U, 16U}) {
                auto q = v.queue; q.sqDepth = depth; q.maxSendWr = maxWr;
                const uint32_t blocks = (48U + 16U * sge + 63U) / 64U;
                const uint32_t sqLimit = (depth - 1U) / blocks;
                const uint32_t expected = sqLimit < maxWr ? sqLimit : maxWr;
                CHECK(W::PipelineWindow(q, sge) == expected);
                CHECK(W::PipelineWindow(q, sge, 7U) == 7U);
            }
        }
    }
    {
        auto bounded = v;
        bounded.queue.sqDepth = 1024; bounded.queue.maxSendWr = 65535;
        bounded.queue.maxSendSge = 16;
        bounded.queue.pendingCapacity = W::PipelineRecordCapacity(1024, bounded.queue.cqDepth, 65535);
        TileXRRDMADeviceSge sges[16];
        for (auto& sge : sges) { sge = {0, 8, 0}; }
        Model fits(bounded), exceeds(bounded);
        CHECK(W::PutSgeListPipeline(bounded, sges, 16, 0, 204, 0, 0, defaults, 3, fits) == 0);
        CHECK(fits.peak == 204 && fits.reads == 0 && fits.state.producer == 1020);
        CHECK(W::WaitPipeline(bounded, 3, fits) == 0 && fits.state.completed == 1020);
        CHECK(W::PutSgeListPipeline(bounded, sges, 16, 0, 205, 0, 0, defaults, 3, exceeds) ==
            TILEXR_RDMA_DEVICE_INVALID);
        CHECK(exceeds.rings == 0 && exceeds.reads == 0 && exceeds.state.producer == 0);
    }
    v.queue.maxSendWr = v.queue.pendingCapacity = 4;
    Model reused(v); defaults.postBatch = 1;
    CHECK(W::PutPipeline(v, 0, 0, 8, 12, 0, 0, defaults, 3, reused) == 0);
    CHECK(reused.reads == 8 && reused.peak == 4 && W::PendingBatchCount(reused.state) == 4);
    CHECK(W::WaitPipeline(v, 3, reused) == 0 && reused.state.completed == 12);

    Model small(v); defaults.postBatch = 64; defaults.maxInFlight = 1;
    CHECK(W::PutPipeline(v, 0, 0, 8, 7, 0, 0, defaults, 3, small) == 0);
    CHECK(small.peak == 1 && small.rings == 7 && small.reads == 6);
    CHECK(W::WaitPipeline(v, 3, small) == 0 && small.state.completed == 7);

    v = View(TILEXR_RDMA_DB_HARD); v.queue.maxSendSge = 16; v.queue.maxSendWr = 256;
    v.queue.pendingCapacity = W::PipelineRecordCapacity(v.queue.sqDepth, v.queue.cqDepth, v.queue.maxSendWr);
    Model mixed(v); W::PipelineOptions varied; varied.postBatch = 16;
    TileXRRDMADeviceSge sges[16];
    for (auto& sge : sges) { sge = {0, 8, 0}; }
    CHECK(W::PutSgeListPipeline(v, sges, 16, 0, 48, 0, 0, varied, 3, mixed) == 0);
    CHECK(W::PutPipeline(v, 0, 0, 8, 80, 0, 0, varied, 3, mixed) == 0);
    CHECK(mixed.reads == 0 && W::PendingBatchCount(mixed.state) == 8 && mixed.state.producer == 320);
    varied.maxInFlight = 64;
    CHECK(W::PutSgeListPipeline(v, sges, 2, 0, 16, 0, 0, varied, 3, mixed) == 0);
    CHECK(mixed.reads == 5 && mixed.state.completed == 272 && mixed.state.producer == 352);
    CHECK(W::PendingBatchCount(mixed.state) == 4 && W::PendingWrCount(mixed.state) == 64);
    CHECK(W::WaitPipeline(v, 3, mixed) == 0 && mixed.state.completed == 352);
}

static void Golden()
{
    auto v = View(TILEXR_RDMA_DB_HARD);
    for (uint32_t seq : {0U, 1U, 3U, 32768U}) {
        Model m(v);
        W::EncodeWrite(m, v, seq, seq + 3U, 37, 53, 512);
        W::EncodePipelineWrite(m, 2, v, seq, seq + 3U, 37, 53, 512, true);
        CHECK(std::memcmp(m.scratch.data(), m.scratch.data() + W::kPipelineWqeOffset + 128, 64) == 0);
    }
}

static void Success(uint32_t mode)
{
    auto v = View(mode);
    for (uint32_t count : {1U, 48U, 64U, 65U, 128U, 512U, 1000U}) {
        Model m(v); W::PipelineOptions options;
        CHECK(W::PutPipeline(v, 0, 0, 512, count, 576, 576, options, 3, m) == 0);
        CHECK(m.state.completed < m.state.producer);
        CHECK(W::WaitPipeline(v, 3, m) == 0);
        CHECK(m.state.producer == count && m.state.completed == count);
        CHECK(m.state.cqConsumer == (count + 63U) / 64U);
        CHECK(m.signalCount == m.state.cqConsumer && m.rings == m.signalCount);
        CHECK(m.publications == m.rings && m.peak == count);
        for (uint32_t i = 0; i < count; ++i) {
            CHECK(m.locals[i] == v.localAddress + uint64_t(i) * 576);
            CHECK(m.remotes[i] == v.remoteAddress + uint64_t(i) * 576);
        }
        CHECK(W::PutPipeline(v, 0, 0, 512, 65, 576, 576, options, 3, m) == 0);
        CHECK(m.state.reserved[1] == count + 65);
        CHECK(W::Put(v, 0, 0, 8, 3, m) == TILEXR_RDMA_DEVICE_UNSUPPORTED);
    }
    Model m(v); W::PipelineOptions o; o.cyclicSlots = 32;
    CHECK(W::Put(v, 0, 0, 8, 3, m) == 0); // old all-signaled WR consumes ordinal zero.
    CHECK(W::PutPipeline(v, 0, 0, 512, 65, 512, 512, o, 3, m) == 0);
    CHECK(W::WaitPipeline(v, 3, m) == 0);
    for (uint32_t i = 0; i < 65; ++i) { CHECK(m.locals[i + 1] == v.localAddress + ((i + 2U) % 32U) * 512U); }
    CHECK(m.state.producer == 66 && m.state.cqConsumer == 3 && m.state.reserved[1] == 66);
}

static void Failures()
{
    auto v = View(TILEXR_RDMA_DB_HARD); W::PipelineOptions o; o.maxInFlight = 128;
    for (uint32_t kind = 0; kind < 8; ++kind) {
        Model m(v); auto opts = o; uint32_t count = 512;
        if (kind == 0) { count = 0; }
        if (kind == 1) { opts.postBatch = 65; }
        if (kind == 2) { opts.maxInFlight = 0; }
        if (kind == 3) { m.state.producer = m.state.completed = 32700; }
        if (kind == 4) { m.state.cqConsumer = 32765; }
        if (kind == 5) { m.state.reserved[0] = 123; }
        if (kind == 6) { m.v.localBytes = 512; }
        if (kind == 7) { m.v.queue.pendingCapacity = 0; }
        CHECK(W::PutPipeline(m.v, 0, 0, 512, count, 576, 576, opts, 3, m) != 0);
        CHECK(m.rings == 0 && m.publications == 0 && m.state.reserved[1] == 0);
    }
    for (uint32_t kind = 0; kind < 3; ++kind) {
        Model m(v); m.failCq = 0; m.timeout = kind == 1; m.mismatch = kind == 2;
        if (kind == 2) { m.failCq = UINT32_MAX; }
        const uint32_t expected = kind == 0 ? TILEXR_RDMA_DEVICE_CQE_ERROR :
            kind == 1 ? TILEXR_RDMA_DEVICE_TIMEOUT : TILEXR_RDMA_DEVICE_CQE_MISMATCH;
        CHECK(W::PutPipeline(v, 0, 0, 512, 512, 576, 576, o, 3, m) == expected);
        CHECK(m.state.producer == 128 && m.state.completed == 0 && m.state.cqConsumer == 0);
        CHECK(m.state.error == expected && m.rings == 2);
        CHECK(W::PutPipeline(v, 0, 0, 512, 512, 576, 576, o, 3, m) == TILEXR_RDMA_DEVICE_POISONED);
    }
    Model partial(v); partial.failCq = 1;
    CHECK(W::PutPipeline(v, 0, 0, 512, 512, 576, 576, o, 3, partial) == TILEXR_RDMA_DEVICE_CQE_ERROR);
    CHECK(partial.state.completed == 64 && partial.state.cqConsumer == 1 && partial.state.producer == 192);
}

static void SgePipeline()
{
    auto v = View(TILEXR_RDMA_DB_HARD); v.queue.maxSendSge = 16;
    TileXRRDMADeviceSge sges[16]{};
    for (uint32_t i = 0; i < 16; ++i) { sges[i] = {uint64_t(16U-i)*4096, 17U+i*13U, 0}; }
    for (uint32_t n = 1; n <= 16; ++n) {
        const uint32_t blocks = W::WqeBlocks(n);
        uint32_t total = 0; for (uint32_t i = 0; i < n; ++i) { total += sges[i].bytes; }
        for (uint32_t seq : {0U, 3U, 32768U}) {
            Model m(v);
            W::EncodeWriteSgeList(m, v, seq, 7, sges, n, 91, total);
            W::EncodePipelineSgeWrite(m, 2, v, seq, 7, sges, n, 0, 91, total, true);
            CHECK(std::memcmp(m.scratch.data(),m.scratch.data()+W::kPipelineWqeOffset+2U*blocks*64U,blocks*64U)==0);
        }
    }
    for (uint32_t n : {1U,2U,4U,8U,16U}) {
        const uint32_t blocks = W::WqeBlocks(n);
        for (uint32_t count : {1U,48U,64U,65U,128U,512U,1000U}) {
            Model m(v); W::PipelineOptions opts; opts.cyclicSlots = 32;
            CHECK(W::PutSgeListPipeline(v,sges,n,17,count,131072,8192,opts,3,m)==0);
            CHECK(m.state.completed < m.state.producer);
            CHECK(W::WaitPipeline(v,3,m)==0);
            CHECK(m.state.producer==uint64_t(count)*blocks && m.state.completed==m.state.producer);
            CHECK(m.state.reserved[1]==count && m.state.cqConsumer==(count+63U)/64U);
            CHECK(m.peak==count && m.signalCount==m.state.cqConsumer);
            for (uint32_t wr=0;wr<count;++wr) {
                const uint64_t index=(wr+1U)%32U;
                CHECK(m.remotes[wr]==v.remoteAddress+17+index*8192);
                for (uint32_t i=0;i<n;++i) {
                    CHECK(m.segmentLocals[wr][i]==v.localAddress+sges[i].localOffset+index*131072);
                }
            }
            CHECK(W::PutSgeListPipeline(v,sges,2,0,65,0,0,opts,3,m)==0);
            CHECK(m.state.reserved[1]==count+65U && m.state.producer==uint64_t(count)*blocks+130);
        }
        Model m(v); m.failCq=0; W::PipelineOptions opts; opts.maxInFlight=128;
        CHECK(W::PutSgeListPipeline(v,sges,n,0,512,0,0,opts,3,m)==TILEXR_RDMA_DEVICE_CQE_ERROR);
        CHECK(m.state.producer==128U*blocks && m.state.completed==0 && m.state.reserved[1]==128);
        CHECK(W::PutSgeListPipeline(v,sges,n,0,1,0,0,opts,3,m)==TILEXR_RDMA_DEVICE_POISONED);
    }
    for (uint32_t kind=0;kind<9;++kind) {
        Model m(v); W::PipelineOptions opts; auto view=v; auto bad=sges[0];
        uint32_t n=16,count=512;
        if (kind==0) { n=0; }
        if (kind==1) { n=17; }
        if (kind==2) { view.queue.maxSendSge=8; }
        if (kind==3) { bad.reserved=1; }
        if (kind==4) { bad.bytes=0x80000000U; }
        if (kind==5) { bad.localOffset=view.localBytes; }
        if (kind==6) { view.remoteBytes=1; }
        if (kind==7) { view.queue.sqDepth=4096;count=1000; }
        if (kind==8) { view.localBytes=bad.localOffset+bad.bytes; }
        auto saved=sges[0];sges[0]=bad;
        CHECK(W::PutSgeListPipeline(view,sges,n,0,count,131072,131072,opts,3,m)!=0);
        CHECK(m.rings==0 && m.publications==0 && m.state.producer==0);
        sges[0]=saved;
    }
}

static void ExplicitWaitAndCrossCallWindow()
{
    for (uint32_t mode : {TILEXR_RDMA_DB_HARD, TILEXR_RDMA_DB_SOFT}) {
        auto v = View(mode); W::PipelineOptions options;
        Model empty(v);
        CHECK(W::WaitPipeline(v, 3, empty) == 0 && empty.reads == 0 && empty.state.reserved[0] == 0);
        for (uint32_t kind = 0; kind < 3; ++kind) {
            Model m(v); m.timeout = kind == 0; m.failCq = kind == 1 ? 0 : UINT32_MAX;
            m.mismatch = kind == 2;
            CHECK(W::PutPipeline(v, 0, 0, 8, 1, 0, 0, options, 3, m) == 0);
            CHECK(m.reads == 0 && m.state.completed == 0 && m.state.producer == 1);
            const uint32_t status = kind == 0 ? TILEXR_RDMA_DEVICE_TIMEOUT :
                kind == 1 ? TILEXR_RDMA_DEVICE_CQE_ERROR : TILEXR_RDMA_DEVICE_CQE_MISMATCH;
            CHECK(W::WaitPipeline(v, 3, m) == status);
            CHECK(m.reads == (kind == 0 ? 3U : 1U) && m.state.error == status);
            CHECK(m.state.completed == 0 && W::PendingBatchCount(m.state) != 0);
            CHECK(W::WaitPipeline(v, 3, m) == TILEXR_RDMA_DEVICE_POISONED);
        }
        Model partial(v); partial.failCq = 1;
        CHECK(W::PutPipeline(v, 0, 0, 8, 65, 0, 0, options, 3, partial) == 0);
        CHECK(W::WaitPipeline(v, 3, partial) == TILEXR_RDMA_DEVICE_CQE_ERROR);
        CHECK(partial.state.completed == 64 && partial.state.cqConsumer == 1);
        CHECK(W::PendingBatchCount(partial.state) == 1 && W::PendingWrCount(partial.state) == 1);
        v.queue.maxSendSge = 16; v.queue.maxSendWr = 2;
        v.queue.pendingCapacity = W::PipelineRecordCapacity(v.queue.sqDepth, v.queue.cqDepth, v.queue.maxSendWr);
        Model m(v); options.postBatch = 1; options.maxInFlight = 2;
        TileXRRDMADeviceSge sges[16];
        for (auto& sge : sges) { sge = {0, 8, 0}; }
        CHECK(W::PutSgeListPipeline(v, sges, 16, 0, 1, 0, 0, options, 3, m) == 0);
        CHECK(W::PutPipeline(v, 0, 0, 8, 1, 0, 0, options, 3, m) == 0);
        CHECK(m.reads == 0 && m.state.producer == 6 && m.state.completed == 0 && m.peak == 2);
        // A smaller next-call window drains both pending WRs, not six WQEBBs.
        options.maxInFlight = 1;
        CHECK(W::PutPipeline(v, 0, 0, 8, 1, 0, 0, options, 3, m) == 0);
        CHECK(m.reads == 2 && m.state.completed == 6 && m.state.producer == 7);
        CHECK(W::WaitPipeline(v, 3, m) == 0 && m.state.completed == 7 && m.reads == 3);
        CHECK(W::WaitPipeline(v, 3, m) == 0 && m.reads == 3);
        CHECK(W::PendingBatchCount(m.state) == 0 && W::PendingWrCount(m.state) == 0);
    }
    auto v = View(TILEXR_RDMA_DB_HARD); v.queue.cqDepth = 4;
    v.queue.pendingCapacity = W::PipelineRecordCapacity(v.queue.sqDepth, v.queue.cqDepth, v.queue.maxSendWr);
    Model m(v); W::PipelineOptions options; options.postBatch = 1;
    CHECK(W::PutPipeline(v, 0, 0, 8, 2, 0, 0, options, 3, m) == 0);
    const auto state = m.state;
    const auto records = m.pending;
    // Four requested CQEs exceed the non-wrap guard even though CI is still zero.
    CHECK(W::PutPipeline(v, 0, 0, 8, 2, 0, 0, options, 3, m) == TILEXR_RDMA_DEVICE_INVALID);
    CHECK(m.reads == 0 && m.rings == 2 && std::memcmp(&state, &m.state, sizeof(state)) == 0);
    CHECK(m.pending == records);
    options.postBatch = 2; // Appending one batch exactly fits the remaining guarded CQ span.
    CHECK(W::PutPipeline(v, 0, 0, 8, 2, 0, 0, options, 3, m) == 0);
    CHECK(m.reads == 0 && m.state.completed == 0 && m.state.producer == 4);
    CHECK(W::WaitPipeline(v, 3, m) == 0 && m.state.cqConsumer == 3 && m.state.completed == 4);
    v = View(TILEXR_RDMA_DB_HARD); v.queue.sqDepth = 131072;
    Model longRun(v); options.postBatch = 64;
    CHECK(W::PutPipeline(v, 0, 0, 8, 65537, 0, 0, options, 3, longRun) == 0);
    CHECK(W::WaitPipeline(v, 3, longRun) == 0);
    CHECK(longRun.state.reserved[1] == 65537 && longRun.state.completed == 65537);
}

int main()
{
    AdaptiveWindow();
    Golden(); Success(TILEXR_RDMA_DB_HARD); Success(TILEXR_RDMA_DB_SOFT); Failures(); SgePipeline();
    ExplicitWaitAndCrossCallWindow();
    std::puts("pipeline CPU model PASS: WQE, sparse CQ, submit/wait, cross-call WR window, no-wrap admission, sticky errors");
}
