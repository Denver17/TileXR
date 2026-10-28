/* Compile as C and C++ to verify the installed Relay header and exported entrypoints. */
#include "tilexr_relay.h"
int main(void)
{
    TileXRRelayPlan plan = 0;
    TileXRRelayExecution execution = 0;
    uint64_t bytes = 0;
    return !(TileXRRelayPlanCreate(0, &plan) == -3 &&
        TileXRRelayPlanGetWorkspaceSize(0, &bytes) == -3 &&
        TileXRRelayLaunch(0, 0, 0, 0, 0, 0, 0) == -3 &&
        TileXRRelayPlanDestroy(0) == -3 &&
        TileXRRelayExecutionCreate(0, 0, 0, 0, 0, 0, &execution) == -3 &&
        execution == 0 && TileXRRelayExecutionLaunch(0, 0) == -3 &&
        TileXRRelayExecutionDestroy(0) == -3);
}
