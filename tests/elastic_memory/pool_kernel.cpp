/*
 * 供 test_pool 和 growth_fault_injection 加载的单核测试入口 ElasticPoolTest。
 * 从 seed 计算实际行数和字节数，调用生产 AllocateBundle；state/mailbox 由 Pool 提供。
 * 分配前先递增独立副作用计数，成功后在返回地址写入并读回数据，使 Host 能检查
 * 增长后是否仍由同一次 launch 继续执行。零字节和分配失败直接返回；读回不一致时
 * 通过 mailbox 发布 ProtocolFailure。辅助内存和 kernel 注册由 Host 测试负责管理。
 */
#include "device/tilexr_elastic_memory_device.h"
using namespace AscendC;
using namespace TileXR::Elastic;
extern "C" __global__ __aicore__ void ElasticPoolTest(uint64_t state, uint64_t mailbox,
                                                     uint64_t seed, GM_ADDR sideEffect)
{
    TPipe pipe;
    TBuf<QuePosition::VECCALC> buffer;
    pipe.InitBuffer(buffer,64);
    auto scratch=buffer.Get<uint64_t>();
    GlobalTensor<uint64_t> side;
    side.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(sideEffect),8);
    for (uint32_t i=0;i<8;++i) scratch.SetValue(i,0);
    scratch.SetValue(0,side.GetValue(0)+1);
    DeviceDetail::Sync<HardEvent::S_MTE3>();
    DataCopy(side,scratch,8);
    DeviceDetail::Sync<HardEvent::MTE3_S>();
    uint64_t address=0;
    const uint64_t rows=seed*3+1;
    const uint64_t bytes=seed==0 ? 0 : rows*32;
    const uint64_t error=AllocateBundle(DeviceView{state,mailbox},bytes,64,seed==0?0:rows,&address);
    if(error!=Success || bytes==0) return;
    GlobalTensor<uint64_t> data;
    data.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(address),bytes/8);
    for(uint64_t start=0;start<bytes/8;start+=4) {
        for(uint32_t i=0;i<4;++i) scratch.SetValue(i,start+i+seed);
        DeviceDetail::Sync<HardEvent::S_MTE3>();
        DataCopy(data[start],scratch,4);
        DeviceDetail::Sync<HardEvent::MTE3_MTE2>();
        DataCopy(scratch,data[start],4);
        DeviceDetail::Sync<HardEvent::MTE2_S>();
        for(uint32_t i=0;i<4;++i) {
            if(scratch.GetValue(i)!=start+i+seed) {
                uint64_t output[8]={0,ProtocolFailure,0,0,0,0,0,0};
                DeviceDetail::Invalidate(mailbox,24);
                DeviceDetail::Publish(mailbox+128,output,DeviceDetail::ReadWord(mailbox,25),scratch);
                return;
            }
        }
    }
}
