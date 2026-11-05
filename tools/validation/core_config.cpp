// Report the same compiled defaults/overrides used by Host, kernels and consumers.
#include <cstdio>
#include "ep_memory_server_dedup_count_layout.h"
#include "ep_memory_server_dedup_combine_config.h"

int main()
{
    std::printf("{\"dispatch_send_core_num\":%u,\"combine_send_core_num\":%u,\"dispatch_count_core_num\":%u}\n",
        TileXREp::kEpServerDedupSendCoreNum, TileXREp::kEpServerDedupCombineSendCoreNum,
        TileXREp::kEpServerDedupCountCoreNum);
}
