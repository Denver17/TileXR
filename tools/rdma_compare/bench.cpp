#include "tilexr_api.h"
#include "tilexr_rdma.h"
#include "tilexr_rdma_wire.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include "bench_abi.h"
#include "case_layout.h"
#include <acl/acl.h>
#include <runtime/kernel.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    Require(*text && *text != '-' && end && *end == 0 && errno == 0 && value >= low && value <= high, "invalid argument");
    return uint32_t(value);
}
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Deadline(int) { const char text[] = "matched benchmark deadline\n"; (void)!write(2, text, sizeof(text)-1); _exit(124); }

class Benchmark {
    uint32_t rank_, device_, payload_, count_, direction_, sgeCount_;
    CaseLayout layout_;
    TileXR::TileXRSockExchange socket_;
    TileXRCommPtr comm_ = nullptr;
    TileXRRDMAMemHandle handle_ = 0;
    void *memory_ = nullptr, *viewGM_ = nullptr, *resultGM_ = nullptr, *binaryHandle_ = nullptr;
    aclrtStream stream_ = nullptr;
    aclrtEvent eventStart_ = nullptr, eventEnd_ = nullptr;
    TileXRRDMADeviceView view_{};
    std::vector<uint8_t> binary_;
    uint64_t phase_ = 0;
    bool initialized_ = false;
public:
    Benchmark(uint32_t rank, uint32_t device, uint32_t payload, uint32_t count,
        uint32_t direction, bool streaming, uint32_t numSge)
        : rank_(rank), device_(device), payload_(payload), count_(count), direction_(direction), sgeCount_(numSge),
          layout_(payload, count, streaming, numSge), socket_(rank, 2, 0) {}

    template<class F> void Phase(F function) {
        uint64_t failed = 0;
        try { function(); } catch (const std::exception& error) {
            std::cerr << "rank=" << rank_ << " phase=" << phase_ << " " << error.what() << std::endl; failed = 1;
        }
        const std::array<uint64_t, 15> packet{{phase_++, failed, payload_, count_, direction_,
            layout_.cycles, layout_.bytes, sgeCount_, kCaseLayoutVersion, layout_.source, layout_.target,
            layout_.stride, layout_.targetStride, layout_.slots, layout_.totalWqeBytes}};
        std::array<std::array<uint64_t, 15>, 2> all{};
        Check(socket_.AllGather(packet.data(), packet.size(), all[0].data(), 20000), "phase exchange");
        for (const auto& peer : all) {
            Require(peer[1] == 0, "local/peer phase failed");
            for (size_t i = 0; i < packet.size(); ++i) {
                if (i != 1) { Require(peer[i] == packet[i], "case mismatch"); }
            }
        }
    }

    BenchResult Launch(bool calibration) {
        Check(aclrtMemset(resultGM_, sizeof(BenchResult), 0, sizeof(BenchResult)), "clear results");
        BenchArgs args{reinterpret_cast<uintptr_t>(viewGM_), reinterpret_cast<uintptr_t>(resultGM_),
            layout_.source, layout_.target, layout_.stride, payload_, count_, rank_, direction_, layout_.cycles,
            uint32_t(calibration), layout_.targetStride, sgeCount_, 0U};
        rtArgsEx_t launch{}; launch.args = &args; launch.argsSize = sizeof(args);
        rtTaskCfgInfo_t task{};
        Check(aclrtRecordEvent(eventStart_, stream_), "start event");
        Check(rtKernelLaunchWithFlagV2(&signature, 1, &launch, nullptr, static_cast<rtStream_t>(stream_), 0, &task), "kernel launch");
        Check(aclrtRecordEvent(eventEnd_, stream_), "end event");
        Check(aclrtSynchronizeStreamWithTimeout(stream_, 20000), "stream sync");
        BenchResult result{};
        Check(aclrtMemcpy(&result, sizeof(result), resultGM_, sizeof(result), ACL_MEMCPY_DEVICE_TO_HOST), "read result");
        if (result.magic != 0x5458524D41544348ULL || result.status || result.error) {
            std::cerr << "KERNEL_RESULT rank=" << rank_ << " calibration=" << calibration
                << " magic=" << result.magic << " status=" << result.status << " error=" << result.error
                << " producer=" << result.producer << " completed=" << result.completed
                << " cq=" << result.cqConsumer << " start=" << result.start << " end=" << result.end << std::endl;
        }
        Require(result.magic == 0x5458524D41544348ULL && result.status == 0 && result.error == 0, "kernel result");
        return result;
    }

    void Run(const char* elf) {
        Phase([&] {
            std::cerr << "INIT_STAGE rank=" << rank_ << " enter=aclInit" << std::endl;
            Check(aclInit(nullptr), "aclInit"); initialized_ = true;
            std::cerr << "INIT_STAGE rank=" << rank_ << " enter=set_device" << std::endl;
            Check(aclrtSetDevice(device_), "set device");
            std::cerr << "INIT_STAGE rank=" << rank_ << " enter=create_stream" << std::endl;
            Check(aclrtCreateStream(&stream_), "create stream");
            std::cerr << "INIT_STAGE rank=" << rank_ << " enter=create_events" << std::endl;
            Check(aclrtCreateEvent(&eventStart_), "create event"); Check(aclrtCreateEvent(&eventEnd_), "create event");
            std::cerr << "INIT_STAGE rank=" << rank_ << " complete=acl_setup" << std::endl;
        });
        Phase([&] {
            Check(TileXRCommInitRankWithDomain(1, 2, rank_, &comm_), "comm init");
            TileXRRDMAInfo info{}; Check(TileXRRDMAQuery(comm_, &info), "query");
            Require(info.deviceDirect && !info.hostDriven && info.connected && !info.failed, "AIV direct required");
        });
        Phase([&] {
            Check(aclrtMalloc(&memory_, layout_.bytes, ACL_MEM_MALLOC_HUGE_ONLY), "payload allocation");
            Check(aclrtMalloc(&viewGM_, sizeof(view_), ACL_MEM_MALLOC_NORMAL_ONLY), "view allocation");
            Check(aclrtMalloc(&resultGM_, sizeof(BenchResult), ACL_MEM_MALLOC_NORMAL_ONLY), "result allocation");
            std::ifstream file(elf, std::ios::binary | std::ios::ate);
            Require(bool(file), "open ELF"); const auto bytes = file.tellg();
            Require(bytes > 64 && bytes < 64 * 1024 * 1024, "ELF extent");
            binary_.resize(size_t(bytes)); file.seekg(0); file.read(reinterpret_cast<char*>(binary_.data()), bytes);
            Require(bool(file) && std::memcmp(binary_.data(), "\177ELF", 4) == 0, "ELF contents");
            rtDevBinary_t binary{}; binary.magic = 0x41415246; binary.data = binary_.data(); binary.length = binary_.size();
            Check(rtDevBinaryRegister(&binary, &binaryHandle_), "register ELF");
            Check(rtFunctionRegister(binaryHandle_, &signature, "tilexr_matched_write", "tilexr_matched_write", 0), "register function");
        });
        Phase([&] { Check(TileXRRDMARegister(comm_, memory_, layout_.bytes, &handle_), "register MR"); });
        Phase([&] {
            Check(TileXRRDMAGetDeviceView(comm_, handle_, 1-rank_, &view_), "device view");
            const uint64_t sqBlocks = uint64_t(count_) * TileXR::RdmaWire::WqeBlocks(sgeCount_);
            Require(view_.localBytes == layout_.bytes && view_.remoteBytes == layout_.bytes &&
                view_.queue.sqEntryBytes == 64U && uint64_t(view_.queue.sqDepth) >= sqBlocks + 1U &&
                view_.queue.cqDepth > (count_+63)/64 && view_.queue.maxSendSge >= sgeCount_ &&
                view_.queue.maxSendWr >= std::min(count_, 128U) &&
                view_.queue.dbMode == TILEXR_RDMA_DB_HARD, "queue admission (SQ blocks plus stop, CQ, SGE, WR, hard DB)");
            Check(aclrtMemcpy(viewGM_, sizeof(view_), &view_, sizeof(view_), ACL_MEMCPY_HOST_TO_DEVICE), "publish view");
            Check(aclrtMemset(memory_, layout_.bytes, 0xA5, layout_.bytes), "initialize MR");
            std::vector<uint64_t> source(payload_/8);
            for (uint32_t slot = 0; slot < layout_.slots; ++slot) {
                for (uint32_t entry = 0; entry < sgeCount_; ++entry) {
                    std::fill(source.begin(), source.end(), CasePattern(rank_, entry, slot, sgeCount_));
                    Check(aclrtMemcpy(static_cast<uint8_t*>(memory_)+layout_.SourceOffset(slot, entry),
                        payload_, source.data(), payload_, ACL_MEMCPY_HOST_TO_DEVICE), "initialize source");
                }
            }
            std::cout << "EFFECTIVE rank=" << rank_ << " device=" << device_ << " qpn=" << view_.queue.qpn
                << " sq=" << view_.queue.sqDepth << " cq=" << view_.queue.cqDepth
                << " max_send_wr=" << view_.queue.maxSendWr << " db_mode=" << view_.queue.dbMode
                << " max_send_sge=" << view_.queue.maxSendSge << " sge_count=" << sgeCount_
                << " sge_bytes=" << payload_ << " total_wqe_bytes=" << layout_.totalWqeBytes
                << " wqe_blocks=" << TileXR::RdmaWire::WqeBlocks(sgeCount_) << " required_sq_blocks=" << sqBlocks + 1U
                << " stride=" << layout_.stride << " source_stride=" << layout_.stride
                << " target_stride=" << layout_.targetStride << " slots=" << layout_.slots
                << " source=" << layout_.source << " target=" << layout_.target << " mr_bytes=" << layout_.bytes << std::endl;
        });
        Phase([&] {
            // First registration/launch can add event time outside the cycle
            // interval. Warm only the calibration kernel; no WQE is posted.
            (void)Launch(true);
            const auto calibration = Launch(true); float ms = 0;
            Check(aclrtEventElapsedTime(&ms, eventStart_, eventEnd_), "timer calibration");
            const double ratio = double(calibration.end-calibration.start)/(double(ms)*1e6);
            std::cout << "TIMER_CALIBRATION rank=" << rank_ << " device=" << device_
                << " ticks=" << calibration.end-calibration.start << " acl_ms=" << ms
                << " ticks_per_event_ns=" << ratio << std::endl;
            Require(ms > 0 && ratio > 0.95 && ratio < 1.01, "1GHz counter calibration");
        });
        BenchResult result{}; uint64_t hostStart = 0, hostEnd = 0;
        Phase([&] { hostStart = Now(); result = Launch(false); hostEnd = Now(); });
        Phase([&] {
            const bool sender = direction_ == 2 || direction_ == rank_;
            const uint64_t sqBlocks = uint64_t(count_) * TileXR::RdmaWire::WqeBlocks(sgeCount_);
            Require(result.producer == (sender ? sqBlocks : 0) && result.completed == result.producer &&
                result.cqConsumer == (sender ? (count_+63)/64 : 0), "queue progress");
            Verify();
        });
        std::cout << std::setprecision(12) << "MATCHED_SAMPLE {\"rank\":" << rank_ << ",\"device\":" << device_
            << ",\"bytes\":" << payload_ << ",\"count\":" << count_ << ",\"direction\":" << direction_
            << ",\"sge_count\":" << sgeCount_ << ",\"sge_bytes\":" << payload_
            << ",\"total_wqe_bytes\":" << layout_.totalWqeBytes
            << ",\"source_stride\":" << layout_.stride << ",\"target_stride\":" << layout_.targetStride
            << ",\"mr_bytes\":" << layout_.bytes << ",\"layout_version\":" << kCaseLayoutVersion
            << ",\"producer\":" << result.producer << ",\"completed\":" << result.completed
            << ",\"cq_consumer\":" << result.cqConsumer
            << ",\"cycles\":" << layout_.cycles << ",\"start_ticks\":" << result.start << ",\"end_ticks\":" << result.end
            << ",\"host_start_ns\":" << hostStart << ",\"host_end_ns\":" << hostEnd
            << ",\"gbps\":" << (result.end > result.start ? double(layout_.totalWqeBytes)*count_*8/(result.end-result.start) : 0)
            << ",\"correct\":true}" << std::endl;
    }
    void Verify() {
        constexpr uint64_t chunk = 8*1024*1024;
        std::vector<uint64_t> data(chunk/8);
        for (uint64_t base = 0; base < layout_.bytes; base += chunk) {
            const auto bytes = std::min(chunk, layout_.bytes-base);
            Check(aclrtMemcpy(data.data(), chunk, static_cast<uint8_t*>(memory_)+base, bytes, ACL_MEMCPY_DEVICE_TO_HOST), "verify read");
            for (uint64_t i=0; i<bytes/8; ++i) {
                const auto off = base+i*8;
                const uint64_t expected = ExpectedCaseWord(layout_, off, rank_, count_, direction_);
                if (data[i] != expected) {
                    std::cerr << "VERIFY_MISMATCH rank=" << rank_ << " offset=" << off
                        << " actual=" << data[i] << " expected=" << expected << std::endl;
                    Require(false, "source/destination/guard mismatch");
                }
            }
        }
    }
    bool Cleanup() {
        try {
            if (stream_) { Check(aclrtSynchronizeStreamWithTimeout(stream_,20000), "cleanup sync"); }
            if (comm_) { Check(TileXRCommDestroy(comm_), "destroy QPs before memory"); comm_=nullptr; }
            if (stream_) { Check(aclrtDestroyStream(stream_), "destroy stream"); }
            if (eventStart_) { Check(aclrtDestroyEvent(eventStart_), "destroy event"); }
            if (eventEnd_) { Check(aclrtDestroyEvent(eventEnd_), "destroy event"); }
            if (binaryHandle_) { Check(rtDevBinaryUnRegister(binaryHandle_), "unregister ELF"); }
            for (void* ptr : {memory_,viewGM_,resultGM_}) { if (ptr) { Check(aclrtFree(ptr), "free"); } }
            if (initialized_) { Check(aclFinalize(), "finalize"); }
            return true;
        } catch (const std::exception& e) { std::cerr<<"cleanup retained resources: "<<e.what()<<std::endl; return false; }
    }
};
}

int main(int argc, char** argv)
{
    Benchmark* bench=nullptr; int status=1;
    try {
        Require(argc==9 || argc==10, "RANK DEVICE BYTES COUNT DIRECTION ADDRESS_MODE ELF --describe|--run [NUM_SGE]");
        const auto rank=Number(argv[1],0,1), device=Number(argv[2],0,15), bytes=Number(argv[3],512,1048576);
        const auto count=Number(argv[4],1,1000), direction=Number(argv[5],0,2), mode=Number(argv[6],0,1);
        const auto numSge=argc==10 ? Number(argv[9],1,16) : 1U;
        Require((bytes&(bytes-1))==0, "payload power of two");
        Require((numSge&(numSge-1))==0, "SGE count must be 1/2/4/8/16");
        const CaseLayout layout(bytes,count,mode!=0,numSge);
        if (std::strcmp(argv[8],"--describe")==0) {
            std::cout<<"MATCHED_CASE bytes="<<bytes<<" count="<<count<<" direction="<<direction
                <<" cycles="<<layout.cycles<<" stride="<<layout.stride<<" mr_bytes="<<layout.bytes
                <<" sge_count="<<numSge<<" sge_bytes="<<bytes<<" total_wqe_bytes="<<layout.totalWqeBytes
                <<" source_stride="<<layout.stride<<" target_stride="<<layout.targetStride
                <<" source="<<layout.source<<" target="<<layout.target<<" slots="<<layout.slots
                <<" wqe_blocks="<<TileXR::RdmaWire::WqeBlocks(numSge)<<" layout_version="<<kCaseLayoutVersion
                <<" post_batch=64 signal_every=64 max_in_flight=128 no_wrap=1 npu_calls=0"<<std::endl;
            return 0;
        }
        Require(std::strcmp(argv[8],"--run")==0, "explicit --run required");
        Require(std::getenv("RDMA_BENCH_CPU_ONLY")==nullptr, "CPU-only mode forbids NPU execution");
        struct sigaction action{}; action.sa_handler=Deadline; sigemptyset(&action.sa_mask);
        Check(sigaction(SIGALRM,&action,nullptr),"sigaction"); alarm(120);
        bench=new Benchmark(rank,device,bytes,count,direction,mode!=0,numSge);
        bench->Run(argv[7]); status=0;
    } catch (const std::exception& e) { std::cerr<<"MATCHED_FAIL "<<e.what()<<std::endl; }
    if (bench && bench->Cleanup()) { delete bench; } else if (bench) { status=1; }
    alarm(0); return status;
}
