/*
 * 通过 ACL 注册外置 ElasticPoolTest，检查 Pool 的增长、FIFO、复用和失败准入。
 * 回读数据及分配前副作用计数，核对新地址可用且调用未重放。
 * 用映射 gate 保持 producer 驻留，检查早期元数据、consumer 依赖和 finalStatus 迟到错误。
 * 零输出通过独立 predecessor 延迟 consumer boundary，区分有无活跃引用时的 Close 行为。
 * 保留 Allocation 引用来检查环绕、乱序释放后的队头阻塞和 trim/close；
 * 正常路径等待释放完成后回收辅助内存、stream 和 kernel 注册。Torch 与多 rank EP 另有测试。
 */
#include "tilexr_elastic_memory.h"
#include <runtime/kernel.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace TileXR::Elastic;
static void Check(int rc,const char* name) { if(rc) throw std::runtime_error(std::string(name)+"="+std::to_string(rc)); }
static void Require(bool yes,const char* name) { if(!yes) throw std::runtime_error(name); }
static void* signature=reinterpret_cast<void*>(0x454d002000ULL);
static std::shared_ptr<Pending> Submit(Pool& pool,aclrtStream stream,void* side,uint64_t seed,
                                      std::function<void()> beforeLaunch = {}) {
    return pool.Submit([side,seed,beforeLaunch](DeviceView view,aclrtStream runStream) {
        if (beforeLaunch) beforeLaunch();
        struct { uint64_t state,mailbox,seed; void* side; } args{view.state,view.mailbox,seed,side};
        rtArgsEx_t info{}; info.args=&args; info.argsSize=sizeof(args);
        rtTaskCfgInfo_t cfg{}; cfg.schemMode=1;
        return int(rtKernelLaunchWithFlagV2(signature,1,&info,nullptr,runStream,0,&cfg));
    },stream);
}
static void Validate(const std::shared_ptr<Allocation>& allocation,uint64_t seed) {
    const uint64_t bytes=seed==0?0:(seed*3+1)*32;
    Require(allocation->Bytes()==bytes,"actual bytes");
    if(!bytes) { Require(!allocation->Data(),"zero address"); return; }
    std::vector<uint64_t> data(bytes/8);
    Check(aclrtMemcpy(data.data(),bytes,allocation->Data(),bytes,ACL_MEMCPY_DEVICE_TO_HOST),"readback");
    for(size_t i=0;i<data.size();++i) Require(data[i]==i+seed,"fresh HBM read/write");
}
static void CheckAccounting(const Stats& stats) {
    Require(stats.committedBytes==stats.liveBytes+stats.pendingRetireBytes+stats.freeBytes,
            "ring accounting partition");
    Require(stats.blockedFreeBytes<=stats.freeBytes,"blocked space is a free subset");
    Require(stats.paddingBytes<=stats.liveBytes+stats.pendingRetireBytes,"padding is a reservation subset");
}
static Stats AwaitRetirement(Pool& pool,uint64_t blockedBytes) {
    for(int i=0;i<500;++i) {
        const Stats stats=pool.Collect(); CheckAccounting(stats);
        if(!stats.pendingRetireBytes && stats.blockedFreeBytes==blockedBytes) return stats;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("ring retirement did not reach expected prefix");
}
static void CheckRingBehavior(aclrtStream stream,void* side,uint64_t& calls) {
    Config config; config.segmentBytes=4096; config.maxCachedFreeBytes=1ULL<<20;
    config.maxCommittedBytes=1ULL<<20;
    auto ring=Pool::Create(config);
    auto pa=Submit(*ring,stream,side,20); ++calls;
    auto a=pa->Wait(); Validate(a,20);
    auto pb=Submit(*ring,stream,side,20); ++calls;
    auto b=pb->Wait(); Validate(b,20);
    const auto first=a->Identity(), second=b->Identity();
    Require(first.segmentId==second.segmentId,"ring setup should fit one segment");
    Require(second.offset+second.bytes<=4096,"ring setup bounds");
    auto lastView=b; pb.reset(); b.reset();
    pa.reset(); a.reset();
    AwaitRetirement(*ring,0);
    const uint64_t before=ring->Snapshot().growthCount;
    auto pc=Submit(*ring,stream,side,7); ++calls;
    auto c=pc->Wait(); Validate(c,7); Validate(lastView,20);
    const auto wrapped=c->Identity();
    Require(wrapped.segmentId==second.segmentId && wrapped.offset<second.offset,
            "nonempty ring must wrap into safe prefix");
    Require(ring->Snapshot().growthCount==before,"nonempty wrap must not grow");
    Stats wrapStats=ring->Snapshot(); CheckAccounting(wrapStats);
    const uint64_t wrapPadding=4096-(second.offset+second.bytes)+wrapped.offset;
    Require(wrapStats.paddingBytes>=wrapPadding,"wrap padding must be charged to reservation");
    pc.reset(); c.reset();
    const Stats blocked=AwaitRetirement(*ring,wrapPadding+wrapped.bytes);
    Validate(lastView,20);
    Require(blocked.committedBytes==4096,"last alias retains ring segment");
    // Although the wrapped output is dead, its bytes remain blocked behind the live view.
    auto pd=Submit(*ring,stream,side,20); ++calls;
    auto d=pd->Wait(); Validate(d,20); Validate(lastView,20);
    Require(d->Identity().segmentId!=second.segmentId,"blocked free region must not be reused");
    Require(ring->Snapshot().growthCount==before+1,"blocked prefix needs one new segment");
    CheckAccounting(ring->Snapshot());
    lastView.reset();
    const Stats prefixReleased=AwaitRetirement(*ring,0);
    Require(prefixReleased.blockedFreeBytes==0,"releasing prefix must consume blocked reservations");
    Require(prefixReleased.committedBytes==8192 && prefixReleased.reclaimableSegmentBytes==4096,
            "retired old ring should remain cached for segment switching");
    // The current segment still contains D, so E must switch to the earlier empty
    // segment. Its size cannot fit the current remainder and must not trigger growth.
    auto pe=Submit(*ring,stream,side,41); ++calls;
    auto e=pe->Wait(); Validate(e,41); Validate(d,20);
    Require(e->Identity().segmentId==second.segmentId,"segment switch must reuse earlier ring");
    Require(ring->Snapshot().growthCount==before+1,"segment switch must not grow");
    CheckAccounting(ring->Snapshot());
    pe.reset(); e.reset();
    const Stats oldTrimmed=ring->Trim(); CheckAccounting(oldTrimmed);
    Require(oldTrimmed.committedBytes==4096,"empty old ring must return while new output is live");
    Validate(d,20);
    pd.reset(); d.reset();
    const Stats trimmed=ring->Trim(); CheckAccounting(trimmed);
    Require(trimmed.committedBytes==0,"ring full trim must return every segment");
    ring->Close();
    std::cout<<"RING_PASS nonempty_wrap=1 growth_on_wrap=0 blocked_bytes="<<blocked.blockedFreeBytes
             <<" wrap_padding="<<wrapPadding<<" last_view_valid=1 blocked_growth=1 prefix_release=1"
             <<" segment_switch=1 growth_on_switch=0 trim=0\n";
}
// Fixture-only flags mirror pool_kernel.cpp and preserve its registered signature.
static constexpr uint64_t kHeldProducer=uint64_t(1)<<63;
static constexpr uint64_t kConsumer=uint64_t(1)<<62;
static constexpr uint64_t kLateFailure=uint64_t(1)<<61;
static constexpr uint64_t kHeldPredecessor=uint64_t(1)<<60;
struct HeldControl {
    uint64_t* host=nullptr;
    void* device=nullptr;
    bool unsafe=false;
    explicit HeldControl(void* side) {
        void* memory=nullptr;
        if(posix_memalign(&memory,4096,4096)) throw std::bad_alloc();
        host=static_cast<uint64_t*>(memory);
        std::memset(host,0,4096);
        host[32]=reinterpret_cast<uint64_t>(side);
        const int status=aclrtHostRegister(host,4096,ACL_HOST_REGISTER_MAPPED,&device);
        if(status) { std::free(host); host=nullptr; Check(status,"held control register"); }
    }
    ~HeldControl() {
        // A failed stream completion is not permission to unmap a Device input.
        if(!unsafe && host && aclrtHostUnregister(host)==ACL_SUCCESS) std::free(host);
    }
    uint64_t Read(size_t word) const { return __atomic_load_n(host+word,__ATOMIC_ACQUIRE); }
    void Release() { __atomic_store_n(host,1,__ATOMIC_RELEASE); }
};
struct HeldWatchdog {
    std::shared_ptr<HeldControl> gate;
    std::atomic<bool> stopped{false},forced{false};
    std::thread thread;
    explicit HeldWatchdog(std::shared_ptr<HeldControl> value) : gate(std::move(value)) {
        thread=std::thread([this] {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!stopped.load() && std::chrono::steady_clock::now()<end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!stopped.load()) { forced.store(true); gate->Release(); }
        });
    }
    void Release() { gate->Release(); stopped.store(true); }
    ~HeldWatchdog() { Release(); if(thread.joinable()) thread.join(); }
};
struct HeldConsumer {
    std::shared_ptr<HeldControl> gate;
    aclrtStream stream=nullptr;
    aclrtEvent done=nullptr;
    explicit HeldConsumer(std::shared_ptr<HeldControl> value) : gate(std::move(value)) {
        Check(aclrtCreateStream(&stream),"held consumer stream");
        const int status=aclrtCreateEvent(&done);
        if(status) { aclrtDestroyStream(stream); stream=nullptr; Check(status,"held consumer event"); }
    }
    ~HeldConsumer() {
        if(!stream) return;
        if(aclrtSynchronizeStream(stream)!=ACL_SUCCESS) { gate->unsafe=true; return; }
        if(done) aclrtDestroyEvent(done);
        aclrtDestroyStream(stream);
    }
};
static std::shared_ptr<Pending> SubmitHeld(Pool& pool,aclrtStream input,
                                         const std::shared_ptr<HeldControl>& gate,
                                         uint64_t seed,bool lateFailure) {
    return pool.Submit([gate,seed,lateFailure](DeviceView view,aclrtStream runStream) {
        struct { uint64_t state,mailbox,seed; void* side; } args{
            view.state,view.mailbox,seed|kHeldProducer|(lateFailure?kLateFailure:0),gate->device};
        rtArgsEx_t info{}; info.args=&args; info.argsSize=sizeof(args);
        rtTaskCfgInfo_t cfg{}; cfg.schemMode=1;
        return int(rtKernelLaunchWithFlagV2(signature,1,&info,nullptr,runStream,0,&cfg));
    },input);
}
static void CheckMetadataEarly(aclrtStream input,void* side,uint64_t& calls,
                               uint64_t seed,bool earlyDrop,bool lateFailure) {
    Config config; config.segmentBytes=4096; config.maxCachedFreeBytes=0;
    auto pool=Pool::Create(config);
    auto gate=std::make_shared<HeldControl>(side);
    HeldConsumer consumer(gate);
    // Declared after consumer so assertion unwinding releases the producer gate before
    // the consumer destructor waits for its stream. The watchdog also bounds WaitMetadata.
    HeldWatchdog watchdog(gate);
    auto pending=SubmitHeld(*pool,input,gate,seed,lateFailure); ++calls;
    auto allocation=pending->WaitMetadata();
    const auto reachedDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!gate->Read(8) && std::chrono::steady_clock::now()<reachedDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(!watchdog.forced.load() && gate->Read(8)==1 && gate->Read(0)==0 && gate->Read(16)==0,
            "metadata must return while producer remains held");
    const uint64_t expectedBytes=seed?(seed*3+1)*32:0;
    Require(allocation->Bytes()==expectedBytes,"early actual bytes");
    Require((allocation->Data()!=nullptr)==(expectedBytes!=0),"early output address");
    const Handle metadataHandle=allocation->Identity();
    Require(pending->WaitMetadata().get()==allocation.get(),"metadata returns stable allocation");
    for(int repeat=0;repeat<129;++repeat) allocation->WaitOnStream(consumer.stream);
    struct { uint64_t state,mailbox,seed; void* side; } args{
        reinterpret_cast<uint64_t>(allocation->Data()),allocation->Bytes(),seed|kConsumer,gate->device};
    rtArgsEx_t info{}; info.args=&args; info.argsSize=sizeof(args);
    rtTaskCfgInfo_t cfg{}; cfg.schemMode=1;
    Check(rtKernelLaunchWithFlagV2(signature,1,&info,nullptr,consumer.stream,0,&cfg),"held consumer launch");
    Check(aclrtRecordEvent(consumer.done,consumer.stream),"held consumer record");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    aclrtEventRecordedStatus state;
    Check(aclrtQueryEventStatus(consumer.done,&state),"held consumer query");
    Require(!watchdog.forced.load() && state!=ACL_EVENT_RECORDED_STATUS_COMPLETE && gate->Read(24)==0,
            "consumer must remain blocked until producer release");
    const Stats held=pool->Snapshot(); CheckAccounting(held);
    Require(held.reclaimableSegmentBytes==0,"held producer is not reclaimable");
    if(!expectedBytes) Require(held.committedBytes==0,"zero metadata must not allocate data HBM");
    if(earlyDrop) { pending.reset(); allocation.reset(); }
    watchdog.Release();
    if(!earlyDrop) {
        bool syncFailed=false,waitFailed=false;
        try { allocation->Synchronize(); } catch(const std::exception&) { syncFailed=true; }
        std::shared_ptr<Allocation> completed;
        try { completed=pending->Wait(); } catch(const std::exception&) { waitFailed=true; }
        Require(syncFailed==lateFailure && waitFailed==lateFailure,"late status must reach both explicit waits");
        if(!waitFailed) Require(completed.get()==allocation.get(),"full wait preserves metadata allocation");
        const Handle after=allocation->Identity();
        Require(after.segmentId==metadataHandle.segmentId && after.offset==metadataHandle.offset &&
                after.bytes==metadataHandle.bytes && after.generation==metadataHandle.generation,
                "late status must preserve published allocation identity");
    }
    Check(aclrtSynchronizeEvent(consumer.done),"held consumer complete");
    Require(gate->Read(16)==1 && gate->Read(24)==1,"consumer must observe completed producer data");
    Require(!watchdog.forced.load(),"watchdog release is a test failure");
    if(allocation && !lateFailure) Validate(allocation,seed);
    if(lateFailure) {
        bool rejected=false;
        try { Submit(*pool,input,side,1); } catch(const std::exception&) { rejected=true; }
        Require(rejected,"late failure poisons subsequent submissions");
    }
    allocation.reset(); pending.reset();
    const Stats retired=pool->Trim(); CheckAccounting(retired);
    Require(retired.committedBytes==0,"early metadata outputs retire after producer and consumer");
    pool->Close();
    std::cout<<"METADATA_EARLY_PASS bytes="<<expectedBytes<<" producer_held=1 consumer_blocked=1"
             <<" repeated_waits=129 early_drop="<<earlyDrop<<" late_failure="<<lateFailure<<" identity_stable=1 trim=0\n";
}
static void CheckZeroBoundaryClose(aclrtStream input,void* side,uint64_t& calls,bool keepLive) {
    auto pool=Pool::Create();
    auto producerGate=std::make_shared<HeldControl>(side);
    auto predecessorGate=std::make_shared<HeldControl>(side);
    HeldConsumer consumer(predecessorGate);
    // Both watchdogs unwind before the consumer stream; even failed assertions release
    // its predecessor and producer before attempting stream completion.
    HeldWatchdog producerWatchdog(producerGate);
    HeldWatchdog predecessorWatchdog(predecessorGate);
    auto pending=SubmitHeld(*pool,input,producerGate,0,false); ++calls;
    auto allocation=pending->WaitMetadata();
    Require(allocation->Bytes()==0 && !allocation->Data(),"zero boundary fixture output");
    struct { uint64_t state,mailbox,seed; void* side; } args{
        0,0,kHeldPredecessor,predecessorGate->device};
    rtArgsEx_t info{}; info.args=&args; info.argsSize=sizeof(args);
    rtTaskCfgInfo_t cfg{}; cfg.schemMode=1;
    Check(rtKernelLaunchWithFlagV2(signature,1,&info,nullptr,consumer.stream,0,&cfg),
          "zero boundary predecessor launch");
    // Producer is still held, so this must enqueue a dependency rather than take
    // WaitOnStream's already-completed fast path.
    allocation->WaitOnStream(consumer.stream);
    Check(aclrtRecordEvent(consumer.done,consumer.stream),"zero boundary consumer record");
    producerWatchdog.Release();
    allocation->Synchronize();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!predecessorGate->Read(8) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(!producerWatchdog.forced.load() && !predecessorWatchdog.forced.load() &&
            predecessorGate->Read(8)==1 && predecessorGate->Read(0)==0,
            "zero boundary predecessor must still be held");
    aclrtEventRecordedStatus state;
    Check(aclrtQueryEventStatus(consumer.done,&state),"zero boundary consumer query");
    Require(state!=ACL_EVENT_RECORDED_STATUS_COMPLETE,"zero boundary must remain pending");
    pending.reset();
    if(!keepLive) allocation.reset();

    std::atomic<bool> returned{false},returnedBeforeRelease{false};
    std::thread release([&] {
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
        while(!returned.load() && std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        returnedBeforeRelease.store(returned.load());
        predecessorGate->Release();
    });
    Stats closed{};
    std::exception_ptr closeError;
    try { closed=pool->Close(); }
    catch(...) { closeError=std::current_exception(); }
    returned.store(true);
    release.join();
    predecessorWatchdog.Release();
    if(closeError) std::rethrow_exception(closeError);
    Require(!producerWatchdog.forced.load() && !predecessorWatchdog.forced.load(),
            "zero boundary watchdog release is a failure");
    Require(returnedBeforeRelease.load()==keepLive,
            "Close must drain unowned zero boundaries but preserve live zero allocations");
    Require(closed.committedBytes==0 && (closed.metadataBytes!=0)==keepLive,
            "Close must retain Core only for the live zero allocation");
    Check(aclrtSynchronizeEvent(consumer.done),"zero boundary consumer complete");
    Require(predecessorGate->Read(16)==1,"zero boundary predecessor finished");
    allocation.reset();
    const Stats final=pool->Close();
    Require(final.committedBytes==0 && final.metadataBytes==0,"zero boundary final cleanup");
    std::cout<<"ZERO_BOUNDARY_CLOSE_PASS live_allocation="<<keepLive
             <<" returned_before_release="<<returnedBeforeRelease.load()
             <<" producer_complete=1 consumer_boundary_delayed=1 metadata_after_close="
             <<closed.metadataBytes<<" final_metadata=0\n";
}
int main(int argc,char** argv) {
    if(argc!=3) { std::cerr<<"usage: test_elastic_pool DEVICE KERNEL\n"; return 2; }
    try {
        Check(aclInit(nullptr),"init"); Check(aclrtSetDevice(std::stoi(argv[1])),"device");
        Check(aclrtSetOpExecuteTimeOut(30),"timeout");
        aclrtStream stream=nullptr; Check(aclrtCreateStream(&stream),"stream");
        void* side=nullptr; Check(aclrtMalloc(&side,64,ACL_MEM_MALLOC_NORMAL_ONLY),"side");
        Check(aclrtMemset(side,64,0,64),"side clear");
        std::ifstream file(argv[2],std::ios::binary);
        std::vector<char> image((std::istreambuf_iterator<char>(file)),{});
        Require(!image.empty(),"kernel file");
        rtDevBinary_t binary{}; binary.data=image.data(); binary.length=image.size(); binary.magic=0x41415246;
        void* registered=nullptr; Check(rtDevBinaryRegister(&binary,&registered),"register");
        Check(rtFunctionRegister(registered,signature,"ElasticPoolTest","ElasticPoolTest",0),"function");
        Config config; config.segmentBytes=4096; config.maxCachedFreeBytes=1ULL<<20; config.maxCommittedBytes=1ULL<<20;
        auto pool=Pool::Create(config);
        Require(pool->Snapshot().committedBytes==0,"initial HBM=0");
        uint64_t calls=0;
        auto first=Submit(*pool,stream,side,7); ++calls;
        auto a=first->Wait(); Validate(a,7);
        auto alias=a; auto generation=a->Identity().generation;
        first.reset(); a.reset(); pool->Trim();
        Require(pool->Snapshot().liveBytes==alias->Bytes(),"alias retains segment");
        auto second=Submit(*pool,stream,side,90); ++calls;
        auto third=Submit(*pool,stream,side,0); ++calls;
        auto b=second->Wait(); auto zero=third->Wait();
        Validate(b,90); Validate(zero,0); Validate(alias,7);
        Require(b->Identity().generation>generation,"FIFO generation");
        second.reset(); b.reset(); third.reset(); zero.reset(); alias.reset();
        auto drained=pool->Trim();
        Require(drained.committedBytes==0,"trim returns HBM");
        auto warm=Submit(*pool,stream,side,7); ++calls;
        a=warm->Wait(); Validate(a,7); uint64_t grows=pool->Snapshot().growthCount;
        warm.reset(); a.reset();
        for(int i=0;i<100 && pool->Collect().liveBytes; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto hit=Submit(*pool,stream,side,7); ++calls;
        a=hit->Wait(); Validate(a,7);
        Require(pool->Snapshot().growthCount==grows,"hot hit avoids malloc");
        auto closed=pool->Close(); Require(closed.liveBytes==a->Bytes(),"close keeps live output");
        Validate(a,7); hit.reset(); a.reset();
        pool->Close(); Require(pool->Snapshot().committedBytes==0,"close eventually frees");
        // Preferred segment size is advisory: a smaller budget can still fit this request.
        Config bounded; bounded.maxCommittedBytes=2048; bounded.maxCachedFreeBytes=0;
        auto boundedPool=Pool::Create(bounded);
        auto boundedPending=Submit(*boundedPool,stream,side,7); ++calls;
        auto boundedAllocation=boundedPending->Wait(); Validate(boundedAllocation,7);
        Require(boundedPool->Snapshot().committedBytes<=2048,"segment preference exceeds budget");
        std::atomic<bool> hookRan{false};
        boundedAllocation->SetRetireHook([&hookRan,boundedPool] {
            // Snapshot/Collect re-entry must not deadlock behind the retirement hook.
            boundedPool->Collect(); hookRan.store(true);
        });
        boundedPending.reset(); boundedAllocation.reset();
        for(int i=0;i<500 && boundedPool->Collect().committedBytes; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Require(hookRan.load(),"native retirement hook did not run");
        Require(boundedPool->Snapshot().committedBytes==0,"cache budget must return empty segments automatically");
        boundedPool->Close();
        // Hold the first launch until its follower is admitted. Both requests are
        // queued against an empty pool; only the first may claim the fresh segment.
        auto fifoPool=Pool::Create(config);
        std::atomic<bool> followerAdmitted{false};
        std::atomic<int> launchOrder{0};
        auto leader=Submit(*fifoPool,stream,side,40,[&] {
            while(!followerAdmitted.load()) std::this_thread::yield();
            Require(launchOrder.fetch_add(1)==0,"FIFO leader launch order");
        }); ++calls;
        auto follower=Submit(*fifoPool,stream,side,1,[&] {
            Require(launchOrder.fetch_add(1)==1,"FIFO follower launch order");
        }); ++calls;
        followerAdmitted.store(true);
        auto firstRange=leader->Wait(); auto nextRange=follower->Wait();
        Validate(firstRange,40); Validate(nextRange,1);
        auto firstHandle=firstRange->Identity(); auto nextHandle=nextRange->Identity();
        Require(firstHandle.generation<nextHandle.generation,"FIFO allocation generation order");
        Require(firstHandle.segmentId==nextHandle.segmentId,"follower should use growth remainder");
        Require(nextHandle.offset>=firstHandle.offset+firstHandle.bytes,"follower stole leader growth range");
        Require(fifoPool->Snapshot().growthCount==1,"FIFO pair should require one growth");
        std::cout<<"FIFO_PASS queued_before_first_launch=2 growth=1 leader_bytes="<<firstHandle.bytes
                 <<" follower_bytes="<<nextHandle.bytes<<" leader_offset="<<firstHandle.offset
                 <<" follower_offset="<<nextHandle.offset<<'\n';
        leader.reset(); follower.reset(); firstRange.reset(); nextRange.reset(); fifoPool->Close();
        CheckRingBehavior(stream,side,calls);
        CheckMetadataEarly(stream,side,calls,7,false,false);
        CheckMetadataEarly(stream,side,calls,7,true,false);
        CheckMetadataEarly(stream,side,calls,0,false,false);
        CheckMetadataEarly(stream,side,calls,7,false,true);
        CheckZeroBoundaryClose(stream,side,calls,false);
        CheckZeroBoundaryClose(stream,side,calls,true);
        Config small; small.segmentBytes=256; small.maxCommittedBytes=256;
        auto failPool=Pool::Create(small);
        bool failed=false; auto fail=Submit(*failPool,stream,side,71); ++calls;
        try { fail->Wait(); } catch(const std::exception& e) { failed=true; std::cout<<"expected_failure="<<e.what()<<'\n'; }
        Require(failed,"budget failure propagated");
        bool rejected=false; try { Submit(*failPool,stream,side,1); } catch(...) { rejected=true; }
        Require(rejected,"failure poisons queued domain");
        fail.reset(); failPool->Close();
        uint64_t sideCount=0; Check(aclrtMemcpy(&sideCount,8,side,8,ACL_MEMCPY_DEVICE_TO_HOST),"side read");
        Require(sideCount==calls,"one side effect per producer launch, no replay");
        std::cout<<"POOL_PASS calls="<<calls<<" side_effect="<<sideCount<<" hot_growth_count="<<grows<<'\n';
        Check(aclrtFree(side),"free side"); Check(aclrtDestroyStream(stream),"destroy stream");
        Check(rtDevBinaryUnRegister(registered),"unregister"); Check(aclFinalize(),"finalize");
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
