/*
 * 功能：检查安装后的 diagnostics 头文件、链接符号、默认配置和非法 sleep_at 调用。
 * 调用关系：安装消费测试构建并运行本程序；不创建 session 或启动 device kernel。
 */
#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"

int main()
{
    TileXRStragglerConfig config {};
    if (TileXRStragglerConfigDefault(&config) != TileXR::TILEXR_SUCCESS) {
        return 1;
    }
    if (TileXRClockSleepAt(nullptr, 0, nullptr) != TileXR::TILEXR_ERROR_PARA_CHECK_FAIL) {
        return 3;
    }
    return config.clockSyncSamples == 128u && config.clockSyncWarmupSamples == 32u &&
        config.clockSyncSamples >= TileXR::TILEXR_CLOCK_SYNC_MIN_SAMPLES &&
        sizeof(TileXR::TileXRStragglerDeviceSlot) == 192u ? 0 : 2;
}
