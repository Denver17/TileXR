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
    q.sqDbValue = W::MakeSqDbValue(q.qpn, 1, 5, 0);
    return v;
}

struct Model {
    TileXRRDMADeviceView v;
    TileXRRDMADeviceState state{};
    std::array<uint8_t, W::kPipelineSgeScratchBytes> scratch{};
    std::vector<uint8_t> sq;
    std::vector<uint64_t> completions, locals, remotes;
    std::vector<std::vector<uint64_t>> segmentLocals;
    uint64_t nextSq = 0, nextWr = 0, peak = 0;
    uint32_t publications = 0, rings = 0, reads = 0, signalCount = 0;
    uint32_t failCq = UINT32_MAX;
    bool timeout = false, mismatch = false;
    explicit Model(const TileXRRDMADeviceView& view) : v(view), sq(view.queue.sqDepth * 64U, 0xCC) {}
    void SetByte(uint32_t off, uint8_t value) { scratch.at(off) = value; }
    uint8_t GetByte(uint32_t off) const { return scratch.at(off); }
    void SetWord(uint32_t off, uint32_t value) {
        CHECK(off % 4U == 0U);
        for (uint32_t i = 0; i < 4; ++i) { SetByte(off + i, uint8_t(value >> (i * 8U))); }
    }
    TileXRRDMADeviceState LoadState() const { return state; }
    void StoreState(const TileXRRDMADeviceState& s) { state = s; }
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
        CHECK(outstandingWr <= 128U);
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
        CHECK(m.state.producer == count && m.state.completed == count);
        CHECK(m.state.cqConsumer == (count + 63U) / 64U);
        CHECK(m.signalCount == m.state.cqConsumer && m.rings == m.signalCount);
        CHECK(m.publications == m.rings && m.peak <= 128);
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
    for (uint32_t i = 0; i < 65; ++i) { CHECK(m.locals[i + 1] == v.localAddress + ((i + 2U) % 32U) * 512U); }
    CHECK(m.state.producer == 66 && m.state.cqConsumer == 3 && m.state.reserved[1] == 66);
}

static void Failures()
{
    auto v = View(TILEXR_RDMA_DB_HARD); W::PipelineOptions o;
    for (uint32_t kind = 0; kind < 8; ++kind) {
        Model m(v); auto opts = o; uint32_t count = 512;
        if (kind == 0) { count = 0; }
        if (kind == 1) { opts.postBatch = 65; }
        if (kind == 2) { opts.maxInFlight = 127; }
        if (kind == 3) { m.state.producer = m.state.completed = 32700; }
        if (kind == 4) { m.state.cqConsumer = 32765; }
        if (kind == 5) { m.state.reserved[0] = 123; }
        if (kind == 6) { m.v.localBytes = 512; }
        if (kind == 7) { m.v.queue.maxSendWr = 127; }
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
            CHECK(m.state.producer==uint64_t(count)*blocks && m.state.completed==m.state.producer);
            CHECK(m.state.reserved[1]==count && m.state.cqConsumer==(count+63U)/64U);
            CHECK(m.peak<=128 && m.signalCount==m.state.cqConsumer);
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
        Model m(v); m.failCq=0; W::PipelineOptions opts;
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

int main()
{
    Golden(); Success(TILEXR_RDMA_DB_HARD); Success(TILEXR_RDMA_DB_SOFT); Failures(); SgePipeline();
    std::puts("pipeline CPU model PASS: golden WQE, sparse CQ ordinals, 1/48/64/65/128/512/1000 tails, two-batch window, no-wrap admission, error ownership");
}
