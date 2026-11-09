/*
 * 供 test_pool 和 growth_fault_injection 注册的单核入口 ElasticPoolTest。
 * 普通 seed 经 AllocateBundle 生成实际 output，递增分配前副作用并写入、读回内容。
 * 测试专用高位选择 held producer 或 consumer：Host 映射 gate 控制生产者在发布元数据后
 * 继续执行，consumer 检查最终数据和完成标志；独立 predecessor 延迟 consumer stream 的依赖。
 * 迟到错误只发布 finalStatus，保持 Output 不变。
 * 辅助内存、gate、stream 和外置 binary 的生命周期由 Host 测试持有。
 */
#include "device/tilexr_elastic_memory_device.h"
#include <cstddef>
using namespace AscendC;
using namespace TileXR::Elastic;
namespace {
// These fixture-only bits share the existing four-argument kernel signature.
constexpr uint64_t kHeldProducer = uint64_t(1) << 63;
constexpr uint64_t kConsumer = uint64_t(1) << 62;
constexpr uint64_t kLateFailure = uint64_t(1) << 61;
constexpr uint64_t kHeldPredecessor = uint64_t(1) << 60;
constexpr uint64_t kSeedMask = ~(kHeldProducer | kConsumer | kLateFailure | kHeldPredecessor);
// Separate 64B cache lines: Host release, producer reached/done, consumer done,
// and immutable Host configuration containing the original side-effect address.
constexpr uint32_t kReleaseWord = 0, kReachedWord = 8, kProducerDoneWord = 16;
constexpr uint32_t kConsumerDoneWord = 24, kSideAddressWord = 32;
__aicore__ inline void Mark(uint64_t gate, uint32_t word, uint64_t value,
                            LocalTensor<uint64_t>& scratch) {
    uint64_t words[8] = {value,0,0,0,0,0,0,0};
    DeviceDetail::WriteWords(gate + word * sizeof(uint64_t), words, 8, scratch);
}
__aicore__ inline void PublishLateFailure(uint64_t mailbox, LocalTensor<uint64_t>& scratch) {
    DeviceDetail::Invalidate(mailbox, offsetof(Mailbox, requestId) / sizeof(uint64_t));
    const uint64_t sequence = DeviceDetail::ReadWord(mailbox, offsetof(Mailbox, requestId) / sizeof(uint64_t));
    uint64_t final[8] = {0,ProtocolFailure,0,0,0,0,0,0};
    DeviceDetail::Publish(mailbox + offsetof(Mailbox, finalStatus), final, sequence, scratch);
}
__aicore__ inline bool CheckData(uint64_t address, uint64_t bytes, uint64_t seed,
                                LocalTensor<uint64_t>& scratch) {
    GlobalTensor<uint64_t> data;
    data.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(address), bytes / 8);
    for (uint64_t start = 0; start < bytes / 8; start += 4) {
        DeviceDetail::Sync<HardEvent::S_MTE2>();
        DataCopy(scratch, data[start], 4);
        DeviceDetail::Sync<HardEvent::MTE2_S>();
        for (uint32_t i = 0; i < 4; ++i)
            if (scratch.GetValue(i) != start + i + seed) return false;
    }
    return true;
}
}
extern "C" __global__ __aicore__ void ElasticPoolTest(uint64_t state, uint64_t mailbox,
                                                     uint64_t seed, GM_ADDR sideEffect)
{
    TPipe pipe;
    TBuf<QuePosition::VECCALC> buffer;
    pipe.InitBuffer(buffer,64);
    auto scratch=buffer.Get<uint64_t>();
    const uint64_t mode=seed & ~kSeedMask;
    seed &= kSeedMask;
    const uint64_t gate=reinterpret_cast<uint64_t>(sideEffect);
    if (mode & kHeldPredecessor) {
        Mark(gate,kReachedWord,1,scratch);
        do { DeviceDetail::Invalidate(gate,kReleaseWord); }
        while(DeviceDetail::ReadWord(gate,kReleaseWord)==0);
        Mark(gate,kProducerDoneWord,1,scratch);
        return;
    }
    if (mode & kConsumer) {
        // In this fixture mode state/mailbox carry the output address/byte count.
        DeviceDetail::Invalidate(gate,kProducerDoneWord);
        const bool producerDone=DeviceDetail::ReadWord(gate,kProducerDoneWord)==1;
        const bool valid=producerDone && (!mailbox || CheckData(state,mailbox,seed,scratch));
        Mark(gate,kConsumerDoneWord,valid?1:2,scratch);
        return;
    }
    uint64_t sideAddress=gate;
    if (mode & kHeldProducer) {
        DeviceDetail::Invalidate(gate,kSideAddressWord);
        sideAddress=DeviceDetail::ReadWord(gate,kSideAddressWord);
    }
    GlobalTensor<uint64_t> side;
    side.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(sideAddress),8);
    for (uint32_t i=0;i<8;++i) scratch.SetValue(i,0);
    scratch.SetValue(0,side.GetValue(0)+1);
    DeviceDetail::Sync<HardEvent::S_MTE3>();
    DataCopy(side,scratch,8);
    DeviceDetail::Sync<HardEvent::MTE3_S>();
    uint64_t address=0;
    const uint64_t rows=seed*3+1;
    const uint64_t bytes=seed==0 ? 0 : rows*32;
    const uint64_t error=AllocateBundle(DeviceView{state,mailbox},bytes,64,seed==0?0:rows,&address);
    if(error!=Success) return;
    if(mode & kHeldProducer) {
        Mark(gate,kReachedWord,1,scratch);
        do { DeviceDetail::Invalidate(gate,kReleaseWord); }
        while(DeviceDetail::ReadWord(gate,kReleaseWord)==0);
    }
    if(bytes) {
        GlobalTensor<uint64_t> data;
        data.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(address),bytes/8);
        for(uint64_t start=0;start<bytes/8;start+=4) {
            for(uint32_t i=0;i<4;++i) scratch.SetValue(i,start+i+seed);
            DeviceDetail::Sync<HardEvent::S_MTE3>();
            DataCopy(data[start],scratch,4);
            DeviceDetail::Sync<HardEvent::MTE3_S>();
        }
        if(!CheckData(address,bytes,seed,scratch)) {
            PublishLateFailure(mailbox,scratch);
            return;
        }
    }
    if(mode & kHeldProducer) Mark(gate,kProducerDoneWord,1,scratch);
    if(mode & kLateFailure) PublishLateFailure(mailbox,scratch);
}
