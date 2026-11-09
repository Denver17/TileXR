/*
 * 声明 DispatchCompact 及其 profiling 入口，将 FP16/BF16 Memory Dispatch 接入
 * ElasticMemory Pool；Device 按实际接收 token 行数申请同一 lease 中的 hidden/assist。
 * 调用者保持 communicator 和输入、计数存活，可通过 CompactParams::keepAlive 持有 owner。
 * profiling 回调在实际 service stream 准备 capture，完成后的采集和关闭由调用者负责。
 */
#ifndef TILEXR_ELASTIC_EP_H
#define TILEXR_ELASTIC_EP_H
#include "tilexr_elastic_memory.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
namespace TileXR { namespace Elastic {
struct CompactParams {
    std::shared_ptr<void> keepAlive; // Optional input/count owners, retained through native completion.
    TileXRCommPtr comm = nullptr;
    void* x = nullptr;
    int32_t* expertIds = nullptr;
    int64_t* expertTokenNums = nullptr;
    int32_t* sendCounts = nullptr;
    int64_t bs = 0, h = 0, topK = 0, experts = 0;
    TileXRDataType dtype = TILEXR_DATA_TYPE_BFP16;
};
// FP16/BF16, ordinary Memory, no quantization/shared experts; old EP entrypoints remain unchanged.
std::shared_ptr<Pending> DispatchCompact(Pool& pool, const CompactParams& params, aclrtStream inputStream);
// Diagnostic overload: prepare runs before launch on the native service thread and returns
// the GM descriptor from TileXR::Profiling::AclCapture::DeviceArgs(). It must not wait for
// this kernel or Collect/Close its capture. Keep capture/context alive through Pending::Wait;
// collect/close in the Pool's creating ACL context before Pool close. A build with
// compact profiling OFF rejects this entry.
using CompactProfilePrepare = std::function<uint64_t(aclrtStream, uint32_t)>;
std::shared_ptr<Pending> DispatchCompactProfiled(Pool& pool, const CompactParams& params,
    aclrtStream inputStream, CompactProfilePrepare prepare);
// hidden occupies rows*h*2 bytes; assist starts at this 64-byte-aligned offset in the same lease.
inline uint64_t CompactAssistOffset(uint64_t rows, uint64_t h) { return (rows*h*2+63)&~uint64_t(63); }
} }
#endif
