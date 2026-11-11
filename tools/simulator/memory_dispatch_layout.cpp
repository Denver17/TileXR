/*
 * 读取当前头文件中的 CommArgs 布局和 Memory window 常量，供仿真生成器序列化输入。
 * 这是 Host 程序，不依赖 CANN，不启动 kernel；输出的 JSON 与生成用例一同保存。
 */
#include <cstddef>
#include <iostream>

#include "comm_args.h"
#include "ep_memory_window.h"

int main()
{
    static_assert(sizeof(GM_ADDR) == 8, "Simulator requires 64-bit addresses");
    std::cout << "{\"comm_args_bytes\":" << sizeof(TileXR::CommArgs)
              << ",\"max_ranks\":" << TileXR::TILEXR_MAX_RANK_SIZE
              << ",\"rank\":" << offsetof(TileXR::CommArgs, rank)
              << ",\"local_rank\":" << offsetof(TileXR::CommArgs, localRank)
              << ",\"rank_size\":" << offsetof(TileXR::CommArgs, rankSize)
              << ",\"local_rank_size\":" << offsetof(TileXR::CommArgs, localRankSize)
              << ",\"extra_flag\":" << offsetof(TileXR::CommArgs, extraFlag)
              << ",\"peer_mems\":" << offsetof(TileXR::CommArgs, peerMems)
              << ",\"peer_mem_bytes\":" << offsetof(TileXR::CommArgs, peerMemBytes)
              << ",\"memory_flags\":"
              << (TileXR::ExtraFlag::TOPO_910A5 | TileXR::ExtraFlag::MEMORY_ONLY)
              << ",\"round_stride\":" << TileXREp::kEpMemoryRoundStrideBytes
              << ",\"peer_bytes\":" << TileXREp::kEpMemoryRequiredPeerBytes
              << ",\"ipc_data_offset\":" << TileXR::IPC_DATA_OFFSET
              << ",\"state_bytes\":" << TileXREp::kEpMemoryStateWindowBytes
              << ",\"count_workspace_end\":" << TileXREp::kEpMemoryRecvCountWorkspaceEnd
              << ",\"cumsum_offset\":" << TileXREp::kEpMemoryDispatchCumsumOffset
              << ",\"cumsum_flag_offset\":" << TileXREp::kEpMemoryDispatchCumsumFlagOffset
              << "}\n";
}
