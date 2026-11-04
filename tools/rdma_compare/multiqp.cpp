#include "tilexr_api.h"
#include "tilexr_rdma.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include "multiqp_layout.h"
#include "multiqp_summary.h"
#include "tilexr_rdma_wire.h"
#include <acl/acl.h>
#include <runtime/kernel.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
uint8_t signature;
void Check(int status, const char* what) {
    if (status) { throw std::runtime_error(std::string(what) + " status=" + std::to_string(status)); }
}
void Require(bool ok, const char* what) { if (!ok) { throw std::runtime_error(what); } }
uint32_t Number(const char* text, uint32_t low, uint32_t high) {
    char* end = nullptr; errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    Require(*text && *text != '-' && end && !*end && !errno && value >= low && value <= high, "invalid argument");
    return uint32_t(value);
}
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Deadline(int) { const char text[] = "multiqp deadline\n"; (void)!write(2,text,sizeof(text)-1); _exit(124); }

class Benchmark {
    uint32_t rank_, device_, payload_, count_, direction_, qps_, sgeCount_;
    CaseLayout layout_;
    TileXR::TileXRSockExchange socket_;
    TileXRCommPtr comm_ = nullptr;
    std::vector<void*> memories_;
    std::vector<TileXRRDMADeviceView> views_;
    void *viewGM_ = nullptr, *resultGM_ = nullptr, *binaryHandle_ = nullptr;
    std::vector<uint8_t> binary_;
    aclrtStream stream_ = nullptr;
    aclrtEvent eventStart_ = nullptr, eventEnd_ = nullptr;
    bool initialized_ = false;
    uint64_t phase_ = 0;
    float lastEventMs_ = 0;
public:
    Benchmark(uint32_t rank,uint32_t device,uint32_t bytes,uint32_t count,uint32_t direction,uint32_t qps,uint32_t numSge)
        : rank_(rank),device_(device),payload_(bytes),count_(count),direction_(direction),qps_(qps),sgeCount_(numSge),
          layout_(bytes,count,false,numSge),socket_(rank,2,0),memories_(qps),views_(qps) {}
    template<class F> void Phase(F function) {
        uint64_t failed = 0;
        try { function(); } catch (const std::exception& e) {
            std::cerr << "PHASE_FAIL rank=" << rank_ << " phase=" << phase_ << " " << e.what() << std::endl; failed = 1;
        }
        const std::array<uint64_t,8> packet{{phase_++,failed,payload_,count_,direction_,qps_,layout_.bytes,sgeCount_}};
        std::array<std::array<uint64_t,8>,2> peers{};
        Check(socket_.AllGather(packet.data(),packet.size(),peers[0].data(),20000),"case agreement");
        for (const auto& peer : peers) {
            Require(!peer[1],"local/peer phase failed");
            for (size_t i=0;i<packet.size();++i) { if (i!=1) { Require(peer[i]==packet[i],"case mismatch"); } }
        }
    }
    std::vector<BenchResult> Launch(bool calibration) {
        Check(aclrtMemset(resultGM_,qps_*sizeof(BenchResult),0,qps_*sizeof(BenchResult)),"clear results");
        BenchArgs args{reinterpret_cast<uintptr_t>(viewGM_),reinterpret_cast<uintptr_t>(resultGM_),
            layout_.source,layout_.target,layout_.stride,payload_,count_,rank_,direction_,layout_.cycles,
            uint32_t(calibration),layout_.targetStride,sgeCount_,qps_};
        rtArgsEx_t launch{}; launch.args=&args; launch.argsSize=sizeof(args); rtTaskCfgInfo_t task{};
        Check(aclrtRecordEvent(eventStart_,stream_),"start event");
        Check(rtKernelLaunchWithFlagV2(&signature,qps_,&launch,nullptr,static_cast<rtStream_t>(stream_),0,&task),"launch all QPs");
        Check(aclrtRecordEvent(eventEnd_,stream_),"end event");
        Check(aclrtSynchronizeStreamWithTimeout(stream_,20000),"stream sync");
        Check(aclrtEventElapsedTime(&lastEventMs_,eventStart_,eventEnd_),"event time");
        std::vector<BenchResult> results(qps_);
        Check(aclrtMemcpy(results.data(),qps_*sizeof(BenchResult),resultGM_,qps_*sizeof(BenchResult),ACL_MEMCPY_DEVICE_TO_HOST),"read results");
        bool failed = false;
        for (uint32_t q=0;q<qps_;++q) {
            const auto& r=results[q];
            if (r.magic!=0x5458524D41544348ULL || r.status || r.error) {
                std::cerr<<"QP_FAIL rank="<<rank_<<" qp="<<q<<" status="<<r.status<<" error="<<r.error
                    <<" producer="<<r.producer<<" completed="<<r.completed<<" cq="<<r.cqConsumer<<std::endl;
                failed = true;
            }
        }
        if (failed) {
            for (uint32_t q=0;q<qps_;++q) {
                const auto& r=results[q];
                std::cerr<<"QP_DIAGNOSTIC rank="<<rank_<<" qp="<<q<<" status="<<r.status<<" error="<<r.error
                    <<" start="<<r.start<<" end="<<r.end<<" producer="<<r.producer
                    <<" completed="<<r.completed<<" cq="<<r.cqConsumer<<std::endl;
                if (r.status || r.error) {
                    DumpQueue(q,"CQ",views_[q].queue.cq,8U*64U);
                    DumpQueue(q,"SQ",views_[q].queue.sq+r.completed*64U,
                        uint32_t(r.producer-r.completed+1U)*64U);
                }
            }
            Require(false,"kernel QP error");
        }
        return results;
    }
    void DumpQueue(uint32_t q,const char* name,uint64_t address,uint32_t bytes) {
        std::vector<uint64_t> words(bytes/8U);
        const auto status=aclrtMemcpy(words.data(),bytes,reinterpret_cast<const void*>(address),bytes,ACL_MEMCPY_DEVICE_TO_HOST);
        std::cerr<<"QUEUE_SNAPSHOT rank="<<rank_<<" qp="<<q<<" name="<<name<<" address="<<address
            <<" bytes="<<bytes<<" status="<<status;
        if (!status) { for (auto word:words) { std::cerr<<' '<<std::hex<<word; } }
        std::cerr<<std::dec<<std::endl;
    }
    void Run(const char* elf) {
        Phase([&] {
            Check(aclInit(nullptr),"aclInit"); initialized_=true;
            Check(aclrtSetDevice(device_),"set device"); Check(aclrtCreateStream(&stream_),"stream");
            Check(aclrtCreateEvent(&eventStart_),"event start"); Check(aclrtCreateEvent(&eventEnd_),"event end");
            Check(aclrtMalloc(&viewGM_,qps_*sizeof(TileXRRDMADeviceView),ACL_MEM_MALLOC_NORMAL_ONLY),"view allocation");
            Check(aclrtMalloc(&resultGM_,qps_*sizeof(BenchResult),ACL_MEM_MALLOC_NORMAL_ONLY),"results allocation");
            std::ifstream file(elf,std::ios::binary|std::ios::ate); Require(bool(file),"open ELF");
            const auto n=file.tellg(); Require(n>64 && n<64*1024*1024,"ELF extent");
            binary_.resize(size_t(n)); file.seekg(0); file.read(reinterpret_cast<char*>(binary_.data()),n);
            Require(bool(file) && !std::memcmp(binary_.data(),"\177ELF",4),"ELF contents");
            rtDevBinary_t binary{}; binary.magic=0x41415246; binary.data=binary_.data(); binary.length=binary_.size();
            Check(rtDevBinaryRegister(&binary,&binaryHandle_),"register ELF");
            Check(rtFunctionRegister(binaryHandle_,&signature,"tilexr_multiqp_write","tilexr_multiqp_write",0),"function");
        });
        Phase([&] {
            Check(TileXRCommInitRankWithDomain(1,2,rank_,&comm_),"comm init");
            TileXRRDMAInfo info{}; Check(TileXRRDMAQuery(comm_,&info),"query");
            Require(info.deviceDirect && !info.hostDriven && info.connected && !info.failed,"AIV direct required");
        });
        Phase([&] { Check(TileXRRDMAConfigureQps(comm_,qps_),"configure independent QPs"); });
        for (uint32_t q=0;q<qps_;++q) {
            Phase([&] { Check(aclrtMalloc(&memories_[q],layout_.bytes,ACL_MEM_MALLOC_HUGE_ONLY),"MR allocation"); });
            TileXRRDMAMemHandle handle = 0;
            Phase([&] { Check(TileXRRDMARegister(comm_,memories_[q],layout_.bytes,&handle),"register MR"); });
            Phase([&] {
                std::vector<TileXRRDMADeviceView> regionViews(qps_);
                Check(TileXRRDMAGetDeviceViews(comm_,handle,1-rank_,regionViews.data(),qps_),"device views");
                views_[q]=regionViews[q]; // Each MR shares all QPs; AIV q uses only MR q's view q.
                const auto& v=views_[q];
                Require(v.version==TILEXR_RDMA_DEVICE_VERSION && v.structBytes==sizeof(v) &&
                    v.localAddress==reinterpret_cast<uintptr_t>(memories_[q]) && v.remoteAddress &&
                    v.localBytes==layout_.bytes && v.remoteBytes==layout_.bytes,"MR view extent/ABI");
                const uint64_t sqBlocks=uint64_t(count_)*TileXR::RdmaWire::WqeBlocks(sgeCount_);
                Require(v.queue.sqEntryBytes==64 && v.queue.sqDepth>sqBlocks && v.queue.cqDepth>(count_+63)/64 &&
                    v.queue.maxSendWr>=std::min(count_,128U) && v.queue.maxSendSge>=sgeCount_ && v.queue.dbMode==TILEXR_RDMA_DB_HARD,
                    "queue admission");
                Check(aclrtMemset(memories_[q],layout_.bytes,0xA5,layout_.bytes),"initialize MR guards");
                std::vector<uint64_t> source(payload_/8);
                for (uint32_t slot=0;slot<layout_.slots;++slot) {
                    for (uint32_t entry=0;entry<sgeCount_;++entry) {
                        std::fill(source.begin(),source.end(),CasePattern(rank_,entry,slot,sgeCount_)^MultiQpPattern(q,sgeCount_));
                        Check(aclrtMemcpy(static_cast<uint8_t*>(memories_[q])+layout_.SourceOffset(slot,entry),payload_,
                            source.data(),payload_,ACL_MEMCPY_HOST_TO_DEVICE),"initialize source");
                    }
                }
                std::cout<<"MULTIQP_EFFECTIVE rank="<<rank_<<" qp="<<q<<" qpn="<<v.queue.qpn
                    <<" state="<<v.queue.state<<" sq="<<v.queue.sqDepth<<" cq="<<v.queue.cqDepth
                    <<" max_send_wr="<<v.queue.maxSendWr<<" db_mode="<<v.queue.dbMode<<" mr_bytes="<<layout_.bytes
                    <<" max_send_sge="<<v.queue.maxSendSge<<" sge_count="<<sgeCount_<<" sge_bytes="<<payload_
                    <<" total_wqe_bytes="<<layout_.totalWqeBytes<<" wqe_blocks="<<TileXR::RdmaWire::WqeBlocks(sgeCount_)
                    <<" required_sq_blocks="<<sqBlocks+1U<<" source_stride="<<layout_.stride<<" target_stride="<<layout_.targetStride
                    <<" doorbell="<<v.queue.doorbell<<" db_value="<<v.queue.sqDbValue<<" shared_provider=1"<<std::endl;
            });
        }
        Phase([&] {
            std::set<uint64_t> states; std::set<uint32_t> qpns;
            for (const auto& view:views_) { states.insert(view.queue.state); qpns.insert(view.queue.qpn); }
            Require(states.size()==qps_ && qpns.size()==qps_,"independent QP identities");
            Check(aclrtMemcpy(viewGM_,qps_*sizeof(TileXRRDMADeviceView),views_.data(),qps_*sizeof(TileXRRDMADeviceView),ACL_MEMCPY_HOST_TO_DEVICE),"publish all views");
            (void)Launch(true); const auto r=Launch(true); MultiQpSummary calibration;
            Require(SummarizeQps(r.data(),qps_,payload_,count_,calibration,sgeCount_),"calibration results");
            const double ratio=double(calibration.duration)/(double(lastEventMs_)*1e6);
            std::cout<<"MULTIQP_TIMER rank="<<rank_<<" qps="<<qps_<<" ticks="<<calibration.duration
                <<" acl_ms="<<lastEventMs_<<" ratio="<<ratio<<std::endl;
            Require(ratio>0.95 && ratio<1.01,"common 1GHz clock calibration");
        });
        std::vector<BenchResult> results; uint64_t hostStart=0,hostEnd=0; float eventMs=0;
        Phase([&] { hostStart=Now(); results=Launch(false); hostEnd=Now(); eventMs=lastEventMs_; });
        Phase([&] {
            const bool send=rank_==direction_;
            for (uint32_t q=0;q<qps_;++q) {
                const auto& r=results[q];
                const uint64_t sqBlocks=uint64_t(count_)*TileXR::RdmaWire::WqeBlocks(sgeCount_);
                Require(r.producer==(send?sqBlocks:0U) && r.completed==r.producer && r.cqConsumer==(send?(count_+63)/64:0U),"QP progress");
                if (!send) { Require(!r.start && !r.end,"receiver must not send"); }
                Verify(q);
            }
        });
        MultiQpSummary total;
        if (rank_==direction_) { Require(SummarizeQps(results.data(),qps_,payload_,count_,total,sgeCount_),"makespan"); }
        std::cout<<std::setprecision(12)<<"MULTIQP_SAMPLE {\"rank\":"<<rank_<<",\"device\":"<<device_
            <<",\"qps\":"<<qps_<<",\"bytes\":"<<payload_<<",\"count_per_qp\":"<<count_<<",\"direction\":"<<direction_
            <<",\"sge_count\":"<<sgeCount_<<",\"sge_bytes\":"<<payload_<<",\"total_wqe_bytes\":"<<layout_.totalWqeBytes
            <<",\"wqe_blocks\":"<<TileXR::RdmaWire::WqeBlocks(sgeCount_)<<",\"source_stride\":"<<layout_.stride
            <<",\"target_stride\":"<<layout_.targetStride<<",\"mr_bytes\":"<<layout_.bytes<<",\"layout_version\":"<<kCaseLayoutVersion
            <<",\"total_wqes\":"<<uint64_t(qps_)*count_<<",\"payload_bytes\":"<<total.payloadBytes
            <<",\"start_ticks\":"<<total.start<<",\"end_ticks\":"<<total.end<<",\"duration_ns\":"<<total.duration
            <<",\"start_skew_ns\":"<<total.latestStart-total.start
            <<",\"all_qps_overlap\":"<<(rank_==direction_ && total.latestStart<total.earliestEnd?"true":"false")
            <<",\"host_duration_ns\":"<<hostEnd-hostStart<<",\"acl_ms\":"<<eventMs<<",\"gbps\":"<<total.gbps
            <<",\"correct\":true,\"qps_detail\":[";
        for (uint32_t q=0;q<qps_;++q) {
            const auto& r=results[q]; if (q) { std::cout<<','; }
            std::cout<<"{\"qp\":"<<q<<",\"qpn\":"<<views_[q].queue.qpn<<",\"start\":"<<r.start<<",\"end\":"<<r.end
                <<",\"producer\":"<<r.producer<<",\"completed\":"<<r.completed<<",\"cq\":"<<r.cqConsumer<<'}';
        }
        std::cout<<"]}"<<std::endl;
    }
    void Verify(uint32_t q) {
        constexpr uint64_t chunk=8*1024*1024;
        std::vector<uint64_t> words(chunk/8);
        for (uint64_t base=0;base<layout_.bytes;base+=chunk) {
            const auto bytes=std::min(chunk,layout_.bytes-base);
            Check(aclrtMemcpy(words.data(),chunk,static_cast<uint8_t*>(memories_[q])+base,bytes,ACL_MEMCPY_DEVICE_TO_HOST),"verify read");
            for (uint64_t i=0;i<bytes/8;++i) {
                const uint64_t expected=ExpectedMultiQpWord(layout_,base+i*8,rank_,count_,direction_,q);
                if (words[i]!=expected) {
                    std::cerr<<"VERIFY_MISMATCH rank="<<rank_<<" qp="<<q<<" offset="<<base+i*8<<std::endl;
                    Require(false,"source/destination/guard mismatch");
                }
            }
        }
    }
    bool Cleanup() {
        try {
            if (stream_) { Check(aclrtSynchronizeStreamWithTimeout(stream_,20000),"cleanup sync"); }
            Phase([&] { if (comm_) { Check(TileXRRDMAShutdown(comm_),"shutdown every QP and MR before freeing memory"); } });
            Phase([&] {
                if (comm_) { Check(TileXRCommDestroy(comm_),"destroy communicator"); comm_=nullptr; }
            });
            if (binaryHandle_) { Check(rtDevBinaryUnRegister(binaryHandle_),"unregister ELF"); }
            if (stream_) { Check(aclrtDestroyStream(stream_),"destroy stream"); }
            if (eventStart_) { Check(aclrtDestroyEvent(eventStart_),"destroy event"); }
            if (eventEnd_) { Check(aclrtDestroyEvent(eventEnd_),"destroy event"); }
            for (void* ptr:memories_) { if (ptr) { Check(aclrtFree(ptr),"free MR"); } }
            for (void* ptr:{viewGM_,resultGM_}) { if (ptr) { Check(aclrtFree(ptr),"free metadata"); } }
            if (initialized_) { Check(aclFinalize(),"aclFinalize"); }
            return true;
        } catch(const std::exception& e) { std::cerr<<"CLEANUP_RETAINED "<<e.what()<<std::endl; return false; }
    }
};
}

int main(int argc,char** argv) {
    Benchmark* b=nullptr; int status=1;
    try {
        Require(argc==9 || argc==10,"RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS ELF --describe|--run [NUM_SGE]");
        const auto rank=Number(argv[1],0,1),device=Number(argv[2],0,15),bytes=Number(argv[3],512,1048576);
        const auto count=Number(argv[4],1,1000),direction=Number(argv[5],0,1),qps=Number(argv[6],1,16);
        const auto numSge=argc==10 ? Number(argv[9],1,16) : 1U;
        Require(!(bytes&(bytes-1U)) && !(qps&(qps-1U)),"bytes and QPs must be powers of two");
        Require(!(numSge&(numSge-1U)),"SGE count must be 1/2/4/8/16");
        if (!std::strcmp(argv[8],"--describe")) {
            const CaseLayout layout(bytes,count,false,numSge);
            std::cout<<"MULTIQP_CASE bytes="<<bytes<<" qps="<<qps<<" count_per_qp="<<count
                <<" total_wqes="<<uint64_t(qps)*count<<" sge="<<numSge<<" sge_count="<<numSge<<" sge_bytes="<<bytes
                <<" total_wqe_bytes="<<layout.totalWqeBytes<<" payload_bytes="<<uint64_t(qps)*count*layout.totalWqeBytes
                <<" wqe_blocks="<<TileXR::RdmaWire::WqeBlocks(numSge)
                <<" required_sq_blocks="<<uint64_t(count)*TileXR::RdmaWire::WqeBlocks(numSge)+1U
                <<" cq_completions="<<(count+63U)/64U<<" source_stride="<<layout.stride<<" target_stride="<<layout.targetStride
                <<" mr_bytes="<<layout.bytes<<" layout_version="<<kCaseLayoutVersion
                <<" batch=64 signal=64 max_in_flight_per_qp=128"
                <<" aivs="<<qps<<" no_wrap=1 shared_provider=1 timer=common_cycle_envelope npu_calls=0"<<std::endl; return 0;
        }
        Require(!std::strcmp(argv[8],"--run") && !std::getenv("RDMA_BENCH_CPU_ONLY"),"explicit --run without CPU-only guard required");
        struct sigaction action{}; action.sa_handler=Deadline; sigemptyset(&action.sa_mask);
        Check(sigaction(SIGALRM,&action,nullptr),"sigaction"); alarm(150);
        b=new Benchmark(rank,device,bytes,count,direction,qps,numSge); b->Run(argv[7]); status=0;
    } catch(const std::exception& e) { std::cerr<<"MULTIQP_FAIL "<<e.what()<<std::endl; }
    if (b && b->Cleanup()) { delete b; } else if (b) { status=1; }
    alarm(0); return status;
}
