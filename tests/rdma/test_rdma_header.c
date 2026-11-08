/*
 * 功能：从 C 包含 tilexr_rdma.h，检查 Host 信息、memory handle 和 device view/queue/state 的 ABI 大小与版本。
 * 调用关系：rdma_c_header 测试使用公共 POD 定义，不创建 communicator 或调用 RDMA runtime。
 *
 * Purpose: Include tilexr_rdma.h from C and check Host info, memory-handle and device view/queue/state ABI sizes/version.
 * Calls: The rdma_c_header test consumes public PODs without creating communicators or calling the RDMA runtime.
 */
#include "tilexr_rdma.h"
int main(void)
{
    TileXRRDMAInfo info = {0};
    TileXRRDMAMemHandle handle = 0;
    return sizeof(info) != 32 || sizeof(handle) != 8 || TILEXR_RDMA_DEVICE_VERSION != 3U ||
        sizeof(TileXRRDMADeviceView) != 168 || sizeof(TileXRRDMADeviceQueue) != 120 ||
        sizeof(TileXRRDMADeviceState) != 64;
}
